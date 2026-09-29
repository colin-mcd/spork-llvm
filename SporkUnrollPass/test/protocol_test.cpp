// Standalone exercise of the Spork unroll protocol without the scheduler.
// Each loop below has the exact shape parfor.hpp produces; the "handler"
// is simulated by never shortening the bound, so results must equal the
// plain sum.  Build with the plugin and run; prints "ok" on success.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <signal.h>

extern "C" void __spork_unroll_loop(const void* site) noexcept;
extern "C" unsigned int __spork_get_unroll_factor(const void* site) noexcept;

// One instantiation, inlined into two callers: both markers share the token.
__attribute__((always_inline)) inline long
sum_shared(const int* data, long i, long j, unsigned& factor) {
  static char site;
  volatile sig_atomic_t progress = i;
  volatile long loop_end = j;
  long a = 0;
  __spork_unroll_loop(&site);
  for (; i < loop_end;) {
    a += data[i];
    ++i;
    progress = static_cast<sig_atomic_t>(i);
  }
  factor = __spork_get_unroll_factor(&site);
  return a;
}
__attribute__((noinline)) long caller_a(const int* d, long n, unsigned& f) {
  return sum_shared(d, 0, n, f);
}
__attribute__((noinline)) long caller_b(const int* d, long n, unsigned& f) {
  return sum_shared(d, 3, n, f);
}

// Unsigned index, no casts between induction and progress/bound.
__attribute__((noinline)) long sum_unsigned(const int* data, unsigned n,
                                            unsigned& factor) {
  static char site;
  unsigned i = 0;
  volatile unsigned progress = i;
  volatile unsigned loop_end = n;
  long a = 0;
  __spork_unroll_loop(&site);
  for (; i < loop_end;) {
    a += data[i] * 3;
    ++i;
    progress = i;
  }
  factor = __spork_get_unroll_factor(&site);
  return a;
}

// Body with a conditional (multi-block loop body) and a float reduction.
__attribute__((noinline)) double sum_branchy(const float* data, int n,
                                             unsigned& factor) {
  static char site;
  int i = 0;
  volatile sig_atomic_t progress = i;
  volatile int loop_end = n;
  double a = 0;
  __spork_unroll_loop(&site);
  for (; i < loop_end;) {
    if (data[i] > 0.5f) a += data[i];
    ++i;
    progress = i;
  }
  factor = __spork_get_unroll_factor(&site);
  return a;
}

// Strided store body: out[i] = in[i] * 2 (no reduction).
__attribute__((noinline)) void scale(const long* in, long* out, long n,
                                     unsigned& factor) {
  static char site;
  long i = 0;
  volatile sig_atomic_t progress = i;
  volatile long loop_end = n;
  __spork_unroll_loop(&site);
  for (; i < loop_end;) {
    out[i] = in[i] * 2;
    ++i;
    progress = static_cast<sig_atomic_t>(i);
  }
  factor = __spork_get_unroll_factor(&site);
}

int main() {
  const int N = 100003;
  int* d = new int[N];
  float* f = new float[N];
  long* in = new long[N];
  long* out = new long[N];
  for (int k = 0; k < N; ++k) {
    d[k] = (k * 7919) % 101 - 50;
    f[k] = static_cast<float>((k * 31) % 97) / 97.0f;
    in[k] = k;
  }
  bool ok = true;
  auto check = [&](const char* name, long got, long want, unsigned factor) {
    std::printf("%-12s factor %2u  %s\n", name, factor,
                got == want ? "ok" : "MISMATCH");
    if (got != want) ok = false;
  };
  for (long n : {0L, 1L, 5L, 15L, 16L, 17L, 63L, 64L, 65L, 1000L, (long)N}) {
    long want0 = 0, want3 = 0, wantu = 0;
    double wantb = 0;
    for (long k = 0; k < n; ++k) {
      want0 += d[k];
      if (k >= 3) want3 += d[k];
      wantu += d[k] * 3;
      if (f[k] > 0.5f) wantb += f[k];
    }
    unsigned fa, fb, fu, fbr, fs;
    check("shared/a", caller_a(d, n, fa), want0, fa);
    check("shared/b", caller_b(d, n, fb), want3, fb);
    check("unsigned", sum_unsigned(d, (unsigned)n, fu), wantu, fu);
    double gotb = sum_branchy(f, (int)n, fbr);
    check("branchy", gotb == wantb, true, fbr);
    scale(in, out, n, fs);
    bool sok = true;
    for (long k = 0; k < n; ++k) sok &= (out[k] == in[k] * 2);
    check("scale", sok, true, fs);
    if (fa != fb) { std::printf("shared token factors differ\n"); ok = false; }
  }
  std::printf("%s\n", ok ? "ALL OK" : "FAILED");
  return ok ? 0 : 1;
}
