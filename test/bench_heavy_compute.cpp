#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <omp.h>
#include <iomanip>

#include "ms_jthread_pool.hpp"

// Heavy compute function: depends on offset to prevent memoization/optimization
double heavy_work(int offset, int iterations) {
    double res = 0;
    for (int i = 0; i < iterations; ++i) {
        res += std::sin(i + offset) * std::cos(i + offset);
    }
    return res;
}

// Fixed work per item
constexpr int WORK_PER_ITEM = 10000;
constexpr int NUM_TRIALS = 3;

// Helper for timing
template <typename Func>
double measure_time_ms(Func&& f) {
    auto start = std::chrono::high_resolution_clock::now();
    f();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

struct BenchmarkResult {
    int num_items;
    double serial_time;
    double omp_time;
    double pool_time;
    bool omp_correct;
    bool pool_correct;
};

BenchmarkResult run_benchmark(int num_items, ms::jthread_pool& pool) {
    BenchmarkResult result;
    result.num_items = num_items;
    
    // Serial baseline
    volatile double baseline_result = 0;
    double baseline_time = 0;
    for(int trial = 0; trial < NUM_TRIALS; ++trial) {
        baseline_time += measure_time_ms([&]() {
            double sum = 0;
            for(int k = 0; k < num_items; ++k) {
                sum += heavy_work(k, WORK_PER_ITEM);
            }
            baseline_result = sum;
        });
    }
    result.serial_time = baseline_time / NUM_TRIALS;

    // OpenMP
    double omp_result = 0;
    double omp_time = 0;
    for(int trial = 0; trial < NUM_TRIALS; ++trial) {
        omp_time += measure_time_ms([&]() {
            double sum = 0;
            #pragma omp parallel for reduction(+:sum)
            for (int k = 0; k < num_items; ++k) {
                sum += heavy_work(k, WORK_PER_ITEM);
            }
            omp_result = sum;
        });
    }
    result.omp_time = omp_time / NUM_TRIALS;
    result.omp_correct = std::abs(omp_result - baseline_result) < 1e-4;

    // Thread pool
    size_t pool_size = pool.size();
    size_t num_chunks = pool_size * 4;
    size_t chunk_size = (num_items + num_chunks - 1) / num_chunks;
    
    double pool_result = 0;
    double pool_time = 0;
    for(int trial = 0; trial < NUM_TRIALS; ++trial) {
        pool_time += measure_time_ms([&]() {
            std::vector<std::future<double>> futures;
            futures.reserve(num_chunks);
            
            for(size_t c = 0; c < num_chunks; ++c) {
                int start = c * chunk_size;
                int end = std::min(start + (int)chunk_size, num_items);
                
                if (start >= end) break;

                futures.push_back(pool.submit([start, end]() {
                    double local_sum = 0;
                    for(int k = start; k < end; ++k) {
                        local_sum += heavy_work(k, WORK_PER_ITEM);
                    }
                    return local_sum;
                }));
            }

            double sum = 0;
            for(auto& f : futures) {
                sum += f.get();
            }
            pool_result = sum;
        });
    }
    result.pool_time = pool_time / NUM_TRIALS;
    result.pool_correct = std::abs(pool_result - baseline_result) < 1e-4;

    return result;
}

int main() {
    ms::jthread_pool pool;
    
    std::cout << "Heavy Compute Benchmark - Performance & Correctness" << std::endl;
    std::cout << "Work per item: " << WORK_PER_ITEM << " iterations" << std::endl;
    std::cout << "Threads: " << pool.size() << " (OpenMP: " << omp_get_max_threads() << ")" << std::endl;
    std::cout << std::endl;

    // Test different sizes
    std::vector<int> sizes = {1000, 5000, 10000, 25000, 50000, 100000, 200000};
    std::vector<BenchmarkResult> results;

    for (int size : sizes) {
        std::cout << "Testing with " << size << " items... " << std::flush;
        results.push_back(run_benchmark(size, pool));
        std::cout << "done" << std::endl;
    }

    // Display results table
    std::cout << "\n";
    std::cout << "┌──────────┬─────────────┬─────────────┬─────────────┬───────────┬─────────────┬─────────────┬───────────┐" << std::endl;
    std::cout << "│   Items  │  Serial(ms) │  OpenMP(ms) │ OMP Speedup │ OMP Check │   Pool(ms)  │ Pool Speedup│ Pool Check│" << std::endl;
    std::cout << "├──────────┼─────────────┼─────────────┼─────────────┼───────────┼─────────────┼─────────────┼───────────┤" << std::endl;
    
    for (const auto& r : results) {
        double omp_speedup = r.serial_time / r.omp_time;
        double pool_speedup = r.serial_time / r.pool_time;
        
        std::cout << "│ " << std::setw(8) << r.num_items << " │ "
                  << std::setw(11) << std::fixed << std::setprecision(2) << r.serial_time << " │ "
                  << std::setw(11) << std::fixed << std::setprecision(2) << r.omp_time << " │ "
                  << std::setw(11) << std::fixed << std::setprecision(2) << omp_speedup << "x │ "
                  << std::setw(9) << (r.omp_correct ? "PASS" : "FAIL") << " │ "
                  << std::setw(11) << std::fixed << std::setprecision(2) << r.pool_time << " │ "
                  << std::setw(11) << std::fixed << std::setprecision(2) << pool_speedup << "x │ "
                  << std::setw(9) << (r.pool_correct ? "PASS" : "FAIL") << " │"
                  << std::endl;
    }
    
    std::cout << "└──────────┴─────────────┴─────────────┴─────────────┴───────────┴─────────────┴─────────────┴───────────┘" << std::endl;

    // Performance comparison
    std::cout << "\nPool vs OpenMP (lower is better):" << std::endl;
    for (const auto& r : results) {
        double ratio = (r.pool_time / r.omp_time) * 100.0;
        std::cout << "  " << std::setw(8) << r.num_items << " items: "
                  << std::setw(6) << std::fixed << std::setprecision(1) << ratio << "%" << std::endl;
    }

    return 0;
}
