#include <iostream>
#include <vector>
#include <thread>
#include <future>
#include <numeric>
#include <cstdint>
#include <mutex>
#include "ms_jthread_pool.hpp"

// closed-form sum on [a, b]
static inline unsigned long long sum_range(unsigned long long a, unsigned long long b) {
  const unsigned long long n = (b - a + 1);
  return n * (a + b) / 2;
}

int main() {
  const unsigned int hw = std::max(2u, std::thread::hardware_concurrency());
  ms::jthread_pool pool(hw);
  std::cout << "pool size: " << pool.size() << "\n";

  // sum 1..N by splitting into many chunks
  const unsigned long long N = 100'000'000ULL;         // big range
  const unsigned long long chunk = 1'000'000ULL;       // per-task size
  const unsigned long long num_chunks = (N + chunk - 1) / chunk;

  // we'll have multiple *producer threads* submitting to the pool concurrently
  const unsigned int num_producers = std::min<unsigned long long>(hw, num_chunks);

  // each producer fills its own futures, then we gather them
  std::vector<std::vector<std::future<unsigned long long>>> futures_by_prod(num_producers);

  std::vector<std::thread> producers;
  producers.reserve(num_producers);

  for (unsigned int p = 0; p < num_producers; ++p) {
    producers.emplace_back([&, p]{
      auto& futs = futures_by_prod[p];
      // assign chunk indices round-robin to producers
      for (unsigned long long i = p; i < num_chunks; i += num_producers) {
        const unsigned long long start = i * chunk + 1;
        const unsigned long long end   = std::min(N, (i + 1) * chunk);
        // submit piecewise job; returns future<unsigned long long>
        futs.emplace_back(pool.submit([start, end]{
          // do the math; could be a loop if you want real CPU work
          return sum_range(start, end);
        }));
      }
    });
  }

  for (auto& t : producers) t.join(); // all submissions finished

  // collect results
  unsigned long long total = 0;
  for (auto& vec : futures_by_prod) {
    for (auto& f : vec) total += f.get();
  }

  // verify against closed form
  const unsigned long long expected = N * (N + 1) / 2;
  std::cout << "total = " << total << "\n"
            << "expected = " << expected << "\n"
            << (total == expected ? "[OK]\n" : "[MISMATCH]\n");

  pool.request_stop(); // optional; destructor will also stop & join
  return 0;
}