#ifndef SPORK_SCAN_H_
#define SPORK_SCAN_H_

#include "scheduler.hpp"
#include "spork.hpp"
#include "par.hpp"
#include "parfor.hpp"

namespace spork {

template <typename idx, typename A, typename BinOp>
void scan(A a, A* arr, idx n, const BinOp&& binop);

template <typename idx, typename A, typename BinOp>
__attribute__((always_inline))
inline void scan(idx n, A* arr, const BinOp&& binop) {
  scan(fwd(binop).identity, arr, n, fwd(binop));
}

namespace { //private
  template <typename idx>
  __attribute__((always_inline))
  inline constexpr idx scan_chunksize(idx n) {
    // TODO: ideally, all splits would be along cache lines
    return (idx) sqrt((double) n);
  }
  
  template <typename idx, typename A, typename BinOp>
  inline A* scan_upsweep(A* arr, idx n, const BinOp&& binop) {
    static_assert(parlay::is_monoid_for_v<BinOp, A>);
    static_assert(std::is_integral_v<idx>);
  
    idx chunksize = scan_chunksize(n);
    idx chunks = 1 + ((n - 1) / chunksize);
    // TODO: consider aligning this allocation
    // so we can split chunks along cache lines
    A* partials = (A*) parlay::p_malloc((size_t) n*sizeof(A));
    if (partials) {
      parfor(chunks, [=,&binop] (idx c) {
        idx start = c*chunksize;
        idx end = (chunksize > n - start) ? n : start + chunksize;
        partials[c] = parfor(start, end, [&, arr] (idx i, A& sum) {
          sum = fwd(binop)(sum, arr[i]); }, fwd(binop));
      });
      
      scan(chunks, partials, fwd(binop));
    }
    return partials;
  }
  
  template <typename idx, typename A, typename BinOp>
  inline void scan_downsweep(A* partials, A* arr, idx n, const BinOp&& binop) {
    static_assert(parlay::is_monoid_for_v<BinOp, A>);
    static_assert(std::is_integral_v<idx>);
    idx chunksize = scan_chunksize(n);
    idx chunks = 1 + ((n - 1) / chunksize);
    
    parfor(chunks, [=,&binop] (idx chunk) {
      A pfx = chunk ? fwd(binop)(arr[-1], partials[chunk - 1]) : arr[-1];
      idx start = chunk*chunksize;
      idx size = (chunksize > n - start) ? (n - start) : chunksize;
      scan(pfx, &arr[start], size, fwd(binop));
    });
    
    parlay::p_free(partials);
  }
  
  template <typename idx>
  __attribute__((always_inline))
  inline constexpr idx third1(idx i, idx j) noexcept {
    static_assert(std::is_integral_v<idx>);
    return i + ((j - i) / 5);
  }
  template <typename idx>
  __attribute__((always_inline))
  inline constexpr idx third2(idx i, idx j) noexcept {
    static_assert(std::is_integral_v<idx>);
    return j - 2 * ((j - i) / 5);
  }
} // private


template <typename idx, typename A, typename BinOp>
inline void scan(A a, A* arr, idx n, const BinOp&& binop) {
  static_assert(parlay::is_monoid_for_v<BinOp, A>);
  static_assert(std::is_integral_v<idx>);
  // make sure that loop index can fit in sig_atomic_t
  static_assert(sizeof(sig_atomic_t) >= sizeof(idx));

  idx i = 0;
  volatile sig_atomic_t sig_safe_i = 0;
  volatile idx loop_end = n;

  struct UpsweepJob : WorkStealingJob {
    const BinOp&& binop;
    volatile idx n;
    A* volatile arr; // before run(): stores arr; after run(): stores sqrt(n) partials
    void run() override { arr = scan_upsweep(arr, n, fwd(binop)); }
    UpsweepJob(const BinOp&& _binop) : binop(fwd(_binop)) {}
  };

  UpsweepJob up(fwd(binop));

  bool promoted = with_prom_handler(
    [&, arr] () { // body
      // for (; i + 5 <= loop_end; sig_safe_i = static_cast<sig_atomic_t>(i += 5)) {
      //   A* arri = &arr[i];
      //   #pragma clang loop unroll(full)
      //   for (idx ii = 0; ii < 5; ii++) {
      //     a = fwd(binop)(a, arri[ii]);
      //     arri[ii] = a;
      //   }
      // }
      for (; i < loop_end; sig_safe_i = static_cast<sig_atomic_t>(++i)) {
        a = fwd(binop)(a, arr[i]);
        arr[i] = a;
      }
    },
    [&, n] () { // prom handler
      idx prom_i = sig_safe_i + 5;//1;
      idx t1 = third1<idx>(prom_i, n);
      idx t2 = third2<idx>(prom_i, n);
      // only break if every third has work to do
      if (prom_i < t1 && t1 < t2 && t2 < n) {
        loop_end = prom_i;
        up.arr = &arr[t1];
        up.n = t2 - t1;
        up.enqueue((heartbeat_tokens + 1) >> 1);
      }
    });

  if (promoted && i != n) {
    idx t1 = third1<idx>(i, n);
    idx t2 = third2<idx>(i, n);
    
    scan(arr[i-1], &arr[i], t1 - i, fwd(binop));
    
    // if up was stolen, sync, then make sure the results array is non-null
    if (up.sync_is_stolen() && up.arr) {
      A* partials = up.arr;
      par([&binop, partials, arrt1 = &arr[t1], t21 = t2 - t1] () {
        scan_downsweep(partials, arrt1, t21, fwd(binop));
      },
        [=,&binop] () {
          A sum12 = partials[(t2 - t1 - 1) / scan_chunksize(t2 - t1)];
          A sum02 = fwd(binop)(arr[t1-1], sum12);
          scan(sum02, &arr[t2], n - t2, fwd(binop));
        });
    } else {
      // if unstolen, just keep going
      scan(arr[t1-1], &arr[t1], n - t1, fwd(binop));
    }
  }
}

}
#endif // SPORK_SCAN_H_
