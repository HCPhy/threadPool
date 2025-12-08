#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ms {

// ---------- minimal hazard pointers (just enough for MS queue head) ----------
struct hazard_domain {
  static constexpr std::size_t max_slots = 1024;
  std::atomic<void*> slots[max_slots]{}; // cleared to nullptr
  std::atomic<unsigned> next{0};

  unsigned acquire_slot() {
    unsigned id = next.fetch_add(1, std::memory_order_relaxed);
    if (id >= max_slots) throw std::runtime_error("hazard_domain: out of slots");
    return id;
  }
  bool any_holds(void* p) const noexcept {
    for (std::size_t i = 0; i < max_slots; ++i)
      if (slots[i].load(std::memory_order_acquire) == p) return true;
    return false;
  }
};
inline hazard_domain& global_hazard_domain() {
  static hazard_domain hd;
  return hd;
}

// --------------------------- Michael–Scott MPMC queue ------------------------
template <class T>
class ms_queue {
  struct node {
    std::atomic<node*> next{nullptr};
    T value; // dummy head has default-constructed T
    node() = default;
    explicit node(T v) : value(std::move(v)) {}
  };

  std::atomic<node*> head_{nullptr};
  std::atomic<node*> tail_{nullptr};

  static thread_local std::vector<node*> retired_;
  static constexpr std::size_t retire_scan_threshold_ = 64;

  unsigned hazard_slot_() const {
    static thread_local unsigned slot = global_hazard_domain().acquire_slot();
    return slot;
  }
  void protect_(node* p) const noexcept {
    global_hazard_domain().slots[hazard_slot_()].store(p, std::memory_order_release);
  }
  void clear_hazard_() const noexcept {
    global_hazard_domain().slots[hazard_slot_()].store(nullptr, std::memory_order_release);
  }
  void retire_or_delete_(node* old) {
    auto& hd = global_hazard_domain();
    if (!hd.any_holds(old)) { delete old; return; }
    retired_.push_back(old);
    if (retired_.size() >= retire_scan_threshold_) {
      auto it = retired_.begin();
      while (it != retired_.end()) {
        if (!hd.any_holds(*it)) { delete *it; it = retired_.erase(it); }
        else { ++it; }
      }
    }
  }

public:
  ms_queue() {
    node* dummy = new node();
    head_.store(dummy, std::memory_order_relaxed);
    tail_.store(dummy, std::memory_order_relaxed);
  }
  ~ms_queue() {
    node* n = head_.load(std::memory_order_relaxed);
    while (n) { node* next = n->next.load(std::memory_order_relaxed); delete n; n = next; }
  }
  ms_queue(const ms_queue&) = delete;
  ms_queue& operator=(const ms_queue&) = delete;

  void enqueue(T v) {
    node* n = new node(std::move(v));
    for (;;) {
      node* t = tail_.load(std::memory_order_acquire);
      node* next = t->next.load(std::memory_order_acquire);
      if (t == tail_.load(std::memory_order_acquire)) {
        if (next == nullptr) {
          if (t->next.compare_exchange_weak(next, n, std::memory_order_release, std::memory_order_acquire)) {
            tail_.compare_exchange_strong(t, n, std::memory_order_release, std::memory_order_acquire);
            return;
          }
        } else {
          tail_.compare_exchange_strong(t, next, std::memory_order_release, std::memory_order_acquire);
        }
      }
    }
  }

  bool try_dequeue(T& out) {
    for (;;) {
      node* h = head_.load(std::memory_order_acquire);
      protect_(h);
      if (h != head_.load(std::memory_order_acquire)) continue;
      node* t = tail_.load(std::memory_order_acquire);
      node* next = h->next.load(std::memory_order_acquire);
      if (h == head_.load(std::memory_order_acquire)) {
        if (h == t) {
          if (next == nullptr) { clear_hazard_(); return false; }
          tail_.compare_exchange_strong(t, next, std::memory_order_release, std::memory_order_acquire);
        } else {
          out = std::move(next->value);
          if (head_.compare_exchange_strong(h, next, std::memory_order_release, std::memory_order_acquire)) {
            clear_hazard_();
            retire_or_delete_(h);
            return true;
          }
        }
      }
    }
  }

  bool empty() const {
    node* h = head_.load(std::memory_order_acquire);
    return h->next.load(std::memory_order_acquire) == nullptr;
  }
};
template <class T>
thread_local std::vector<typename ms_queue<T>::node*> ms_queue<T>::retired_{};

// --------------------------------- thread pool --------------------------------
class jthread_pool {
public:
  using task_type = std::function<void()>; // needs to be copyable for std::function

  explicit jthread_pool(std::size_t threads = std::thread::hardware_concurrency()) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this](std::stop_token st){ worker_loop_(st); });
    }
  }

  ~jthread_pool() {
    request_stop();
    cv_.notify_all(); // wake sleepers so they observe stop
    // std::jthread dtor joins (and requests stop) automatically
  }

  jthread_pool(const jthread_pool&) = delete;
  jthread_pool& operator=(const jthread_pool&) = delete;

  void request_stop() noexcept { stop_.store(true, std::memory_order_release); }

  template <class F, class... Args>
  auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using R = std::invoke_result_t<F, Args...>;

    if (stop_.load(std::memory_order_acquire)) throw std::runtime_error("jthread_pool stopped");

    // Bind to nullary and wrap in shared_ptr so the task is copyable for std::function
    auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    auto sp_task = std::make_shared<std::packaged_task<R()>>(std::move(bound));
    std::future<R> fut = sp_task->get_future();

    task_type t = [sp_task]() mutable { (*sp_task)(); }; // copies shared_ptr
    
    // Increment valid task count before enqueueing to ensure a waking worker sees it.
    pending_task_count_.fetch_add(1, std::memory_order_release);
    q_.enqueue(std::move(t));

    // Wake one worker. We do not need to hold the lock for notification.
    cv_.notify_one();
    return fut;
  }

  std::size_t size() const noexcept { return workers_.size(); }

private:
  void worker_loop_(std::stop_token st) {
    for (;;) {
      task_type task;
      // Fast path: loop while we can get tasks, without locking.
      while (q_.try_dequeue(task)) {
        // We consumed a task from the queue.
        pending_task_count_.fetch_sub(1, std::memory_order_release);
        task();
      }

      if (st.stop_requested() || stop_.load(std::memory_order_acquire)) {
        // Drain remaing tasks then exit
        while (q_.try_dequeue(task)) task();
        return;
      }

      // Slow path: no task found, wait for notification.
      std::unique_lock lk(m_);
      // Predicate: stop requested OR pending tasks available.
      // Note: We might wake up and fail to dequeue (some other worker got it), 
      // but that is handled by the outer loop which retries the inner while(try_dequeue).
      cv_.wait(lk, st, [this]{ 
        return stop_.load(std::memory_order_acquire) || 
               pending_task_count_.load(std::memory_order_acquire) > 0; 
      });
    }
  }

  ms_queue<task_type> q_{};

  std::mutex m_{};
  std::condition_variable_any cv_{};
  std::atomic<bool> stop_{false};
  std::atomic<std::ptrdiff_t> pending_task_count_{0};
  
  std::vector<std::jthread> workers_{};
};

} // namespace ms