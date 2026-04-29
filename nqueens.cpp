#include "scheduler.hpp"
#include "parlay/primitives.h"
#include "fib.hpp"
#include "benchmark.hpp"
#include "parfor.hpp"

struct Queens {
  uint x;
  uint y;
  Queens *next;
};

bool safe(uint i, uint j, Queens *q) {
  while (q) {
    if (i == q->x || j == q->y ||
        i - j == q->x - q->y || i + j == q->x + q->y) {
      return false;
    } else {
      q = q->next;
    }
  }
  return true;
}

uint countSol(uint i, uint n, Queens *q) {
  if (i >= n) {
    return 1;
  } else {
    return spork::parfor(n, [i, n, q] (uint j, uint& sols) {
      if (safe(i, j, q)) {
        Queens nq = {i, j, q};
        sols += countSol(i + 1, n, &nq);
      }
    }, parlay::plus<uint>());
  }
}

int main(int argc, char *argv[]) {
  int n;
  if (argc == 3 && strcmp(argv[1], "-n") == 0) {
    n = atoi(argv[2]);
  } else {
    n = 8;
  }
  benchmark([n] () {return countSol(0, (uint) n, nullptr);});
}
