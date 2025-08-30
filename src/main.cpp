#include <ms_jthread_pool.hpp>

#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

int main() {
  msjp::jthread_pool pool(4);  // 4 worker threads

  // Example 1: simple parallel jobs returning values
  auto f1 = pool.submit([] { return 40 + 2; });
  auto f2 = pool.submit([](int x){ return x * x; }, 13);
  std::cout << "f1=" << f1.get() << ", f2=" << f2.get() << "\n";

  // Example 2: parallel accumulate
  std::vector<int> data(1'000'000);
  std::mt19937 rng(123);
  std::uniform_int_distribution<int> dist(0, 9);
  for (auto& v : data) v = dist(rng);

  const std::size_t parts = 8;
  std::vector<std::future<long long>> partials;
  partials.reserve(parts);

  auto chunk = data.size() / parts;
  for (std::size_t i = 0; i < parts; ++i) {
    auto begin = data.begin() + i * chunk;
    auto end   = (i + 1 == parts) ? data.end() : begin + chunk;
    partials.emplace_back(pool.submit([begin, end] {
      return std::accumulate(begin, end, 0LL);
    }));
  }
  long long total = 0;
  for (auto& f : partials) total += f.get();
  std::cout << "sum=" << total << "\n";

  // Example 3: fire-and-forget tasks
  for (int i = 0; i < 5; ++i) {
    pool.submit([i]{
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(50ms);
      std::printf("task %d done\n", i);
    });
  }

  // The pool destructor will request stop, drain tasks, and join.
  return 0;
}