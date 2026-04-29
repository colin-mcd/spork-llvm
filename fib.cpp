#include "scheduler.hpp"
#include "parlay/primitives.h"
#include "fib.hpp"
#include "benchmark.hpp"

int main(int argc, char* argv[]) {
  benchmark([] () {return spork::fib(40);});
}
