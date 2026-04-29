#include "scheduler.hpp"
#include "parlay/primitives.h"
#include "scan.hpp"
#include "fib.hpp"
#include "parlay/sequence.h"
#include "benchmark.hpp"
// #include "parlay/parallel.h"
// #include "parlay/primitives.h"

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
  using datnum = short;

  constexpr idxnum n = 800000000;
  datnum* data = make_data<idxnum, datnum>(n);
  parlay::sequence<datnum> dataseq = parlay::sequence<datnum>(data, &data[n]);
  std::cout << "dataseq.size() = " << dataseq.size() << std::endl;
  data = dataseq.data();
  
  
  auto total_time = 0;

  benchmark([&] () {
    datnum total = 0;
    total = spork::parlayfor(n, [&] (idxnum i, datnum& a) { a += data[i]; }, parlay::plus<datnum>());
    // total = spork::seqfor(n, [&] (idxnum i, datnum& a) { a += data[i]; }, parlay::plus<datnum>());
    // spork::parfor(n, [&] (idxnum i) {data[i]++;});
    // spork::scan(n, data, parlay::plus<datnum>());
    // parlay::scan_inclusive_inplace(dataseq, parlay::plus<datnum>());
    // spork::seqfor(n, [&] (idxnum i, datnum& a) { a += data[i]; data[i] = a; }, parlay::plus<datnum>());

    // total = spork::fib(38);
    return total;
  });
  free_data(data);
}
