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
#include <stdexcept>
#include <stack>
#include <optional>
#include <algorithm>

namespace ms {

// ---------- minimal hazard pointers (just enough for MS queue head) ----------
struct hazard_domain {
  static constexpr std::size_t max_slots = 2048; 
  std::atomic<void*> slots[max_slots]{}; // cleared to nullptr
  
  // Slot management
  std::mutex m_slot;
  std::stack<unsigned> free_slots;
  unsigned next_index{0}; 

  unsigned acquire_slot() {
    std::lock_guard<std::mutex> lk(m_slot);
    if (!free_slots.empty()) {
      unsigned id = free_slots.top();
      free_slots.pop();
      return id;
    }
    if (next_index >= max_slots) throw std::runtime_error("hazard_domain: out of slots");
    return next_index++;
  }

  void return_slot(unsigned id) {
    std::lock_guard<std::mutex> lk(m_slot);
    slots[id].store(nullptr, std::memory_order_release); 
    free_slots.push(id);
  }
};

inline hazard_domain& global_hazard_domain() {
  static hazard_domain* hd = new hazard_domain(); // Leaky singleton (immortal)
  return *hd;
}

// --------------------------- Michael–Scott MPMC queue ------------------------
template <class T>
class ms_queue {
  struct node {
    std::atomic<node*> next{nullptr};
    std::optional<T> value; 
    node() = default;
    explicit node(T v) : value(std::move(v)) {}
  };

  std::atomic<node*> head_{nullptr};
  std::atomic<node*> tail_{nullptr};

  // Thread-local retirement manager
  struct RetirementManager {
      std::vector<node*> retired_list;
      static constexpr std::size_t threshold = 64;

      ~RetirementManager() {
          // Push remaining retired nodes to global pool on thread exit
          global_retire().add(retired_list);
      }

      void push(node* n) {
          retired_list.push_back(n);
          if (retired_list.size() >= threshold) {
              scan();
          }
      }

      void scan() {
          auto& hd = global_hazard_domain();
          std::vector<void*> snapshot;
          
          {
             std::lock_guard<std::mutex> lk(hd.m_slot);
             // Snapshot active hazards
             unsigned limit = hd.next_index;
             snapshot.reserve(limit);
             for (unsigned i = 0; i < limit; ++i) {
                 snapshot.push_back(hd.slots[i].load(std::memory_order_acquire));
             }
          } 

          // Steal from global retirement occasionally (e.g. if we have few local nodes, or always try)
          global_retire().steal_to(retired_list);

          // Sort snapshot for binary search O(S log S)
          std::sort(snapshot.begin(), snapshot.end());
          
          // Skip nullptrs (which sort to the beginning)
          auto range_start = std::upper_bound(snapshot.begin(), snapshot.end(), (void*)nullptr);

          // Filter without holding the lock
          auto it = retired_list.begin();
          while (it != retired_list.end()) {
              if (std::binary_search(range_start, snapshot.end(), static_cast<void*>(*it))) {
                  // Held
                  ++it;
              } else { 
                  delete *it; 
                  it = retired_list.erase(it); 
              }
          }
      }
  };

  struct GlobalRetirement {
      std::mutex m;
      std::vector<node*> list;
      
      void add(std::vector<node*>& v) {
          if (v.empty()) return;
          std::lock_guard<std::mutex> lk(m);
          list.insert(list.end(), v.begin(), v.end());
          v.clear();
      }
      
      void steal_to(std::vector<node*>& v) {
          std::unique_lock<std::mutex> lk(m, std::try_to_lock);
          if (lk.owns_lock() && !list.empty()) {
              v.insert(v.end(), list.begin(), list.end());
              list.clear();
          }
      }
      
      ~GlobalRetirement() {
          // Intentionally leak on shutdown to avoid race with live hazards/threads.
          // OS will reclaim memory.
      }
      
      void drain() {
          std::lock_guard<std::mutex> lk(m);
          for (auto n : list) delete n;
          list.clear();
      }
  };

  static GlobalRetirement& global_retire() {
      static GlobalRetirement* gr = new GlobalRetirement(); // Leaky singleton (immortal)
      return *gr;
  }

  static RetirementManager& get_retire_manager() {
      static thread_local RetirementManager rm;
      return rm;
  }

  struct hp_owner {
    unsigned slot0, slot1;
    hp_owner() {
      auto& hd = global_hazard_domain();
      slot0 = hd.acquire_slot();
      slot1 = hd.acquire_slot();
    }
    ~hp_owner() {
      auto& hd = global_hazard_domain();
      hd.return_slot(slot0);
      hd.return_slot(slot1);
    }
  };

  hp_owner& get_hp_() const {
    static thread_local hp_owner hp;
    return hp;
  }

  void retire_or_delete_(node* old) {
      get_retire_manager().push(old);
  }

public:
  // DANGER: Only call this when ALL threads that ever touched ANY ms_queue instance 
  // are dead or quiescent (no active hazards, no TLS flushing).
  // Specifically: all thread_local destructors for RetirementManager must have completed.
  // This is typically called at strictly serialized process exit.
  static void drain_retired() {
      global_retire().drain();
  }

  ms_queue() {
    node* dummy = new node();
    head_.store(dummy, std::memory_order_relaxed);
    tail_.store(dummy, std::memory_order_relaxed);
  }
  ~ms_queue() {
    // REQUIREMENT: External synchronization required.
    // In practice, safe reclamation requires all threads' TLS retirement managers to be destroyed.
    // But since pool destroys queue after joining threads, it is safe to walk and delete.
    node* n = head_.load(std::memory_order_relaxed);
    while (n) {
      node* next = n->next.load(std::memory_order_relaxed);
      delete n;
      n = next;
    }
  }
  ms_queue(const ms_queue&) = delete;
  ms_queue& operator=(const ms_queue&) = delete;

  void enqueue(T v) {
    node* n = new node(std::move(v));
    auto& hp = get_hp_();
    auto& hd = global_hazard_domain();
    
    for (;;) {
      node* t = tail_.load(std::memory_order_acquire);
      // Protect tail (slot 0)
      hd.slots[hp.slot0].store(t, std::memory_order_release);
      if (t != tail_.load(std::memory_order_acquire)) {
          // Retry immediately.
          hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
          continue; 
      }

      node* next = t->next.load(std::memory_order_acquire);
      // Protect next (slot 1)
      hd.slots[hp.slot1].store(next, std::memory_order_release);
      
      // Strict Re-validation
      if (t != tail_.load(std::memory_order_acquire)) {
          hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
          continue;
      }
      
      if (next != t->next.load(std::memory_order_acquire)) {
          hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
          continue; 
      }
      
      if (t == tail_.load(std::memory_order_acquire)) {
        if (next == nullptr) {
          if (t->next.compare_exchange_weak(next, n, std::memory_order_acq_rel, std::memory_order_acquire)) {
            tail_.compare_exchange_strong(t, n, std::memory_order_acq_rel, std::memory_order_acquire);
            hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
            hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
            return;
          }
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
        } else {
          // tail lagging, help advance
          tail_.compare_exchange_strong(t, next, std::memory_order_acq_rel, std::memory_order_acquire);
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
        }
      } 
      // Failed CAS, clear slots and retry
      hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
      hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
    }
  }

  bool try_dequeue(T& out) {
    auto& hp = get_hp_();
    auto& hd = global_hazard_domain();

    for (;;) {
      node* h = head_.load(std::memory_order_acquire);
      // Protect head (slot 0)
      hd.slots[hp.slot0].store(h, std::memory_order_release);
      if (h != head_.load(std::memory_order_acquire)) {
          hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
          continue; 
      }
      
      node* t = tail_.load(std::memory_order_acquire);
      node* next = h->next.load(std::memory_order_acquire);

      // Protect next (slot 1)
      hd.slots[hp.slot1].store(next, std::memory_order_release);

      // Re-validate head
      if (h != head_.load(std::memory_order_acquire)) {
          hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
          continue;
      }
      
      // Re-validate next is still h->next
      if (next != h->next.load(std::memory_order_acquire)) {
          hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
          continue;
      }

      if (h == t) {
        if (next == nullptr) {
          // Empty
          hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
          return false;
        }
        // Help advance tail
        tail_.compare_exchange_strong(t, next, std::memory_order_acq_rel, std::memory_order_acquire);
        
        hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
        hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
      } else {
        if (next == nullptr) {
             // Inconsistent, retry
             hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
             hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
             continue; 
        }
        
        // Move AFTER winning the CAS
        if (head_.compare_exchange_strong(h, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
          assert(next->value.has_value());
          out = std::move(*next->value);
          next->value.reset(); // Destroy moved-from object immediately
          
          hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
          hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
          
          retire_or_delete_(h);
          return true;
        }
        hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
        hd.slots[hp.slot1].store(nullptr, std::memory_order_release);
      }
    }
  }

  bool empty() const {
    auto& hp = get_hp_();
    auto& hd = global_hazard_domain();
    
    node* h = head_.load(std::memory_order_acquire);
    hd.slots[hp.slot0].store(h, std::memory_order_release);
    
    if (h != head_.load(std::memory_order_acquire)) {
        hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
        return false; // conservative
    }
    
    bool is_empty = (h->next.load(std::memory_order_acquire) == nullptr);
    hd.slots[hp.slot0].store(nullptr, std::memory_order_release);
    return is_empty;
  }
};


// --------------------------------- thread pool --------------------------------
class jthread_pool {
public:
  using task_type = std::function<void()>;

  explicit jthread_pool(std::size_t threads = std::thread::hardware_concurrency()) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this](std::stop_token st){ worker_loop_(st); });
    }
  }

  ~jthread_pool() {
    request_stop();
    workers_.clear();                  // joins all workers
    ms_queue<task_type>::drain_retired(); // safe now: no live worker hazards
  }

  jthread_pool(const jthread_pool&) = delete;
  jthread_pool& operator=(const jthread_pool&) = delete;

  // Serialize stop with submit via submit_mutex_, keep lock order:
  // submit_mutex_ -> cv_mutex_
  void request_stop() noexcept {
    {
      std::lock_guard lk(submit_mutex_);
      std::lock_guard lk2(cv_mutex_);
      stop_.store(true, std::memory_order_release);
      wake_seq_.fetch_add(1, std::memory_order_release); // publish a wake event
    }
    cv_.notify_all();
  }

  template <class F, class... Args>
  auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using R = std::invoke_result_t<F, Args...>;

    auto bound   = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    auto sp_task = std::make_shared<std::packaged_task<R()>>(std::move(bound));
    std::future<R> fut = sp_task->get_future();
    task_type t = [sp_task]() mutable { (*sp_task)(); };

    {
      std::lock_guard lk(submit_mutex_);
      if (stop_.load(std::memory_order_acquire))
        throw std::runtime_error("jthread_pool stopped");

      // 1) enqueue first (so work is really present)
      q_.enqueue(std::move(t));

      // 2) publish a wake event under cv_mutex_ for coherent waiting
      {
        std::lock_guard lk2(cv_mutex_);
        wake_seq_.fetch_add(1, std::memory_order_release);
      }
    }

    cv_.notify_one();
    return fut;
  }

  std::size_t size() const noexcept { return workers_.size(); }

private:
  void worker_loop_(std::stop_token /*st*/) {
    std::uint64_t seen = wake_seq_.load(std::memory_order_acquire);

    for (;;) {
      task_type task;

      // Drain available tasks without holding cv_mutex_
      while (q_.try_dequeue(task)) {
        task();
      }

      // If stop requested, drain remaining and exit.
      if (stop_.load(std::memory_order_acquire)) {
        while (q_.try_dequeue(task)) task();
        return;
      }

      // Sleep until a new wake event or stop.
      std::unique_lock lk(cv_mutex_);
      cv_.wait(lk, [this, &seen]{
        return stop_.load(std::memory_order_acquire) ||
               wake_seq_.load(std::memory_order_acquire) != seen;
      });
      seen = wake_seq_.load(std::memory_order_acquire);
      // loop continues; we’ll try_dequeue again
    }
  }

  ms_queue<task_type> q_{};

  std::condition_variable_any cv_;
  std::mutex cv_mutex_;
  std::mutex submit_mutex_;
  std::atomic<bool> stop_{false};

  // Monotonic “event count” for waking sleepers (no underflow / no spin window)
  std::atomic<std::uint64_t> wake_seq_{0};

  std::vector<std::jthread> workers_{};
};

} // namespace ms