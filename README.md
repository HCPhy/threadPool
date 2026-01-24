# Lock-Free C++20 Thread Pool

A high-performance, header-only thread pool implementation demonstrating modern C++ concurrency patterns. It features a lock-free Michael-Scott MPMC queue, Hazard Pointer memory reclamation, and C++20 thread management.

> **Status**: Experimental / Educational.
> Verified with ThreadSanitizer (TSan) and AddressSanitizer (ASan).

## Key Features

* **Lock-Free Queue**: Michael-Scott MPMC algorithm for non-blocking task submission.
* **Hazard Pointers**: Robust Safe Memory Reclamation (SMR) solving the ABA problem without locks.
* **C++20 Integration**: Utilizes `std::jthread` for automatic joining and `std::stop_token` for cooperative cancellation.
* **Event Count Synchronization**: Optimizes the sleep/wake cycle to prevent lost wakeups and minimize syscall overhead.
* **Exception Safe**: RAII wrappers ensure hazard slots are always returned, even on thread panic.

## Architecture Overview

| Component       | Implementation                | Notes                                            |
| --------------- | ----------------------------- | ------------------------------------------------ |
| **Queue**       | Michael-Scott                 | Linearizable, Non-blocking.                      |
| **Reclamation** | Hazard Pointers               | Scan threshold = 64. Binary search optimization. |
| **Threads**     | `std::jthread`                | Auto-joining.                                    |
| **Signaling**   | `std::condition_variable_any` | Uses `wake_seq` (Event Count) logic.             |

## Documentation

* **[Algorithms Explained](docs/algorithms.md)**: Deep dive into the correctness of the Michael-Scott queue and Hazard Pointer logic.
* **[Implementation Details](docs/implementation_details.md)**: C++ memory ordering choices (`acquire`/`release`), Type erasure, and Singleton lifecycles.
* **[Performance Analysis](docs/performance_analysis.md)**: Benchmarks against OpenMP and Serial baselines.

## Quick Start

### Requirements

* C++20 compliant compiler (GCC 10+, Clang 11+, MSVC 19.28+)
* Pthreads (on Linux/Mac)

### Usage

```cpp
#include "ms_jthread_pool.hpp"
#include <iostream>

int main() {
    // 1. Initialize pool with hardware concurrency
    ms::jthread_pool pool;

    // 2. Submit a fire-and-forget task
    pool.submit([]{ 
        std::cout << "Task executing on " << std::this_thread::get_id() << "\n"; 
    });

    // 3. Submit a task with a return value (Future)
    auto future = pool.submit([](int a, int b) { 
        return a + b; 
    }, 10, 20);

    std::cout << "Result: " << future.get() << "\n"; // Blocks until ready
    
    // Pool destructor automatically joins threads and reclaims memory.
}
Build & Test
Bash
# Build the stress test (High contention verification)
make run_stress

# Run compute benchmarks
make run_compute
