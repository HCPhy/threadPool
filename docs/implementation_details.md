# Implementation Details

This document dives into the C++ implementation specifics of the `jthread_pool`.

## 1. C++20 Concurrency Primitives

### `std::jthread` & `std::stop_token`

We use `std::jthread` (C++20) instead of `std::thread`.

- **Auto-join**: `jthread` automatically joins in its destructor. This simplifies the cleaner shutdown logic in `~jthread_pool`.
- **Cooperative Interruption**: We pass a `std::stop_token` to the worker loop. This allows us to request cancellation cleanly via `st.stop_requested()`.

### `std::atomic` Memory Ordering

We use fine-grained memory orderings for performance:

- `std::memory_order_relaxed`: Used for counters or operations where ordering respecting other threads doesn't matter.
- `std::memory_order_acquire`: Used when reading `head`/`tail` or the `stop_` flag. Ensures we see writes from other threads *before* this point.
- `std::memory_order_release`: Used when updating pointers or flags. Ensures our writes are visible to other threads doing an acquire.

## 2. Thread Pool Architecture

### The Worker Loop

The core of the pool is the `worker_loop_`. It faces a classic challenge: **Sleeping vs. Spinning**.

- **Lock-Free Fast Path**: The worker first tries to dequeue a task from the Michael-Scott queue *without* holding any mutex. This is extremely fast and scalable.
- **Notification Slow Path**: If the queue is empty, the worker locks a mutex and waits on a `std::condition_variable`.

### Optimization: Atomic Pending Count

We optimized the pool by introducing `std::atomic<std::ptrdiff_t> pending_task_count_`.

- **Old Way**: Lock mutex -> increment count -> unlock. (For EVERY task!)
- **New Way**: Atomic `fetch_add` (wait-free).

The worker loop logic:

```cpp
// Fast path: Keep working if tasks are available
while (q_.try_dequeue(task)) {
    pending_task_count_.fetch_sub(1, release);
    task();
}
// Slow path: Sleep only if really necessary
wait_on_cv();
```

## 3. Type Erasure (`std::function` vs Templates)

The queue stores `std::function<void()>`.

- **Pros**: It can store *any* callable (lambdas, function pointers, functors) with any return type (wrapped in a closure).
- **Cons**: It requires heap allocation and virtual dispatch details.

### `submit` Wrapper

The `submit` function bridges the gap between the user's typed callable and the generic `void()` task:

1. **`std::bind`**: Binds arguments to the function.
2. **`std::packaged_task<R()>`**: Wraps the bound function to create a `std::future`.
3. **`std::shared_ptr`**: `std::function` requires the callable to be copyable. `std::packaged_task` is move-only. We wrap it in a `shared_ptr` to make it copyable (the pointer is copied, not the task).

## 4. Hazard Pointers Implementation

The `hazard_domain` is a simplified global singleton.

- **`slots`**: A fixed array of atomic pointers.
- **`acquire_slot`**: A simple linear search (or fetch_add index) to assign a slot to a thread.
- **Complexity**: O(N) scan for `any_holds`. For a production system with thousands of threads, a more complex data structure (like a hash map or thread-local lists) would be needed.
