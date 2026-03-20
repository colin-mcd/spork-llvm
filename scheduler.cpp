#include "parlay/internal/work_stealing_deque.h"
#include "parlay/internal/work_stealing_job.h"
#include "parlay/parallel.h"
#include "parlay/scheduler.h"

//#include <atomic>
//#include <csignal>
//#include <cstddef>
//#include <cstdint>
//#include <signal.h>
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
#define SPORK_PROMOTED (true)
#define SPORK_UNPROMOTED (false)
#define SPORK_IS_UNPR(x) (!x)
#define SPORK_IS_PROM(x) (x)
#define SPORK_PROMOTE(x) (*x = SPORK_PROMOTED)
#endif

typedef unsigned int spork_id_t;
#define FRESH_SPORK_ID __COUNTER__

/*
 * IDEA:
 * using macros(?), make an (extensible?) enum of all lambda types
 * and store the specific instance value in the spork table.
 * Then, do_promote() splits on that value and instantiates
 * the spwn lambda pointer to the correct type.
 * I believe this would allow "perfect forwarding"?
 */

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

thread_local unw_cursor_t resume_job_cursor;

struct SpwnJob : WorkStealingJob {
  explicit SpwnJob(void* spwn_ip,
                   unw_cursor_t cursor,
                   unsigned int hbt) :
    WorkStealingJob(),
    spwn_ip(spwn_ip),
    cursor(cursor),
    num_heartbeat_tokens(hbt) { }

  // __attribute__((noinline))
  void execute() override {
    heartbeat_tokens.fetch_add(num_heartbeat_tokens);
    unw_set_reg(&cursor, (unw_regnum_t) UNW_REG_IP, (unw_word_t) spwn_ip);
    // TODO: do we need to do this every time?
    // probably, since thread local?
    // but does it actually need to be thread local...?

    unw_word_t THIS_SP_OFFSET = 0x10000;
    unw_word_t this_sp, this_bp, spwn_sp, spwn_bp, next_sp, next_bp, this_size, spwn_size;
    unw_get_reg(&cursor, UNW_REG_SP, &spwn_sp);
    unw_get_reg(&cursor, UNW_X86_64_RBP, &spwn_bp);

    unw_context_t uc;
    unw_getcontext(&uc);
    unw_init_local(&resume_job_cursor, &uc);

    unw_set_reg(&resume_job_cursor, UNW_REG_IP, (unw_word_t) &&after_spwn);
    unw_get_reg(&resume_job_cursor, UNW_REG_SP, &this_sp);
    unw_get_reg(&resume_job_cursor, UNW_X86_64_RBP, &this_bp);

    this_size = this_bp - this_sp;
    next_bp = this_sp - THIS_SP_OFFSET;
    next_sp = next_bp - spwn_size;

    //std::memcpy((void*) next_sp, (void*) spwn_sp, spwn_size);
    //unw_set_reg(&cursor, (unw_regnum_t) UNW_REG_SP, (unw_word_t) this_sp - THIS_SP_OFFSET);
    unw_resume(&cursor);
    // TODO: maybe have spwn branch of spork function be the thing
    // to construct a closure and push it onto work stealing deque?
    after_spwn:
    std::cout << "after spawn!!!" << std::endl;
    return;
  }
 private:
  void* spwn_ip;
  unw_cursor_t cursor;
 public:
  unsigned int num_heartbeat_tokens;
};

typedef uint_fast8_t spork_slot_idx_t;

typedef struct spork_entry_t {
  TokenPolicy token_policy;
  volatile bool* flag_offset;
  void* spwn;

  void offset(uintptr_t bp) {
    flag_offset = (volatile bool*) (bp - (uintptr_t) flag_offset);
  }
} spork_entry_t;

typedef struct spork_row_t {
  // number of sporks this code location is nested inside
  const spork_slot_idx_t num_sporks;
  // num_sporks-length array of (promoted, spwn) offsets
  const spork_entry_t* sporks;
} spork_row_t;

//extern const spork_row_t* spork_table_lookup_ip(unw_word_t ip);
volatile bool* ad_hoc_spork_flag;
void* ad_hoc_spwn_ip;
void* ad_hoc_spork_ip;

spork_entry_t colin_default;
spork_row_t colin_default_row = {1, &colin_default};
void set_colin_default() {
  colin_default = {TokenPolicyFair, ad_hoc_spork_flag, ad_hoc_spwn_ip};
}
spork_row_t* spork_table_lookup_ip(unw_word_t ip) {
  if ((void*) ip == ad_hoc_spork_ip) return &colin_default_row;
  else return nullptr;
}

using scheduler_t = parlay::scheduler<SpwnJob>;

extern inline scheduler_t& get_current_scheduler() {
  auto current_scheduler = scheduler_t::get_current_scheduler();
  if (current_scheduler == nullptr) {
    static thread_local scheduler_t local_scheduler(internal::init_num_workers());
    return local_scheduler;
  }
  return *current_scheduler;
}

unsigned int splitTokens(TokenPolicy policy, unsigned int tokens) {
  unsigned int give_hbt = 0;
  switch (policy) {
    case TokenPolicyFair:
      give_hbt = tokens >> 1;
      break;
    case TokenPolicyGive:
      give_hbt = tokens;
      break;
    case TokenPolicyKeep:
      give_hbt = 0;
      break;
  }
  return give_hbt;
}

// TODO: consider how to do this from signal handler
// (ip not a return address in current frame)
unsigned int promotes(unsigned int tokens, unw_cursor_t& cursor, unw_context_t& uc) {
  if (tokens == 0) return 0;
  unw_word_t frame_ip, frame_sp, frame_bp;
  const spork_row_t* row;

  // find first stack frame with sporks
  while (unw_step(&cursor) > 0) {
    // get stored register values for this stack frame
    unw_get_reg(&cursor, UNW_REG_SP, &frame_sp);
    unw_get_reg(&cursor, UNW_REG_IP, &frame_ip);
    unw_get_reg(&cursor, UNW_X86_64_RBP, &frame_bp);
    

    std::cout << "frame ip = " << (void*) frame_ip << ", frame bp = " << (void*) frame_bp << ", frame sp = " << (void*) frame_sp << std::endl;

    row = spork_table_lookup_ip(frame_ip);
    std::cout << "row = " << (void*) row << std::endl;
    if ((row != NULL) && (row->num_sporks > 0)) break;
  }

  if (row == NULL) return tokens;

  bool no_promoted_slots = true;

  // inspect each spork slot
  for (spork_slot_idx_t slot = 0; slot < row->num_sporks && tokens > 0; ++slot) {
    spork_entry_t p = row->sporks[slot];
    // p's offsets are relative to frame's bp and ip values
    volatile bool* flag = (volatile bool*) (frame_bp - (uintptr_t) p.flag_offset);
    std::cout << "checking flag at " << (void*) flag << " = " << *flag << std::endl;
  
    if (SPORK_IS_UNPR(*flag)) {
      // this slot is not yet promoted
  
      // try promoting above us first,
      // unless we already passed a promoted slot in this for loop
      if (no_promoted_slots) {
        // TODO: these copies might be somewhat expensive...
        // investigate if there is a way to avoid copying
        unw_context_t backup_uc = uc;
        unw_cursor_t backup_cursor = cursor;
        tokens = promotes(tokens, cursor, uc);
        if (tokens > 0) {
          uc = backup_uc;
          cursor = backup_cursor;
          
        }
      }
  
      if (tokens > 0) {
        std::cout << "TOP SPORK flag = " << (void*) flag << ", spwn job = " << (void*) p.spwn << std::endl;
        SPORK_PROMOTE(flag);
        tokens--;
        
        unsigned int give_hbt = splitTokens(p.token_policy, tokens);
        tokens -= give_hbt;
        // TODO: ensure spawn(...)'s call to wake_up_a_worker is not blocking
        // get_current_scheduler().spawn(new SpwnJob(p.spwn, frame_sp, frame_bp, give_hbt));
        get_current_scheduler().spawn(new SpwnJob(p.spwn, cursor, give_hbt));
      } else { // no more tokens
        break;
      }
    } else { // think about terminating thread? Or return to spwnjob execute, which then does stuff
      no_promoted_slots = false;
    }
  }
  return tokens;
}

// TODO: consider how to do this from signal handler
// (ip not a return address in current frame)
// bool do_promote() {
//   unw_cursor_t cursor;
//   unw_context_t uc;
//   unw_getcontext(&uc);
//   unw_init_local(&cursor, &uc);
//   // TODO if called from signal handler, above should be:
//   // (although, confirm that this doesn't just search the signal stack!!)
//   // unw_init_local2(&cursor, &uc, UNW_INIT_SIGNAL_FRAME);

//   // possible optimization TODO:
//   // only call spork_table_lookup_ip(ip)
//   // if no frame above us succeeded.
//   // implement this with an array buffer of
//   // (flag,spwn) pairs and only do non-tail
//   // recursive call when the buffer is full
//   spork_entry_t top_spork;

//   unw_word_t frame_ip, frame_sp, frame_bp;
  
//   bool cont = true;
//   bool can_prom = false;
//   while (cont && unw_step(&cursor) > 0) {
//     unw_get_reg(&cursor, UNW_REG_SP, &frame_sp);
//     unw_get_reg(&cursor, UNW_REG_IP, &frame_ip);
//     unw_get_reg(&cursor, UNW_X86_64_RBP, &frame_bp);
//     std::cout << "frame ip = " << (void*) frame_ip << ", frame bp = " << (void*) frame_bp << ", frame sp = " << (void*) frame_sp << std::endl;
//     const spork_row_t* row = spork_table_lookup_ip(frame_ip);
//     // if return address is a function with sporks
//     if (row) {
//       // now check if sporks are unpromoted
//       const spork_slot_idx_t num_sporks = row->num_sporks;
//       for (spork_slot_idx_t slot = 0; slot < num_sporks; ++slot) {
//         auto p = row->sporks[slot];
//         // p's offsets are relative to retaddr_sp
//         p.offset(frame_bp, frame_ip);
//         std::cout << "checking flag at " << (void*) p.flag_offset << " = " << *p.flag_offset << std::endl;
//         if (*p.flag_offset == SPORK_UNPROMOTED) {
//           // this slot is not yet promoted
//           top_spork = p;
//           can_prom = true;
//           break;
//         } else {
//           // since this slot was already promoted, there cannot be any unpromoted above
//           cont = false;
//         }
//       }
//     }
//   }
//   if (can_prom) {
//     std::cout << "TOP SPORK flag = " << (void*) top_spork.flag_offset << ", spwn job = " << (void*) top_spork.spwn_offset << std::endl;
//     *top_spork.flag_offset = SPORK_PROMOTED;

//     unsigned int give_hbt = 0;
//     // this probably doesn't have to be thread-safe
//     // because promotion should be run atomically, right?
//     unsigned int cur_hbt = heartbeat_tokens.exchange(0);
//     switch (top_spork.token_policy) {
//       case TokenPolicyFair:
//         give_hbt = cur_hbt >> 1;
//         break;
//       case TokenPolicyGive:
//         give_hbt = cur_hbt;
//         break;
//       case TokenPolicyKeep:
//         give_hbt = 0;
//         break;
//     }
//     //top_spork.spwn_offset->num_heartbeat_tokens += give_hbt;
//     heartbeat_tokens.fetch_add(cur_hbt - give_hbt);
//     // TODO: ensure spawn(job)'s call to wake_up_a_worker is not blocking
//     get_current_scheduler().spawn(new SpwnJob(top_spork.spwn_offset, TODO, TODO, give_hbt));
//     return true;
//   } else {
//     return false; 
//   }
// }

// helper function for `try_consume_tokens()`
// __attribute__((noinline))
// unsigned int promote_until_failure() {
//   unsigned int old_tokens = heartbeat_tokens.exchange(0);
//   unsigned int new_tokens = old_tokens;
//   while (new_tokens && do_promote()) new_tokens--;
//   heartbeat_tokens.fetch_add(new_tokens);
//   return old_tokens - new_tokens;
// }

__attribute__((noinline))
unsigned int promotes_until_failure() {
  ad_hoc_spork_ip = __builtin_return_address(0);
  std::cout << "ad_hoc_spork_ip = " << ad_hoc_spork_ip << std::endl;
  set_colin_default();

  unsigned int old_tokens = heartbeat_tokens.exchange(0);
  unw_cursor_t cursor;
  unw_context_t uc;
  unw_getcontext(&uc);
  unw_init_local(&cursor, &uc);
  // TODO if called from signal handler, above should be:
  // (although, confirm that this doesn't just search the signal stack!!)
  // unw_init_local2(&cursor, &uc, UNW_INIT_SIGNAL_FRAME);
  unsigned int new_tokens = promotes(old_tokens, cursor, uc);
  heartbeat_tokens.fetch_add(new_tokens);
  return old_tokens - new_tokens;
}


// Repeatedly tries to consume 1 heartbeat token until failure,
// returning number of times this was successful
__attribute__((always_inline))
unsigned int try_consume_tokens() {
  if (heartbeat_tokens) [[unlikely]] {
    //return promote_until_failure();
    return promotes_until_failure();
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
void __RTS_record_spork(const TokenPolicy tokenPolicy, volatile bool* flag, const void* spwn) {}

//#include "UnwindRegisterRestore.S"

// TODO: make this architecture-independent
//extern "C" void __libunwind_Registers_x86_64_jumpto(unw_context_t);

void dbgmsg(const void* ptr, uint64_t rbp) {
  std::cout << "ptr = " << (void*) ptr << ", rbp = " << (void*) rbp << ", diff = " << (void*) (rbp - (uint64_t) ptr) << std::endl;
}

// TODO: exception handling
template <typename A1, typename A2, typename A3, typename A4, typename A5, typename A6>
__attribute__((always_inline))
static void spork(A1&& body,
                  A2&& unpr,
                  const TokenPolicy tokenPolicy,
                  A6&& spwn,
                  A3&& prom,
                  A4&& unst,
                  A5&& sync) {
  volatile bool __SPORK0_flag = SPORK_UNPROMOTED;
  ad_hoc_spwn_ip = &&spwn_label;
  ad_hoc_spork_flag = (volatile bool*) ((uintptr_t) __builtin_frame_address(0) - (uintptr_t) &__SPORK0_flag);

  //const int spid0 = FRESH_SPORK_ID;

  // spork
  if (SPORK_IS_UNPR(__SPORK0_flag)) [[likely]] { // body
    try_consume_tokens();
    std::forward<A1>(body)();
    // arbitrary code to take time
    int result = 151515;
    for (int i = 0; i < 100000; i++) {
      result ^= 0x351141421;
      result -= 14;
    }
    std::cout << result << std::endl;
  
    // spoin
    if (SPORK_IS_UNPR(__SPORK0_flag)) [[likely]] { // unpromoted
      std::forward<A2>(unpr)();
    } else [[unlikely]] { // promoted
      std::forward<A3>(prom)();
      scheduler_t& scheduler = get_current_scheduler();
      const SpwnJob* spwn_job = scheduler.get_own_job();
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
  } else [[unlikely]] { // spwn
    spwn_label:
    std::cout << "inside spwn!" << std::endl;
    std::forward<A6>(spwn)();
    int result = 1234556;
    for (int i = 0; i < 1000000; i++) {
      result ^= 0x351141421;
      result -= 14;
    }
    std::cout << result << std::endl;
    unw_resume(&resume_job_cursor);
  }
} // void spork(...)
} // namespace spork_spoin
} // namespace parlay

int main(int argc, char* argv[]) {
  parlay::spork_spoin::get_current_scheduler();
  volatile int x = 10;
  //for (int i = 0; i < 10000; i++)
  auto spwn =
    [&] () { std::cout << "spwn!" << std::endl;
             x += 4;
             std::cout << "spwn incremented x to " << (int) x << std::endl; };
  // const std::type_info tp = typeid(;
  std::cout << typeid(spwn).name() << ", " << sizeof(spwn) << std::endl;
  parlay::spork_spoin::spork
    ([&] () { std::cout << "body!" << std::endl;
              return x++;},
     [&] () { std::cout << "unpromoted!" << std::endl;
              return x += 5; },
     parlay::spork_spoin::TokenPolicyFair,
     spwn,
     [&] () { std::cout << "promoted! x = " << (int) x << std::endl;
              return x; },
     [&] () { std::cout << "unstolen!" << std::endl;
              return x; },
     [&] () { std::cout << "synchronize!" << std::endl;
              return x + x; }
     );
}
