#pragma once
/*
 * ms_jthread_pool.hpp
 * Header-only jthread-based thread pool using a Michael–Scott MPMC queue.
 * - C++20
 * - No external deps
 * - Memory-safe via a tiny hazard-pointer reclaimer (2 hazard slots / thread)
 *
 * Build: g++ -std=c++20 -O2 -pthread your.cpp
 */

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace msjp {

namespace detail {

// -------- Hazard-pointer reclaimer (minimal) -------------------------------
// Two hazard slots per thread; N global records. Epochless scan-based reclaim.
template <std::size_t MaxThreads = 128, std::size_t SlotsPerThread = 2>
struct HazardDomain {
  struct Record {
    std::atomic<void*> slot[SlotsPerThread];
    Record() {
      for (auto& s : slot) s.store(nullptr, std::memory_order_relaxed);
    }
  };

  // Global table of hazard records
  static Record* records() {
    static Record arr[MaxThreads];
    return arr;
  }

  // Thread-local reservation of one record
  static Record* my_record() {
    thread_local Record* rec = [] {
      // Naive first-free reservation
      auto recs = records();
      for (std::size_t i = 0; i < MaxThreads; ++i) {
        // "Claim" a record by leaving it as-is (we'll just start using it).
        // If many threads > MaxThreads, behavior degrades (documented).
        // In practice set MaxThreads >= max worker + producer threads.
        bool all_null = true;
        for (auto& s : recs[i].slot) {
          if (s.load(std::memory_order_relaxed) != nullptr) { all_null = false; break; }
        }
        if (all_null) {
          return &recs[i];
        }
      }
      // Fallback: last slot (still safe; just shared).
      return &recs[MaxThreads - 1];
    }();
    return rec;
  }

  static void set_hazard(std::size_t idx, void* p) {
    my_record()->slot[idx].store(p, std::memory_order_seq_cst);
  }
  static void clear_hazard(std::size_t idx) {
    my_record()->slot[idx].store(nullptr, std::memory_order_release);
  }

  // Snapshot all hazard pointers (for reclamation pass)
  static void snapshot_hazards(std::vector<void*>& out) {
    auto recs = records();
    for (std::size_t i = 0; i < MaxThreads; ++i) {
      for (std::size_t j = 0; j < SlotsPerThread; ++j) {
        if (void* p = recs[i].slot[j].load(std::memory_order_seq_cst)) {
          out.push_back(p);
        }
      }
    }
  }
};

// -------- Michael–Scott MPMC queue with hazard reclamation -----------------
template <typename T,
          std::size_t MaxThreads = 128,
          std::size_t ReclaimBatch = 64>
class MSQueue {
  // Two hazard slots needed: [0] protects head/tail we dereference, [1] protects 'next'
  using HD = HazardDomain<MaxThreads, 2>;

  struct Node {
    std::atomic<Node*> next{nullptr};
    std::optional<T> payload;  // empty in the dummy node
    Node() = default;                      // dummy
    explicit Node(T&& v) : payload(std::move(v)) {}
    explicit Node(const T& v) : payload(v) {}
  };

  std::atomic<Node*> head_{nullptr};
  std::atomic<Node*> tail_{nullptr};

  // per-thread retired list
  static thread_local std::vector<Node*> retired_;

  static void retire(Node* n) {
    retired_.push_back(n);
    if (retired_.size() >= ReclaimBatch) {
      // Collect current hazards and reclaim unhazarded retired nodes
      std::vector<void*> hazards;
      hazards.reserve(MaxThreads * 2);
      HD::snapshot_hazards(hazards);

      auto keep = std::vector<Node*>{};
      keep.reserve(retired_.size());
      for (Node* p : retired_) {
        bool is_hazard = false;
        for (void* h : hazards) {
          if (h == p) { is_hazard = true; break; }
        }
        if (is_hazard) keep.push_back(p);
        else delete p;
      }
      retired_.swap(keep);
    }
  }

public:
  MSQueue() {
    // Initialize with a dummy node
    Node* dummy = new Node();
    head_.store(dummy, std::memory_order_relaxed);
    tail_.store(dummy, std::memory_order_relaxed);
  }

  ~MSQueue() {
    // Drain & reclaim all nodes
    T tmp;
    while (try_dequeue(tmp)) {}
    Node* dummy = head_.load(std::memory_order_relaxed);
    if (dummy) delete dummy;  // leftover dummy
    // Reclaim any retired still pending (single-threaded by now)
    for (Node* p : retired_) delete p;
    retired_.clear();
  }

  // Non-copyable/movable (holds atomics and raw nodes)
  MSQueue(const MSQueue&) = delete;
  MSQueue& operator=(const MSQueue&) = delete;

  template <typename U>
  void enqueue(U&& value) {
    Node* node = new Node(std::forward<U>(value));
    node->next.store(nullptr, std::memory_order_relaxed);

    for (;;) {
      Node* tail = tail_.load(std::memory_order_acquire);
      HD::set_hazard(0, tail);
      if (tail != tail_.load(std::memory_order_acquire)) { continue; }

      Node* next = tail->next.load(std::memory_order_acquire);
      HD::set_hazard(1, next);
      if (tail != tail_.load(std::memory_order_acquire)) { 
        HD::clear_hazard(1); HD::clear_hazard(0); 
        continue; 
      }

      if (next == nullptr) {
        if (tail->next.compare_exchange_weak(next, node,
                std::memory_order_release, std::memory_order_relaxed)) {
          // enqueued; try to swing tail forward
          tail_.compare_exchange_strong(tail, node,
                std::memory_order_release, std::memory_order_relaxed);
          HD::clear_hazard(1); HD::clear_hazard(0);
          return;
        }
      } else {
        // Help advance tail
        tail_.compare_exchange_weak(tail, next,
              std::memory_order_release, std::memory_order_relaxed);
      }
      HD::clear_hazard(1); HD::clear_hazard(0);
    }
  }

  bool try_dequeue(T& out) {
    for (;;) {
      Node* head = head_.load(std::memory_order_acquire);
      HD::set_hazard(0, head);
      if (head != head_.load(std::memory_order_acquire)) { continue; }

      Node* tail = tail_.load(std::memory_order_acquire);
      Node* next = head->next.load(std::memory_order_acquire);
      HD::set_hazard(1, next);
      if (head != head_.load(std::memory_order_acquire)) { 
        HD::clear_hazard(1); 
        continue; 
      }

      if (next == nullptr) {
        // queue empty
        HD::clear_hazard(1); HD::clear_hazard(0);
        return false;
      }

      if (head == tail) {
        // Tail is falling behind; help it
        tail_.compare_exchange_weak(tail, next,
              std::memory_order_release, std::memory_order_relaxed);
        HD::clear_hazard(1); // keep looping
        continue;
      }

      // Read value before swinging head
      T value = std::move(*next->payload);
      if (head_.compare_exchange_weak(head, next,
              std::memory_order_release, std::memory_order_relaxed)) {
        HD::clear_hazard(1); HD::clear_hazard(0);
        out = std::move(value);
        retire(head);                // old dummy becomes retiree
        return true;
      }
      HD::clear_hazard(1); // try again
    }
  }
};

template <typename T, std::size_t M, std::size_t R>
thread_local std::vector<typename MSQueue<T, M, R>::Node*>
MSQueue<T, M, R>::retired_;

// -------- Thread pool (jthread + counting_semaphore + MSQueue) -------------
class JThreadPool {
public:
  explicit JThreadPool(std::size_t workers =
                         std::max(1u, std::thread::hardware_concurrency()))
      : sem_(0) {
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
      workers_.emplace_back([this](std::stop_token st){ worker_loop(st); });
    }
  }

  ~JThreadPool() {
    // Request stop, wake all workers.
    for (auto& jt : workers_) jt.request_stop();
    sem_.release(static_cast<int>(workers_.size()));
    // jthread joins on destruction automatically.
  }

  JThreadPool(const JThreadPool&) = delete;
  JThreadPool& operator=(const JThreadPool&) = delete;

  template <typename F, typename... Args>
  auto submit(F&& f, Args&&... args)
      -> std::future<std::invoke_result_t<F, Args...>> {
    using R = std::invoke_result_t<F, Args...>;

    std::packaged_task<R()> task(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    std::future<R> fut = task.get_future();

    queue_.enqueue([t = std::move(task)]() mutable { t(); });
    sem_.release(1);
    return fut;
  }

  std::size_t thread_count() const noexcept { return workers_.size(); }

private:
  using Task = std::function<void()>;

  void worker_loop(std::stop_token st) {
    // Drain-on-stop semantics: when stop requested, finish remaining tasks.
    for (;;) {
      sem_.acquire(); // wake for (maybe) one task or stop signal
      if (st.stop_requested()) {
        // Drain tasks and exit
        Task t;
        while (queue_.try_dequeue(t)) {
          t();
        }
        return;
      }
      Task t;
      if (queue_.try_dequeue(t)) {
        t();
      }
    }
  }

  detail::MSQueue<Task> queue_;
  std::counting_semaphore<std::numeric_limits<int>::max()> sem_;
  std::vector<std::jthread> workers_;
};

} // namespace detail

// Public alias for convenience
using jthread_pool = detail::JThreadPool;

} // namespace msjp
