#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <string>
#include "../include/ms_jthread_pool.hpp"

// Stress test for ms_queue
int main() {
    std::cout << "Starting stress test..." << std::endl;
    ms::jthread_pool pool(8); // 8 worker threads
    
    std::atomic<int> counter{0};
    constexpr int NUM_TASKS = 1000000;
    
    std::cout << "Submitting " << NUM_TASKS << " tasks..." << std::endl;
    for (int i = 0; i < NUM_TASKS; ++i) {
        pool.submit([&counter]{
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    std::cout << "Waiting for tasks to complete..." << std::endl;
    
    while (counter.load(std::memory_order_relaxed) < NUM_TASKS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "Tasks done: " << counter.load() << std::endl;
    if (counter.load() == NUM_TASKS) {
        std::cout << "Stress test passed!" << std::endl;
        return 0;
    } else {
        std::cout << "Stress test failed! Expected " << NUM_TASKS << ", got " << counter.load() << std::endl;
        return 1;
    }
}
