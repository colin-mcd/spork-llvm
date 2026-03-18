#include "parlay/internal/work_stealing_deque.h"
#include "parlay/internal/work_stealing_job.h"
#include "parlay/parallel.h"
#include "parlay/scheduler.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <signal.h>
//#include <ucontext.h>
#include <libunwind.h>
#include <iostream>

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

/*
 * IDEA:
 * using a macros(?), make an (extensible?) enum of all lambda types
 * and store the specific instance value in the spork table.
 * Then, do_promote() splits on that value and instantiates
 * the spwn lambda pointer to the correct type.
 * I believe this would allow "perfect forwarding"?
 */

// std::exception_ptr ex = std::current_exception();
// std::rethrow_exception(ex);
// struct joinpoint {
//   std::thread leftSideThread; // = std::this_thread::get_id();
//   std::thread::id* _Nullable rightSideThread;
//   void* _Nullable rightSideResult;
//   std::exception_ptr* _Nullable rightSideExn;

//   // 2: both sides executing, 
//   // 1: other side finished,
//   // 0: both sides finished
//   std::atomic_uint_fast8_t incounter;

//   unsigned int spareHeartbeatsGiven;
//   TokenPolicy tokenPolicy;

//   joinpoint(std::thread::id leftSideThread) :
//     leftSideThread(leftSideThread),
//     rightSideThread(nullptr),
//     rightSideResult(nullptr),
//     rightSideExn(nullptr),
//     incounter(2),
//     spareHeartbeatsGiven(0),
//     tokenPolicy(TokenPolicyFair) {}

//   joinpoint() : joinpoint(std::this_thread::get_id()) {}

//   // Atomically decrement incounter and
//   // return whether that operation reached 0
//   bool decrementIncounterHitsZero() {
//     return incounter.fetch_sub(1) == 1;
//   }
// };

thread_local volatile std::atomic_uint heartbeat_tokens = 10;

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

struct SpwnJob4 : WorkStealingJob {
  explicit SpwnJob4(const std::function<void()> _f, unsigned int hbt) : WorkStealingJob(), f(_f), num_heartbeat_tokens(hbt) { }
  void execute() override {
    heartbeat_tokens.fetch_add(num_heartbeat_tokens);
    f();
  }
 private:
  const std::function<void()> f;
 public:
  unsigned int num_heartbeat_tokens;
};

typedef uint_fast8_t spork_slot_idx_t;

typedef struct spork_entry_t {
  TokenPolicy token_policy;
  volatile bool* flag_offset;
  const std::function<void()>* spwn_offset;

  void offset(uintptr_t bp) {
    flag_offset = (volatile bool*) (bp - (uintptr_t) flag_offset);
    spwn_offset = (const std::function<void()>*) (bp - (uintptr_t) spwn_offset);
  }
} spork_entry_t;

typedef struct spork_row_t {
  // number of sporks this code location is nested inside
  const spork_slot_idx_t num_sporks;
  // num_sporks-length array of (promoted, spwn) offsets
  const spork_entry_t* spork_spwn_offsets;
} spork_row_t;

//extern const spork_row_t* spork_table_lookup_ip(unw_word_t ip);
spork_entry_t colin_default = {TokenPolicyFair, (volatile bool*) 0xd0, (const std::function<void()>*) 0x41};
spork_row_t colin_default_row = {1, &colin_default};
spork_row_t* spork_table_lookup_ip(unw_word_t ip) {
  switch (ip) {
    case 0x555555557c7b:
      return &colin_default_row;
    default:
      //return &colin_default_row;
      return nullptr;
  }
}

// struct SpwnJob : WorkStealingJob {
//   explicit SpwnJob(void (*_f)()) : WorkStealingJob(), f(_f) { }
//   void execute() override { f(); }
//  private:
//   void (*f)();
// };

// template <class _Rp, class... _ArgTypes>
// struct SpwnJob2 : WorkStealingJob {
//   explicit SpwnJob2(std::function<_Rp(_ArgTypes...)> _f) : WorkStealingJob(), f(_f) { }
//   void execute(_ArgTypes... args) override { f(args...); }
//  private:
//   std::function<_Rp(_ArgTypes...)> f;
// };

// template <typename F>
// struct SpwnJob3 : WorkStealingJob {
//   explicit SpwnJob3(F& _f) : WorkStealingJob(), f(_f) { }
//   void execute() override { f(); }
//  private:
//   F& f;
// };

using scheduler_t = parlay::scheduler<SpwnJob4>;

extern inline scheduler_t& get_current_scheduler() {
  auto current_scheduler = scheduler_t::get_current_scheduler();
  if (current_scheduler == nullptr) {
    static thread_local scheduler_t local_scheduler(internal::init_num_workers());
    return local_scheduler;
  }
  return *current_scheduler;
}

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
  // (flag,spwn) pairs and only do non-tail
  // recursive call when the buffer is full
  spork_entry_t top_spork;

  unw_word_t frame_ip, frame_sp, frame_bp;
  
  bool cont = true;
  bool can_prom = false;
  while (cont && unw_step(&cursor) > 0) {
    unw_get_reg(&cursor, UNW_REG_SP, &frame_sp);
    unw_get_reg(&cursor, UNW_REG_IP, &frame_ip);
    unw_get_reg(&cursor, UNW_X86_64_RBP, &frame_bp);
    std::cout << "frame ip = " << (void*) frame_ip << ", frame bp = " << (void*) frame_bp << ", frame sp = " << (void*) frame_sp << std::endl;
    const spork_row_t* row = spork_table_lookup_ip(frame_ip);
    // if return address is a function with sporks
    if (row) {
      // now check if sporks are unpromoted
      const spork_slot_idx_t num_sporks = row->num_sporks;
      for (spork_slot_idx_t slot = 0; slot < num_sporks; ++slot) {
        auto p = row->spork_spwn_offsets[slot];
        // p's offsets are relative to retaddr_sp
        p.offset(frame_bp);
        std::cout << "checking flag at " << (void*) p.flag_offset << " = " << *p.flag_offset << std::endl;
        if (*p.flag_offset == SPORK_UNPROMOTED) {
          // this slot is not yet promoted
          top_spork = p;
          can_prom = true;
          break;
        } else {
          // since this slot was already promoted, there cannot be any unpromoted above
          cont = false;
        }
      }
    }
  }
  if (can_prom) {
    std::cout << "TOP SPORK flag = " << (void*) top_spork.flag_offset << ", spwn job = " << (void*) top_spork.spwn_offset << std::endl;
    *top_spork.flag_offset = SPORK_PROMOTED;

    unsigned int give_hbt = 0;
    // this probably doesn't have to be thread-safe
    // because promotion should be run atomically, right?
    unsigned int cur_hbt = heartbeat_tokens.exchange(0);
    switch (top_spork.token_policy) {
      case TokenPolicyFair:
        give_hbt = cur_hbt >> 1;
        break;
      case TokenPolicyGive:
        give_hbt = cur_hbt;
        break;
      case TokenPolicyKeep:
        give_hbt = 0;
        break;
    }
    //top_spork.spwn_offset->num_heartbeat_tokens += give_hbt;
    heartbeat_tokens.fetch_add(cur_hbt - give_hbt);
    // TODO: ensure spawn(job)'s call to wake_up_a_worker is not blocking
    get_current_scheduler().spawn(new SpwnJob4(*top_spork.spwn_offset, give_hbt));
    return true;
  } else {
    return false; 
  }
}

// helper function for `try_consume_tokens()`
__attribute__((noinline))
unsigned int promote_until_failure() {
  unsigned int old_tokens = heartbeat_tokens.exchange(0);
  unsigned int new_tokens = old_tokens;
  while (new_tokens && do_promote()) new_tokens--;
  heartbeat_tokens.fetch_add(new_tokens);
  return old_tokens - new_tokens;
}

// Repeatedly tries to consume 1 heartbeat token until failure,
// returning number of times this was successful
__attribute__((always_inline))
unsigned int try_consume_tokens() {
  if (heartbeat_tokens) [[unlikely]] {
    return promote_until_failure();
  } else [[likely]] {
    return 0;
  }
}

// Acquires `new_tokens`, then promotes until failure
void acquire_heartbeat_tokens(unsigned int new_tokens) {
  heartbeat_tokens.fetch_add(new_tokens);
  try_consume_tokens();
}

//extern void __RTS_record_spork(const TokenPolicy tokenPolicy, volatile bool* flag, const SpwnJob4* spwn);
void __RTS_record_spork(const TokenPolicy tokenPolicy, volatile bool* flag, const std::function<void()>* spwn) {}

void dbgmsg(const void* ptr, uint64_t rbp) {
  std::cout << "ptr = " << (void*) ptr << ", rbp = " << (void*) rbp << ", diff = " << (void*) (rbp - (uint64_t) ptr) << std::endl;
}

// TODO: exception handling
template <typename A1, typename A2, typename A3, typename A4, typename A5>
__attribute__((always_inline))
static void spork(A1&& body,
                  A2&& unpr,
                  const TokenPolicy tokenPolicy,
                  const std::function<void()>& spwn,
                  A3&& prom,
                  A4&& unst,
                  A5&& sync) {
  //const int spid0 = FRESH_SPORK_ID;
  // TODO: does making this spawn job introduce overheads?
  //SpwnJob4 spwnj = SpwnJob4(spwn);
  //const std::function<void()> spwnjob = spwn;
  volatile bool __SPORK0_flag = SPORK_UNPROMOTED;
  uint64_t rbp_value;
  // Inline assembly to move the value of rsp into the C++ variable rsp_value
  __asm__("mov %%rbp, %0" : "=r"(rbp_value));
  dbgmsg(&spwn, rbp_value);
  dbgmsg((void*) &__SPORK0_flag, rbp_value);

  // spork body
  try_consume_tokens();
  std::forward<A1>(body)();
  int result = 151515;
  for (int i = 0; i < 100000; i++) {
    result ^= 0x351141421;
    result -= 14;
  }
  std::cout << result << std::endl;

  // spoin
  if (SPORK_IS_UNPR(__SPORK0_flag)) [[likely]] {
    // spoin unpromoted
    std::forward<A2>(unpr)();
  } else [[unlikely]] {
    std::forward<A3>(prom)();
    // spoin promoted
    scheduler_t& scheduler = get_current_scheduler();
    const SpwnJob4* spwn_job = scheduler.get_own_job();
    if (spwn_job != nullptr) [[likely]] {
      // unstolen
      heartbeat_tokens.fetch_add(spwn_job->num_heartbeat_tokens);
      std::forward<A4>(unst)();
    } else [[unlikely]] {
      // TODO: make sure this optimizes away
      __RTS_record_spork(tokenPolicy, &__SPORK0_flag, &spwn);
      // TODO: steal other work from this thread,
      // make spwn's thread resume this job when it finishes
      auto done = [&]() { return spwn_job->finished(); };
      scheduler.wait_until(done, false);
      assert(spwn_job->finished());
      delete spwn_job;
      std::forward<A5>(sync)();
    }
  }
} // void spork(...)
} // namespace spork_spoin
} // namespace parlay

int main(int argc, char* argv[]) {
  parlay::spork_spoin::get_current_scheduler();
  int x = 10;
  //for (int i = 0; i < 10000; i++)
  parlay::spork_spoin::spork
    ([&] () { std::cout << "body!" << std::endl;
              return x++;},
     [&] () { std::cout << "unpromoted!" << std::endl;
              return x += 5; },
     parlay::spork_spoin::TokenPolicyFair,
     [&] () { x += 4; },
     [&] () { std::cout << "promoted!" << std::endl;
              return x; },
     [&] () { std::cout << "unstolen!" << std::endl;
              return x; },
     [&] () { std::cout << "synchronize!" << std::endl;
              return x + x; }
     );
}
