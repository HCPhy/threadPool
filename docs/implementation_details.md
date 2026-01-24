# Implementation Details

This document bridges the gap between the algorithms and the C++20 implementation in `include/ms_jthread_pool.hpp`.

## 1. Memory Model & Ordering

We utilize C++11 memory model semantics to ensure correctness while minimizing hardware fence overhead.

### Acquire-Release Semantics

We strictly avoid `std::memory_order_seq_cst` (Sequentially Consistent) due to its high cost on non-x86 architectures (e.g., ARM/Apple Silicon).

* **Producing (Store)**: Used when updating pointers (e.g., `tail_.store(...)`). We use `std::memory_order_release`. This ensures that any writes to the node's payload happen-before the pointer becomes visible to other threads.
* **Consuming (Load)**: Used when reading pointers (e.g., `head_.load(...)`). We use `std::memory_order_acquire`. This ensures we see the data written by the releasing thread.

### The Hazard Pointer Handshake

Hazard pointers require a specific ordering to guarantee safety:

1. **Store HP (Release)**: `hp_slot.store(ptr, std::memory_order_release)` ensures the protection is visible before we check the validity.
2. **Check Ptr (Acquire)**: `ptr.load(std::memory_order_acquire)` ensures we re-validate against the latest global state.

---

## 2. Hazard Domain Architecture

The `hazard_domain` is a singleton managing the pool of reservation slots.

* **Slot Allocation**: We use a `std::stack<unsigned>` protected by a `std::mutex` to manage free slot indices.
  * *Note*: While the queue operations are lock-free, *acquiring* a hazard slot technically involves a lock. However, slots are cached in Thread Local Storage (TLS) via the `hp_owner` struct, so this lock is hit only once per thread creation, effectively making the hot-path lock-free.
* **Leaky Singleton**: The global `hazard_domain` is allocated via `new` and never deleted.
  * *Reason*: To prevent the **Static Deinitialization Order Fiasco**. If a worker thread (managed by a static pool) tries to access the hazard domain during program exit after the domain has been destroyed, a segfault occurs. Leaking the domain ensures it survives until the OS reclaims process memory.

---

## 3. The Thread Pool Engine

The `jthread_pool` builds upon the queue to provide task scheduling.

### The "Fast-Path/Slow-Path" Loop

The worker loop is designed to minimize latency:

1. **Fast Path (Spin/Poll)**: The worker loops purely on `q_.try_dequeue()`. If tasks are available, it executes them immediately without touching mutexes or condition variables.
2. **Slow Path (Wait)**: If the queue is empty, the thread checks the **Event Count** (`wake_seq_`) and waits on `std::condition_variable`.

### Event Count Optimization

To avoid the "Lost Wakeup" problem (where a notification is sent *after* a thread checks empty but *before* it sleeps):

* **Sequence Number**: We maintain a monotonic atomic counter `wake_seq_`.
* **Snapshot**: The worker snapshots `wake_seq_` *before* checking the queue.
* **Predicate**: The condition variable wait includes a check: `current_seq != snapshot`.
* This guarantees that if a task was added while the thread was transitioning to sleep, the sequence number mismatch will prevent the thread from sleeping.

### Type Erasure

We use `std::function<void()>` for tasks.

* **Issue**: `std::packaged_task` (used for `std::future`) is move-only, but `std::function` requires copy-constructibility.
* **Solution**: We wrap the `packaged_task` in a `std::shared_ptr`. The `shared_ptr` is copyable, satisfying `std::function`, while the underlying task remains unique and shared.

---

## 4. Lifecycle & Safety

### Safe Shutdown

1. **Stop Token**: We use `std::stop_token` (C++20). The worker loop checks `st.stop_requested()` periodically.
2. **Join**: The `std::jthread` destructor automatically joins.
3. **Drain**: The pool destructor ensures `ms_queue::drain_retired()` is called. This forces the deletion of any nodes lingering in the global retirement list, ensuring zero memory leaks (verified by Valgrind/ASAN).
