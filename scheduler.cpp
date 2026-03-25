#include "parlay/internal/work_stealing_deque.h"
#include "parlay/internal/work_stealing_job.h"
#include "parlay/parallel.h"
#include "parlay/scheduler.h"
#include "parlay/alloc.h"

//#include <atomic>
//#include <csignal>
//#include <cstddef>
//#include <cstdint>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <signal.h>
#include <libunwind.h>
#include <sys/types.h>
#include <thread>
#include <time.h>
#include <iostream>
#include <utility>

using namespace parlay;

namespace spork {

enum TokenPolicy {
  TokenPolicyFair,
  TokenPolicyGive,
  TokenPolicyKeep
};

// #ifndef SPORK_PROMOTED
// #define SPORK_IS_UNPR(x) (x == nullptr)
// #define SPORK_IS_PROM(x) (x != nullptr)
// #define SPORK_SET_PROM(x) (x = SPORK_PROMOTED)
// #define SPORK_RESET(x) x = nullptr
// #endif

typedef uint spork_id_t;
#define FRESH_SPORK_ID __COUNTER__

/*
 * IDEA:
 * using macros(?), make an (extensible?) enum of all lambda types
 * and store the specific instance value in the spork table.
 * Then, do_promote() splits on that value and instantiates
 * the spwn lambda pointer to the correct type.
 * I believe this would allow "perfect forwarding"?
 */

static const uint TOKENS_PER_HEARTBEAT = 30;
static const uint HEARTBEAT_INTERVAL_US = 500;
// I set this arbitrarily: consider tweaking
static const uint MAX_HEARTBEAT_TOKENS = TOKENS_PER_HEARTBEAT*4;
thread_local volatile std::atomic_uint heartbeat_tokens = 0;

//auto x = std::this_thread::get_id();

struct SpwnJob : WorkStealingJob {

  using scheduler_t = parlay::scheduler<SpwnJob>;

  static scheduler_t& get_current_scheduler(){
    auto current_scheduler = scheduler_t::get_current_scheduler();
    if (current_scheduler == nullptr) {
      static thread_local scheduler_t local_scheduler(internal::init_num_workers());
      return local_scheduler;
    }
    return *current_scheduler;
  }

  explicit SpwnJob(void* spwn,
                   void* (*exec_spwn)(void*),
                   uint hbt,
                   SpwnJob* next = nullptr) :
    WorkStealingJob(),
    spwn(spwn),
    exec_spwn(exec_spwn),
    num_heartbeat_tokens(hbt),
    result(nullptr),
    next(next) {}

  SpwnJob& operator=(const SpwnJob& other) {
    if (this == &other) return *this;
    spwn = other.spwn;
    exec_spwn = other.exec_spwn;
    num_heartbeat_tokens = other.num_heartbeat_tokens;
    result = other.result;
    next = other.next;
    return *this;
  }
  
  SpwnJob* fresh() {
    SpwnJob* other = (SpwnJob*) p_malloc(sizeof(SpwnJob));
    if (other) *other = *this;
    return other;
  }

  SpwnJob(SpwnJob&& other) :
    SpwnJob(other.spwn,
            other.exec_spwn,
            other.num_heartbeat_tokens,
            other.next) { result = other.result; }

  void enqueue() {
    get_current_scheduler().spawn(this);
  }

  bool try_dequeue() {
    return get_current_scheduler().get_own_job() != nullptr;
  }

  // __attribute__((noinline))
  void execute() override {
    heartbeat_tokens.fetch_add(num_heartbeat_tokens);
    std::cout << "Executing spwn" << std::endl;
    result = exec_spwn(spwn);
    std::cout << "after spawn!!!" << std::endl;
    return;
  }

  void execute_wo_tokens() {
    result = exec_spwn(spwn);
  }

  void wait_or_execute() {
    if (try_dequeue()) { // unstolen
      execute_wo_tokens();
    } else { // stolen
      wait();
    }
  }

  __attribute__((always_inline))
  void sync() noexcept {
    SpwnJob* jp = this;
    while (jp != nullptr) {
      jp->wait_or_execute();
      SpwnJob* next = jp->next;
      p_free(jp);
      jp = next;
    }
  }
  
  template<typename A, typename CombLambda>
  __attribute__((always_inline))
  void syncUpdate(A& a, CombLambda&& combine) {
    SpwnJob* jp = this;
    while (jp != nullptr) {
      jp->wait_or_execute();
      SpwnJob* next = jp->next;
      A* r = (A*) jp->result;
      a = std::forward<CombLambda>(combine)(a, *r);
      p_free(r);
      p_free(jp);
      jp = next;
    }
  }

 private:
  void* spwn;
  void* (*exec_spwn)(void*);
  uint num_heartbeat_tokens;
 public:
  void* result;
  SpwnJob* next;
};

// using scheduler_t = parlay::scheduler<SpwnJob>;

// extern inline scheduler_t& get_current_scheduler() {
//   auto current_scheduler = scheduler_t::get_current_scheduler();
//   if (current_scheduler == nullptr) {
//     std::cout << "Initializing scheduler for n = " << internal::init_num_workers() << " workers" << std::endl;
//     static thread_local scheduler_t local_scheduler(internal::init_num_workers());
//     return local_scheduler;
//   }
//   return *current_scheduler;
// }

typedef uint_fast8_t spork_slot_idx_t;

typedef SpwnJob* volatile VolSpwnJob;

typedef struct spork_entry_t {
  volatile bool* promotable_flag;
  volatile uint* num_promotions;
  void* prom;
  bool (*exec_prom)(void* prom, uint tokens);
  
  spork_entry_t offset(unw_word_t bp) const noexcept {
    return {
      (volatile bool*) (bp - (unw_word_t) promotable_flag),
      (volatile uint*) (bp - (unw_word_t) num_promotions),
      (void*) (bp - (unw_word_t) prom),
      exec_prom
    };
  }
} spork_entry_t;

typedef struct spork_row_t {
  // number of sporks this code location is nested inside
  const spork_slot_idx_t num_sporks;
  // num_sporks-length array of (promoted, spwn) offsets
  const spork_entry_t* sporks;
} spork_row_t;

volatile bool* ad_hoc_promotable_flag = nullptr;
volatile uint* ad_hoc_num_promotions = nullptr;
void* ad_hoc_prom;
bool (*ad_hoc_exec_prom)(void*, uint);
void* ad_hoc_spork_ip_min = nullptr;
void* ad_hoc_spork_ip_max = nullptr;

spork_entry_t colin_default;
spork_row_t colin_default_row = {1, &colin_default};
void set_colin_default() {
  colin_default = {
    ad_hoc_promotable_flag,
    ad_hoc_num_promotions,
    ad_hoc_prom,
    ad_hoc_exec_prom
  };
}
spork_row_t* spork_table_lookup_ip(unw_word_t ip) {
  void* _ip = (void*) ip;
  if (ad_hoc_spork_ip_min <= _ip && _ip <= ad_hoc_spork_ip_max) return &colin_default_row;
  else return nullptr;
}

// TODO: make sure prom doesn't throw any exceptions
void do_promotion(const spork_entry_t& slot, uint& tokens) noexcept {
  uint give_hbt = tokens >> 1;
  tokens -= 1 + give_hbt;
  (*slot.num_promotions)++;
  // run slot's prom function,
  // then update if this spork is promotable any further
  *slot.promotable_flag = slot.exec_prom(slot.prom, give_hbt);
}

uint promotes(uint tokens, unw_cursor_t& cursor, unw_context_t& uc) noexcept {
  if (tokens == 0) return 0;
  unw_word_t frame_ip, frame_sp, frame_bp;
  const spork_row_t* row;

  // find first stack frame with sporks
  while (unw_step(&cursor) > 0) {
    // get stored register values for this stack frame
    unw_get_reg(&cursor, UNW_REG_SP, &frame_sp);
    unw_get_reg(&cursor, UNW_REG_IP, &frame_ip);
    unw_get_reg(&cursor, UNW_X86_64_RBP, &frame_bp);
    

    // std::cout << "frame ip = " << (void*) frame_ip << ", frame bp = " << (void*) frame_bp << ", frame sp = " << (void*) frame_sp << std::endl;

    row = spork_table_lookup_ip(frame_ip);
    // std::cout << "row = " << (void*) row << std::endl;
    if ((row != nullptr) && (row->num_sporks > 0)) break;
  }

  if (row == nullptr) return tokens;

  bool frame_has_promoted_slots = false;

  // inspect each spork slot
  for (spork_slot_idx_t slot_idx = 0; slot_idx < row->num_sporks && tokens > 0; ++slot_idx) {
    spork_entry_t slot = row->sporks[slot_idx].offset(frame_bp);
  
    frame_has_promoted_slots |= *slot.num_promotions > 0;
    if (*slot.promotable_flag) {
      // this slot is not yet promoted
  
      // try promoting above us first,
      // unless we already passed a promoted slot in this for loop
      if (!frame_has_promoted_slots) {
        tokens = promotes(tokens, cursor, uc);
      }
  
      if (tokens > 0) {
        do { do_promotion(slot, tokens); }
        while (tokens > 0 && *slot.promotable_flag);
        frame_has_promoted_slots = true;
      }
    }
  }
  return tokens;
}

std::atomic<bool> during_promotion = false;

__attribute__((noinline))
uint promotes_until_failure() noexcept {
  // std::cout << "ad_hoc_spork_ip = " << ad_hoc_spork_ip << std::endl;
  set_colin_default();

  // make sure we don't execute two promotes_until_failure concurrently
  bool expected = false;
  if (!during_promotion.compare_exchange_strong(expected, true)) return 0;

  uint old_tokens;
  uint new_tokens;
  uint during_tokens;
  unw_cursor_t cursor;
  unw_context_t uc;

  do {
    old_tokens = heartbeat_tokens.exchange(0);
    unw_getcontext(&uc);
    unw_init_local(&cursor, &uc);
    new_tokens = promotes(old_tokens, cursor, uc);
    during_tokens = heartbeat_tokens.fetch_add(new_tokens);
    // keep trying iff this promotes() call spent all tokens AND
    // more tokens were received by a heartbeat during that call
    // TODO: maybe refactor promotes() to automatically notice newly received tokens?
  } while (new_tokens == 0 && during_tokens > 0);

  during_promotion.store(false);
  return old_tokens - new_tokens;
}


// Repeatedly tries to consume 1 heartbeat token until failure,
// returning number of times this was successful
__attribute__((always_inline))
uint try_consume_tokens() noexcept {
  if (heartbeat_tokens) [[unlikely]] {
    //return promote_until_failure();
    return promotes_until_failure();
  } else [[likely]] {
    return 0;
  }
}

// Acquires `new_tokens`, then promotes until failure
// void acquire_heartbeat_tokens(uint new_tokens) noexcept {
//   heartbeat_tokens.fetch_add(new_tokens);
//   try_consume_tokens();
// }

//extern void __RTS_record_spork(const TokenPolicy tokenPolicy, volatile bool* flag, const SpwnJob4* spwn);
static void __RTS_record_spork(const TokenPolicy tokenPolicy, VolSpwnJob* job, const void* spwn, void (*exec_spwn)(void*)) noexcept {}

void dbgmsg(const void* ptr, uint64_t rbp) {
  std::cout << "ptr = " << (void*) ptr << ", rbp = " << (void*) rbp << ", diff = " << (void*) (rbp - (uint64_t) ptr) << std::endl;
}

// x must be a pointer to something invocable
// which returns a Result
//template <typename SpwnLambda, typename Result>
template <typename _Ret, typename _Fn, typename... _Args>
_Ret execute_lambda(void* f, _Args... args) {
  static_assert(std::is_invocable_r_v<_Ret, _Fn&, _Args...>);
  _Fn* _f = (_Fn*) f;
  return (*_f)(args...);
}

#ifndef FRAME_OFFSET
#define FRAME_OFFSET(x) ((uintptr_t) __builtin_frame_address(0) - (uintptr_t) (x))
#endif

// TODO: exception handling
template <typename BodyLambda, typename PromLambda>
__attribute__((always_inline))
static void spork(BodyLambda&& body, PromLambda&& prom) {
  static_assert(std::is_invocable_v<BodyLambda&&>);
  static_assert(std::is_invocable_r_v<bool, PromLambda&&, uint>);

  volatile bool promotable_flag = true;
  volatile uint num_promotions = 0;
  ad_hoc_promotable_flag = (volatile bool*) FRAME_OFFSET(&promotable_flag);
  ad_hoc_num_promotions = (volatile uint*) FRAME_OFFSET(&num_promotions);
  ad_hoc_prom = (void*) FRAME_OFFSET(&prom);
  ad_hoc_exec_prom = &execute_lambda<bool, PromLambda, uint>;
  ad_hoc_spork_ip_min = &&begin_body;
  ad_hoc_spork_ip_max = &&end_body;

  // begin body
  begin_body:
  try_consume_tokens();
  std::forward<BodyLambda>(body)();
  end_body:
  // end body
  return;
}

// TODO: look into `clone` function from /usr/include/x86_64-linux-gnu/bits/sched.h

// template<typename Lambda_t>
// static void* fresh_lambda(Lambda_t& lam) {
//   Lambda_t* ptr = (Lambda_t*) p_malloc(sizeof(Lambda_t));
//   std::byte b [sizeof(Lambda_t)];
//   for (unsigned int i = 0; i < sizeof(Lambda_t); i++) {
//     *(ptr + i*sizeof(std::byte)) = *((uintptr_t) lam + i*sizeof(std::byte));
//   }
//   if (ptr) *ptr = lam;
//   return (void*) ptr;
// }

template <typename LambdaL, typename LambdaR>
__attribute__((always_inline))
static void par(LambdaL&& lamL, LambdaR&& lamR) {
  static_assert(std::is_invocable_v<LambdaL&>);
  static_assert(std::is_invocable_v<LambdaR&>);
  VolSpwnJob jp = nullptr;

  spork(lamL, // TODO: should this be forwarded?
        [&] (uint tokens) {
          auto lamR2 = [&] () { std::forward<LambdaR>(lamR)(); return nullptr; };
          // TODO: fresh lamR2, then free in execute_lambda itself
          jp = SpwnJob(&lamR2, &execute_lambda<void*, decltype(lamR2)>, tokens).fresh();
          jp->enqueue();
          return false; // can do no more promotions here
        });

  if (jp == nullptr) [[likely]] {
    lamR();
  } else [[unlikely]] {
    jp->sync();
  }
}

__attribute__((always_inline))
static const uint midpoint(uint i, uint j) noexcept {
  return i + ((j - i) >> 1);
}

template <typename A, typename BodyLambda, typename CombLambda>
__attribute__((always_inline))
A reduce(A z, CombLambda&& combine, BodyLambda&& body, uint range_start, uint range_end) {
  static_assert(std::is_invocable_r_v<A, BodyLambda&, uint, A>);
  static_assert(std::is_invocable_r_v<A, CombLambda&, A, A>);
  VolSpwnJob jp = nullptr;
  volatile uint j = range_end;
  uint i = range_start;
  A a = z;
  spork(
    [&] () {
      while (i < j) {
        a = std::forward<BodyLambda>(body)(i, a);
        i++;
      }
    },
    [&] (uint tokens) {
      // Make sure to capture i and j by value so
      // their values aren't updated by body loop
      auto spwn = [&, i, j] () {
        A* aptr = (A*) p_malloc(sizeof(A));
        *aptr = reduce(z, combine, body, midpoint(i+1, j), j);
        return (void*) aptr;
      };
      j = midpoint(i+1, j);
      jp = SpwnJob(&spwn, &execute_lambda<void*, decltype(spwn)>, tokens).fresh();
      jp->enqueue();
      return j > i; // determine if more promotions are possible here
    });

  if (jp != nullptr) [[unlikely]] {
    jp->syncUpdate(a, combine);
  }
  return a;
}

__attribute__((noinline))
uint fib3(uint n) {
  return (n <= 1) ? n : fib3(n - 1) + fib3(n - 2);
}

__attribute__((noinline))
uint fib2(uint n) {
  // std::cout << "Fib2 called! n = " << n << std::endl;
  if (n <= 1) {
    return n;
  } else {
    uint l, r;
    par([&] () {l = fib2(n - 1);},
        [&] () {r = fib2(n - 2);});
    return l + r;
  }
}

__attribute__((noinline))
uint fib(uint n) {
  // std::cout << "Fib called! n = " << n << std::endl;
  if (n <= 1) {
    return n;
  } else {
    uint l, r;
    par([&] () {l = fib3(n - 1);},
        [&] () {r = fib3(n - 2);});
    return l + r;
  }
}

void heartbeat_handler(int sig, siginfo_t* info, void* ucontext) {
  int saved_errno = errno;
  //ucontext_t* ctx = (ucontext_t*) ucontext;

  uint tokens = heartbeat_tokens.exchange(0);
  tokens += tokens + TOKENS_PER_HEARTBEAT;
  if (tokens > MAX_HEARTBEAT_TOKENS) {
    heartbeat_tokens.store(MAX_HEARTBEAT_TOKENS);
  } else {
    heartbeat_tokens.fetch_add(tokens);
  }
  promotes_until_failure();
  
  errno = saved_errno;
}
int setup_handler() {
  struct sigaction sa = {};
  sa.sa_sigaction = heartbeat_handler;
  sa.sa_flags = SA_SIGINFO;// | SA_RESTART;
  //sigemptyset(&sa.sa_mask); // don't block extra signals during handler
  sigaction(SIGALRM, &sa, nullptr);

  timer_t tid;
  struct sigevent sev{};
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo  = SIGALRM;
  
  timer_create(CLOCK_MONOTONIC, &sev, &tid);
  
  struct itimerspec its{};
  its.it_value.tv_nsec    = HEARTBEAT_INTERVAL_US*1000;
  its.it_interval.tv_nsec = HEARTBEAT_INTERVAL_US*1000;
  
  timer_settime(tid, 0, &its, nullptr);
  return 0;
}
} // namespace spork


int main(int argc, char* argv[]) {
  spork::SpwnJob::get_current_scheduler();
  spork::setup_handler();
  
  // std::cout << parlay::spork::fib(40) << std::endl;
  // uint n = atoi(argv[1]);
  uint n = 1000000;
  unsigned long long total =
    spork::reduce<unsigned long long>(
      0,
      [&] (unsigned long long a, unsigned long long b) { return a + b; },
      [] (uint i, unsigned long long a) { return (unsigned long long) i/10000 + a; },
      0, n);
  std::cout << total << std::endl;
  // volatile int x = 10;
  // for (int i = 0; i < 100; i++) {
  // std::cout << "hello world inside loop" << std::endl;
  // parlay::spork::spork
  //   ([&] () { std::cout << "body!" << std::endl;
  //             return x++;},
  //    [&] () { std::cout << "unpromoted!" << std::endl;
  //             return x += 5; },
  //    parlay::spork::TokenPolicyFair,
  //    [&] () { std::cout << "spwn!" << std::endl;
  //             x += 4;
  //             std::cout << "spwn incremented x to " << (int) x << std::endl; },
  //    [&] () { std::cout << "promoted! x = " << (int) x << std::endl;
  //             return x; },
  //    [&] () { std::cout << "unstolen!" << std::endl;
  //             return x; },
  //    [&] () { std::cout << "synchronize! x = " << (int) x << std::endl;
  //             return x + x; }
  //    );
  // }
}
