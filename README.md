# Educational Lock-Free Thread Pool (Beginner Friendly!)

Welcome! 👋

Concurrency is hard. Locks are slow. Thread pools are confusing.
This project is a **safe space** to learn how modern, high-performance C++ thread pools work under the hood.

We use **C++20**, **Lock-Free Queues**, and **Hazard Pointers**, but we explain it all in plain English.

> **Status**: Hardened & Verified.
> This implementation now includes a robust Hazard Pointer memory reclamation system, exception safety, and clean shutdown logic.

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
- **Atomic Memory Ordering** (Sending packages across threads)
- **The Worker Loop** (How threads find work)
- **Memory Reclamation** (How we clean up trash safely)

---

## 🚀 Quick Start

Want to see it run? Copy-paste this into your terminal:

```bash
# Compile (You need a modern C++ compiler like g++ 10 or clang 10)
g++ -std=c++20 -pthread -Iinclude src/main.cpp -o main

# Run it!
./main
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
- `src/main.cpp`: A test script to prove it works.
- `docs/`: The friendly documentation.

## ⚠️ Safety Notes

This is a lock-free implementation designed for education and high performance.

- **Shutdown**: By default, the pool "safely leaks" some memory on process exit to avoid crashes (races during static destruction).
- **Manual Drain**: If you need 0 leaks (e.g. for valgrind tests), you can call `ms::ms_queue<T>::drain_retired()`, but **ONLY** when all threads are finished.
