#ifndef SPORK_FIB_H_
#define SPORK_FIB_H_

#include "scheduler.hpp"
#include "par.hpp"

namespace spork {

inline uint fibE(uint n) {
  if (n <= 1) { return n; }
  struct SpwnJob : WorkStealingJob {
    const uint n;
    uint r;
    void run() override { r = fibE(n - 2); }
    
    SpwnJob(const uint _n) : WorkStealingJob(), n(_n) {}
    SpwnJob(SpwnJob&& other) : WorkStealingJob(fwd(other)), n(fwd(other.n)) {}
  };
  
  volatile SpwnJob jp(n);
  uint l;

  bool promoted = with_prom_handler(
    [&l, n] () { l = fibE(n - 1); },
    [&] () {
      SpwnJob& jpnv = *((SpwnJob*) &jp);
      jpnv.enqueue(heartbeat_tokens >> 1);
    });

  if (promoted) [[unlikely]] { // promoted
    ((SpwnJob*) &jp)->sync(false);
    return l + jp.r;
  } else [[likely]] { // unpromoted
    return l + fibE(n - 2);
  }
}

inline uint fibSeq(uint n) {
  if (n <= 1) {
    return n;
  } else {
    return fibSeq(n-1) + fibSeq(n-2);
    // uint l, r;
    // parSeq([&, n] () {l = fibSeq(n - 1);},
    //        [&, n] () {r = fibSeq(n - 2);});
    // return l + r;
  }
}

inline uint fibParlay(uint n) {
  if (n <= 1) {
    return n;
  } else {
    uint l, r;
    parlay::par_do([&, n] () {l = fibParlay(n - 1);},
                   [&, n] () {r = fibParlay(n - 2);});
    return l + r;
  }
}

inline uint fib(uint n) {
  if (n <= 1) {
    return n;
  } else {
    uint l, r;
    par([&, n] () {l = fib(n - 1);},
        [&, n] () {r = fib(n - 2);});
    return l + r;
  }
}

// uint fibspork(uint n) {
//   if (n <= 1) {
//     return n;
//   } else {
//     return
//       spork<uint, uint, uint>
//         ([n] () {return fibspork(n - 1);},
//          [n] () {return fibspork(n - 2);},
//          [n] (uint l) {return l + fibspork(n - 2);},
//          [] (uint l, uint r) { return l + r; },
//          [n] () {});
//   }
// }
}

#endif // SPORK_FIB_H_
