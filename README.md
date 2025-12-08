# Educational Lock-Free Thread Pool

A modern C++20 thread pool implementation designed for educational purposes. It demonstrates advanced concurrency concepts including lock-free data structures, hazard pointers, and cooperative interruption.

## Key Features

- **Lock-Free Queue**: Implements the Michael-Scott MPMC queue algorithm.
- **Hazard Pointers**: Safe memory reclamation tackling the ABA problem.
- **C++20**: Uses `std::jthread`, `std::stop_token`, and concepts.
- **High Performance**: Optimized worker loop using atomic counters to avoid mutex contention on the hot path.

## Documentation

This project is documented to help you understand the internal workings:

- 📘 **[Algorithms Explained](docs/algorithms.md)**: Logic behind the Lock-Free Queue and Hazard Pointers.
- ⚙️ **[Implementation Details](docs/implementation_details.md)**: Deep dive into the C++ code, optimizations, and design choices.

## Directory Structure

- `include/ms_jthread_pool.hpp`: The single-header library containing the queue and thread pool.
- `src/main.cpp`: Example usage and verification script.
- `docs/`: Educational documentation.

## Quick Start

### Requirements

- A C++20 compliant compiler (GCC 10+, Clang 10+, MSVC 2019+).

### Build & Run

```bash
# Compile
g++ -std=c++20 -Iinclude src/main.cpp -o main

# Run
./main
```

### Example Usage

```cpp
#include "ms_jthread_pool.hpp"

int main() {
    ms::jthread_pool pool(4); // 4 workers

    // Fire and forget
    pool.submit([]{ std::cout << "Task executed!\n"; });

    // Return a future
    auto future = pool.submit([](int x) { return x * x; }, 10);
    std::cout << "Result: " << future.get() << "\n";
}
```
