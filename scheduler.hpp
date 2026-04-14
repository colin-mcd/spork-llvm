#ifndef SPORK_SCHEDULER_H_
#define SPORK_SCHEDULER_H_

#include "parlay/internal/work_stealing_deque.h"
#include "parlay/internal/work_stealing_job.h"
#include "parlay/parallel.h"
#include "parlay/alloc.h"
#include "parlay/monoid.h"

#include <atomic>
#include <type_traits>
#include <new> // for std::hardware_destructive_interference_size

//#include <linux/perf_event.h>
//#include <sys/ioctl.h>
//#include <fcntl.h>

#define RECORD_HEARTBEAT_STATS

#define fwd(x) std::forward<std::remove_reference_t<decltype(x)>>(x)

// // Common cache line size is 64, but C++17 provides a portable constant
// #ifdef __cpp_lib_hardware_interference_size
// constexpr std::size_t CL_SIZE = std::hardware_destructive_interference_size;
// #else
// constexpr std::size_t CL_SIZE = 64;
// #endif

// TODO: consistent name casing (camel or snake)
// TODO: consider adaptive heartbeat timer intervals
// TODO: look into if loop unrolling is why reduce is slower than reduceSeq
// TODO: consider if writing pointers is async-signal-safe

namespace spork {

//#define FRESH_SPORK_ID __COUNTER__

namespace { // private
  inline constexpr uint TOKENS_PER_HEARTBEAT = 30;
  inline constexpr uint HEARTBEAT_INTERVAL_US = 500;
  // I set this arbitrarily: consider tweaking
  inline constexpr uint MAX_HEARTBEAT_TOKENS = TOKENS_PER_HEARTBEAT*1;
  inline constinit thread_local volatile uint heartbeat_tokens = 0;
  inline constinit thread_local volatile bool disable_heartbeats = false;
} // private

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
  
  void operator()() {
    heartbeat_tokens = hbt;
    start_heartbeats();
    run();
    pause_heartbeats();
    assert(!done.test_and_set(std::memory_order_release));
  }

  [[nodiscard]] bool finished() volatile const noexcept {
    return done.test(std::memory_order_acquire);
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
    const_cast<WorkStealingJob*>(this)->run();
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

  virtual void run() = 0;
  volatile std::atomic_flag done;
  volatile uint hbt; // heartbeat tokens
};

namespace { // private
  // TODO: make sure prom doesn't throw any exceptions
  struct PromFn {
    virtual void operator()() const = 0;
  };
  
  struct SporkSlot {
    // TODO: otherwise, we can probably view this as nonvolatile from this struct
    volatile bool promoted;
    const PromFn* promfn;
    // `prev` does not need to be volatile itself, since the signal handler never uses it
    SporkSlot*volatile* prev;
    // Pointer to the next spork slot. Before following it,
    // check if this is already the back of the spork deque:
    // `&next == spork_deque_back`.
    // Note: `next` may be a dangling pointer.
    SporkSlot* volatile next;
  
    // Constructs the sentinel spork slot for `spork_deque_front`.
    // DO NOT USE OTHERWISE!
    consteval explicit SporkSlot() :
      promoted(true), promfn(nullptr), prev(nullptr), next(nullptr) {}
  
    explicit SporkSlot(const PromFn* _promfn);
    static void reset();
    bool close();
    void promote();
    static void promote_front();

    template <typename PromLambda>
    void eager_promote(const PromLambda&& prom);
  };
  
  // sentinel node for spork deque
  inline constinit thread_local SporkSlot spork_deque_front;
  // `spork_deque_back` points to the `next` pointer of the last slot in spork deque
  // (a slot is at the back if the address of its `next` pointer equals `spork_deque_back`)
  inline constinit thread_local SporkSlot * volatile * volatile spork_deque_back;
  
  // The constructor and `close()` for `SporkSlot` may
  // write to `spork_deque_back`, but the signal handler may only read.
  // If we try to change `spork_deque_back` in the signal handler,
  // it can cause issues because it is a (nonatomic) thread_local variable,
  // __attribute__((always_inline))
  inline SporkSlot::SporkSlot(const PromFn* _promfn)
    : promoted(false), promfn(_promfn), prev(spork_deque_back) {
    *prev = this;
    // if (heartbeat_tokens) [[unlikely]] eager_promote();
    // now commit these changes to the spork stack, allowing promotions
    spork_deque_back = &next;
  }
  
  inline void SporkSlot::reset() {
    spork_deque_back = &spork_deque_front.next;
  }
  
  // NOTE: `this` *must* be the slot at `spork_deque_back`
  // __attribute__((always_inline))
  inline bool SporkSlot::close() {
    // TODO idea: just disable heartbeats for this and also constructor above,
    // then perhaps we can remove promoted slots from the spork stack?
    spork_deque_back = prev;
    return promoted;
  }
  
  inline void SporkSlot::promote() {
    //assert(promotable);
    heartbeat_tokens = heartbeat_tokens - 1;
    promoted = true;
    (*((PromFn*) promfn))();
  }
  
  inline void SporkSlot::promote_front() {
    if (&spork_deque_front.next == spork_deque_back) return;
    SporkSlot* slot = spork_deque_front.next;
    while (heartbeat_tokens) {
      if (!slot->promoted) slot->promote();
      if (&slot->next == spork_deque_back) break;
      else slot = slot->next;
    }
  }


  template <typename PromLambda>
  __attribute__((noinline))
  void SporkSlot::eager_promote(const PromLambda&& prom) {
    // save
    bool before = disable_heartbeats;
    disable_heartbeats = true;
    // check again to make sure a heartbeat didn't
    // eat all the tokens or promote this already
    if (heartbeat_tokens && !promoted) {
      heartbeat_tokens = heartbeat_tokens - 1;
      promoted = true;
      fwd(prom)();
    }
    // restore
    disable_heartbeats = before;
  }

} // private

inline volatile uint* num_heartbeats = nullptr;
inline volatile uint* missed_heartbeats = nullptr;

inline void init_heartbeat_stats() {
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

inline void reset_heartbeat_stats() {
  uint nw = WorkStealingJob::num_workers();
  for (uint wi = 0; wi < nw; wi++) {
    num_heartbeats[wi] = 0;
    missed_heartbeats[wi] = 0;
  }
}

// sa.sa_sigaction = heartbeat_handler;
// sa.sa_flags = SA_SIGINFO
// void heartbeat_handler(int sig, siginfo_t* info, void* ucontext) {
//   ucontext_t* ctx = (ucontext_t*) ucontext;
inline void heartbeat_handler(int sig) {
  int saved_errno = errno;
  if (saved_errno) { std::cout << "SAVED_ERRNO = " << saved_errno << std::endl; }
  if (!disable_heartbeats) {
#ifdef RECORD_HEARTBEAT_STATS
    volatile uint& hbs = num_heartbeats[spork::WorkStealingJob::worker_id()];
    hbs = hbs + 1;
#endif
    heartbeat_tokens = heartbeat_tokens + TOKENS_PER_HEARTBEAT;
    if (heartbeat_tokens > MAX_HEARTBEAT_TOKENS) {
      heartbeat_tokens = MAX_HEARTBEAT_TOKENS;
    }
    SporkSlot::promote_front();
  } else {
#ifdef RECORD_HEARTBEAT_STATS
    volatile uint& mhbs = missed_heartbeats[spork::WorkStealingJob::worker_id()];
    mhbs = mhbs + 1;
#endif
  }
  errno = saved_errno;
}

namespace { // private
  inline constinit thread_local timer_t heartbeat_timer;
  inline constinit itimerspec heartbeat_its_zero = {};
  
  consteval itimerspec init_heartbeat_its() {
    itimerspec its = {};
    its.it_value   .tv_nsec = HEARTBEAT_INTERVAL_US * 1000;
    its.it_interval.tv_nsec = HEARTBEAT_INTERVAL_US * 1000;
    return its;
  }
  inline constinit itimerspec heartbeat_its = init_heartbeat_its();
} // private

inline void start_heartbeats() noexcept {
  constinit static thread_local bool thread_initialized = false;
  if (!thread_initialized) { // only first time
    thread_initialized = true;
    SporkSlot::reset();

    struct sigaction sa = {};
    //sa.sa_sigaction = heartbeat_handler;
    //sa.sa_flags |= SA_SIGINFO;
    sa.sa_flags |= SA_RESTART;
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

inline void pause_heartbeats() noexcept {
  timer_settime(heartbeat_timer, 0, &heartbeat_its_zero, nullptr);
}

template <typename PromLambda>
struct PromSpork : PromFn {
  const PromLambda&& prom;
  PromSpork(const PromLambda&& _prom) :
    prom(fwd(_prom)) {}
  void operator()() const override {
    fwd(prom)();
  }
};

// TODO: exception handling
// Look into parlay's `padded<...>`
template <typename BodyLambda, typename PromLambda>
__attribute__((always_inline))
inline bool with_prom_handler(const BodyLambda&& body, const PromLambda&& prom) {
  static_assert(std::is_invocable_v<BodyLambda&&>);
  static_assert(std::is_invocable_v<PromLambda&&>);

  const PromSpork<PromLambda> promfn(fwd(prom));

  SporkSlot slot(&promfn);
  if (heartbeat_tokens) [[unlikely]]
    slot.eager_promote(fwd(prom));
  fwd(body)();
  return slot.close();
}
}

#endif // SPORK_SCHEDULER_H_
