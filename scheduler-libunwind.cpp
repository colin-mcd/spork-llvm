#include "parlay/internal/work_stealing_job.h"
#include "parlay/parallel.h"
#include "parlay/alloc.h"
#include "parlay/monoid.h"

#include <atomic>
#include <libunwind.h>
#include <sys/cdefs.h>
#include <type_traits>

//#include <linux/perf_event.h>
//#include <sys/ioctl.h>
//#include <fcntl.h>

// TODO: consider a (libunwind) setjmp/longjmp based implementation
// TODO: libunwind implementation broken for any (heterogenous) nested parallelism
#define PROM_USE_LIBUNWIND 0
#define RECORD_HEARTBEAT_STATS 0

// TODO: consistent name casing (camel or snake)
// TODO: consider adaptive heartbeat timer intervals
// TODO: look into if loop unrolling is why reduce is slower than reduceSeq

//using namespace parlay;

namespace spork {

typedef uint spork_id_t;
//#define FRESH_SPORK_ID __COUNTER__

constexpr uint TOKENS_PER_HEARTBEAT = 30;
constexpr uint HEARTBEAT_INTERVAL_US = 500;
// I set this arbitrarily: consider tweaking
constexpr uint MAX_HEARTBEAT_TOKENS = TOKENS_PER_HEARTBEAT*1;
constinit thread_local volatile uint heartbeat_tokens = 0;
constinit thread_local volatile bool disable_heartbeats = false;

void start_heartbeats() noexcept;
void pause_heartbeats() noexcept;

// Copied from `parlaylib/include/parlay/internal/work_stealing_job.h`
struct WorkStealingJob {
  using scheduler_t = parlay::scheduler<WorkStealingJob>;
  static scheduler_t& get_current_scheduler() {
    scheduler_t* current_scheduler = scheduler_t::get_current_scheduler();
    if (current_scheduler == nullptr) {
      static thread_local scheduler_t local_scheduler(parlay::internal::init_num_workers());
      return local_scheduler;
    }
    return *current_scheduler;
  }
  static uint worker_id() {
    return get_current_scheduler().worker_id();
  }
  static uint num_workers() {
    return get_current_scheduler().num_workers();
  }

  WorkStealingJob() {}

  WorkStealingJob(WorkStealingJob&& other) : done(false), hbt(other.hbt) {}
  // virtual ~WorkStealingJob() = default;
  
  void operator()() {
    assert(done.load(std::memory_order_relaxed) == false);
    heartbeat_tokens = hbt;
    start_heartbeats();
    run();
    pause_heartbeats();
    done.store(true, std::memory_order_release);
  }
  [[nodiscard]] bool finished() const noexcept {
    return done.load(std::memory_order_acquire);
  }
  void wait() const noexcept {
    // while (!finished()) std::this_thread::yield();
    auto done = [&] () { return finished(); };
    get_current_scheduler().wait_until(done);
    //assert(finished());
  }

  void enqueue() {
    // TODO: make sure queue doesn't overflow (max 1000, aborts if hit)
    get_current_scheduler().spawn(this);
  }

  bool try_dequeue() {
    return get_current_scheduler().get_own_job() != nullptr;
  }

  void fast_clone() {
    heartbeat_tokens = heartbeat_tokens + hbt;
    run();
  }

  void sync() {
    if (try_dequeue()) { // unstolen
      fast_clone();
    } else { // stolen
      // since we ALWAYS promote innermost-first,
      // all potential parallelism is fully promoted by now
      // thus, no reason to get heartbeats while waiting
      if (!finished()) {
        pause_heartbeats();
        wait();
        start_heartbeats();
      }
    }
  }

  // pretends a volatile WorkStealingJob is nonvolatile
  __attribute__((always_inline))
  WorkStealingJob& pretend_nonvolatile() volatile noexcept {
    return (WorkStealingJob&) *this;
  }
  
  void consume_these_hbt() noexcept {
    heartbeat_tokens = heartbeat_tokens - hbt;
  }

 //protected:
  virtual void run() = 0;
  std::atomic<bool> done;
  uint hbt; // heartbeat tokens
};

typedef uint_fast8_t spork_slot_idx_t;

struct spork_entry_t {
  volatile bool* promotable_flag;
  void* prom;
  bool (*exec_prom)(void* prom);
  
  spork_entry_t offset(unw_word_t bp) const noexcept {
    return {
      (volatile bool*) (bp - (unw_word_t) promotable_flag),
      (void*) (bp - (unw_word_t) prom),
      exec_prom
    };
  }
};

struct spork_row_t {
  // number of sporks this code location is nested inside
  const spork_slot_idx_t num_sporks;
  // num_sporks-length array of (promoted, spwn) offsets
  const spork_entry_t* sporks;
};

// TODO: make sure prom doesn't throw any exceptions
void do_promotion(const spork_entry_t& slot) noexcept {
  heartbeat_tokens = heartbeat_tokens - 1;
  // run slot's prom function,
  // then update if this spork is promotable any further
  *slot.promotable_flag = slot.exec_prom(slot.prom);
}

#if PROM_USE_LIBUNWIND
constinit volatile bool* ad_hoc_promotable_flag = nullptr;
constinit void* ad_hoc_prom;
constinit bool (*ad_hoc_exec_prom)(void*);
constinit void* ad_hoc_spork_ip_min = nullptr;
constinit void* ad_hoc_spork_ip_max = nullptr;

extern void __RECORD_SPORK
  (void* promotable_flag,
   void* prom,
   void* exec_prom,
   void* beg,
   void* end) noexcept {};

constinit spork_entry_t colin_default = {};
constinit spork_row_t colin_default_row = {1, &colin_default};
void set_colin_default() {
  colin_default = {
    ad_hoc_promotable_flag,
    ad_hoc_prom,
    ad_hoc_exec_prom
  };
}
spork_row_t* spork_table_lookup_ip(unw_word_t ip) {
  void* _ip = (void*) ip;
  if (ad_hoc_spork_ip_min <= _ip && _ip <= ad_hoc_spork_ip_max) return &colin_default_row;
  else return nullptr;
}
void promote_h(unw_cursor_t& cursor, unw_context_t& uc) noexcept {
  if (heartbeat_tokens == 0) return;
  unw_word_t frame_ip, frame_sp, frame_bp;
  const spork_row_t* row;

  // find first stack frame with sporks
  while (unw_step(&cursor) > 0) {
    // get stored register values for this stack frame
    unw_get_reg(&cursor, UNW_REG_SP, &frame_sp);
    unw_get_reg(&cursor, UNW_REG_IP, &frame_ip);
    unw_get_reg(&cursor, UNW_X86_64_RBP, &frame_bp);

    row = spork_table_lookup_ip(frame_ip);
    if ((row != nullptr) && (row->num_sporks > 0)) break;
  }

  if (row == nullptr) return;

  bool frame_has_promoted_slots = false;

  // inspect each spork slot
  for (spork_slot_idx_t slot_idx = 0;
       slot_idx < row->num_sporks && heartbeat_tokens > 0;
       slot_idx++) {
    spork_entry_t slot = row->sporks[slot_idx].offset(frame_bp);
  
    frame_has_promoted_slots |= !*slot.promotable_flag;
    if (*slot.promotable_flag) {
      // this slot is not yet promoted
  
      // try promoting above us first,
      // unless we already passed a promoted slot in this for loop
      if (!frame_has_promoted_slots) {
        promote_h(cursor, uc);
      }
  
      if (heartbeat_tokens > 0) {
        do { do_promotion(slot); }
        while (heartbeat_tokens > 0 && *slot.promotable_flag);
        frame_has_promoted_slots = true;
      }
    }
  }
}

__attribute__((noinline))
void promote() noexcept {
  set_colin_default();
  unw_cursor_t cursor;
  unw_context_t uc;
  unw_getcontext(&uc);
  unw_init_local(&cursor, &uc);
  promote_h(cursor, uc);
}
#else
struct PromFn {
  virtual bool operator()() = 0;
};

struct SporkSlot {
  // TODO: consider making `prom_flag` not a pointer,
  // storing `spork`'s `volatile bool promotable_flag` inside this struct
  // TODO: otherwise, we can probably view this as nonvolatile from this struct
  volatile bool* const prom_flag;
  const PromFn* const promfn;
  SporkSlot* volatile next;
  SporkSlot* volatile prev;

  explicit SporkSlot(volatile bool* _prom_flag, const PromFn* _promfn);
  explicit SporkSlot(volatile bool* _prom_flag, const PromFn* _promfn,
                     SporkSlot* _next, SporkSlot* _prev) :
    prom_flag(_prom_flag),
    promfn(_promfn),
    next(_next),
    prev(_prev) {}
  explicit SporkSlot(const SporkSlot& other) :
    prom_flag(other.prom_flag),
    promfn(other.promfn),
    next(other.next),
    prev(other.prev) {}
  explicit SporkSlot(SporkSlot&& other) :
    prom_flag(std::forward<volatile bool* const>(other.prom_flag)),
    promfn(std::forward<const PromFn* const>(other.promfn)),
    next(std::forward<SporkSlot* volatile>(other.next)),
    prev(std::forward<SporkSlot* volatile>(other.prev)) {};
  // sentinel spork slot for `spork_stack_top`
  consteval explicit SporkSlot() :
    prom_flag(nullptr), promfn(nullptr), next(nullptr), prev(nullptr) {}

  // NOTE: `this` *must* be the slot at `spork_stack_bot`
  ~SporkSlot();

  // TODO: casting from const to non-const might cause issues?
  // not sure bc theoretically it shouldn't change the promfn
  void promote() { *prom_flag = (*((PromFn*) promfn))(); }
  bool promotable() volatile { return *prom_flag; }
  static void promote_top();
};

constinit thread_local SporkSlot spork_stack_top;
constinit thread_local SporkSlot* volatile spork_stack_bot;

// The constructor and destructor for `SporkSlot` may
// change `spork_stack_bot`, but the signal handler may not.
// If we try to change `spork_stack_bot` in the signal handler,
// it can cause issues because it is a (nonatomic) thread_local variable,
// and therefore a write may not be compiled to a single instruction.
SporkSlot::SporkSlot(volatile bool* _promotable, const PromFn* _promfn)
  : prom_flag(_promotable), promfn(_promfn), prev(spork_stack_bot) {
  prev->next = this;
  // now commit these changes to the spork stack, allowing promotions
  spork_stack_bot = this;
}

// NOTE: `this` *must* be the slot at `spork_stack_bot`
SporkSlot::~SporkSlot() {
  // TODO idea: just disable heartbeats for this and also constructor above,
  // then perhaps we can remove promoted slots from the spork stack?
  spork_stack_bot = prev;
}

void SporkSlot::promote_top() {
  if (&spork_stack_top == spork_stack_bot) return;
  SporkSlot* slot = spork_stack_top.next;
  while (heartbeat_tokens) {
    if (slot->promotable()) {
      slot->promote();
      // if this slot can no longer be promoted,
      // we can skip it next time we search
      // TODO: fix this below
      // if (!(*(slot->entry->promotable_flag))) {
      //   if (slot != spork_stack_bot) {
      //     slot = slot->next;
      //     spork_stack_top.next = slot;
      //   } else {
      //     slot->prev = &spork_stack_top;
      //   }
      // }
    } else if (slot == spork_stack_bot) {
      //slot->prev = &spork_stack_top;
      break;
    } else {
      slot = slot->next;
      //spork_stack_top.next = slot;
    }
  }
}
#endif


// Repeatedly tries to consume 1 heartbeat token until failure,
// returning number of times this was successful
// __attribute__((always_inline))
// void try_consume_tokens() noexcept {
//   if (heartbeat_tokens) [[unlikely]] {
//     //promotes_until_failure();
//     manual_heartbeat = true;
//     std::raise(SIGALRM);
//   }
// }


#if RECORD_HEARTBEAT_STATS
volatile uint* num_heartbeats = nullptr;
volatile uint* missed_heartbeats = nullptr;

void init_heartbeat_stats() {
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    uint nw = 8; //internal::init_num_workers();
    num_heartbeats = new uint[nw];
    missed_heartbeats = new uint[nw];
    for (uint wi = 0; wi < nw; wi++) {
      num_heartbeats[wi] = 0;
      missed_heartbeats[wi] = 0;
    }
  }
}

void reset_heartbeat_stats() {
  uint nw = SpwnJob::num_workers();
  for (uint wi = 0; wi < nw; wi++) {
    num_heartbeats[wi] = 0;
    missed_heartbeats[wi] = 0;
  }
}
#endif

// sa.sa_sigaction = heartbeat_handler;
// sa.sa_flags = SA_SIGINFO
// void heartbeat_handler(int sig, siginfo_t* info, void* ucontext) {
//   ucontext_t* ctx = (ucontext_t*) ucontext;
void heartbeat_handler(int sig) {
  int saved_errno = errno;
  if (!disable_heartbeats) {
#if RECORD_HEARTBEAT_STATS
    num_heartbeats[spork::SpwnJob::worker_id()]++;
#endif
    heartbeat_tokens = heartbeat_tokens + TOKENS_PER_HEARTBEAT;
    if (heartbeat_tokens > MAX_HEARTBEAT_TOKENS) {
      heartbeat_tokens = MAX_HEARTBEAT_TOKENS;
    }
#if PROM_USE_LIBUNWIND
    promote();
#else
    SporkSlot::promote_top();
#endif
  } else {
#if RECORD_HEARTBEAT_STATS
    missed_heartbeats[spork::SpwnJob::worker_id()]++;
#endif
  }
  errno = saved_errno;
}

constinit thread_local timer_t heartbeat_timer;
constinit itimerspec heartbeat_its_zero = {};

consteval itimerspec init_heartbeat_its() {
  itimerspec its = {};
  its.it_value   .tv_nsec = HEARTBEAT_INTERVAL_US * 1000;
  its.it_interval.tv_nsec = HEARTBEAT_INTERVAL_US * 1000;
  return its;
}
constinit itimerspec heartbeat_its = init_heartbeat_its();

void start_heartbeats() noexcept {
  constinit static thread_local bool thread_initialized = false;
  if (!thread_initialized) { // only first time
    thread_initialized = true;
    //heartbeat_tokens = 0;
    //disable_heartbeats = false;
    
#if !PROM_USE_LIBUNWIND
    spork_stack_bot = &spork_stack_top;
#endif

    struct sigaction sa = {};
    //sa.sa_sigaction = heartbeat_handler;
    //sa.sa_flags = SA_SIGINFO;// | SA_RESTART;
    sa.sa_handler = heartbeat_handler;
    
    //sigemptyset(&sa.sa_mask); // don't block extra signals during handler
    //sigaddset(&sa.sa_mask, SIGALRM); // block SIGALRM while in handler
    
    sigaction(SIGALRM, &sa, nullptr);
  
    struct sigevent sev{};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo  = SIGALRM;
    sev._sigev_un._tid = gettid();
    
    timer_create(CLOCK_MONOTONIC, &sev, &heartbeat_timer);
  }
  
  // resume timer
  timer_settime(heartbeat_timer, 0, &heartbeat_its, nullptr);
}

void pause_heartbeats() noexcept {
  timer_settime(heartbeat_timer, 0, &heartbeat_its_zero, nullptr);
}

// TODO: need to do this for every thread
// int stop_heartbeats() noexcept {
//   int r = timer_delete(heartbeat_timer);
//   heartbeats_running = false;
//   return r;
// }

// x must be a pointer to something invocable
// which returns a Result
template <typename _Ret, typename _Fn, typename... _Args>
_Ret execute_lambda(void* f, _Args... args) {
  static_assert(std::is_invocable_r_v<_Ret, _Fn&, _Args...>);
  _Fn* _f = (_Fn*) f;
  return (*_f)(args...);
}

template <typename _Fn, typename... _Args>
void execute_lambda2(void* f, _Args... args) {
  static_assert(std::is_invocable_r_v<void, _Fn&, _Args...>);
  _Fn* _f = (_Fn*) f;
  (*_f)(args...);
}

#ifndef FRAME_OFFSET
#define FRAME_OFFSET(x) ((uintptr_t) __builtin_frame_address(0) - (uintptr_t) (x))
#endif

template <typename PromLambda>
static void manualProm(PromLambda&& prom, volatile bool& promotable_flag) {
  // save
  bool before = disable_heartbeats;
  disable_heartbeats = true;
  // now check again to make sure a signal didn't eat all the tokens
  while (heartbeat_tokens && promotable_flag) {
    heartbeat_tokens = heartbeat_tokens - 1;
    promotable_flag = std::forward<PromLambda>(prom)();
    //spork_stack_top.next = spork_stack_bot;
  }
  // restore
  disable_heartbeats = before;
}

// // TODO: exception handling
// template <typename BodyLambda, typename PromLambda>
// //__attribute__((always_inline))
// static void spork(volatile bool& promotable_flag, BodyLambda&& body, PromLambda&& prom) {
//   static_assert(std::is_invocable_v<BodyLambda&&>);
//   static_assert(std::is_invocable_r_v<bool, PromLambda&&>);

// #if PROM_USE_LIBUNWIND
//   if (ad_hoc_promotable_flag == nullptr) {
//     ad_hoc_promotable_flag = (volatile bool*) FRAME_OFFSET(&promotable_flag);
//     ad_hoc_prom = (void*) FRAME_OFFSET(&prom);
//     ad_hoc_exec_prom = &execute_lambda<bool, PromLambda>;
//     ad_hoc_spork_ip_min = &&begin_body;
//     ad_hoc_spork_ip_max = &&end_body;
//   }
//   begin_body:
//   {
//     // __attribute__((annotate("spork_range", (void*) execute_lambda<bool, PromLambda>)))
//     //   void* thisspork[3] = {(void*) &promotable_flag,
//     //                         (void*) &prom};
//     if (heartbeat_tokens) [[unlikely]] {
//       manualProm(prom, promotable_flag);
//     }
//     std::forward<BodyLambda>(body)();
//   }
//   end_body:
//   // __RECORD_SPORK(ad_hoc_promotable_flag, (void*) &prom, ad_hoc_exec_prom);
//   // TODO: check if using these labels is too brittle;
//   // could some optimization passes move blocks into/out of the begin-end range?
//   __RECORD_SPORK((void*) &promotable_flag,
//                  (void*) &prom,
//                  (void*) &execute_lambda<bool, PromLambda>,
//                  &&begin_body,
//                  &&end_body);
// #else
//   spork_entry_t en =
//     {&promotable_flag, &prom, execute_lambda<bool, PromLambda>};
//   {
//     SporkEntry sporke(&en);
//     if (heartbeat_tokens) [[unlikely]] {
//       manualProm(prom, promotable_flag);
//     }
//     std::forward<BodyLambda>(body)();
//   }
//   (void) en; // TODO: can we remove this line?
// #endif
//   return;
// }

// TODO: exception handling
template <typename BodyLambda, typename PromLambda>
//__attribute__((always_inline))
void spork(volatile bool& promotable_flag, const BodyLambda&& body, const PromLambda&& prom) {
  static_assert(std::is_invocable_v<BodyLambda&&>);
  static_assert(std::is_invocable_r_v<bool, PromLambda&&>);

  struct PromSpork : PromFn {
    const PromLambda&& prom;
    PromSpork(const PromLambda&& _prom) :
      prom(std::forward<const PromLambda>(_prom)) {}
    bool operator()() override {
      return std::forward<const PromLambda>(prom)();
    }
  };
  const PromSpork promfn(std::forward<const PromLambda>(prom));

  {
    SporkSlot slot(&promotable_flag, &promfn);
    if (heartbeat_tokens) [[unlikely]] {
      manualProm(prom, promotable_flag);
    }
    std::forward<const BodyLambda>(body)();
  }
}

template <typename LambdaL, typename LambdaR>
__attribute__((always_inline))
static void parSeq(LambdaL&& lamL, LambdaR&& lamR) {
  static_assert(std::is_invocable_v<LambdaL&>);
  static_assert(std::is_invocable_v<LambdaR&>);
  std::forward<LambdaL>(lamL)();
  std::forward<LambdaR>(lamR)();
}

template <typename LambdaL, typename LambdaR>
//__attribute__((always_inline))
void par(const LambdaL&& lamL, const LambdaR&& lamR) {
  static_assert(std::is_invocable_v<const LambdaL&>);
  static_assert(std::is_invocable_v<const LambdaR&>);
  
  struct SpwnJob : WorkStealingJob {
    const LambdaR&& lamR;
    void run() override {
      std::forward<const LambdaR>(lamR)();
    }
    
    SpwnJob(const LambdaR&& lamR) : WorkStealingJob(), lamR(std::forward<const LambdaR>(lamR)) {}
    SpwnJob(SpwnJob&& other) : WorkStealingJob(std::forward<SpwnJob>(other)), lamR(std::forward<const LambdaR>(other.lamR)) {}
  };
  
  volatile bool promotable = true;
  volatile SpwnJob jp(std::forward<const LambdaR>(lamR));

  spork(
    promotable,
    std::forward<const LambdaL>(lamL),
    [&] () {
      SpwnJob& jpnv = *((SpwnJob*) &jp);
      jpnv.done.store(false, std::memory_order_relaxed);
      jpnv.hbt = heartbeat_tokens >> 1;
      jpnv.consume_these_hbt();
      jpnv.enqueue();
      return false; // can do no more promotions here
    });

  if (promotable) [[likely]] { // unpromoted
    std::forward<const LambdaR>(jp.lamR)();
  } else [[unlikely]] { // promoted
    SpwnJob& jpnv = *((SpwnJob*) &jp);
    jpnv.sync();
  }
}

__attribute__((always_inline))
constexpr static const uint midpoint(uint i, uint j) noexcept {
  return i + ((j - i) >> 1);
}

// void waste_some_time() {
//   for (uint i = 0; i < 1000000; i++) {
//     if (i % 12345 == 678901241) [[likely]] {
//       // should never happen
//       std::cout << "waste_some_time i = " << i << std::endl;
//     }
//   }
// }

// int setup_perf_interrupt(long long period_cycles) {
//   struct perf_event_attr pe{};
//   pe.type           = PERF_TYPE_HARDWARE;
//   pe.config         = PERF_COUNT_HW_CPU_CYCLES;
//   pe.sample_period  = period_cycles;
//   pe.watermark      = 0;
  
//   // pid=0 targets the calling thread, cpu=-1 = any CPU
//   int fd = syscall(SYS_perf_event_open, &pe, 0, -1, -1, 0);
  
//   // Route overflow to SIGIO on this thread
//   fcntl(fd, F_SETFL, O_ASYNC);
//   fcntl(fd, F_SETOWN, gettid());
  
//   ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
//   return fd;
// }

template <typename BodyLambda, typename BinaryOp>
__attribute__((always_inline)) // TODO: investigate what this attribute actually does
parlay::monoid_value_type_t<BinaryOp> parforSeq(const BinaryOp&& binop, const BodyLambda&& body, uint i, uint j) {
  using A = parlay::monoid_value_type_t<BinaryOp>;
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint, A&>);
  static_assert(parlay::is_monoid_v<BinaryOp>);

  A a = binop.identity;
  for (; i < j; i++)
    std::forward<const BodyLambda>(body)(i, a);
  return a;
}

template <typename BodyLambda, typename BinaryOp>
__attribute__((always_inline)) // TODO: investigate what this attribute actually does
parlay::monoid_value_type_t<BinaryOp> parfor(const BinaryOp&& binop, const BodyLambda&& body, uint i, uint _j) {
  using A = parlay::monoid_value_type_t<BinaryOp>;
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint, A&>);
  static_assert(parlay::is_monoid_v<BinaryOp>);

  struct SpwnJob : WorkStealingJob {
    uint i, j;
    A a;
    const BinaryOp&& binop;
    const BodyLambda&& body;
    void run() override {
      a = parfor<BodyLambda, BinaryOp>(std::forward<const BinaryOp>(binop), std::forward<const BodyLambda>(body), i, j);
    }
    SpwnJob(const BinaryOp&& _binop, const BodyLambda&& _body) : WorkStealingJob(), binop(std::forward<const BinaryOp>(_binop)), body(std::forward<const BodyLambda>(_body)) {}
  };

  SpwnJob l(std::forward<const BinaryOp>(binop), std::forward<const BodyLambda>(body));
  SpwnJob r(std::forward<const BinaryOp>(binop), std::forward<const BodyLambda>(body));
  volatile bool promotable = true;
  volatile uint j = _j;
  A a = binop.identity;

  spork(
    promotable,
    [&] () {
      for (; i < j; i++) {
        std::forward<const BodyLambda>(body)(i, a);
      }
    },
    [&] () {
      uint _i = i + 1;
      uint _j = j;
      if (_i >= _j) { r.i = r.j = 0; l.i = l.j = 0; return false; }
      uint mid = midpoint(_i, _j);
      j = _i;

      r.done.store(false, std::memory_order_relaxed);
      r.hbt = (heartbeat_tokens + 1) >> 1;
      r.i = mid;
      r.j = _j;
      r.consume_these_hbt();
      r.enqueue();

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = l.j = 0; return false; }
      l.done.store(false, std::memory_order_relaxed);
      l.hbt = heartbeat_tokens;
      l.i = _i;
      l.j = mid;
      l.consume_these_hbt();
      l.enqueue();
      return false;
    });
  if (!promotable) [[unlikely]] {
    if (l.i < l.j) [[likely]] {
      l.sync();
      a = binop(a, l.a);
    }
    if (r.i < r.j) [[likely]] {
      r.sync();
      a = binop(a, r.a);
    }
  }
  return a;
}

template <typename BodyLambda>
__attribute__((always_inline)) // TODO: investigate what this attribute actually does
void parfor(const BodyLambda&& body, uint i, uint _j) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint>);

  struct SpwnJob : WorkStealingJob {
    uint i, j;
    const BodyLambda&& body;
    void run() override {
      parfor<BodyLambda>(std::forward<const BodyLambda>(body), i, j);
    }
    SpwnJob(const BodyLambda&& _body) :
      WorkStealingJob(), body(std::forward<const BodyLambda>(_body)) {}
  };

  SpwnJob l(std::forward<const BodyLambda>(body));
  SpwnJob r(std::forward<const BodyLambda>(body));
  volatile bool promotable = true;
  volatile uint j = _j;

  spork(
    promotable,
    [&] () {
      for (; i < j; i++) {
        std::forward<const BodyLambda>(body)(i);
      }
    },
    [&] () {
      uint _i = i + 1;
      uint _j = j;
      if (_i >= _j) { r.i = r.j = 0; l.i = l.j = 0; return false; }
      uint mid = midpoint(_i, _j);
      j = _i;

      r.done.store(false, std::memory_order_relaxed);
      r.hbt = (heartbeat_tokens + 1) >> 1;
      r.i = mid;
      r.j = _j;
      r.consume_these_hbt();
      r.enqueue();

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = l.j = 0; return false; }
      l.done.store(false, std::memory_order_relaxed);
      l.hbt = heartbeat_tokens;
      l.i = _i;
      l.j = mid;
      l.consume_these_hbt();
      l.enqueue();
      return false;
    });
  if (!promotable) [[unlikely]] {
    if (l.i < l.j) [[likely]] l.sync();
    if (r.i < r.j) [[likely]] r.sync();
  }
}

uint fibSeq(uint n) {
  if (n <= 1) {
    return n;
  } else {
    uint l, r;
    parSeq([&, n] () {l = fibSeq(n - 1);},
           [&, n] () {r = fibSeq(n - 2);});
    return l + r;
  }
}

uint fib(uint n) {
  if (n <= 1) {
    return n;
  } else {
    uint l, r;
    par([&, n] () {l = fib(n - 1);},
        [&, n] () {r = fib(n - 2);});
    return l + r;
  }
}
} // namespace spork

void print_uint_arr(const uint* arr, uint len) {
  std::cout << "[";
  if (arr && len) {
    std::cout << arr[0];
    for (uint i = 1; i < len; i++) std::cout << ", " << arr[i];
  }
  std::cout << "]";
}

template <typename F>
__attribute__((always_inline))
void p4(size_t s, size_t e, F&& f) {
  //parlay::parallel_for(s, e, std::forward<F>(f), 1);
  spork::parfor(std::forward<F>(f), s, e);
}

// template<typename A, typename Body, typename Combine>
// auto parlayreduce(A z, const Body&& body, const Combine&& binop, uint i, uint j) {
//   static_assert(std::is_invocable_r_v<void, Body&, uint, A&>);
//   static_assert(std::is_invocable_r_v<void, Combine&, A&, A>);
//   long n = j - i;
//   long block_size = 100;
//   if (n == 0) return z;
//   if (n <= block_size) {
//     A a = z;
//     for (; i < j; i++)
//       a = std::forward<Body>(body)(a, i);
//     return a;
//   }

//   A L, R;
//   parlay::par_do([&] {L = reduce(z, body, binop, i, i + ((j - i) >> 1));},
//                  [&] {R = reduce(z, body, binop, i + ((j - i) >> 1), j);});
//   std::forward<Combine>(binop)(L,R);
//   return L;
// }


int main(int argc, char* argv[]) {
  // this might take a sec the first time it is called
  //spork::SpwnJob::get_current_scheduler();

  //size_t n = atoi(argv[1]);
  constexpr uint n = 8000000;
  char data[n];
  for (uint i = 0; i < n; i++) {
    data[i] = 1 + (i % 5);
  }

  spork::WorkStealingJob::get_current_scheduler();
#if RECORD_HEARTBEAT_STATS
  spork::init_heartbeat_stats();
#endif
  
  using num = unsigned long long;

  // num total = 0;
  // volatile uint j = n*50;
  // for (uint i = 0; i < j; i++) {
  //   total += data[i % n];
  // }
  // std::cout << total << std::endl;
  
  auto total_time = 0;
  constexpr uint WARMUP = 10;
  constexpr uint NUM_TRIALS = 30;

  for (uint r = 0; r < WARMUP + NUM_TRIALS; r++) {
#if RECORD_HEARTBEAT_STATS
    spork::reset_heartbeat_stats();
#endif
    spork::start_heartbeats();
    
    auto start = std::chrono::steady_clock::now();
    //parlay::parallel_for(0, n, [&] (uint i) { irregular_body(data, i, n); });
    // p4(0, n, [&] (uint i) {
    //   volatile char x = 0;
    //   if (i < 10) {
    //     p4(0, 10000000, [&] (uint j) { x = x + i*j; });
    //   }
    //   data[i] = x;
    // });
    // parlay::parallel_for(0, n*50, [&] (uint i) { data[i % n] = 5; });
    // spork::parfor([&] (uint i) { data[i % n] = 5; }, 0, n*50);
    num total =
      // spork::parfor(
      //   parlay::plus<num>(),
      //   [&] (uint i, num& a) { a += data[i % n]; },
      //   0, n*50);
      spork::fib(40);
    auto end = std::chrono::steady_clock::now();

    spork::pause_heartbeats();
    
    auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << /*(uint) data[n - 1]*/ total << " in " << time_ms << " ms";
#if RECORD_HEARTBEAT_STATS
    std::cout <" (";
    print_uint_arr((uint*) spork::num_heartbeats, spork::SpwnJob::num_workers());
    std::cout << " heartbeats, ";
    print_uint_arr((uint*) spork::missed_heartbeats, spork::SpwnJob::num_workers());
    std::cout << " missed)";
#endif
    std::cout << std::endl;
    if (r >= WARMUP) total_time += time_ms;
  }
  std::cout << "Average " << (total_time / NUM_TRIALS) << " ms" << std::endl;

  spork::pause_heartbeats();
}
