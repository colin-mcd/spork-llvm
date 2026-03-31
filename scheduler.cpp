//#include "parlay/internal/work_stealing_deque.h"
//#include "parlay/internal/work_stealing_job.h"
#include "parlay/parallel.h"
//#include "parlay/scheduler.h"
#include "parlay/alloc.h"

#include <libunwind.h>
#include <type_traits>
#include <utility>

//#include <atomic>
//#include <atomic>
//#include <bits/types/time_t.h>
//#include <cstddef>
//#include <cstdint>
//#include <cstdlib>
//#include <csignal>
//#include <ostream>
//#include <ratio>
//#include <signal.h>
//#include <sys/types.h>
//#include <thread>
//#include <time.h>
//#include <iostream>
//#include <utility>

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

struct SpwnJob : parlay::WorkStealingJob {

  using scheduler_t = parlay::scheduler<SpwnJob>;

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

  private:
  void* (*exec_spwn)(void* spwn, void* data);
  void* spwn;
  void* data;
  uint num_heartbeat_tokens;
  //scheduler_t* sched;
  
  public:
  void* result;
  SpwnJob* next;

  explicit SpwnJob() {}

  explicit SpwnJob(void* (*_exec_spwn)(void* spwn, void* data),
                   void* _spwn,
                   void* _data,
                   uint _hbt = 0,
                   SpwnJob* _next = nullptr, 
                   // scheduler_t* _sched = nullptr,
                   void* _result = nullptr) :
      WorkStealingJob(),
      exec_spwn(_exec_spwn),
      spwn(_spwn),
      data(_data),
      num_heartbeat_tokens(_hbt),
      // sched(_sched),
      result(_result),
      next(_next) {
    // if (sched == nullptr) {
    //   sched = &get_current_scheduler();
    // }
    heartbeat_tokens = heartbeat_tokens - _hbt;
  }

  SpwnJob& operator=(const SpwnJob& other) = delete;
  SpwnJob& operator=(SpwnJob&& other) noexcept {
    if (this != &other) {
      exec_spwn = std::exchange(other.exec_spwn, nullptr);
      spwn = std::exchange(other.spwn, nullptr);
      data = std::exchange(other.data, nullptr);
      num_heartbeat_tokens = std::exchange(other.num_heartbeat_tokens, 0);
      // sched = std::exchange(other.sched, nullptr);
      result = std::exchange(other.result, nullptr);
      next = std::exchange(other.next, nullptr);
      // exec_spwn = other.exec_spwn;
      // spwn = other.spwn;
      // data = other.data;
      // num_heartbeat_tokens = other.num_heartbeat_tokens;
      // sched = other.sched;
      // result = other.result;
      // next = other.next;
    }
    return *this;
  }

  // pretends a volatile SpwnJob is nonvolatile
  __attribute__((always_inline))
  SpwnJob* pretend_nonvolatile() volatile {
    return (SpwnJob*) this;
  }

  using JobAllocator = parlay::type_allocator<SpwnJob>;

  template<typename... Args>
  static SpwnJob* create(Args... args) {
    SpwnJob* jp = JobAllocator::create(args...);
    jp->enqueue();
    return jp;
  }

  static void destroy(SpwnJob* job) {
    JobAllocator::destroy(job);
  }

  SpwnJob(SpwnJob&& other) :
    SpwnJob(std::exchange(other.exec_spwn, nullptr),
            std::exchange(other.spwn, nullptr),
            std::exchange(other.data, nullptr),
            std::exchange(other.num_heartbeat_tokens, 0),
            std::exchange(other.next, nullptr),
            // std::exchange(other.sched, nullptr),
            std::exchange(other.result, nullptr)) {}

  void enqueue() {
    // TODO: make sure queue doesn't overflow (max 1000, aborts if hit)
    //sched->spawn(this);
    get_current_scheduler().spawn(this);
  }

  bool try_dequeue() {
    //return sched->get_own_job() != nullptr;
    return get_current_scheduler().get_own_job() != nullptr;
  }

  // __attribute__((noinline))
  void execute() override {
    heartbeat_tokens = num_heartbeat_tokens;
    start_heartbeats();
    result = exec_spwn(spwn, data);
    pause_heartbeats();
  }

  void execute_fast_clone() {
    heartbeat_tokens = heartbeat_tokens + num_heartbeat_tokens;
    result = exec_spwn(spwn, data);
  }

  void wait_or_execute() {
    if (try_dequeue()) { // unstolen
      execute_fast_clone();
    } else { // stolen
      // since we ALWAYS promote innermost-first,
      // all potential parallelism is fully promoted by now
      // thus, no reason to get heartbeats while waiting
      pause_heartbeats();
      wait();
      start_heartbeats();
    }
  }

  //__attribute__((always_inline))
  void sync() noexcept {
    SpwnJob* jp = this;
    while (jp != nullptr) {
      jp->wait_or_execute();
      SpwnJob* next = jp->next;
      destroy(jp);
      jp = next;
    }
  }

  template<typename CombLambda>
  //__attribute__((always_inline))
  void sync(CombLambda&& combine) {
    static_assert(std::is_invocable_r_v<void, CombLambda&, void*>);
    SpwnJob* jp = this;
    while (jp != nullptr) {
      jp->wait_or_execute();
      SpwnJob* old = jp;
      std::forward<CombLambda>(combine)(jp->result);
      jp = jp->next;
      destroy(old);
    }
  }
};

typedef uint_fast8_t spork_slot_idx_t;

typedef SpwnJob* volatile VolSpwnJob;

struct spork_entry_t {
  volatile bool* promotable_flag;
  volatile uint* num_promotions;
  void* prom;
  bool (*exec_prom)(void* prom);
  
  spork_entry_t offset(unw_word_t bp) const noexcept {
    return {
      (volatile bool*) (bp - (unw_word_t) promotable_flag),
      (volatile uint*) (bp - (unw_word_t) num_promotions),
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
  *slot.num_promotions = *slot.num_promotions + 1;
  // run slot's prom function,
  // then update if this spork is promotable any further
  *slot.promotable_flag = slot.exec_prom(slot.prom);
}

#if PROM_USE_LIBUNWIND
volatile bool* ad_hoc_promotable_flag = nullptr;
volatile uint* ad_hoc_num_promotions = nullptr;
void* ad_hoc_prom;
bool (*ad_hoc_exec_prom)(void*);
void* ad_hoc_spork_ip_min = nullptr;
void* ad_hoc_spork_ip_max = nullptr;

static void __RTS_record_spork
  (volatile bool* promotable_flag,
   volatile uint* num_promotions,
   void* prom,
   bool (*exec_prom)(void*)) noexcept {}

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
  
    frame_has_promoted_slots |= *slot.num_promotions > 0;
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
struct SporkEntry {
  // WARNING: `next` may be a dangling pointer
  SporkEntry* volatile next;
  SporkEntry* volatile prev;
  spork_entry_t* const entry;

  explicit SporkEntry(SporkEntry* next, SporkEntry* prev, spork_entry_t* entry) :
    next(next), prev(prev), entry(entry) {}
  
  explicit SporkEntry(spork_entry_t* entry);

  explicit SporkEntry(const SporkEntry& other) :
    next(other.next), prev(other.prev), entry(other.entry) {}

  explicit SporkEntry() : next(nullptr), prev(nullptr), entry(nullptr) {}

  // SporkEntry& operator=(SporkEntry&& other) {
  //   if (this != &other) {
  //     next = std::exchange(other.next, nullptr);
  //     prev = std::exchange(other.prev, nullptr);
  //     entry = std::exchange(other.entry, nullptr);
  //   }
  //   return *this;
  // }
  
  // NOTE: `this` *must* be the entry at `spork_stack_bot`
  ~SporkEntry();

  // void close();

  void do_promotion();
  static void promote(); // TODO: noexcept?
};

thread_local SporkEntry spork_stack_top;
thread_local SporkEntry* volatile spork_stack_bot; // initialized to spork_stack_top

// The constructor and destructor for `SporkEntry` may
// change `spork_stack_bot`, but the signal handler may not.
// If we try to change `spork_stack_bot` in the signal handler,
// it can cause issues because it is a (nonatomic) thread_local variable,
// and therefore a write may not be compiled to a single instruction.
SporkEntry::SporkEntry(spork_entry_t* entry)
  : prev(spork_stack_bot), entry(entry) {
  prev->next = this;
  // now commit these changes to the spork stack, allowing promotions
  spork_stack_bot = this;
}

// NOTE: `this` *must* be the entry at `spork_stack_bot`
SporkEntry::~SporkEntry() {
  spork_stack_bot = prev;
}

void SporkEntry::promote() {
  if (&spork_stack_top == spork_stack_bot) return;
  SporkEntry* slot = spork_stack_top.next;
  while (heartbeat_tokens) {
    if (*slot->entry->promotable_flag) {
      slot->do_promotion();
      // if this slot can no longer be promoted,
      // we can skip it next time we search
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

void SporkEntry::do_promotion() {
  spork::do_promotion(*this->entry);
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
    std::cout << "Initialized num_heartbeats nw=" << nw << std::endl;
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
    SporkEntry::promote();
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
constinit itimerspec heartbeat_its = {};

void start_heartbeats() noexcept {
  static thread_local bool thread_initialized = false;
  static bool global_initialized = false;
  if (!global_initialized) {
    global_initialized = true;
    //heartbeat_its_zero = {};

    //heartbeat_its = {};
    heartbeat_its.it_value.tv_nsec    = HEARTBEAT_INTERVAL_US*1000;
    heartbeat_its.it_interval.tv_nsec = HEARTBEAT_INTERVAL_US*1000;
  }
  if (!thread_initialized) { // only first time
    thread_initialized = true;
    //heartbeat_tokens = 0;
    //disable_heartbeats = false;
    
#if !PROM_USE_LIBUNWIND
    spork_stack_top.next = nullptr;
    spork_stack_top.prev = nullptr;
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

#ifndef FRAME_OFFSET
#define FRAME_OFFSET(x) ((uintptr_t) __builtin_frame_address(0) - (uintptr_t) (x))
#endif

template <typename PromLambda>
static void manualProm(PromLambda&& prom, volatile bool& promotable_flag, volatile uint& num_promotions) {
  // save
  bool before = disable_heartbeats;
  disable_heartbeats = true;
  // now check again to make sure a signal didn't eat all the tokens
  while (heartbeat_tokens && promotable_flag) {
    num_promotions = num_promotions + 1;
    heartbeat_tokens = heartbeat_tokens - 1;
    promotable_flag = std::forward<PromLambda>(prom)();
    //spork_stack_top.next = spork_stack_bot;
  }
  // restore
  disable_heartbeats = before;
}

// TODO: exception handling
template <typename BodyLambda, typename PromLambda>
__attribute__((always_inline))
static void spork2(volatile bool& promotable_flag, volatile uint& num_promotions,
                   BodyLambda&& body, PromLambda&& prom) {
  static_assert(std::is_invocable_v<BodyLambda&&>);
  static_assert(std::is_invocable_r_v<bool, PromLambda&&>);

#if PROM_USE_LIBUNWIND
  if (ad_hoc_promotable_flag == nullptr) {
    ad_hoc_promotable_flag = (volatile bool*) FRAME_OFFSET(&promotable_flag);
    ad_hoc_num_promotions = (volatile uint*) FRAME_OFFSET(&num_promotions);
    ad_hoc_prom = (void*) FRAME_OFFSET(&prom);
    ad_hoc_exec_prom = &execute_lambda<bool, PromLambda>;
    ad_hoc_spork_ip_min = &&begin_body;
    ad_hoc_spork_ip_max = &&end_body;
  }
  begin_body:
  {
    if (heartbeat_tokens) [[unlikely]] {
      manualProm(prom, promotable_flag, num_promotions);
    }
    std::forward<BodyLambda>(body)();
  }
  end_body:
  __RTS_record_spork(ad_hoc_promotable_flag, ad_hoc_num_promotions, (void*) &prom, ad_hoc_exec_prom);
#else
  spork_entry_t en =
    {&promotable_flag, &num_promotions, &prom, execute_lambda<bool, PromLambda>};
  {
    SporkEntry sporke = SporkEntry(&en);
    if (heartbeat_tokens) [[unlikely]] {
      manualProm(prom, promotable_flag, num_promotions);
    }
    std::forward<BodyLambda>(body)();
  }
  (void) en; // TODO: can we remove this line?
#endif
  return;
}

template <typename BodyLambda, typename PromLambda>
__attribute__((always_inline))
static void spork(BodyLambda&& body, PromLambda&& prom) {
  volatile bool promotable_flag = true;
  volatile uint num_promotions = 0;
  spork2(promotable_flag, num_promotions, std::forward<BodyLambda>(body), std::forward<PromLambda>(prom));
}

template <typename LambdaL, typename LambdaR>
__attribute__((always_inline))
static void par(LambdaL&& lamL, LambdaR&& lamR) {
  static_assert(std::is_invocable_v<LambdaL&>);
  static_assert(std::is_invocable_v<LambdaR&>);
  volatile SpwnJob jp;
  volatile bool promotable = true;
  volatile uint num_promotions = 0;
  // `lamR` wrapper
  auto lamRw = [&] (void* null_data) { std::forward<LambdaR>(lamR)(); return nullptr; };

  spork2(
    promotable,
    num_promotions,
    std::forward<LambdaL>(lamL), // TODO: should this be forwarded?
    [&] () {
      *jp.pretend_nonvolatile() =
        SpwnJob(execute_lambda<void*, decltype(lamRw), void*>,
                &lamRw, nullptr, heartbeat_tokens >> 1);
      jp.pretend_nonvolatile()->enqueue();
      return false; // can do no more promotions here
    });

  if (promotable) [[likely]] { // unpromoted
    lamRw(nullptr);
  } else [[unlikely]] { // promoted
    jp.pretend_nonvolatile()->wait_or_execute();
  }
}

__attribute__((always_inline))
constexpr static const uint midpoint(uint i, uint j) noexcept {
  return i + ((j - i) >> 1);
}

struct ReduceData {
  uint i;
  uint j;
  
  ReduceData(uint i, uint j) : i(i), j(j) {}
  
  using ReduceAllocator = parlay::type_allocator<ReduceData>;
  
  static ReduceData* create(uint i, uint j) {
    return ReduceAllocator::create(i, j);
  }
  static void destroy(ReduceData* data) {
    ReduceAllocator::destroy(data);
  }
};

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

template <typename A, typename BodyLambda, typename CombLambda>
__attribute__((always_inline)) // TODO: investigate what this attribute actually does
A reduceSeq(A z, A a, CombLambda&& combine, BodyLambda&& body, uint i, uint j) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint, A&>);
  static_assert(std::is_invocable_r_v<void, CombLambda&, A&, A>);
  for (; i < j; i++) {
    std::forward<BodyLambda>(body)(i, a);
  }
  return a;
}

// template <typename A>
// struct VolReduceData {
//   volatile SpwnJob jpL, jpR;
//   volatile bool promotable = false;
//   volatile uint num_promotions = 0;
//   volatile uint next_i, next_j;
//   volatile A aL, aR;
  
// };

// TODO: ideally, we just reserve enough space for spwn without initializing it,
// then have the prom lambda write to spwn
template <typename A, typename BodyLambda, typename CombLambda>
__attribute__((always_inline)) // TODO: investigate what this attribute actually does
A reduce(A z, A a, CombLambda&& combine, BodyLambda&& body, uint i, uint _j) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint, A&>);
  static_assert(std::is_invocable_r_v<void, CombLambda&, A&, A>);

  // TODO: consolidate these local vars
  volatile bool promotable = true;
  volatile uint num_promotions = 0;
  volatile SpwnJob jpL, jpR;
  volatile uint next_i, next_j;
  volatile A aL, aR;
  volatile uint j = _j;

  auto spwn =
    [&] (void* isLeft) {
      uint mid = midpoint(next_i, next_j);
      if (isLeft) {
        aL = reduce(z, z, combine, body, next_i, mid);
      } else {
        aR = reduce(z, z, combine, body, mid, next_j);
      }
      return nullptr;
    };
 
  spork2(
    promotable,
    num_promotions,
    [&] () {
      for (; i < j; i++) {
        std::forward<BodyLambda>(body)(i, a);
      }
    },
    [&] () {
      if (i >= j) { num_promotions = num_promotions - 1; return false; }
      next_i = i + 1;
      next_j = j;
      j = next_i;
      *jpR.pretend_nonvolatile() =
        SpwnJob(&execute_lambda<void*, decltype(spwn), void*>,
                &spwn, (void*) false, (heartbeat_tokens + 1) >> 1);
      // TODO: check if work stealing deque is full before enqueueing
      jpR.pretend_nonvolatile()->enqueue();
      if (next_i >= midpoint(next_i, next_j)) { return false; }
      *jpL.pretend_nonvolatile() =
        SpwnJob(&execute_lambda<void*, decltype(spwn), void*>,
                &spwn, (void*) true, heartbeat_tokens);
      jpL.pretend_nonvolatile()->enqueue();
      return false;
    });
  if (num_promotions) [[unlikely]] {
    if (next_i < midpoint(next_i, next_j)) [[likely]] {
      jpL.pretend_nonvolatile()->wait_or_execute();
      std::forward<CombLambda>(combine)(a, aL);
    }
    jpR.pretend_nonvolatile()->wait_or_execute();
    std::forward<CombLambda>(combine)(a, aR);
  }
  //std::cout << i << " " << j << " " << next_i << " " << next_j << std::endl;
  return a;
}

template <typename A, typename BodyLambda, typename CombLambda>
__attribute__((always_inline))
A reduceAlloc(A z, A a, CombLambda&& combine, BodyLambda&& body, uint i, uint _j) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint, A&>);
  static_assert(std::is_invocable_r_v<void, CombLambda&, A&, A>);
  VolSpwnJob jp = nullptr;
  using AAllocator = parlay::type_allocator<A>;

  volatile uint j = _j;

  auto spwn =
    ([&] (void* data) {
      ReduceData* rdata = (ReduceData*) data;
      uint i2 = rdata->i;
      uint j2 = rdata->j;
      ReduceData::destroy(rdata);
      return (void*) AAllocator::create(reduceAlloc(z, z, combine, body, midpoint(i2+1, j2), j2));
    });

  spork(
    [&] () {
      for (; i < j; i++) {
        std::forward<BodyLambda&>(body)(i, a);
      }
    },
    [&] () {
      // Make sure to snapshot i and j so
      // their values aren't updated by body loop
      if (i >= j) { return false; }
      ReduceData* data = ReduceData::create(i, j);
      j = midpoint(i+1, j);
      jp = SpwnJob::create(execute_lambda<void*, decltype(spwn), void*>,
                           &spwn, (void*) data, heartbeat_tokens >> 1, jp);
      return j > i+1; // determine if more promotions are possible here
    });

  if (jp != nullptr) [[unlikely]] {
    jp->sync([&] (void* b) {
      std::forward<CombLambda>(combine)(a, *((A*) b));
      AAllocator::destroy((A*) b);
    });
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

uint fib(uint n) {
  // std::cout << "Fib called! n = " << n << std::endl;
  if (n <= 1) {
    return n;
  } else {
    uint l, r;
    par([&] () {l = fib(n - 1);},
        [&] () {r = fib(n - 2);});
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

int main(int argc, char* argv[]) {
  // this might take a sec the first time it is called
  //spork::SpwnJob::get_current_scheduler();

  //size_t n = atoi(argv[1]);
  constexpr uint n = 8000000;
  char data[n];
  for (uint i = 0; i < n; i++) {
    data[i] = 1 + (i % 5);
  }

  spork::SpwnJob::get_current_scheduler();
#if RECORD_HEARTBEAT_STATS
  spork::init_heartbeat_stats();
#endif
  
  using num = unsigned long long;
  
  auto total_time = 0;
  constexpr uint WARMUP = 10;
  constexpr uint NUM_TRIALS = 30;

  for (uint r = 0; r < WARMUP + NUM_TRIALS; r++) {
#if RECORD_HEARTBEAT_STATS
    spork::reset_heartbeat_stats();
#endif
    spork::start_heartbeats();
    
    auto start = std::chrono::steady_clock::now();
    num total =
      // spork::reduce<num>(
      //   0,
      //   0,
      //   [] (num& a, num b) { a += b; },
      //   [&] (uint i, num& a) { a += data[i % n]; },
      //   0, n*50);
      spork::fib(40);
    auto end = std::chrono::steady_clock::now();

    spork::pause_heartbeats();
    
    auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << total << " in " << time_ms << " ms";
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
