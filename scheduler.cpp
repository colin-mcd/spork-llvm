#include "parlay/internal/work_stealing_job.h"
#include "parlay/parallel.h"
#include "parlay/alloc.h"
#include "parlay/monoid.h"

#include <atomic>
#include <type_traits>

//#include <linux/perf_event.h>
//#include <sys/ioctl.h>
//#include <fcntl.h>

#define RECORD_HEARTBEAT_STATS 1

// TODO: consistent name casing (camel or snake)
// TODO: consider adaptive heartbeat timer intervals
// TODO: look into if loop unrolling is why reduce is slower than reduceSeq

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

// Derived from `parlaylib/include/parlay/internal/work_stealing_job.h`
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
    auto done = [&] () { return finished(); };
    get_current_scheduler().wait_until(done);
  }

  void enqueue() {
    // TODO: make sure queue doesn't overflow (max 1000, aborts if hit)
    get_current_scheduler().spawn(this);
  }

  static bool try_dequeue() {
    return get_current_scheduler().get_own_job() != nullptr;
  }

  void fast_clone(bool reclaim_tokens) {
    if (reclaim_tokens) heartbeat_tokens = heartbeat_tokens + hbt;
    run();
  }

  void sync(bool reclaim_tokens) {
    if (try_dequeue()) { // unstolen
      fast_clone(reclaim_tokens);
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

  virtual void run() = 0;
  std::atomic<bool> done;
  uint hbt; // heartbeat tokens
};

// TODO: make sure prom doesn't throw any exceptions
struct PromFn {
  virtual void operator()() = 0;
};

struct SporkSlot {
  // TODO: consider making `prom_flag` not a pointer,
  // storing `spork`'s `volatile bool promotable_flag` inside this struct
  // TODO: otherwise, we can probably view this as nonvolatile from this struct
  volatile bool promoted;
  const PromFn* promfn;
  SporkSlot* volatile next;
  SporkSlot* volatile prev;

  explicit SporkSlot(const PromFn* _promfn, SporkSlot* _next, SporkSlot* _prev) :
    promoted(false),
    promfn(_promfn),
    next(_next),
    prev(_prev) {}
  explicit SporkSlot(const SporkSlot& other) :
    promoted(other.promoted),
    promfn(other.promfn),
    next(other.next),
    prev(other.prev) {}
  explicit SporkSlot(SporkSlot&& other) :
    promoted(other.promoted),
    promfn(std::forward<const PromFn* const>(other.promfn)),
    next(std::forward<SporkSlot* volatile>(other.next)),
    prev(std::forward<SporkSlot* volatile>(other.prev)) {};
  // sentinel spork slot for `spork_deque_front`
  consteval explicit SporkSlot() :
    promoted(true), promfn(nullptr), next(nullptr), prev(nullptr) {}

  explicit SporkSlot(const PromFn* _promfn);
  static void reset();
  bool close();
  void promote();
  static void promote_front();
};

// sentinel node for spork deque
constinit thread_local SporkSlot spork_deque_front;
// `spork_deque_back` points to the back of the spork deque
constinit thread_local SporkSlot* volatile spork_deque_back;

// The constructor and `close()` for `SporkSlot` may
// write to `spork_deque_back`, but the signal handler may only read.
// If we try to change `spork_deque_back` in the signal handler,
// it can cause issues because it is a (nonatomic) thread_local variable,
SporkSlot::SporkSlot(const PromFn* _promfn)
  : promoted(false), promfn(_promfn), prev(spork_deque_back) {
  // prev->next = this;
  spork_deque_back->next = this;
  // now commit these changes to the spork stack, allowing promotions
  spork_deque_back = this;
}

void SporkSlot::reset() {
  spork_deque_back = &spork_deque_front;
}

// NOTE: `this` *must* be the slot at `spork_deque_back`
bool SporkSlot::close() {
  // TODO idea: just disable heartbeats for this and also constructor above,
  // then perhaps we can remove promoted slots from the spork stack?
  spork_deque_back = prev;
  return promoted;
}

void SporkSlot::promote() {
  //assert(promotable);
  heartbeat_tokens = heartbeat_tokens - 1;
  promoted = true;
  (*((PromFn*) promfn))();
}

void SporkSlot::promote_front() {
  if (&spork_deque_front == spork_deque_back) return;
  SporkSlot* slot = spork_deque_front.next;
  while (heartbeat_tokens) {
    if (!slot->promoted) {
      slot->promote();
      // if (slot == spork_deque_back) {
      // } else {
      //   slot->prev = &spork_deque_front;
      // }
      //slot = slot->next;
      // if this slot can no longer be promoted,
      // we can skip it next time we search
      // TODO: fix this below
      // if (!(*(slot->entry->promotable_flag))) {
      //   if (slot != spork_deque_back) {
      //     slot = slot->next;
      //     spork_deque_front.next = slot;
      //   } else {
      //     slot->prev = &spork_deque_front;
      //   }
      // }
    }
    if (slot == spork_deque_back) {
      //slot->prev = &spork_deque_front;
      break;
    } else {
      slot = slot->next;
      //spork_deque_front.next = slot;
    }
  }
}

#if RECORD_HEARTBEAT_STATS
volatile uint* num_heartbeats = nullptr;
volatile uint* missed_heartbeats = nullptr;

void init_heartbeat_stats() {
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    uint nw = parlay::internal::init_num_workers();
    num_heartbeats = new uint[nw];
    missed_heartbeats = new uint[nw];
    for (uint wi = 0; wi < nw; wi++) {
      num_heartbeats[wi] = 0;
      missed_heartbeats[wi] = 0;
    }
  }
}

void reset_heartbeat_stats() {
  uint nw = WorkStealingJob::num_workers();
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
    volatile uint& hbs = num_heartbeats[spork::WorkStealingJob::worker_id()];
    hbs = hbs + 1;
#endif
    heartbeat_tokens = heartbeat_tokens + TOKENS_PER_HEARTBEAT;
    if (heartbeat_tokens > MAX_HEARTBEAT_TOKENS) {
      heartbeat_tokens = MAX_HEARTBEAT_TOKENS;
    }
    SporkSlot::promote_front();
  } else {
#if RECORD_HEARTBEAT_STATS
    volatile uint& mhbs = missed_heartbeats[spork::WorkStealingJob::worker_id()];
    mhbs = mhbs + 1;
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
    SporkSlot::reset();

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

template <typename PromLambda>
void manualProm(const PromLambda&& prom, volatile bool& promoted) {
  // save
  bool before = disable_heartbeats;
  disable_heartbeats = true;
  // check again to make sure a heartbeat didn't
  // eat all the tokens or promote this already
  if (heartbeat_tokens && !promoted) {
    heartbeat_tokens = heartbeat_tokens - 1;
    promoted = true;
    std::forward<const PromLambda>(prom)();
    //spork_deque_front.next = spork_deque_back;
  }
  // restore
  disable_heartbeats = before;
}

// TODO: exception handling
template <typename BodyLambda, typename PromLambda>
//__attribute__((always_inline))
bool spork(const BodyLambda&& body, const PromLambda&& prom) {
  static_assert(std::is_invocable_v<BodyLambda&&>);
  static_assert(std::is_invocable_v<PromLambda&&>);

  struct PromSpork : PromFn {
    const PromLambda&& prom;
    PromSpork(const PromLambda&& _prom) :
      prom(std::forward<const PromLambda>(_prom)) {}
    void operator()() override {
      std::forward<const PromLambda>(prom)();
    }
  };
  const PromSpork promfn(std::forward<const PromLambda>(prom));

  SporkSlot slot(&promfn);
  if (heartbeat_tokens) [[unlikely]] {
    manualProm(std::forward<const PromLambda>(prom), slot.promoted);
  }
  std::forward<const BodyLambda>(body)();
  return slot.close();
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
  
  volatile SpwnJob jp(std::forward<const LambdaR>(lamR));

  bool promoted = spork(
    std::forward<const LambdaL>(lamL),
    [&] () {
      SpwnJob& jpnv = *((SpwnJob*) &jp);
      jpnv.done.store(false, std::memory_order_release);
      jpnv.hbt = heartbeat_tokens >> 1;
      jpnv.consume_these_hbt();
      jpnv.enqueue();
    });

  if (promoted) [[unlikely]] { // promoted
    ((SpwnJob*) &jp)->sync(false);
  } else [[likely]] { // unpromoted
    std::forward<const LambdaR>(jp.lamR)();
  }
}

uint fibE(uint n) {
  if (n <= 1) { return n; }
  struct SpwnJob : WorkStealingJob {
    const uint n;
    uint r;
    void run() override { r = fibE(n - 2); }
    
    SpwnJob(const uint _n) : WorkStealingJob(), n(_n) {}
    SpwnJob(SpwnJob&& other) : WorkStealingJob(std::forward<SpwnJob>(other)), n(std::forward<const uint>(other.n)) {}
  };
  
  volatile SpwnJob jp(n);
  uint l;

  bool promoted = spork(
    [&l, n] () { l = fibE(n - 1); },
    [&] () {
      SpwnJob& jpnv = *((SpwnJob*) &jp);
      jpnv.done.store(false, std::memory_order_release);
      jpnv.hbt = heartbeat_tokens >> 1;
      jpnv.consume_these_hbt();
      jpnv.enqueue();
    });

  if (promoted) [[unlikely]] { // promoted
    ((SpwnJob*) &jp)->sync(false);
    return l + jp.r;
  } else [[likely]] { // unpromoted
    return l + fibE(n - 2);
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
__attribute__((always_inline))
parlay::monoid_value_type_t<BinaryOp> seqfor(uint i, uint j, const BodyLambda&& body, const BinaryOp&& binop) {
  using A = parlay::monoid_value_type_t<BinaryOp>;
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint, A&>);
  static_assert(parlay::is_monoid_v<BinaryOp>);

  A a = binop.identity;
  for (; i < j; i++) std::forward<const BodyLambda>(body)(i, a);
  return a;
}

template <typename BodyLambda>
__attribute__((always_inline))
void seqfor(uint i, uint j, const BodyLambda&& body) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint>);
  for (; i < j; i++) std::forward<const BodyLambda>(body)(i);
}

// #ifndef UNROLL
// #define UNROLL(n, body, i, a) UNROLL_##n(body, i, a)
// #define UNROLL_1(body, i, a) body(i, a)
// #define UNROLL_2(body, i, a) body(i, a); body(i+1, a)
// #define UNROLL_4(body, i, a) body(i, a); body(i+1, a); body(i+2, a); body(i+3, a)
// #define UNROLL_8(body, i, a) body(i, a); body(i+1, a); body(i+2, a); body(i+3, a); body(i+4, a); body(i+5, a); body(i+6, a); body(i+7, a)
// #endif

// template <uint unroll>
// uint unroll_trunc(uint j) { return j; } // todo unimplemented error?

// template <> uint unroll_trunc<1>(uint j) { return j; }
// template <> uint unroll_trunc<2>(uint j) { return (j >> 1) << 1; }
// template <> uint unroll_trunc<4>(uint j) { return (j >> 2) << 2; }
// template <> uint unroll_trunc<8>(uint j) { return (j >> 3) << 3; }

template <typename BodyLambda, typename BinaryOp>
__attribute__((always_inline)) // TODO: investigate what this attribute actually does
parlay::monoid_value_type_t<BinaryOp> parfor_unroll2(uint i, uint _j, const BodyLambda&& body, const BinaryOp&& binop) {
  using A = parlay::monoid_value_type_t<BinaryOp>;
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint, A&>);
  static_assert(parlay::is_monoid_v<BinaryOp>);

  struct SpwnJob : WorkStealingJob {
    uint i, j;
    const BodyLambda&& body;
    const BinaryOp&& binop;
    A a;
    void run() override {
      a = parfor_unroll2<BodyLambda, BinaryOp>
        (i, j, std::forward<const BodyLambda>(body), std::forward<const BinaryOp>(binop));
    }
    SpwnJob(const BodyLambda&& _body, const BinaryOp&& _binop) :
      WorkStealingJob(),
      body(std::forward<const BodyLambda>(_body)),
      binop(std::forward<const BinaryOp>(_binop)) {}
  };

  SpwnJob l(std::forward<const BodyLambda>(body), std::forward<const BinaryOp>(binop));
  SpwnJob r(std::forward<const BodyLambda>(body), std::forward<const BinaryOp>(binop));
  uint oj = i + (((_j - i) >> 1) << 1);
  volatile uint j = oj;
  A a = binop.identity;

  bool promoted = spork(
    [&] () {
      for (; i < j; i += 2) {
        std::forward<const BodyLambda>(body)(i, a);
        std::forward<const BodyLambda>(body)(i+1, a);
      }
    },
    [&] () {
      uint _i = i + 2;
      uint _j = j;
      if (_i >= _j) { r.i = r.j = 0; l.i = l.j = 0; return; }
      uint mid = midpoint(_i, _j);
      j = _i;

      r.done.store(false, std::memory_order_release);
      r.hbt = (heartbeat_tokens + 1) >> 1;
      r.i = mid;
      r.j = _j;
      r.consume_these_hbt();
      r.enqueue();

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = l.j = 0; return; }
      l.done.store(false, std::memory_order_release);
      l.hbt = heartbeat_tokens;
      l.i = _i;
      l.j = mid;
      l.consume_these_hbt();
      l.enqueue();
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
  for (i = oj; i < _j; i++) {
    std::forward<const BodyLambda>(body)(i, a);
  }
  return a;
}


template <typename BodyLambda, typename BinaryOp>
__attribute__((always_inline)) // TODO: investigate what this attribute actually does
parlay::monoid_value_type_t<BinaryOp> parfor(uint i, uint _j, const BodyLambda&& body, const BinaryOp&& binop) {
  using A = parlay::monoid_value_type_t<BinaryOp>;
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint, A&>);
  static_assert(parlay::is_monoid_v<BinaryOp>);

  struct SpwnJob : WorkStealingJob {
    uint i, j;
    const BodyLambda&& body;
    const BinaryOp&& binop;
    A a;
    void run() override {
      a = parfor<BodyLambda, BinaryOp>
        (i, j, std::forward<const BodyLambda>(body), std::forward<const BinaryOp>(binop));
    }
    SpwnJob(const BodyLambda&& _body, const BinaryOp&& _binop) :
      WorkStealingJob(),
      body(std::forward<const BodyLambda>(_body)),
      binop(std::forward<const BinaryOp>(_binop)) {}
  };

  SpwnJob l(std::forward<const BodyLambda>(body), std::forward<const BinaryOp>(binop));
  SpwnJob r(std::forward<const BodyLambda>(body), std::forward<const BinaryOp>(binop));
  volatile uint j = _j;
  A a = binop.identity;

  bool promoted = spork(
    [&] () { for (; i < j; i++) std::forward<const BodyLambda>(body)(i, a); },
    [&] () {
      uint _i = i + 1;
      uint _j = j;
      if (_i >= _j) { r.i = r.j = 0; l.i = l.j = 0; return; }
      uint mid = midpoint(_i, _j);
      j = _i;

      r.done.store(false, std::memory_order_release);
      r.hbt = (heartbeat_tokens + 1) >> 1;
      r.i = mid;
      r.j = _j;
      r.consume_these_hbt();
      r.enqueue();

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = l.j = 0; return; }
      l.done.store(false, std::memory_order_release);
      l.hbt = heartbeat_tokens;
      l.i = _i;
      l.j = mid;
      l.consume_these_hbt();
      l.enqueue();
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


template <typename BodyLambda>
__attribute__((always_inline)) // TODO: investigate what this attribute actually does
void parfor(uint i, uint _j, const BodyLambda&& body) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, uint>);

  struct SpwnJob : WorkStealingJob {
    uint i, j;
    const BodyLambda&& body;
    void run() override {
      parfor<BodyLambda>(i, j, std::forward<const BodyLambda>(body));
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
    [&] () { for (; i < j; i++) std::forward<const BodyLambda>(body)(i); },
    [&] () {
      uint _i = i + 1;
      uint _j = j;
      if (_i >= _j) { r.i = r.j = 0; l.i = l.j = 0; return; }
      // assumes midpoint favors more iterations on the right
      uint mid = midpoint(_i, _j);
      j = _i;

      r.done.store(false, std::memory_order_release);
      r.hbt = (heartbeat_tokens + 1) >> 1;
      r.i = mid;
      r.j = _j;
      r.consume_these_hbt();
      r.enqueue();

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = l.j = 0; return; }
      l.done.store(false, std::memory_order_release);
      l.hbt = heartbeat_tokens;
      l.i = _i;
      l.j = mid;
      l.consume_these_hbt();
      l.enqueue();
    });
  if (!promotable) [[unlikely]] {
    if (l.i < l.j) [[unlikely]] l.sync();
    if (r.i < r.j) [[likely]] r.sync();
  }
}

uint fibSeq(uint n) {
  if (n <= 1) {
    return n;
  } else {
    return fibSeq(n-1) + fibSeq(n-2);
    // uint l, r;
    // parSeq([&, n] () {l = fibSeq(n - 1);},
    //        [&, n] () {r = fibSeq(n - 2);});
    // return l + r;
  }
}

uint fibParlay(uint n) {
  if (n <= 1) {
    return n;
  } else {
    uint l, r;
    parlay::par_do([&, n] () {l = fibParlay(n - 1);},
                   [&, n] () {r = fibParlay(n - 2);});
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

void print_uint_avg(const uint* arr, uint len) {
  if (arr && len) {
    uint total = 0;
    for (uint i = 0; i < len; i++) total += arr[i];
    std::cout << (total / len) << " avg";
  } else {
    std::cout << "NaN avg";
  }
}

template <typename F>
__attribute__((always_inline))
void p4(size_t s, size_t e, F&& f) {
  //parlay::parallel_for(s, e, std::forward<F>(f), 1);
  spork::parfor(s, e, std::forward<F>(f));
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
//     for (; i < j; i++) a = std::forward<Body>(body)(a, i);
//     return a;
//   }

//   A L, R;
//   parlay::par_do([&] {L = reduce(z, body, binop, i, i + ((j - i) >> 1));},
//                  [&] {R = reduce(z, body, binop, i + ((j - i) >> 1), j);});
//   std::forward<Combine>(binop)(L,R);
//   return L;
// }


int main(int argc, char* argv[]) {
  //size_t n = atoi(argv[1]);
  constexpr uint n = 8000000;
  char data[n];
  for (uint i = 0; i < n; i++) {
    data[i] = 1 + (i % 5);
  }

  // this might take a sec the first time it is called
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
      // spork::seqfor(0, n*50, [&] (uint i, num& a) {a += data[i % n];}, parlay::plus<num>());
      // spork::parfor_unroll2(0, n*50, [&] (uint i, num& a) {a += data[i % n];}, parlay::plus<num>());
      spork::fibE(38);
    auto end = std::chrono::steady_clock::now();

    spork::pause_heartbeats();
    
    auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << /*(uint) data[n - 1]*/ total << " in " << time_ms << " ms";
#if RECORD_HEARTBEAT_STATS
    std::cout <<" (";
    print_uint_avg((uint*) spork::num_heartbeats, spork::WorkStealingJob::num_workers());
    std::cout << " heartbeats, ";
    print_uint_avg((uint*) spork::missed_heartbeats, spork::WorkStealingJob::num_workers());
    std::cout << " missed during eager proms)";
#endif
    std::cout << std::endl;
    if (r >= WARMUP) total_time += time_ms;
  }
  std::cout << "Average " << (total_time / NUM_TRIALS) << " ms" << std::endl;

  spork::pause_heartbeats();
}
