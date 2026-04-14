#include "scheduler.hpp"
#include "scan.hpp"
#include "fib.hpp"


void print_uint_arr(const uint* arr, uint len) {
  std::cout << "[";
  if (arr && len) {
    std::cout << arr[0];
    for (uint i = 1; i < len; i++) std::cout << ", " << arr[i];
  }
  std::cout << "]";
}

void print_uint_avg(const uint* arr, uint len) {
  if (arr && len) {
    uint total = 0;
    for (uint i = 0; i < len; i++) total += arr[i];
    std::cout << (total / len) << " avg";
  } else {
    std::cout << "NaN avg";
  }
}

void waste_some_time() {
  static uint x = 110101241;
  for (uint i = 0; i < 10000000; i++) {
    x ^= x + 21521809;
    if (x % 12345 == 67890) [[likely]] {
      // should never happen
      std::cout << "waste_some_time i = " << i << std::endl;
    }
  }
}

// int setup_perf_interrupt(long long period_cycles) {
//   struct perf_event_attr pe{};
//   pe.type           = PERF_TYPE_HARDWARE;
//   pe.config         = PERF_COUNT_HW_CPU_CYCLES;
//   pe.sample_period  = period_cycles;
//   pe.watermark      = 0;
  
//   // pid=0 targets the calling thread, cpu=-1 = any CPU
//   int fd = syscall(SYS_perf_event_open, &pe, 0, -1, -1, 0);
  
//   // Route overflow to SIGIO on this thread
//   fcntl(fd, F_SETFL, O_ASYNC);
//   fcntl(fd, F_SETOWN, gettid());
  
//   ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
//   return fd;
// }

// template <typename F>
// __attribute__((always_inline))
// void p4(size_t s, size_t e, F&& f) {
//   //parlay::parallel_for(s, e, fwd(f), 1);
//   spork::parfor<size_t>(s, e, fwd(f));
// }

template <typename idx, typename datnum>
datnum* make_data(idx n) {
  datnum* arr = (datnum*) parlay::p_malloc(static_cast<size_t>(n) * sizeof(datnum));
  if (arr) {
    for (idx i = 0; i < n; i++) {
      arr[i] = (i % (idx) 2);
    }
  } else {
    std::cerr << "Failed to allocate data array" << std::endl;
    exit(1);
  }
  return arr;
}

template <typename datnum>
void free_data(datnum* arr) {
  parlay::p_free(arr);
}

int main(int argc, char* argv[]) {
  //size_t n = atoi(argv[1]);
  using idxnum = unsigned;
  using datnum = int;

  // num total = 0;
  // volatile uint j = n*50;
  // for (uint i = 0; i < j; i++) {
  //   total += data[i % n];
  // }
  // std::cout << total << std::endl;

  constexpr idxnum n = 800000000;
  datnum* data = make_data<idxnum, datnum>(n);
  
  auto total_time = 0;
  constexpr uint WARMUP = 10;
  constexpr uint NUM_TRIALS = 30;

  // this might take a sec the first time it is called
  spork::WorkStealingJob::get_current_scheduler();
  spork::init_heartbeat_stats();

  for (uint r = 0; r < WARMUP + NUM_TRIALS; r++) {
    spork::reset_heartbeat_stats();
    spork::start_heartbeats();
    
    auto start = std::chrono::steady_clock::now();
    //parlay::parallel_for(0, n, [&] (uint i) { irregular_body(data, i, n); });
    // p4(0, n, [&] (uint i) {
    //   volatile char x = 0;
    //   if (i < 10) {
    //     p4(0, 10000000, [&] (uint j) { x = x + i*j; });
    //   }
    //   data[i] = x;
    // });
    // parlay::parallel_for(0, n*50, [&] (uint i) { data[i % n] = 5; });
    // spork::parfor([&] (uint i) { data[i % n] = 5; }, 0, n*50);
    // num total =
      // spork::seqfor(0, n*50, [&] (uint i, num& a) {a += data[i % n];}, parlay::plus<num>());
      // spork::parfor_unroll2<uint>(0, n*50, [&] (uint i, num& a) {a += data[i % n];}, parlay::plus<num>());
    datnum total = 0;
    // total = spork::parfor(n, [&] (idxnum i, datnum& a) { a += data[i]; }, parlay::plus<datnum>());
    // spork::parfor(n, [&] (idxnum i) {data[i]++;});
    // total = spork::parfor<idxnum>(0, n, [&] (idxnum i, datnum& a) {waste_some_time(); a += data[i];}, parlay::plus<datnum>());
    // total = spork::seqfor<idxnum, datnum>(0, n, [&] (idxnum i, datnum& a) {a += data[i];}, parlay::plus<datnum>());
    // total = spork::seqfor<idxnum, datnum>(0, n, [&] (idxnum i, datnum& a) {a += data[i]; data[i] = a;}, parlay::plus<datnum>());
    // spork::scan(n, data, parlay::plus<datnum>());
    total = spork::fib(38);
    auto end = std::chrono::steady_clock::now();

    spork::pause_heartbeats();
    
    auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << (datnum) data[n - 2] << " " << total << " in " << time_ms << " ms";
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

  spork::pause_heartbeats();
  free_data(data);
}
