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
void scan(idx n, A* arr, const BinOp&& binop) {
  scan<idx, A, BinOp>(fwd(binop).identity, arr, n, fwd(binop));
}

template <typename idx>
__attribute__((always_inline))
constexpr static idx scan_chunksize(idx n) {
  // TODO: ideally, all splits would be along cache lines
  return (idx) sqrt((double) n);
}

template <typename idx, typename A, typename BinOp>
static A* scan_upsweep(A* arr, idx n, const BinOp&& binop) {
  static_assert(parlay::is_monoid_for_v<BinOp, A>);
  static_assert(std::is_integral_v<idx>);

  idx chunksize = scan_chunksize(n);
  idx chunks = 1 + ((n - 1) / chunksize);
  // TODO: consider aligning this allocation
  // so we can split chunks along cache lines
  A* partials = (A*) parlay::p_malloc((size_t) n*sizeof(A));
  if (partials) {
    parfor<idx>(0, chunks, [=,&binop] (idx c) {
      idx start = c*chunksize;
      idx end = (chunksize > n - start) ? n : start + chunksize;
      partials[c] = parfor<idx, A>(start, end, [&, arr] (idx i, A& sum) {
        sum = fwd(binop)(sum, arr[i]); }, fwd(binop));
    });
    
    scan<idx, A, BinOp>(chunks, partials, fwd(binop));
  }
  return partials;
}

template <typename idx, typename A, typename BinOp>
static void scan_downsweep(A* partials, A* arr, idx n, const BinOp&& binop) {
  static_assert(parlay::is_monoid_for_v<BinOp, A>);
  static_assert(std::is_integral_v<idx>);
  idx chunksize = scan_chunksize(n);
  idx chunks = 1 + ((n - 1) / chunksize);
  
  parfor<idx>(0, chunks, [=,&binop] (idx chunk) {
    A pfx = chunk ? fwd(binop)(arr[-1], partials[chunk - 1]) : arr[-1];
    idx start = chunk*chunksize;
    idx size = (chunksize > n - start) ? (n - start) : chunksize;
    scan<idx, A, BinOp>(pfx, &arr[start], size, fwd(binop));
  });
  
  parlay::p_free(partials);
}

template <typename idx>
__attribute__((always_inline))
static constexpr idx third1(idx i, idx j) noexcept {
  static_assert(std::is_integral_v<idx>);
  return i + ((j - i) / 5);
}
template <typename idx>
__attribute__((always_inline))
static constexpr idx third2(idx i, idx j) noexcept {
  static_assert(std::is_integral_v<idx>);
  return j - 1 * ((j - i) / 5);
}

template <typename idx, typename A, typename BinOp>
void scan(A a, A* arr, idx n, const BinOp&& binop) {
  static_assert(parlay::is_monoid_for_v<BinOp, A>);
  static_assert(std::is_integral_v<idx>);

  idx volatile j = n;
  idx i = 0;

  bool promoted = with_prom_handler(
    [&, arr] () { // body
      for (; i < j; i++) {
        a = fwd(binop)(a, arr[i]);
        arr[i] = a;
      }
    },
    [&, n] () { // prom handler
      idx _i = i + 1;
      idx t1 = third1<idx>(_i, n);
      idx t2 = third2<idx>(_i, n);
      // only break if every third has work to do
      if (_i < t1 && t1 < t2 && t2 < n) j = _i;
    });

  // TODO: could start spwn from prom handler above, then run body, then sync
  // (slightly less delayed spwn)
  if (promoted && i != n) {
    idx t1 = third1<idx>(i, n);
    idx t2 = third2<idx>(i, n);
    spork<char, A*, char>(
      [=,&binop] () { // body
        scan<idx, A, BinOp>(arr[i-1], &arr[i], t1 - i, fwd(binop));
        return '\0';},
      [=,&binop] () { // spwn
        return scan_upsweep<idx, A, BinOp>(&arr[t1], t2 - t1, fwd(binop));},
      [=,&binop] (char _) { // unpr
        scan<idx, A, BinOp>(arr[t1-1], &arr[t1], n - t1, fwd(binop));
        return '\0';},
      [=,&binop] (char _, A* partials) { // prom
        if (partials) {
          par([&binop, partials, arrt1 = &arr[t1], t21 = t2 - t1] () {
                scan_downsweep<idx, A, BinOp>(partials, arrt1, t21, fwd(binop));
              },
              [=,&binop] () {
                A sum12 = partials[(t2 - t1 - 1) / scan_chunksize(t2 - t1)];
                A sum02 = fwd(binop)(arr[t1-1], sum12);
                scan<idx, A, BinOp>(sum02, &arr[t2], n - t2, fwd(binop));
              });
        } else { // could not allocate the partials array
          scan<idx, A, BinOp>(arr[t1-1], &arr[t1], n - t1, fwd(binop));
        }
        return '\0';
      },
      [=,&binop] (char _) { // unstolen
        scan<idx, A, BinOp>(arr[t1-1], &arr[t1], n - t1, fwd(binop));
        return '\0';
      });
  }
}

}
#endif // SPORK_SCAN_H_
