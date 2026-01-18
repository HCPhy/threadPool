#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <cassert>
#include <mutex>
#include <map>
#include <algorithm>
#include "../include/ms_jthread_pool.hpp"

// Precise Multi-Producer Multi-Consumer Correctness Test
// Verifies that every single enqueued item is dequeued exactly once.
void test_mpmc_correctness() {
    std::cout << "[MPMC Correctness] Starting..." << std::endl;
    
    constexpr int NUM_PRODUCERS = 4;
    constexpr int NUM_CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 50000;
    constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    
    ms::ms_queue<int> queue;
    
    // Track received items: received[producer_id][item_id]
    // using vector of bools (not atomic, so we need a lock for final verification or per-slot protection)
    // To minimize verification locking, we can just use a large vector of atomics or a mutex per "row".
    // Let's use a simple mutex-protected global verification structure, 
    // but to avoid bottlenecking the test on the verification lock, we'll store results in thread-local buckets 
    // and merge at end? No, that doesn't prove "exactly once" globally during the run.
    // 
    // Better: Global std::vector<std::atomic<bool>> received(TOTAL_ITEMS)
    // Map (producer, item) -> unique global index.
    
    std::vector<std::atomic<bool>> received(TOTAL_ITEMS);
    for(int i=0; i<TOTAL_ITEMS; ++i) received[i] = false;
    
    std::vector<std::jthread> producers;
    std::vector<std::jthread> consumers;
    
    std::atomic<int> production_done_count{0};
    
    // Producers
    for(int p=0; p<NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p](){
            for(int i=0; i<ITEMS_PER_PRODUCER; ++i) {
                // Unique check_id = p * ITEMS_PER_PRODUCER + i
                int val = p * ITEMS_PER_PRODUCER + i;
                queue.enqueue(val);
            }
            production_done_count.fetch_add(1, std::memory_order_release);
        });
    }
    
    std::atomic<int> consumed_count{0};
    std::atomic<int> duplicate_errors{0};
    std::atomic<int> range_errors{0};
    
    // Consumers
    for(int c=0; c<NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&](){
            int val;
            while(true) {
                // Try dequeue
                if(queue.try_dequeue(val)) {
                    consumed_count.fetch_add(1, std::memory_order_relaxed);
                    
                    if(val < 0 || val >= TOTAL_ITEMS) {
                        range_errors.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        bool prev = received[val].exchange(true, std::memory_order_relaxed);
                        if(prev) {
                            duplicate_errors.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                } else {
                    // Stop condition: all producers done AND queue likely empty
                    // We must be careful: try_dequeue failing doesn't mean empty forever if producers are running.
                    if(production_done_count.load(std::memory_order_acquire) == NUM_PRODUCERS) {
                        // Double check emptiness
                        if(queue.empty()) {
                             // Drain one last time to be sure
                             if(!queue.try_dequeue(val)) {
                                 break;
                             } else {
                                 // Received one last item
                                 consumed_count.fetch_add(1, std::memory_order_relaxed);
                                 if(val >= 0 && val < TOTAL_ITEMS) {
                                     bool prev = received[val].exchange(true, std::memory_order_relaxed);
                                     if(prev) duplicate_errors.fetch_add(1, std::memory_order_relaxed);
                                 }
                             }
                        }
                    } else {
                        std::this_thread::yield();
                    }
                }
            }
        });
    }
    
    producers.clear(); // join producers
    consumers.clear(); // join consumers
    
    std::cout << "  Producers: " << NUM_PRODUCERS << ", Consumers: " << NUM_CONSUMERS << std::endl;
    std::cout << "  Total Items: " << TOTAL_ITEMS << std::endl;
    std::cout << "  Consumed:    " << consumed_count.load() << std::endl;
    std::cout << "  Duplicates:  " << duplicate_errors.load() << std::endl;
    std::cout << "  Range Errs:  " << range_errors.load() << std::endl;
    
    if (consumed_count.load() != TOTAL_ITEMS) {
        std::cout << "  [FAIL] Count mismatch!" << std::endl;
        // Check what's missing
        int missing = 0;
        for(int i=0; i<TOTAL_ITEMS; ++i) if(!received[i]) missing++;
        std::cout << "  Missing: " << missing << std::endl;
        std::exit(1);
    }
    
    if (duplicate_errors.load() > 0 || range_errors.load() > 0) {
        std::cout << "  [FAIL] Errors detected!" << std::endl;
        std::exit(1);
    }
    
    std::cout << "[MPMC Correctness] PASSED" << std::endl;
    
    // Explicitly drain retired nodes (since we used the queue standalone)
    ms::ms_queue<int>::drain_retired();
}

int main() {
    test_mpmc_correctness();
    return 0;
}
