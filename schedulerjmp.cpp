#include "parlay/internal/work_stealing_deque.h"
#include "parlay/internal/work_stealing_job.h"
#include "parlay/parallel.h"
#include "parlay/scheduler.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <signal.h>
#include <sys/ucontext.h>
#include <ucontext.h>
#include <libunwind.h>
#include <setjmp.h>
//#include <bits/setjmp.h>
//#include <bits/types/struct___jmp_buf_tag.h>

namespace parlay {
namespace spork_spoin {

enum TokenPolicy {
  TokenPolicyFair,
  TokenPolicyGive,
  TokenPolicyKeep
};

#ifndef SPORK_PROMOTED
#define SPORK_PROMOTED true
#define SPORK_UNPROMOTED false
#define SPORK_IS_UNPR(x) (!x)
#define SPORK_IS_PROM(x) (x)
#endif

typedef unsigned int spork_id_t;
#define FRESH_SPORK_ID __COUNTER__

// std::exception_ptr ex = std::current_exception();
// std::rethrow_exception(ex);

thread_local volatile std::atomic_uint heartbeat_tokens = 0;

// void handler(int sig, siginfo_t* info, void* ucontext) {
//   int saved_errno = errno;
//   ucontext_t* ctx = (ucontext_t*) ucontext;
  
//   errno = saved_errno;
// }

// void setup_handler() {
//   struct sigaction sa = {};
//   sa.sa_sigaction = handler;
//   sa.sa_flags = SA_SIGINFO | SA_RESTART;
//   //sigemptyset(&sa.sa_mask); // don't block extra signals during handler
  
// }

typedef uint_fast8_t spork_slot_idx_t;

typedef struct spork_entry_t {
  // number of sporks this code location is nested inside
  const spork_slot_idx_t num_sporks;
  // num_sporks-length array of (SP, IP) offsets
  const std::pair<void*, void*>* spork_spwn_offsets;
} spork_entry_t;

extern spork_entry_t* spork_table_lookup_ip(unw_word_t ip);

// TODO: consider how to do this from signal handler
// (ip not a return address in current frame)
bool do_promote() {
  unw_cursor_t cursor;
  unw_context_t uc;
  unw_getcontext(&uc);
  unw_init_local(&cursor, &uc);
  // TODO if called from signal handler, above should be:
  // (although, confirm that this doesn't just search the signal stack!!)
  // unw_init_local2(&cursor, &uc, UNW_INIT_SIGNAL_FRAME);

  // possible optimization TODO:
  // only call spork_table_lookup_ip(ip)
  // if no frame above us succeeded.
  // implement this with an array buffer of
  // (ip,sp) pairs and only do non-tail
  // recursive call when the buffer is full
  bool* top_spork_flag = nullptr;
  void* top_spork_sp = nullptr;
  void* top_spork_ip = nullptr;
  unw_word_t top_spork_retaddr_ip;
  unw_word_t top_spork_retaddr_sp;
  unw_word_t retaddr_ip, retaddr_sp;
  while (unw_step(&cursor) > 0) {
    unw_get_reg(&cursor, UNW_REG_IP, &retaddr_ip);
    unw_get_reg(&cursor, UNW_REG_SP, &retaddr_sp);
    spork_entry_t* row = spork_table_lookup_ip(retaddr_ip);
    // if return address is a function with sporks
    if (row) {
      // now check if sporks are unpromoted
      const spork_slot_idx_t num_sporks = row->num_sporks;
      for (spork_slot_idx_t slot = 0; slot < num_sporks; ++slot) {
        auto p = row->spork_spwn_offsets[slot];
        bool* promoted = (bool*) ((unw_word_t) p.first + retaddr_sp);
        if (! *promoted) {
          top_spork_flag = promoted;
          top_spork_sp = p.first;
          top_spork_ip = p.second;
          top_spork_retaddr_sp = retaddr_sp;
          top_spork_retaddr_ip = retaddr_ip;
          break;
        }
      }
    }
  }
  if (top_spork_flag) {
    *top_spork_flag = SPORK_PROMOTED;
    // TODO: actually do the promotion
    jmp_buf* buf = (jmp_buf*) top_spork_sp; // TODO: probably not correct, need to modify above
    longjmp(buf, 1);
    //longjmp();
    return true;
  } else {
    return false; 
  }
}

bool try_promote() {
  unsigned int old_tokens = heartbeat_tokens.exchange(0);
  if (old_tokens) [[unlikely]] {
    unsigned int new_tokens = old_tokens - 1;
    heartbeat_tokens.fetch_add(new_tokens);
    return do_promote();
  } else [[likely]] {
    return false;
  }
}

// Tries to consume 1 heartbeat token, returning whether this was successful
__attribute__((always_inline))
bool try_consume_token() {
  if (heartbeat_tokens) [[unlikely]] {
    return try_promote();
  } else [[likely]] {
    return false;
  }
  // TODO: could try making a fallback return false (below) instead of else branches (above)
  // return false;
}

using Job = WorkStealingJob;
using scheduler_t = parlay::scheduler<Job>;

template <typename A, typename B, typename C, typename X>
__attribute__((always_inline))
X spork(A&& body,
        X&& unpr(A a),
        TokenPolicy tokenPolicy,
        B&& spwn(),
        C&& prom(A a),
        X&& unst(C c),
        X&& sync(C c, B b)) {
  //const int spid0 = FRESH_SPORK_ID;
  // TODO: handle token policy
  A a;
  volatile B b;
  X result;
  // body
  static jmp_buf __SPORK0_buf;
  volatile bool __SPORK0_flag = SPORK_UNPROMOTED;
  if (!setjmp(__SPORK0_buf)) [[likely]] {
    a = std::forward<A>(body)();
  } else [[unlikely]] {
    // set __SPORK0_flag to SPORK_PROMOTED;
    // this already happens in do_promote(),
    // but may as well do it here too,
    // to help guide CFG analysis(?)
    __SPORK0_flag = SPORK_PROMOTED;
    parlay::JobImpl<B> spwn_job = make_job([&] () {b = std::forward<B>(spwn)();});
    scheduler_t& scheduler = parlay::internal::get_current_scheduler();
    scheduler.spawn(&spwn_job);
  }
  if (SPORK_IS_UNPR(__SPORK0_flag)) [[likely]] {
    // unpromoted
    result = std::forward<X>(unpr)(a);
  } else [[unlikely]] {
    // promoted
    scheduler_t& scheduler = parlay::internal::get_current_scheduler();
    const Job* spwn_job = scheduler.get_own_job();
    if (spwn_job != nullptr) [[likely]] {
      // unstolen
      result = std::forward(unst)(a);
    } else [[unlikely]] {
      const C c = std::forward(prom)(a);
      // TODO: steal other work from this thread,
      // make spwn's thread resume this job when it finishes
      auto done = [&]() { return spwn_job->finished(); };
      scheduler.wait_until(done, false);
      assert(spwn_job->finished());
    }
    __SPORK0_flag = SPORK_UNPROMOTED;
  }

  return result;
}

} // namespace spork_spoin
}  // namespace parlay
