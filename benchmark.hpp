#include "scheduler.hpp"
#include <type_traits>

namespace {
  inline void print_uint_arr(const uint* arr, uint len) {
    std::cout << "[";
    if (arr && len) {
      std::cout << arr[0];
      for (uint i = 1; i < len; i++) std::cout << ", " << arr[i];
    }
    std::cout << "]";
  }
  
  inline void print_uint_avg(const uint* arr, uint len) {
    if (arr && len) {
      uint total = 0;
      for (uint i = 0; i < len; i++) total += arr[i];
      std::cout << (total / len) << " avg";
    } else {
      std::cout << "NaN avg";
    }
  }
}

template <typename Bench>
inline void benchmark(const Bench&& bench, uint WARMUP = 10, uint NUM_TRIALS = 30) {
  auto total_time = 0;

  // this might take a sec the first time it is called
  spork::WorkStealingJob::get_current_scheduler();
  spork::init_heartbeat_stats();

  for (uint r = 0; r < WARMUP + NUM_TRIALS; r++) {
    spork::reset_heartbeat_stats();
    spork::start_heartbeats();
    auto start = std::chrono::steady_clock::now();
    std::invoke_result_t<Bench> result = fwd(bench)();
    auto end = std::chrono::steady_clock::now();
    spork::pause_heartbeats();
    auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << result << " in " << time_ms << " ms";
#ifdef RECORD_HEARTBEAT_STATS
    std::cout <<" (";
    print_uint_avg((uint*) spork::num_heartbeats, spork::WorkStealingJob::num_workers());
    std::cout << " heartbeats, ";
    print_uint_avg((uint*) spork::missed_heartbeats, spork::WorkStealingJob::num_workers());
    std::cout << " missed during eager proms)";
#endif
    std::cout << std::endl;
    if (r >= WARMUP) total_time += time_ms;
  }
  std::cout << "Average " << (total_time / NUM_TRIALS) << " ms" << std::endl;
}
