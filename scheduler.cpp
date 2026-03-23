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
#define SPORK_IS_UNPR(x) (x == NULL)
#define SPORK_IS_PROM(x) (x != NULL)
#define SPORK_SET_PROM(x) (x = SPORK_PROMOTED)
#define SPORK_RESET(x) x = NULL
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

//thread_local unsigned int this_prom_heartbeat_tokens;
//thread_local std::function<void()> this_prom_spwn;


// struct WorkStealingJob {
//   WorkStealingJob() : done{false} { }
//   virtual ~WorkStealingJob() = default;
//   void operator()() {
//     assert(done.load(std::memory_order_relaxed) == false);
//     execute();
//     done.store(true, std::memory_order_release);
//   }
//   [[nodiscard]] bool finished() const noexcept {
//     return done.load(std::memory_order_acquire);
//   }
//   void wait() const noexcept {
//     while (!finished())
//       std::this_thread::yield();
//   }
//  protected:
//   virtual void execute() = 0;
//   std::atomic<bool> done;
// };

// thread_local std::atomic<bool>* this_spwn_done;

// void this_spwn_wait() noexcept {
//   while (!this_spwn_done->load(std::memory_order_acquire))
//     std::this_thread::yield();
// }

struct SpwnJob : WorkStealingJob {
  explicit SpwnJob(void* spwn,
                   void (*exec_spwn)(void*),
                   unsigned int hbt) :
    WorkStealingJob(),
    spwn(spwn),
    exec_spwn(exec_spwn),
    num_heartbeat_tokens(hbt) { }

  // __attribute__((noinline))
  void execute() override {
    heartbeat_tokens.fetch_add(num_heartbeat_tokens);
    // this_spwn_done = &done;
    exec_spwn(spwn);
    num_heartbeat_tokens = heartbeat_tokens.exchange(0);
    std::cout << "after spawn!!!" << std::endl;
    return;
  }
 private:
  void* spwn;
  void (*exec_spwn)(void*);
  unw_cursor_t cursor;
 public:
  unsigned int num_heartbeat_tokens;
};

typedef uint_fast8_t spork_slot_idx_t;

typedef SpwnJob* volatile VolSpwnJob;

typedef struct spork_entry_t {
  TokenPolicy token_policy;
  VolSpwnJob* job_offset;
  void* spwn_offset;
  void (*exec_spwn)(void*);
} spork_entry_t;

typedef struct spork_row_t {
  // number of sporks this code location is nested inside
  const spork_slot_idx_t num_sporks;
  // num_sporks-length array of (promoted, spwn) offsets
  const spork_entry_t* sporks;
} spork_row_t;

VolSpwnJob* ad_hoc_job;
void* ad_hoc_spwn;
void (*ad_hoc_exec_lambda)(void*);
void* ad_hoc_spork_ip;

spork_entry_t colin_default;
spork_row_t colin_default_row = {1, &colin_default};
void set_colin_default() {
  colin_default = {TokenPolicyFair, ad_hoc_job, ad_hoc_spwn, ad_hoc_exec_lambda};
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

unsigned int splitTokens(TokenPolicy policy, unsigned int tokens) noexcept {
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

void do_promotion(VolSpwnJob* job,
                  void* spwn, void (*exec_spwn)(void*),
                  unsigned int& tokens, TokenPolicy token_policy) noexcept {
  tokens--;
  unsigned int give_hbt = splitTokens(token_policy, tokens);
  tokens -= give_hbt;
  SpwnJob* j = new SpwnJob(spwn, exec_spwn, give_hbt);
  // marks this spork as promoted:
  *job = j;
  // TODO: ensure spawn(...)'s call to wake_up_a_worker is not blocking
  get_current_scheduler().spawn(j);
}

// TODO: consider how to do this from signal handler
// (ip not a return address in current frame)
unsigned int promotes(unsigned int tokens, unw_cursor_t& cursor, unw_context_t& uc) noexcept {
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
  for (spork_slot_idx_t slot_idx = 0; slot_idx < row->num_sporks && tokens > 0; ++slot_idx) {
    spork_entry_t slot = row->sporks[slot_idx];
    // p's offsets are relative to frame's bp and ip values
    VolSpwnJob* jp = (VolSpwnJob*) (frame_bp - (uintptr_t) slot.job_offset);
    std::cout << "checking jp at " << (void*) jp << " = " << (void*) *jp << std::endl;
  
    if (SPORK_IS_UNPR(*jp)) {
      // this slot is not yet promoted
  
      // try promoting above us first,
      // unless we already passed a promoted slot in this for loop
      if (no_promoted_slots) {
        // TODO: these copies might be somewhat expensive...
        // investigate if there is a way to avoid copying
        // unw_context_t backup_uc = uc;
        // unw_cursor_t backup_cursor = cursor;
        tokens = promotes(tokens, cursor, uc);
        // if (tokens > 0) {
        //   uc = backup_uc;
        //   cursor = backup_cursor;
        // }
      }
  
      if (tokens > 0) {
        void* spwn = (void*) (frame_bp - (uintptr_t) slot.spwn_offset);
        //VolSpwnJob* job = (VolSpwnJob*) (frame_bp - (uintptr_t) slot.job_offset);
        do_promotion(jp, spwn, slot.exec_spwn, tokens, slot.token_policy);
      } else { // no more tokens
        break;
      }
    } else { // think about terminating thread? Or return to spwnjob execute, which then does stuff
      no_promoted_slots = false;
    }
  }
  return tokens;
}

__attribute__((noinline))
unsigned int promotes_until_failure() noexcept {
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
unsigned int try_consume_tokens() noexcept {
  if (heartbeat_tokens) [[unlikely]] {
    //return promote_until_failure();
    return promotes_until_failure();
  } else [[likely]] {
    return 0;
  }
}

// Acquires `new_tokens`, then promotes until failure
void acquire_heartbeat_tokens(unsigned int new_tokens) noexcept {
  heartbeat_tokens.fetch_add(new_tokens);
  try_consume_tokens();
}

//extern void __RTS_record_spork(const TokenPolicy tokenPolicy, volatile bool* flag, const SpwnJob4* spwn);
static void __RTS_record_spork(const TokenPolicy tokenPolicy, VolSpwnJob* job, const void* spwn, void (*exec_spwn)(void*)) noexcept {}

void dbgmsg(const void* ptr, uint64_t rbp) {
  std::cout << "ptr = " << (void*) ptr << ", rbp = " << (void*) rbp << ", diff = " << (void*) (rbp - (uint64_t) ptr) << std::endl;
}

template <typename SpwnLambda>
void execute_lambda(void* x) {
  SpwnLambda* f = (SpwnLambda*) x;
  static_assert(std::is_invocable_v<SpwnLambda&>);
  (*f)();
}

// TODO: exception handling
template <typename BodyLambda,
          typename UnprLambda,
          typename SpwnLambda,
          typename PromLambda,
          typename UnstLambda,
          typename SyncLambda>
__attribute__((always_inline))
static void spork(BodyLambda&& body,
                  UnprLambda&& unpr,
                  const TokenPolicy tokenPolicy,
                  SpwnLambda&& spwn,
                  PromLambda&& prom,
                  UnstLambda&& unst,
                  SyncLambda&& sync) {
  SPORK_RESET(VolSpwnJob jp);

  ad_hoc_spwn = (void*) ((uintptr_t) __builtin_frame_address(0) - (uintptr_t) &spwn);
  ad_hoc_exec_lambda = &execute_lambda<SpwnLambda>;
  ad_hoc_job = (VolSpwnJob*) ((uintptr_t) __builtin_frame_address(0) - (uintptr_t) &jp);

  //const int spid0 = FRESH_SPORK_ID;

  // spork
  if (SPORK_IS_UNPR(jp)) [[likely]] { // body
    try_consume_tokens();
    std::forward<BodyLambda>(body)();
    // arbitrary code to take time
    int result = 151515;
    for (int i = 0; i < 100000; i++) {
      result ^= 0x351141421;
      result -= 14;
    }
    std::cout << result << std::endl;
  
    // spoin
    if (SPORK_IS_UNPR(jp)) [[likely]] { // unpromoted
      std::forward<UnprLambda>(unpr)();
    } else [[unlikely]] { // promoted
      // reset flag so if we run this spork again later, the flag is unpromoted
      // TODO: make sure __SPORK0_flag initialization gets moved
      // outside loops (may need to be done with a special pass)
      std::forward<PromLambda>(prom)();
      scheduler_t& scheduler = get_current_scheduler();
      const SpwnJob* spwn_job = scheduler.get_own_job();
      if (spwn_job != nullptr) [[likely]] { // unstolen
        heartbeat_tokens.fetch_add(spwn_job->num_heartbeat_tokens);
        delete spwn_job;
        std::forward<UnstLambda>(unst)();
      } else [[unlikely]] { // stolen
        // TODO: make sure this optimizes away
        __RTS_record_spork(tokenPolicy, &jp, &spwn, &execute_lambda<SpwnLambda>);
        // TODO: steal other work from this thread,
        // make spwn's thread resume this job when it finishes
        //auto done = [&]() { return spwn_job->finished(); };
        //scheduler.wait_until(done, false);
        jp->wait();
        delete jp;
        SPORK_RESET(jp);
        std::forward<SyncLambda>(sync)();
      }
    }
  } else [[unlikely]] { // spwn
    // this branch may or may not be necessary;
    // technically, it is *never* reached
    // but may be necessary to stop DCE from eliminating spwn
    std::forward<SpwnLambda>(spwn)();
  }
} // void spork(...)
} // namespace spork_spoin
} // namespace parlay

int main(int argc, char* argv[]) {
  parlay::spork_spoin::get_current_scheduler();
  volatile int x = 10;
  for (int i = 0; i < 12345; i++) {
  std::cout << "hello world inside loop" << std::endl;
  parlay::spork_spoin::spork
    ([&] () { std::cout << "body!" << std::endl;
              return x++;},
     [&] () { std::cout << "unpromoted!" << std::endl;
              return x += 5; },
     parlay::spork_spoin::TokenPolicyFair,
     [&] () { std::cout << "spwn!" << std::endl;
              x += 4;
              std::cout << "spwn incremented x to " << (int) x << std::endl; },
     [&] () { std::cout << "promoted! x = " << (int) x << std::endl;
              return x; },
     [&] () { std::cout << "unstolen!" << std::endl;
              return x; },
     [&] () { std::cout << "synchronize! x = " << (int) x << std::endl;
              return x + x; }
     );
  }
}
