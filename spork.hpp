#ifndef SPORK_SPORK_H_
#define SPORK_SPORK_H_

#include "scheduler.hpp"

namespace spork {

template <typename BR, typename SR, typename RR, typename BodyL, typename SpwnL, typename UnprL, typename PromL, typename UnstL>
__attribute__((always_inline))
RR spork(const BodyL&& body, const SpwnL&& spwn, const UnprL&& unpr, const PromL&& prom, const UnstL&& unst) {
  static_assert(std::is_invocable_r_v<BR, const BodyL&>);
  static_assert(std::is_invocable_r_v<SR, const SpwnL&>);
  static_assert(std::is_invocable_r_v<RR, const UnprL&, BR>);
  static_assert(std::is_invocable_r_v<RR, const PromL&, BR, SR>);
  static_assert(std::is_invocable_r_v<RR, const UnstL&, BR>);
  
  struct SpwnJob : WorkStealingJob {
    const SpwnL&& spwn;
    SR sr;
    void run() override { sr = fwd(spwn)(); }
    SpwnJob(const SpwnL&& _spwn) :
      WorkStealingJob(),
      spwn(fwd(_spwn)) {}
  };
  
  SpwnJob jp(fwd(spwn));
  BR br;

  bool promoted = with_prom_handler(
    [&] () { br = fwd(body)(); },
    [&] () { jp.enqueue(heartbeat_tokens >> 1); });

  if (promoted) [[unlikely]] { // promoted
    if (jp.sync_is_stolen()) {
      return fwd(prom)(br, jp.sr);
    } else {
      return fwd(unst)(br);
    }
  } else [[likely]] { // unpromoted
    return fwd(unpr)(br);
  }
}

}

#endif // SPORK_SPORK_H_
