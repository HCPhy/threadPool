# Implementation Details (The Nuts and Bolts)

This document connects the high-level theories to the actual C++ code you see in `include/ms_jthread_pool.hpp`.

--

## 1. C++20 Cool Tools

We use modern C++20 features to make life easier.

### `std::jthread` (The Auto-Joiner)

In older C++, `std::thread` is like a leaky faucet. If you forget to turn it off (call `.join()`), your program crashes.
`std::jthread` is a "Joining Thread". When it goes out of scope, it **automatically** cleans itself up. It's safe by default.

### `std::stop_token` (The Polite Stop Button)

How do you kill a thread?

- **The Old Way**: `pthread_kill` (shooting it in the head). Dangerous.
- **The Bad Way**: A global boolean flag `bool running = true`. Messy.
- **The C++20 Way**: `std::stop_token`.

Is a standard way to say "Hey, please finish up what you're doing and exit." inside the thread loop, checking `st.stop_requested()`.

---

## 2. Memory Ordering (The Brainless Stuff)

This is the most confusing part of C++. We use `std::memory_order_...` everywhere. Here is the translation:

### `std::memory_order_relaxed`

#### "The Post-it Note"

I'm writing a note to myself. It doesn't matter if you see it now or in 5 minutes.
*Used for:* Incrementing counters, statistics.

### `std::memory_order_release` & `std::memory_order_acquire`

#### "The Package Delivery"

- **Release (Sending)**: I pack a box with data, tape it shut, and hand it to the driver. nothing I put *inside* the box can fall out.
- **Acquire (Receiving)**: You get the box and open it. You are guaranteed to see everything I put in there.

We use this for the Queue and Hazard Pointers.

1. **Producer**: Writes the data, then uses `Release` to update the `Tail` pointer.
2. **Consumer**: Uses `Acquire` to read the `Head` pointer. This guarantees they see the data the producer wrote.
3. **Hazards**: We use `acc_rel` (Acquire-Release) to ensure published hazard pointers are visible to everyone instantly.

---

## 3. Hazard Pointers & Reclamation (The Safety Net)

We implemented a custom, lightweight Hazard Pointer system.

### The `hp_owner` (RAII)

Instead of manually managing slots, we use a C++ idiom called **RAII** (Resource Acquisition Is Initialization).
When a thread wants to touch the queue, it creates an `hp_owner` object.

- **Constructor**: Automatically borrows 2 slots from the global `hazard_domain`.
- **Destructor**: Automatically returns them when the thread leaves the function.

This guarantees we never "leak" slots, even if an exception is thrown!

### The Retirement Strategy

When we delete a node, we can't just `delete` it (someone might be looking at it).

1. **Thread-Local Stash**: Each thread keeps a small list of "trash" nodes.
2. **Scan**: When the stash gets full (64 items), we scan the global Hazard Pointers.
   - We take a snapshot of all active hazards.
   - We sort them (fast search!).
   - If a node is NOT in the hazard list, we delete it.
   - If it IS in the list, we keep it for next time.
3. **Global Retirement**: If a thread dies, it pushes its leftovers to a Global Retirement list so they aren't lost.

### Clean Shutdown

Threads are messy. When the program ends, we need to be careful about destruction order.

1. **Leaky Singletons**: We use "Immortal" global objects for the Hazard Domain. They are allocated with `new` and never `delete`d.
    - *Why?* To prevent "Use-After-Destruct" bugs where a thread tries to retire a node after the global manager has already been destroyed at program exit.
2. **Automatic Cleanup**: The Pool destructor joins all threads first. Once all threads are stopped, we know no one is using the queue.
    - Then we safely walk the queue and delete all remaining nodes. No leaks!

### Advanced: `drain_retired()` and Global Quiescence

If you use `ms_queue` *outside* of `jthread_pool`, you face a challenge: safely cleaning up the "Global Retirement" list at the end of the program.

We provide a static method `ms_queue<T>::drain_retired()`.

> **⚠️ WARNING:** This function is dangerous.
> It deletes **ALL** nodes in the global retirement list.
> You must only call this when **Strict Global Quiescence** is met:
>
> 1. All threads that ever accessed the queue have been joined/destroyed.
> 2. No thread is currently accessing the queue.
> 3. No thread-local `RetirementManager` destructors are pending (i.e., all worker threads have fully exited).

The `jthread_pool` destructor guarantees this sequence:

1. `request_stop()` signals threads to exit.
2. `workers_.clear()` joins all `jthread`s. This ensures all worker loops are done and their thread-local destructors (which flush to global retirement) have run.
3. Finally, it calls `drain_retired()`.

---

## 4. The Worker Loop (The Engine)

Each thread runs a loop that looks for work. It has two modes: **Fast Mode** and **Slow Mode**.

```mermaid
graph TD
    Start[Start Loop] --> CheckQ{Try Dequeue}
    CheckQ -- Success --> Execute[Run Task]
    Execute --> Start
    CheckQ -- Fail --> SlowPath[Slow Path: Check Event Count]
    SlowPath --> Wait[Wait on Condition Variable]
    Wait --> WakeUp[Woken Up!]
    WakeUp --> Start
```

### The Fast Path (Lock-Free)

If there is work, we just grab it. No locks, no waiting. It's blazing fast ⚡️.

```cpp
// Fast path: loop while we can get tasks, without locking.
while (q_.try_dequeue(task)) {
  task(); // Just do it!
}
```

### The Slow Path (Event Counts)

If the queue is empty, we don't want to spin. But checking `empty()` and then sleeping is race-prone (the "lost wakeup" problem).

We use a monotonic **Event Count** (`wake_seq_`):

1. **Submitter**: Enqueues task -> Increments `wake_seq_` -> Notifies.
2. **Worker**: Memorizes `wake_seq_` -> Tries Dequeue -> If empty, Sleeps *only if* `wake_seq_` hasn't changed.

This guarantees we never go to sleep after a task has arrived.

---

## 5. Type Erasure (The Universal Box)

The queue stores `std::function<void()>`. This is a "Type Erased" container.
It means it can hold *any* function:

- A function pointer `void foo()`
- A lambda `[]{ std::cout << "Hi"; }`
- A method on a class object.

However, `std::function` needs the task to be **Copyable**.
`std::packaged_task` (which gives us the return value `future`) is **NOT Copyable**.

**The Hack:**
We wrap the task in a `std::shared_ptr`.

- The `shared_ptr` itself is copyable (it's just a small pointer).
- So `std::function` is happy copying the pointer.
- Both copies point to the same task. Success!
