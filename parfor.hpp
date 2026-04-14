#ifndef SPORK_PARFOR_H_
#define SPORK_PARFOR_H_

#include "parlay/monoid.h"
#include "scheduler.hpp"
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


template <typename idx>
__attribute__((always_inline))
constexpr static const idx midpoint(idx i, idx j) noexcept {
  static_assert(std::is_integral_v<idx>);
  return i + ((j - i) / 2);
}

template <typename idx, typename BodyLambda, typename BinOp>
//__attribute__((always_inline)) // TODO: investigate what this attribute actually does
parlay::monoid_value_type_t<BinOp> parfor(idx _i, idx _j, const BodyLambda&& body, const BinOp&& binop) {
  static_assert(std::is_integral_v<idx>);
  static_assert(parlay::is_monoid_v<BinOp>);
  using A = parlay::monoid_value_type_t<BinOp>;
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx, A&>);

  struct SpwnJob : WorkStealingJob {
    idx i, j;
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

  volatile SpwnJob l(fwd(body), fwd(binop));
  volatile SpwnJob r(fwd(body), fwd(binop));

  // make sure that loop index can fit in `i`
  static_assert(sizeof(sig_atomic_t) >= sizeof(idx));
  volatile sig_atomic_t i = _i;
  volatile idx j = _j;
  A a = binop.identity;

  bool promoted = with_prom_handler(
    [&] () {
      for (idx ii = _i; ii < j;) {
        fwd(body)(ii, a);
        i = ++ii;
      }
    },
    [&] () {
      idx _i = i + 1;
      if (_i >= _j) { r.i = 0; r.j = 0; l.i = 0; l.j = 0; return; }
      idx mid = midpoint<idx>(_i, _j);
      j = _i;

      r.i = mid;
      r.j = _j;
      r.enqueue((heartbeat_tokens + 1) >> 1);

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = 0; l.j = 0; return; }
      l.i = _i;
      l.j = mid;
      l.enqueue(heartbeat_tokens);
    });
  if (promoted) [[unlikely]] {
    if (l.i < l.j) [[likely]] {
      l.sync(true);
      a = binop(a, l.a);
    }
    if (r.i < r.j) [[likely]] {
      r.sync(false);
      a = binop(a, r.a);
    }
  }
  return a;
}


template <typename idx, typename BodyLambda>
//__attribute__((always_inline)) // TODO: investigate what this attribute actually does
void parfor(idx _i, idx _j, const BodyLambda&& body) {
  static_assert(std::is_integral_v<idx>);
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx>);

  struct SpwnJob : WorkStealingJob {
    idx i, j;
    const BodyLambda&& body;
    void run() override {
      parfor(i, j, fwd(body));
    }
    SpwnJob(const BodyLambda&& _body) :
      WorkStealingJob(),
      body(fwd(_body)) {}
  };

  SpwnJob l(fwd(body));
  SpwnJob r(fwd(body));

  // make sure that loop index can fit in `i`
  static_assert(sizeof(sig_atomic_t) >= sizeof(idx));
  volatile sig_atomic_t i = _i;
  
  volatile idx j = _j;

  bool promoted = with_prom_handler(
    [&] () {
      for (idx ii = _i; ii < j;) {
        fwd(body)(ii);
        i = ++ii;
      }
    },
    [&] () {
      idx _i = i + 1;
      if (_i >= _j) { r.i = 0; r.j = 0; l.i = 0; l.j = 0; return; }
      idx mid = midpoint<idx>(_i, _j);
      j = _i;

      r.i = mid;
      r.j = _j;
      r.enqueue((heartbeat_tokens + 1) >> 1);

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = 0; l.j = 0; return; }
      l.i = _i;
      l.j = mid;
      l.enqueue(heartbeat_tokens);
    });
  if (promoted) [[unlikely]] {
    if (l.i < l.j) [[likely]] l.sync(true);
    if (r.i < r.j) [[likely]] r.sync(false);
  }
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
