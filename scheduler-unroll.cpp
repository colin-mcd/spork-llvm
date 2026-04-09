#include "parlay/internal/work_stealing_job.h"
#include "parlay/parallel.h"
#include "parlay/alloc.h"
#include "parlay/monoid.h"

#include <atomic>
#include <type_traits>
#include <new> // For std::hardware_destructive_interference_size

//#include <linux/perf_event.h>
//#include <sys/ioctl.h>
//#include <fcntl.h>

#define RECORD_HEARTBEAT_STATS 1

#ifndef fwd
#define fwd(x) std::forward<std::remove_reference_t<decltype(x)>>(x)
#endif

// Common cache line size is 64, but C++17 provides a portable constant
#ifdef __cpp_lib_hardware_interference_size
constexpr std::size_t CL_SIZE = std::hardware_destructive_interference_size;
#else
constexpr std::size_t CL_SIZE = 64;
#endif

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

// TODO: consistent name casing (camel or snake)
// TODO: consider adaptive heartbeat timer intervals
// TODO: look into if loop unrolling is why reduce is slower than reduceSeq

namespace spork {

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
    // since we ALWAYS promote innermost-first,
    // all potential parallelism is fully promoted by now
    // thus, no reason to get heartbeats while waiting
    pause_heartbeats();
    auto done = [&] () { return finished(); };
    get_current_scheduler().wait_until(done);
    start_heartbeats();
  }

  void enqueue(uint with_tokens = 0) {
    done.store(false, std::memory_order_release);
    hbt = with_tokens;
    if (with_tokens) heartbeat_tokens = heartbeat_tokens - with_tokens;
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
      if (!finished()) wait();
    }
  }

  // if stolen, waits until finished and then returns true;
  // otherwise, returns false.
  bool sync_is_stolen() {
    if (!try_dequeue()) {
      if (!finished()) wait();
      return true;
    }
    return false;
  }

  // pretends a volatile WorkStealingJob is nonvolatile
  __attribute__((always_inline))
  WorkStealingJob& pretend_nonvolatile() volatile noexcept {
    return (WorkStealingJob&) *this;
  }

  virtual void run() = 0;
  std::atomic<bool> done;
  uint hbt; // heartbeat tokens
};

// TODO: make sure prom doesn't throw any exceptions
struct PromFn {
  virtual void operator()() const = 0;
};

struct SporkSlot {
  // TODO: consider making `prom_flag` not a pointer,
  // storing `spork`'s `volatile bool promotable_flag` inside this struct
  // TODO: otherwise, we can probably view this as nonvolatile from this struct
  volatile bool promoted;
  const PromFn* promfn;
  SporkSlot*volatile* prev;
  SporkSlot* next;

  explicit SporkSlot(const PromFn* _promfn, SporkSlot** _prev, SporkSlot* _next) :
    promoted(false),
    promfn(_promfn),
    prev(_prev),
    next(_next) {}
  explicit SporkSlot(const SporkSlot& other) :
    promoted(other.promoted),
    promfn(other.promfn),
    prev(other.prev),
    next(other.next) {}
  explicit SporkSlot(SporkSlot&& other) :
    promoted(other.promoted),
    promfn(fwd(other.promfn)),
    prev(fwd(other.prev)),
    next(fwd(other.next)) {};
  // sentinel spork slot for `spork_deque_front`
  consteval explicit SporkSlot() :
    promoted(true), promfn(nullptr), prev(nullptr), next(nullptr) {}

  explicit SporkSlot(const PromFn* _promfn);
  static void reset();
  bool close();
  void promote();
  static void promote_front();
  // void eager_promote();
};

// sentinel node for spork deque
constinit thread_local SporkSlot spork_deque_front;
// `spork_deque_back` points to the back of the spork deque
constinit thread_local SporkSlot*volatile* spork_deque_back;



// The constructor and `close()` for `SporkSlot` may
// write to `spork_deque_back`, but the signal handler may only read.
// If we try to change `spork_deque_back` in the signal handler,
// it can cause issues because it is a (nonatomic) thread_local variable,
__attribute__((always_inline))
SporkSlot::SporkSlot(const PromFn* _promfn)
  : promoted(false), promfn(_promfn), prev(spork_deque_back) {
  *prev = this;
  // if (heartbeat_tokens) [[unlikely]] eager_promote();
  // now commit these changes to the spork stack, allowing promotions
  spork_deque_back = &next;
}

void SporkSlot::reset() {
  spork_deque_back = &spork_deque_front.next;
}

// NOTE: `this` *must* be the slot at `spork_deque_back`
__attribute__((always_inline))
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

// __attribute__((noinline))
// void SporkSlot::eager_promote() {
//   bool before = disable_heartbeats;
//   disable_heartbeats = true;
//   promote();
//   disable_heartbeats = before;
// }

void SporkSlot::promote_front() {
  if (&spork_deque_front.next == spork_deque_back) return;
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
    if (&slot->next == spork_deque_back) {
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
__attribute__((noinline))
void manualProm(const PromLambda&& prom, volatile bool& promoted) {
  // save
  bool before = disable_heartbeats;
  disable_heartbeats = true;
  // check again to make sure a heartbeat didn't
  // eat all the tokens or promote this already
  if (heartbeat_tokens && !promoted) {
    heartbeat_tokens = heartbeat_tokens - 1;
    promoted = true;
    fwd(prom)();
    //spork_deque_front.next = spork_deque_back;
  }
  // restore
  disable_heartbeats = before;
}

// TODO: exception handling
// TODO: look into using `parlay::copyable_function_wrapper` from `utilities.h`
// (and perhaps also `padded<...>`)
template <typename BodyLambda, typename PromLambda>
__attribute__((always_inline))
bool spork(const BodyLambda&& body, const PromLambda&& prom) {
  static_assert(std::is_invocable_v<BodyLambda&&>);
  static_assert(std::is_invocable_v<PromLambda&&>);

  struct PromSpork : PromFn {
    const PromLambda&& prom;
    PromSpork(const PromLambda&& _prom) :
      prom(fwd(_prom)) {}
    void operator()() const override {
      fwd(prom)();
    }
  };
  const PromSpork promfn(fwd(prom));

  SporkSlot slot(&promfn);
  if (heartbeat_tokens) [[unlikely]]
    manualProm(fwd(prom), slot.promoted);
  fwd(body)();
  return slot.close();
}

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

  bool promoted = spork(
    fwd(lamL),
    [&jp] () { ((SpwnJob*) &jp)->enqueue(heartbeat_tokens >> 1); });

  if (promoted) [[unlikely]] { // promoted
    ((SpwnJob*) &jp)->sync(false);
  } else [[likely]] { // unpromoted
    fwd(lamR)();
  }
}

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
    SpwnJob(SpwnJob&& other) :
      WorkStealingJob(fwd(other)),
      spwn(fwd(other.spwn)) {}
  };
  
  volatile SpwnJob jp(fwd(spwn));
  BR br;

  bool promoted = spork(
    [&] () { br = fwd(body)(); },
    [&] () { ((SpwnJob*) &jp)->enqueue(heartbeat_tokens >> 1); });

  if (promoted) [[unlikely]] { // promoted
    if (((SpwnJob*) &jp)->sync_is_stolen()) {
      return fwd(prom)(br, jp.sr);
    } else {
      return fwd(unst)(br);
    }
  } else [[likely]] { // unpromoted
    return fwd(unpr)(br);
  }
}

// // allows code in promoted case before sync
// template <typename BR, typename SR, typename PR, typename RR, typename BodyL, typename SpwnL, typename UnprL, typename PromL, typename SyncL>
// __attribute__((always_inline))
// RR spork(const BodyL&& body, const SpwnL&& spwn, const UnprL&& unpr, const PromL&& prom, const SyncL&& sync) {
//   static_assert(std::is_invocable_r_v<BR, const BodyL&>);
//   static_assert(std::is_invocable_r_v<SR, const SpwnL&>);
//   static_assert(std::is_invocable_r_v<RR, const UnprL&, BR>);
//   static_assert(std::is_invocable_r_v<PR, const PromL&, BR>);
//   static_assert(std::is_invocable_r_v<RR, const PromL&, PR, SR>);
  
//   struct SpwnJob : WorkStealingJob {
//     const SpwnL&& spwn;
//     SR sr;
//     void run() override { sr = fwd(spwn)(); }
//     SpwnJob(const SpwnL&& _spwn) :
//       WorkStealingJob(),
//       spwn(fwd(_spwn)) {}
//     SpwnJob(SpwnJob&& other) :
//       WorkStealingJob(fwd(other)),
//       spwn(fwd(other.spwn)) {}
//   };
  
//   volatile SpwnJob jp(fwd(spwn));
//   BR br;

//   bool promoted = spork(
//     [&] () { br = fwd(body)(); },
//     [&] () { ((SpwnJob*) &jp)->enqueue(heartbeat_tokens >> 1); });

//   if (promoted) [[unlikely]] { // promoted
//     PR pr = fwd(prom)(br);
//     ((SpwnJob*) &jp)->sync(false);
//     return fwd(sync)(pr, jp.sr);
//   } else [[likely]] { // unpromoted
//     return fwd(unpr)(br);
//   }
// }

uint fibE(uint n) {
  if (n <= 1) { return n; }
  struct SpwnJob : WorkStealingJob {
    const uint n;
    uint r;
    void run() override { r = fibE(n - 2); }
    
    SpwnJob(const uint _n) : WorkStealingJob(), n(_n) {}
    SpwnJob(SpwnJob&& other) : WorkStealingJob(fwd(other)), n(fwd(other.n)) {}
  };
  
  volatile SpwnJob jp(n);
  uint l;

  bool promoted = spork(
    [&l, n] () { l = fibE(n - 1); },
    [&] () {
      SpwnJob& jpnv = *((SpwnJob*) &jp);
      jpnv.enqueue(heartbeat_tokens >> 1);
    });

  if (promoted) [[unlikely]] { // promoted
    ((SpwnJob*) &jp)->sync(false);
    return l + jp.r;
  } else [[likely]] { // unpromoted
    return l + fibE(n - 2);
  }
}

template <typename idx>
__attribute__((always_inline))
constexpr static const idx midpoint(idx i, idx j) noexcept {
  static_assert(std::is_integral_v<idx>);
  return i + ((j - i) >> 1);
}

template <typename idx>
__attribute__((always_inline))
constexpr static idx third1(idx i, idx j) noexcept {
  static_assert(std::is_integral_v<idx>);
  return i + ((j - i) / 5);
}
template <typename idx>
__attribute__((always_inline))
constexpr static idx third2(idx i, idx j) noexcept {
  static_assert(std::is_integral_v<idx>);
  return j - 2 * ((j - i) / 5);
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

template <typename idx, typename A, typename BodyLambda, typename BinOp>
// __attribute__((always_inline))
A seqfor(idx i, idx j, const BodyLambda&& body, const BinOp&& binop) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx, A&>);
  static_assert(parlay::is_monoid_for_v<BinOp, A>);

  A a = fwd(binop).identity;
  for (; i < j; i++) fwd(body)(i, a);
  return a;
}

template <typename idx, typename BodyLambda>
// __attribute__((always_inline))
void seqfor(idx i, idx j, const BodyLambda&& body) {
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx>);
  for (; i < j; i++) fwd(body)(i);
}

template <const uint unroll, typename idx, typename A, typename BodyLambda, typename BinOp>
//__attribute__((always_inline)) // TODO: investigate what this attribute actually does
A parfor(idx i, idx _j, const BodyLambda&& body, const BinOp&& binop) {
  static_assert(std::is_integral_v<idx>);
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx, A&>);
  static_assert(parlay::is_monoid_for_v<BinOp, A>);

  struct SpwnJob : WorkStealingJob {
    idx i, j;
    const BodyLambda&& body;
    const BinOp&& binop;
    A a;
    void run() override {
      a = parfor<unroll, idx, A, BodyLambda, BinOp>
        (i, j, fwd(body), fwd(binop));
    }
    SpwnJob(const BodyLambda&& _body, const BinOp&& _binop) :
      WorkStealingJob(),
      body(fwd(_body)),
      binop(fwd(_binop)) {}
  };

  SpwnJob l(fwd(body), fwd(binop));
  SpwnJob r(fwd(body), fwd(binop));
  volatile idx j = i + (_j - i - ((_j - i) % unroll));
  A a = binop.identity;

  bool promoted = spork(
    [&] () {
      for (; i < j; i += unroll) {
        idx i2 = i;
        #pragma unroll(unroll)
        for (idx r = 0; r < unroll; r++) {
          fwd(body)(i2, a);
          i2++;
        }
      }
    },
    [&] () {
      idx _i = i + unroll;
      if (_i >= _j) { r.i = r.j = 0; l.i = l.j = 0; return; }
      idx mid = midpoint<idx>(_i, _j);
      j = _i;

      r.i = mid;
      r.j = _j;
      r.enqueue((heartbeat_tokens + 1) >> 1);

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = l.j = 0; return; }
      l.i = _i;
      l.j = mid;
      l.enqueue(heartbeat_tokens);
    });
  if (promoted) [[unlikely]] {
    if (l.i < l.j) [[likely]] {
      l.sync(true);
      a = binop(a, l.a);
    }
    if (r.i < r.j) [[likely]] {
      r.sync(false);
      a = binop(a, r.a);
    } else {
      goto last_loop;
    }
    goto done;
  }
  last_loop:
  for (; i < _j; i++) fwd(body)(i, a);
  done:
  return a;
}


template <const uint unroll, typename idx, typename BodyLambda>
//__attribute__((always_inline)) // TODO: investigate what this attribute actually does
void parfor(idx i, idx _j, const BodyLambda&& body) {
  static_assert(std::is_integral_v<idx>);
  static_assert(std::is_invocable_r_v<void, BodyLambda&, idx>);

  struct SpwnJob : WorkStealingJob {
    idx i, j;
    const BodyLambda&& body;
    void run() override {
      parfor<unroll, idx, BodyLambda> (i, j, fwd(body));
    }
    SpwnJob(const BodyLambda&& _body) :
      WorkStealingJob(),
      body(fwd(_body)) {}
  };

  SpwnJob l(fwd(body));
  SpwnJob r(fwd(body));
  volatile idx j = i + (_j - i - ((_j - i) % unroll));

  bool promoted = spork(
    [&] () {
      for (; i < j; i += unroll) {
        idx i2 = i;
        #pragma unroll(unroll)
        for (idx r = 0; r < unroll; r++) {
          fwd(body)(i2);
          i2++;
        }
      }
    },
    [&] () {
      idx _i = i + unroll;
      if (_i >= _j) { r.i = r.j = 0; l.i = l.j = 0; return; }
      idx mid = midpoint<idx>(_i, _j);
      j = _i;

      r.i = mid;
      r.j = _j;
      r.enqueue((heartbeat_tokens + 1) >> 1);

      // TODO: check if work stealing deque is full before enqueueing

      if (_i >= mid) { l.i = l.j = 0; return; }
      l.i = _i;
      l.j = mid;
      l.enqueue(heartbeat_tokens);
    });
  if (promoted) [[unlikely]] {
    if (l.i < l.j) [[likely]] {
      l.sync(true);
    }
    if (r.i < r.j) [[likely]] {
      r.sync(false);
    } else {
      goto last_loop;
    }
  }
  last_loop:
  for (; i < _j; i++) fwd(body)(i);
  done:
  return;
}

template <typename idx>
constexpr idx scan_chunksize(idx n) {
  // TODO: ideally, all splits would be along cache lines
  return (idx) sqrt((double) n);
}

template <const uint unroll, typename idx, typename A, typename BinOp>
void scan(A a, A* arr, idx n, const BinOp&& binop);

template <const uint unroll, typename idx, typename A, typename BinOp>
__attribute__((always_inline))
void scan(idx n, A* arr, const BinOp&& binop) {
  scan<unroll, idx, A, BinOp>(fwd(binop).identity, arr, n, fwd(binop));
}

template <const uint unroll, typename idx, typename A, typename BinOp>
A* scan_upsweep(A* arr, idx n, const BinOp&& binop) {
  static_assert(parlay::is_monoid_for_v<BinOp, A>);
  static_assert(std::is_integral_v<idx>);

  idx chunksize = scan_chunksize(n);
  idx chunks = 1 + ((n - 1) / chunksize);
  // TODO: consider aligning this allocation
  // so we can split chunks along cache lines
  A* partials = (A*) parlay::p_malloc((size_t) n*sizeof(A));
  if (partials) {
    parfor<1, idx>(0, chunks, [=,&binop] (idx c) {
      idx start = c*chunksize;
      idx end = (chunksize > n - start) ? n : start + chunksize;
      partials[c] = parfor<1, idx, A>(start, end, [&, arr] (idx i, A& sum) {
        sum = fwd(binop)(sum, arr[i]); }, fwd(binop));
    });
    
    scan<unroll, idx, A, BinOp>(chunks, partials, fwd(binop));
  }
  return partials;
}

template <const uint unroll, typename idx, typename A, typename BinOp>
void scan_downsweep(A* partials, A* arr, idx n, const BinOp&& binop) {
  static_assert(parlay::is_monoid_for_v<BinOp, A>);
  static_assert(std::is_integral_v<idx>);
  idx chunksize = scan_chunksize(n);
  idx chunks = 1 + ((n - 1) / chunksize);
  
  parfor<1, idx>(0, chunks, [=,&binop] (idx chunk) {
    A pfx = chunk ? fwd(binop)(arr[-1], partials[chunk - 1]) : arr[-1];
    idx start = chunk*chunksize;
    idx size = (chunksize > n - start) ? (n - start) : chunksize;
    scan<unroll, idx, A, BinOp>(pfx, &arr[start], size, fwd(binop));
  });
  
  parlay::p_free(partials);
}

template <const uint unroll, typename idx, typename A, typename BinOp>
void scan(A a, A* arr, idx n, const BinOp&& binop) {
  static_assert(parlay::is_monoid_for_v<BinOp, A>);
  static_assert(std::is_integral_v<idx>);

  // idx volatile j = n;
  idx nmod = n - (n % unroll);
  idx volatile j = nmod;
  idx i = 0;

  bool promoted = spork(
    [&, arr] () { // body
      for (; i < j; i += unroll) {
        A* arr_i = &arr[i];
        #pragma unroll(unroll)
        for (idx r = 0; r < unroll; r++) {
          a = fwd(binop)(a, arr_i[r]);
          arr_i[r] = a;
        }
      }
    },
    [&, n] () {
      idx _i = i + unroll;
      idx t1 = third1<idx>(_i, n);
      idx t2 = third2<idx>(_i, n);
      // only break if every third has work to do
      if (_i < t1 && t1 < t2 && t2 < n) j = _i;
    });

  // TODO: could start spwn from prom handler above, then run body, then sync
  // (slightly less delayed spwn)
  if (promoted && i != nmod) {
    idx t1 = third1<idx>(i, n);
    idx t2 = third2<idx>(i, n);
    spork<char, A*, char>(
      [=,&binop] () { // body
        scan<unroll, idx, A, BinOp>(arr[i-1], &arr[i], t1 - i, fwd(binop));
        return '\0';},
      [=,&binop] () { // spwn
        return scan_upsweep<unroll, idx, A, BinOp>(&arr[t1], t2 - t1, fwd(binop));},
      [=,&binop] (char _) { // unpr
        scan<unroll, idx, A, BinOp>(arr[t1-1], &arr[t1], n - t1, fwd(binop));
        return '\0';},
      [=,&binop] (char _, A* partials) { // prom
        if (partials) {
          par([&binop, partials, arrt1 = &arr[t1], t21 = t2 - t1] () {
                scan_downsweep<unroll, idx, A, BinOp>(partials, arrt1, t21, fwd(binop));
              },
              [=,&binop] () {
                A sum12 = partials[(t2 - t1 - 1) / scan_chunksize(t2 - t1)];
                A sum02 = fwd(binop)(arr[t1-1], sum12);
                scan<unroll, idx, A, BinOp>(sum02, &arr[t2], n - t2, fwd(binop));
              });
        } else { // could not allocate the partials array
          scan<unroll, idx, A, BinOp>(arr[t1-1], &arr[t1], n - t1, fwd(binop));
        }
        return '\0';
      },
      [=,&binop] (char _) { // unstolen
        scan<unroll, idx, A, BinOp>(arr[t1-1], &arr[t1], n - t1, fwd(binop));
        return '\0';
      });
  } else {
    for (; i < n; i++) {
      a = fwd(binop)(a, arr[i]);
      arr[i] = a;
    }
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

// uint fibspork(uint n) {
//   if (n <= 1) {
//     return n;
//   } else {
//     return
//       spork<uint, uint, uint>
//         ([n] () {return fibspork(n - 1);},
//          [n] () {return fibspork(n - 2);},
//          [n] (uint l) {return l + fibspork(n - 2);},
//          [] (uint l, uint r) { return l + r; },
//          [n] () {});
//   }
// }
} // namespace spork

template <typename F>
__attribute__((always_inline))
void p4(size_t s, size_t e, F&& f) {
  //parlay::parallel_for(s, e, fwd(f), 1);
  spork::parfor<size_t>(s, e, fwd(f));
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
//     for (; i < j; i++) a = fwd(body)(a, i);
//     return a;
//   }

//   A L, R;
//   parlay::par_do([&] {L = reduce(z, body, binop, i, i + ((j - i) >> 1));},
//                  [&] {R = reduce(z, body, binop, i + ((j - i) >> 1), j);});
//   fwd(binop)(L,R);
//   return L;
// }

template <typename idx, typename datnum>
datnum* make_data(idx n) {
  datnum* arr = (datnum*) parlay::p_malloc((size_t) n * sizeof(datnum));
  if (arr) {
    for (idx i = 0; i < n; i++) {
      arr[i] = 1 + (i % 5);
    }
  } else {
    std::cerr << "Failed to allocate data array" << std::endl;
  }
  return arr;
}

int main(int argc, char* argv[]) {
  //size_t n = atoi(argv[1]);
  using idxnum = unsigned;
  using datnum = unsigned;

  // num total = 0;
  // volatile uint j = n*50;
  // for (uint i = 0; i < j; i++) {
  //   total += data[i % n];
  // }
  // std::cout << total << std::endl;

  constexpr idxnum n = 80000000;
  datnum* data = make_data<idxnum, datnum>(n);
  
  auto total_time = 0;
  constexpr uint WARMUP = 10;
  constexpr uint NUM_TRIALS = 30;

  // this might take a sec the first time it is called
  spork::WorkStealingJob::get_current_scheduler();
#if RECORD_HEARTBEAT_STATS
  spork::init_heartbeat_stats();
#endif

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
    // num total =
      // spork::seqfor(0, n*50, [&] (uint i, num& a) {a += data[i % n];}, parlay::plus<num>());
      // spork::parfor_unroll2<uint>(0, n*50, [&] (uint i, num& a) {a += data[i % n];}, parlay::plus<num>());
    datnum total = 0;
    // total = spork::parfor<4, idxnum, datnum>(0, n, [&] (idxnum i, datnum& a) {a += data[i];}, parlay::plus<datnum>());
    // total = spork::seqfor<idxnum, datnum>(0, n, [&] (idxnum i, datnum& a) {a += data[i];}, parlay::plus<datnum>());
    // total = spork::seqfor<idxnum, datnum>(0, n, [&] (idxnum i, datnum& a) {a += data[i]; data[i] = a;}, parlay::plus<datnum>());
    spork::scan<5>(n, data, parlay::plus<datnum>());
    // total = spork::fib(38);
    auto end = std::chrono::steady_clock::now();

    spork::pause_heartbeats();
    
    auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << (datnum) data[n - 2] << " " << total << " in " << time_ms << " ms";
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
