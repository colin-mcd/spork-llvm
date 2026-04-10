#ifndef SPORK_PARFOR_H_
#define SPORK_PARFOR_H_

#include "scheduler.hpp"

namespace spork {

template <typename idx, typename A, typename BodyLambda, typename BinOp>
// __attribute__((always_inline))
A seqfor(idx i, idx j, const BodyLambda&& body, const BinOp&& binop) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx, A&>);
  static_assert(parlay::is_monoid_for_v<BinOp, A>);

  A a = fwd(binop).identity;
  for (; i < j; i++) fwd(body)(i, a);
  return a;
}

template <typename idx, typename BodyLambda>
// __attribute__((always_inline))
void seqfor(idx i, idx j, const BodyLambda&& body) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx>);
  for (; i < j; i++) fwd(body)(i);
}

template <typename idx>
__attribute__((always_inline))
constexpr static const idx midpoint(idx i, idx j) noexcept {
  static_assert(std::is_integral_v<idx>);
  return i + ((j - i) >> 1);
}

template <typename idx, typename A, typename BodyLambda, typename BinOp>
//__attribute__((always_inline)) // TODO: investigate what this attribute actually does
A parfor(idx i, idx _j, const BodyLambda&& body, const BinOp&& binop) {
  static_assert(std::is_integral_v<idx>);
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx, A&>);
  static_assert(parlay::is_monoid_for_v<BinOp, A>);

  struct SpwnJob : WorkStealingJob {
    idx i, j;
    const BodyLambda&& body;
    const BinOp&& binop;
    A a;
    void run() override {
      a = parfor<idx, A, BodyLambda, BinOp>
        (i, j, fwd(body), fwd(binop));
    }
    SpwnJob(const BodyLambda&& _body, const BinOp&& _binop) :
      WorkStealingJob(),
      body(fwd(_body)),
      binop(fwd(_binop)) {}
  };

  SpwnJob l(fwd(body), fwd(binop));
  SpwnJob r(fwd(body), fwd(binop));
  volatile idx j = _j;
  A a = binop.identity;

  bool promoted = with_prom_handler(
    [&] () {
      for (; i < j; i++) {
        fwd(body)(i, a);
      }
    },
    [&] () {
      idx _i = i + 1;
      if (_i >= _j) { r.i = r.j = 0; l.i = l.j = 0; return; }
      idx mid = midpoint<idx>(_i, _j);
      j = _i;

      r.i = mid;
      r.j = _j;
      r.enqueue((heartbeat_tokens + 1) >> 1);

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = l.j = 0; return; }
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
void parfor(idx i, idx _j, const BodyLambda&& body) {
  static_assert(std::is_integral_v<idx>);
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx>);

  struct SpwnJob : WorkStealingJob {
    idx i, j;
    const BodyLambda&& body;
    void run() override {
      parfor<idx, BodyLambda> (i, j, fwd(body));
    }
    SpwnJob(const BodyLambda&& _body) :
      WorkStealingJob(),
      body(fwd(_body)) {}
  };

  SpwnJob l(fwd(body));
  SpwnJob r(fwd(body));
  volatile idx j = _j;

  bool promoted = with_prom_handler(
    [&] () { for (; i < j; i++) { fwd(body)(i); } },
    [&] () {
      idx _i = i + 1;
      if (_i >= _j) { r.i = r.j = 0; l.i = l.j = 0; return; }
      idx mid = midpoint<idx>(_i, _j);
      j = _i;

      r.i = mid;
      r.j = _j;
      r.enqueue((heartbeat_tokens + 1) >> 1);

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = l.j = 0; return; }
      l.i = _i;
      l.j = mid;
      l.enqueue(heartbeat_tokens);
    });
  if (promoted) [[unlikely]] {
    if (l.i < l.j) [[likely]] {
      l.sync(true);
    }
    if (r.i < r.j) [[likely]] {
      r.sync(false);
    }
  }
}

}

#endif // SPORK_PARFOR_H_
