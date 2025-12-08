# Algorithms Explained

This project implements a **Lock-Free Thread Pool** using advanced concurrency algorithms. This document explains the core theoretical components: the Michael-Scott Queue and Hazard Pointers.

## 1. Michael-Scott Queue (Lock-Free MPMC)

The cornerstone of this thread pool is the **Michael-Scott Queue**, a Multiple-Producer Multiple-Consumer (MPMC) queue that uses atomic operations instead of mutex locks to ensure thread safety.

### Concept

In a traditional queue, we lock the entire structure or the head/tail separately. In a lock-free queue, we use **Compare-And-Swap (CAS)** (C++ `compare_exchange_strong/weak`) to modify links atomically.

The queue maintains two pointers:

- `Head`: Points to the beginning of the list (dummy node or first actual node).
- `Tail`: Points to the last node (or one of the last nodes).

### Enqueue (Push)

1. **Create** a new node `N`.
2. **Read** current `Tail` (`t`) and its `next` pointer (`next`).
3. **Consistency Check**: Verify that `t` is still the current `Tail`.
4. **Append**:
    - If `next` is `nullptr`: Try to CAS `t->next` from `nullptr` to `N`.
        - If successful, the link is established. Now try to CAS `Tail` from `t` to `N` (swing the tail). **Done.**
        - If failed, some other thread added a node. Retry loop.
    - If `next` is NOT `nullptr`: The `Tail` is lagging behind (someone else linked a node but didn't update `Tail`). Help them by trying to CAS `Tail` from `t` to `next`. Retry loop.

### Dequeue (Pop)

1. **Read** current `Head` (`h`), `Tail` (`t`), and `Head->next` (`next`).
2. **Consistency Check**: Verify that `h` is still the current `Head`.
3. **Empty Check**: If `h == t` and `next == nullptr`, the queue is empty.
4. **Extract**:
    - If `h == t` but `next != nullptr`: `Tail` is lagging. Help advance `Tail`.
    - Otherwise, try to CAS `Head` from `h` to `next`.
        - If successful, return `next->value`. The node `h` is now "removed" and can be reclaimed.
        - If failed, retry.

---

## 2. The ABA Problem & Memory Reclamation

Lock-free algorithms suffer from the **ABA Problem**:

1. Thread 1 reads `A`.
2. Thread 2 pops `A`, frees it, allocates a new node at address `A`, and pushes it back.
3. Thread 1 performs CAS expecting `A`. The CAS succeeds (addresses match), but the node has logically changed (or its `next` pointer is different)!

To solve this, we cannot simply `delete` nodes immediately after dequeue. OTHER threads might still be looking at that node pointer.

### Hazard Pointers

This project uses a simplified **Hazard Pointer** scheme for safe memory reclamation.

**Mechanism:**

1. **Hazard Slot**: Each thread has a dedicated "slot" (global atomic variable) where it publishes the pointer it is currently accessing (protecting).
2. **Protect**: Before accessing a node (e.g., `Head`), a thread writes the node's address to its Hazard Slot.
3. **Retire**: When a thread dequeues a node `N`, it doesn't delete it immediately. It calls `Retire(N)`.
4. **Scan & Delete**:
    - The `Retire` function checks: "Is this pointer `N` currently in ANY thread's Hazard Slot?"
    - If **YES**: We cannot delete it yet. Stash it in a thread-local `retired_list`.
    - If **NO**: It is safe to `delete N`.
    - Periodically, we scan the `retired_list` to see if old nodes are now safe to delete.

This ensures that no memory is freed while another thread is holding a reference to it, effectively solving the ABA problem and use-after-free bugs in lock-free structures.
