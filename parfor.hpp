#ifndef SPORK_PARFOR_H_
#define SPORK_PARFOR_H_

#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/portability.h"
#include "scheduler.hpp"
#include <atomic>
#include <bits/types/sig_atomic_t.h>
#include <limits.h>

namespace spork {

template <typename idx, typename BodyLambda, typename BinOp>
parlay::monoid_value_type_t<BinOp> seqfor(idx i, idx j, const BodyLambda&& body, const BinOp&& binop) {
  static_assert(parlay::is_monoid_v<BinOp>);
  using A = parlay::monoid_value_type_t<BinOp>;
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx, A&>);

  A a = fwd(binop).identity;
  for (; i < j; i++) fwd(body)(i, a);
  return a;
}

template <typename idx, typename BodyLambda>
void seqfor(idx i, idx j, const BodyLambda&& body) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx>);
  for (; i < j; i++) fwd(body)(i);
}

template <typename idx, typename BodyLambda, typename BinOp>
parlay::monoid_value_type_t<BinOp> seqfor(idx n, const BodyLambda&& body, const BinOp&& binop) {
  return seqfor((idx) 0, n, fwd(body), fwd(binop));
}

template <typename idx, typename BodyLambda>
void seqfor(idx n, const BodyLambda&& body) {
  seqfor((idx) 0, n, fwd(body));
}


namespace { // private
  template <typename idx>
  __attribute__((always_inline))
  constexpr const idx midpoint(idx i, idx j) noexcept {
    static_assert(std::is_integral_v<idx>);
    return i + ((j - i) / 2);
  }
} // private

template <typename idx, typename BodyLambda, typename BinOp>
//__attribute__((always_inline)) // TODO: investigate what this attribute actually does
parlay::monoid_value_type_t<BinOp> parfor(idx i, idx j, const BodyLambda&& body, const BinOp&& binop) {
  static_assert(std::is_integral_v<idx>);
  static_assert(parlay::is_monoid_v<BinOp>);
  using A = parlay::monoid_value_type_t<BinOp>;
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx, A&>);

  struct SpwnJob : WorkStealingJob {
    volatile idx i, j;
    const BodyLambda&& body;
    const BinOp&& binop;
    A a;
    void run() override {
      a = parfor<idx, BodyLambda, BinOp>(i, j, fwd(body), fwd(binop));
    }
    SpwnJob(const BodyLambda&& _body, const BinOp&& _binop) :
      WorkStealingJob(),
      body(fwd(_body)),
      binop(fwd(_binop)) {}
  };

  SpwnJob l(fwd(body), fwd(binop));
  SpwnJob r(fwd(body), fwd(binop));

  // make sure that loop index can fit in `i`
  static_assert(sizeof(sig_atomic_t) >= sizeof(idx));
  // static_assert(std::numeric_limits<sig_atomic_t>::max() >= std::numeric_limits<idx>::max());

  // main code may write `sig_safe_i`; signal handler may only read
  volatile sig_atomic_t sig_safe_i = i;
  // main code may only read `loop_end`; signal handler may write
  // __attribute__((, 12345));
  volatile idx loop_end = j;
  A a = fwd(binop).identity;

  bool promoted = with_prom_handler(
    [&] () {
      for (; i < loop_end;) {
        fwd(body)(i, a);
        sig_safe_i = static_cast<sig_atomic_t>(++i);
      }
    },
    [&] () {
      idx prom_i = sig_safe_i + 1;
      if (prom_i >= loop_end) { r.i = 0; r.j = 0; l.i = 0; l.j = 0; return; }
      idx mid = midpoint<idx>(prom_i, loop_end);
      loop_end = prom_i;

      r.i = mid;
      r.j = j;
      r.enqueue((heartbeat_tokens + 1) >> 1);

      // TODO: check if work stealing deque is full before enqueueing

      if (prom_i >= mid) { l.i = 0; l.j = 0; return; }
      l.i = prom_i;
      l.j = mid;
      l.enqueue(heartbeat_tokens);
    });
  if (promoted) [[unlikely]] {
    if (l.i < l.j) [[likely]] {
      l.sync(true);
      a = fwd(binop)(a, l.a);
    }
    if (r.i < r.j) [[likely]] {
      r.sync(false);
      a = fwd(binop)(a, r.a);
    }
  }
  return a;
}

// TODO
// struct Unit {
//   Unit() {}
//   Unit(Unit& other) {}
//   Unit(Unit&& other) {}
//   Unit& operator=(Unit& other) { return *this; }
//   Unit& operator=(Unit&& other) { return *this; }
// };

//#include <__functional/binary_function.h>

template <typename idx, typename BodyLambda>
//__attribute__((always_inline)) // TODO: investigate what this attribute actually does
void parfor(idx i, idx j, const BodyLambda&& body) {
  // TODO
  // parfor<idx>(_i, _j, [&] (idx i, Unit& u) {fwd(body)(i);}, fwd(u));

  char _ = parfor(i, j, [&] (idx i, char _) {fwd(body)(i);}, parlay::plus<char>());
}

template <typename idx, typename BodyLambda>
void parfor(idx n, const BodyLambda&& body) {
  parfor((idx) 0, n, fwd(body));
}

template <typename idx, typename BodyLambda, typename BinOp>
parlay::monoid_value_type_t<BinOp> parfor(idx n, const BodyLambda&& body, const BinOp&& binop) {
  return parfor((idx) 0, n, fwd(body), fwd(binop));
}

}

#endif // SPORK_PARFOR_H_
