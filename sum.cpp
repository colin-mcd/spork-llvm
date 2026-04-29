#include "scheduler.hpp"
#include "parlay/primitives.h"
#include "parfor.hpp"
#include "benchmark.hpp"

int main(int argc, char* argv[]) {
  int n;
  if (argc == 3 && strcmp(argv[1], "-n") == 0) {
    n = atoi(argv[2]);
  } else {
    n = 800000000;
  }

  using idxnum = unsigned;
  using datnum = short;
  parlay::sequence<datnum> data = parlay::tabulate(n, [&] (long k) -> datnum { return 1; });
  benchmark([n, &data] () {
    return spork::parfor(n, [&] (idxnum i, datnum& total) {
      total += data[i];
    }, parlay::plus<datnum>());
  });
}
