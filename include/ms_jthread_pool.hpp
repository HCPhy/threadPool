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
  static constexpr std::size_t max_slots = 2048; // enough for ~1000 threads with 2 slots each
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

  struct thread_slots {
    unsigned slot0;
    unsigned slot1;
  };

  const thread_slots& get_slots_() const {
    static thread_local thread_slots s = {
      global_hazard_domain().acquire_slot(),
      global_hazard_domain().acquire_slot()
    };
    return s;
  }

  void protect_(unsigned slot_idx, node* p) const noexcept {
    global_hazard_domain().slots[slot_idx].store(p, std::memory_order_release);
  }
  void clear_slot_(unsigned slot_idx) const noexcept {
    global_hazard_domain().slots[slot_idx].store(nullptr, std::memory_order_release);
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
    const auto& s = get_slots_();
    for (;;) {
      node* t = tail_.load(std::memory_order_acquire);
      protect_(s.slot0, t);
      if (t != tail_.load(std::memory_order_acquire)) continue; // tail changed

      node* next = t->next.load(std::memory_order_acquire);
      if (t == tail_.load(std::memory_order_acquire)) {
        if (next == nullptr) {
          if (t->next.compare_exchange_weak(next, n, std::memory_order_release, std::memory_order_acquire)) {
            tail_.compare_exchange_strong(t, n, std::memory_order_release, std::memory_order_acquire);
            clear_slot_(s.slot0);
            return;
          }
        } else {
          tail_.compare_exchange_strong(t, next, std::memory_order_release, std::memory_order_acquire);
        }
      }
    }
  }

  bool try_dequeue(T& out) {
    const auto& s = get_slots_();
    for (;;) {
      node* h = head_.load(std::memory_order_acquire);
      protect_(s.slot0, h);
      if (h != head_.load(std::memory_order_acquire)) {
        // Head changed during protection, retry
        continue;
      }
      
      node* t = tail_.load(std::memory_order_acquire);
      node* next = h->next.load(std::memory_order_acquire);
      
      // Protect next *before* we potentially use it or retire h
      protect_(s.slot1, next);
      
      // Re-validate h has not changed to ensure 'next' is still a valid successor of the current head
      // and that h has not been retired/reused.
      if (h != head_.load(std::memory_order_acquire)) {
        continue;
      }
      
      if (next == nullptr) {
        // Queue likely empty, but need to check if t == h
        // If h == t and next is null, queue is truly empty
        if (h == t) {
            clear_slot_(s.slot0);
            clear_slot_(s.slot1);
            return false;
        }
        // If h != t (and next is null), concurrent enqueue in progress?
        // Fall through to retry or advance tail logic?
        // Standard MS queue algorithm: if next is null, it might be empty if h==t.
        // If h!=t and next is null, then tail is lagging.
        // But with next == null, we can't dequeue.
        // Let's re-read standard logic.
        // "if head == tail ... if next == null -> empty"
        // Wait, if I'm here, h is protected.
        // If queue is empty, next is null. 
        // We cleared slot0/1 and returned false.
      }
      
      // If we are here, next might be non-null.
      
      // Standard MS checks:
      if (h == t) {
        // Queue might be empty or tail falling behind
        if (next == nullptr) {
          clear_slot_(s.slot0);
          clear_slot_(s.slot1);
          return false;
        }
        // Tail is falling behind, try to advance it
        tail_.compare_exchange_strong(t, next, std::memory_order_release, std::memory_order_acquire);
      } else {
        // h != t, and we have a next.
        // Read value from next BEFORE CAS head.
        // next is protected by slot1. 
        if (next) { // Should be non-null if h!=t in a valid state usually, but check
             out = std::move(next->value);
             if (head_.compare_exchange_strong(h, next, std::memory_order_release, std::memory_order_acquire)) {
               // Success!
               
               // We need to clear slots before retiring h, 
               // although retiring h is safe because we (this thread) hold h in slot0? 
               // No, we retire h so WE release our claim, but we check if OTHERS hold it.
               // We must clear OUR slot0 so we don't prevent reclamation of h in our own scan?
               // Actually scan checks "any_holds".
               // Conventionally: clear slots then retire.
               clear_slot_(s.slot0);
               clear_slot_(s.slot1); 
               retire_or_delete_(h);
               return true;
             }
        }
      }
    }
  }

  bool empty() const {
    const auto& s = get_slots_();
    node* h = head_.load(std::memory_order_acquire);
    protect_(s.slot0, h);
    if (h != head_.load(std::memory_order_acquire)) {
        // It changed, just best effort
        clear_slot_(s.slot0);
        return false; // conservative
    }
    bool is_empty = (h->next.load(std::memory_order_acquire) == nullptr);
    clear_slot_(s.slot0);
    return is_empty;
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
    // std::jthread dtor joins (and requests stop) automatically
    // but we notify in request_stop now.
  }

  jthread_pool(const jthread_pool&) = delete;
  jthread_pool& operator=(const jthread_pool&) = delete;

  void request_stop() noexcept { 
      stop_.store(true, std::memory_order_release); 
      cv_.notify_all();
  }

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