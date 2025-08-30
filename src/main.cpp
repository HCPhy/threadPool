#include <iostream>
#include <vector>
#include <numeric>
#include <chrono>
#include <thread>
#include "ms_jthread_pool.hpp"

int main() {
  using namespace std::chrono_literals;

  ms::jthread_pool pool(std::max(2u, std::thread::hardware_concurrency()));
  std::cout << "pool size: " << pool.size() << "\n";

  auto f1 = pool.submit([v = std::vector<int>{1,2,3,4,5}]{
    return std::accumulate(v.begin(), v.end(), 0);
  });

  auto f2 = pool.submit([](int n){
    std::this_thread::sleep_for(50ms);
    long long s = 0; for (int i = 1; i <= n; ++i) s += i; return s;
  }, 100000);

  pool.submit([]{ std::cout << "hello from pool task\n"; }).wait();

  std::cout << "sum(v) = " << f1.get() << "\n";
  std::cout << "sum(1..100000) = " << f2.get() << "\n";

  pool.request_stop();
  return 0;
}