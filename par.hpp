#ifndef SPORK_PAR_H_
#define SPORK_PAR_H_

#include "scheduler.hpp"

namespace spork {

template <typename LambdaL, typename LambdaR>
__attribute__((always_inline))
static void parSeq(LambdaL&& lamL, LambdaR&& lamR) {
  static_assert(std::is_invocable_v<LambdaL&>);
  static_assert(std::is_invocable_v<LambdaR&>);
  fwd(lamL)();
  fwd(lamR)();
}

template <typename LambdaL, typename LambdaR>
__attribute__((always_inline))
void par(const LambdaL&& lamL, const LambdaR&& lamR) {
  static_assert(std::is_invocable_v<const LambdaL&>);
  static_assert(std::is_invocable_v<const LambdaR&>);
  
  struct SpwnJob : WorkStealingJob {
    const LambdaR&& lamR;
    void run() override { fwd(lamR)(); }
    SpwnJob(const LambdaR&& lamR) :
      WorkStealingJob(),
      lamR(fwd(lamR)) {}
    SpwnJob(SpwnJob&& other) :
      WorkStealingJob(fwd(other)),
      lamR(fwd(other.lamR)) {}
  };
  
  volatile SpwnJob jp(fwd(lamR));

  bool promoted = with_prom_handler(
    fwd(lamL),
    [&jp] () { ((SpwnJob*) &jp)->enqueue(heartbeat_tokens >> 1); });

  if (promoted) [[unlikely]] { // promoted
    ((SpwnJob*) &jp)->sync(false);
  } else [[likely]] { // unpromoted
    fwd(lamR)();
  }
}
}

#endif // SPORK_PAR_H_
