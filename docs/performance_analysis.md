# Performance Analysis

## Overview

This document summarizes the performance characteristics of `ms::jthread_pool` compared to OpenMP and a single-threaded baseline. The analysis focuses on a compute-intensive workload to isolate threading overhead from memory bandwidth limitations.

## Benchmark Methodology

- **Workload:** Heavy trigonometric calculations (`sin` * `cos`) over a range of inputs.
- **Verification:** Results are verified against a serial baseline to ensure correctness.
- **Hardware:** Apple Silicon (M-series).
- **Settings:**
  - 10 threads
  - Work per item: 10,000 iterations
  - Trials: 3 (averaged)

## Results Summary

| Items | Serial (ms) | OpenMP (ms) | Speedup | Pool (ms) | Speedup | Note |
|-------|-------------|-------------|---------|-----------|---------|------|
| 1,000 | ~58.00      | ~10.00      | ~5.5-6x | ~8.00     | ~7-7.5x | **Pool is ~20% faster** |
| 5,000 | ~278.00     | ~46.50      | ~6.0x   | ~37.50    | ~7.5x   | **Pool is ~19% faster** |
| 10,000| ~557.00     | ~78.50      | ~7.1x   | ~78.00    | ~7.1x   | Parity reached |
| 25,000| ~1400.00    | ~197.00     | ~7.1x   | ~220.00   | ~6.3x   | OpenMP is ~10% faster |
| 50,000| ~2800.00    | ~400.00     | ~7.0x   | ~395.00   | ~7.1x   | Parity restored |

## Detailed Analysis

### 1. Small Workloads (1K - 5K Items)

**Winner: Thread Pool**

- **Reason:** Lower latency and startup overhead. OpenMP creates/initializes a parallel region which has a fixed cost. The thread pool has worker threads already spinning or waiting on a condition variable, allowing for faster dispatch of small tasks.

### 2. Large Workloads (25K+ Items)

**Winner: OpenMP (Slightly)**

- **Reason:**
  - **Optimization:** OpenMP is compiler-integrated and can perform better loop optimizations and vectorization.
  - **Overhead:** The thread pool approach uses `std::future`, which incurs heap allocation and synchronization overhead per chunk. At mid-sized workloads (25K), this overhead accumulates.
  - **Contention:** Rapid submission of many chunks can cause momentary contention on the pool's internal queue.

### Conclusion

The `ms::jthread_pool` implementation is highly efficient, particularly for:

- **Heterogeneous tasks:** Where work isn't a simple loop.
- **Latency-sensitive workloads:** Where the overhead of spinning up a full parallel region is too high.
- **Continuous submission:** Unlike OpenMP's "fork-join" model, the pool stays alive.

For massive, uniform "number crunching" arrays, OpenMP remains the industry standard, but the thread pool performs competitively (within 10-15%) while offering greater flexibility for general software architecture.
