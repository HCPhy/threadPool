# Educational Lock-Free Thread Pool (Beginner Friendly!)

Welcome! 👋

Concurrency is hard. Locks are slow. Thread pools are confusing.
This project is a **safe space** to learn how modern, high-performance C++ thread pools work under the hood.

We use **C++20**, **Lock-Free Queues**, and **Hazard Pointers**, but we explain it all in plain English.

> **Status**: Hardened & Verified.
> This implementation includes a robust Lock-Free MPMC Queue, Hazard Pointer memory reclamation, and correct "Event Count" notification logic to prevent lost wakeups and shutdowns races.

---

## 📚 Read the "For Idiots" Docs

We have written special documentation just for you. No PhD required.

### 1. [The Theory (start here)](docs/algorithms.md)

Learn about:

- **Lock-Free vs Locking** (The polite buffet vs the bathroom key)
- **The Queue** (How we push/pop without locks)
- **The ABA Problem** (Why bad things happen to good threads)
- **Hazard Pointers** (How we solve the ABA problem without locks)

### 2. [The Code](docs/implementation_details.md)

Learn about:

- **`std::jthread`** (The self-cleaning thread)
- **Event Counts** (How we wake threads without races)
- **The Worker Loop** (How threads find work efficiently)
- **Memory Reclamation** (How we clean up trash safely)

---

## 🚀 Quick Start

Want to see it run? Copy-paste this into your terminal:

```bash
# Compile (You need a modern C++ compiler like g++ 10 or clang 10)
make run_stress

# Run performance benchmark
make run_compute
```

### Code Example

Using the pool is super simple:

```cpp
#include "ms_jthread_pool.hpp"
#include <iostream>

int main() {
    // 1. Create a pool with 4 workers
    ms::jthread_pool pool(4);

    // 2. Give it a job (Fire and forget)
    pool.submit([]{ 
        std::cout << "Hello from a worker thread!\n"; 
    });

    // 3. Give it a job and wait for the result
    auto future = pool.submit([](int x) { 
        return x * x; 
    }, 10);

    std::cout << "Result: " << future.get() << "\n"; // Prints 100
}
```

## 📁 What's Inside?

- `include/ms_jthread_pool.hpp`: **The Whole Enchilada**. The entire library is in this one file.
- `test/stress_test.cpp`: Proof that it handles heavy load.
- `docs/`: The friendly documentation.

## ⚠️ Safety Notes

This is a lock-free implementation designed for education and high performance.

- **Shutdown**: The pool correctly serializes shutdown with submission. It joins workers and cleans up the queue automatically.
- **Teardown**: Internal "Leaky Singletons" ensure Global Hazard Pointers survive thread-local destruction, so no crashes at program exit.
- **Verification**: Run `make run_queue` to verify MPMC correctness under rigorous contention.

> **Note for Standalone Queue Usage**: If you use `ms::ms_queue` directly (without the thread pool), you **must** call `ms::ms_queue<T>::drain_retired()` at the very end of your program (after all threads are joined) to reclaim remaining memory. The `jthread_pool` handles this automatically in its destructor.
