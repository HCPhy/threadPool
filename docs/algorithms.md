# Algorithms & Theoretical Basis

This document details the non-blocking synchronization algorithms used in `ms_jthread_pool`. It assumes familiarity with basic concurrency concepts (threads, atomics) and focuses on the specific lock-free mechanisms employed.

## 1. Michael-Scott MPMC Queue

The core data structure is a **Lock-Free Multiple-Producer Multiple-Consumer (MPMC) Queue**, based on the classic algorithm by Michael and Scott (1996).

### Structure

The queue is a linked list of nodes.

* **Invariants:** The queue always contains at least one node (a sentinel/dummy node).
* **Head Pointer:** Points to the beginning of the list (or the dummy node).
* **Tail Pointer:** Points to the last node (or close to it).

### The "Helping" Mechanism

A defining feature of this algorithm is cooperative concurrency.

1. **Enqueue:** When a thread tries to add a node, it first checks if the `Tail` is pointing to the actual last node.
2. If the `Tail`'s `next` pointer is *not* null, it means another thread successfully linked a new node but fell asleep before updating the `Tail`.
3. **Helping:** The current thread will use a CAS (Compare-And-Swap) to push the `Tail` forward on behalf of the sleeping thread *before* attempting its own operation. This ensures the queue never blocks even if a thread stalls mid-operation.

### Consistency

The queue is **Linearizable**.

* **Enqueue point:** The successful CAS on the `next` pointer of the last node.
* **Dequeue point:** The successful CAS on the `head` pointer.

---

## 2. The ABA Problem

Lock-free algorithms relying on pointers face the **ABA Problem**:

1. Thread T1 reads pointer `A` from the stack.
2. Thread T2 pops `A`, frees it, allocates a new node at address `A` (recycling the memory), and pushes it back.
3. Thread T1 attempts a CAS assuming the state hasn't changed because the pointer address is still `A`.
4. **Result:** Corruption. The logical identity of the node changed, but the physical address did not.

To solve this, we cannot rely on standard `delete`. We must ensure memory remains valid as long as *any* thread holds a reference to it.

---

## 3. Hazard Pointers (Memory Reclamation)

We implement a **Hazard Pointer (HP)** scheme to guarantee safe memory reclamation (SMR). This provides wait-free readers and lock-free reclamation.

### The Protocol

1. **Publish:** Before a thread accesses a node (e.g., dereferencing `head` or `tail`), it "publishes" the pointer into a global `hazard_domain` array visible to all threads.
2. **Validate:** After publishing, the thread *re-reads* the global pointer. If it has changed, the hazard pointer is cleared, and the operation retries. This ensures we are protecting a node that is arguably still part of the data structure.
3. **Retire:** When a node is removed from the queue, it is not deleted. It is placed in a **Thread-Local Retirement List**.

### The Scan Algorithm (Reclamation)

When a thread's local retirement list reaches a threshold (e.g., 64 nodes), it performs a scan to free memory:

1. **Snapshot:** The thread reads all active Hazard Pointers from the global domain into a local vector.
2. **Sort:** The snapshot is sorted ($O(H \log H)$, where $H$ is the number of hazards).
3. **Search:** The thread iterates through its retirement list. For each candidate node, it performs a binary search ($O(\log H)$) against the snapshot.
    * **Found:** The node is currently in use. Keep it in the list.
    * **Not Found:** No thread is reading this node. It is safe to `delete`.

This approach guarantees that no memory is freed while a thread holds a reference to it, effectively solving the ABA problem without heavy locks or reference counting.
