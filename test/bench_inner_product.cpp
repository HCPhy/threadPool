#include <iostream>
#include <vector>
#include <numeric>
#include <chrono>
#include <random>
#include <omp.h>
#include <cmath>
#include <iomanip>

#include "ms_jthread_pool.hpp"

// Benchmark parameters
constexpr size_t N = 800'000'000;
constexpr int NUM_TRIALS = 5;

// Helper for timing
template <typename Func>
double measure_time_ms(Func&& f) {
    auto start = std::chrono::high_resolution_clock::now();
    f();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

int main() {
    std::cout << "Initializing data (" << N << " elements)..." << std::endl;
    // Align memory for better SIMD performance (optional but good practice)
    // For standard vectors, we just use default allocator for fairness unless specific custom allocator is used.
    std::vector<double> v1(N, 1.0);
    std::vector<double> v2(N, 1.0);

    // Randomize slightly to avoid optimization tricks on constants? 
    // Actually constants are fine for purely checking threading overhead/throughput.
    // Let's use 1.0 so we know the correct answer is N.
    
    // ----------------------------------------------------------------
    // Baseline: std::inner_product (Single Thread)
    // ----------------------------------------------------------------
    double baseline_result = 0;
    std::cout << "\nRunning Baseline (std::inner_product)..." << std::endl;
    double baseline_time = 0;
    for(int i=0; i<NUM_TRIALS; ++i) {
        baseline_time += measure_time_ms([&]() {
            baseline_result = std::inner_product(v1.begin(), v1.end(), v2.begin(), 0.0);
        });
    }
    baseline_time /= NUM_TRIALS;
    std::cout << "Baseline Avg Time: " << baseline_time << " ms" << std::endl;
    std::cout << "Result: " << baseline_result << std::endl;

    // ----------------------------------------------------------------
    // OpenMP
    // ----------------------------------------------------------------
    double omp_result = 0;
    std::cout << "\nRunning OpenMP (" << omp_get_max_threads() << " threads)..." << std::endl;
    double omp_time = 0;
    for(int i=0; i<NUM_TRIALS; ++i) {
        omp_time += measure_time_ms([&]() {
            double sum = 0;
            #pragma omp parallel for reduction(+:sum)
            for (size_t k = 0; k < N; ++k) {
                sum += v1[k] * v2[k];
            }
            omp_result = sum;
        });
    }
    omp_time /= NUM_TRIALS;
    std::cout << "OpenMP Avg Time: " << omp_time << " ms" << std::endl;
    std::cout << "Result: " << omp_result << (std::abs(omp_result - baseline_result) < 1e-5 ? " [OK]" : " [FAIL]") << std::endl;
    std::cout << "Speedup vs Baseline: " << baseline_time / omp_time << "x" << std::endl;

    // ----------------------------------------------------------------
    // ms::jthread_pool
    // ----------------------------------------------------------------
    ms::jthread_pool pool; 
    size_t pool_size = pool.size();
    
    // We want to divide work into chunks. 
    // Strategy: One chunk per thread, or more? 
    // Usually a bit more structure helps load balancing, but for uniform vector ops, 
    // static partition (like OMP static schedule) is best.
    size_t num_chunks = pool_size; 
    size_t chunk_size = (N + num_chunks - 1) / num_chunks;

    double pool_result = 0;
    std::cout << "\nRunning ms::jthread_pool (" << pool_size << " threads)..." << std::endl;
    double pool_time = 0;

    for(int i=0; i<NUM_TRIALS; ++i) {
        pool_time += measure_time_ms([&]() {
            std::vector<std::future<double>> futures;
            futures.reserve(num_chunks);
            
            for(size_t c = 0; c < num_chunks; ++c) {
                size_t start = c * chunk_size;
                size_t end = std::min(start + chunk_size, N);
                
                if (start >= end) break; 

                futures.push_back(pool.submit([start, end, &v1, &v2]() {
                    double local_sum = 0;
                    // Manual loop for vectorization friendliness
                    // or use std::inner_product on subranges
                    for(size_t k=start; k<end; ++k) {
                        local_sum += v1[k] * v2[k];
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
    pool_time /= NUM_TRIALS;
    
    std::cout << "Pool Avg Time: " << pool_time << " ms" << std::endl;
    std::cout << "Result: " << pool_result << (std::abs(pool_result - baseline_result) < 1e-5 ? " [OK]" : " [FAIL]") << std::endl;
    std::cout << "Speedup vs Baseline: " << baseline_time / pool_time << "x" << std::endl;
    std::cout << "Relative to OpenMP: " << (pool_time / omp_time) * 100.0 << "% time (Lower is better)" << std::endl;

    return 0;
}
