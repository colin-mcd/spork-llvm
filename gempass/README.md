# Spork unroll pass

`SporkUnroll` is an LLVM new-pass-manager plugin for the interruptible loop in
Spork's `parfor`. It lets LLVM choose a target-specific unroll factor while
preserving the loop's asynchronous-promotion protocol.

The source loop publishes its progress after every iteration and reloads a
volatile bound that a heartbeat handler may shorten. Those operations make the
protocol correct, but they also prevent LLVM from treating several iterations
as one efficient batch. This pass turns a supported loop into:

1. an unrolled main loop that executes complete batches;
2. one volatile progress update and bound check per batch; and
3. an untouched scalar cleanup loop for the final partial batch.

The chosen batch size is also substituted into the promotion handler, so the
handler never splits work in the middle of an unrolled batch.

## Source-level protocol

The producer and consumer communicate through two compile-time-only calls:

```cpp
extern "C" void __spork_unroll_loop(const void* site) noexcept;
extern "C" unsigned int
__spork_get_unroll_factor(const void* site) noexcept;
```

Each static loop site supplies a unique global token, currently a function-local
`static char` in Spork's `parfor` implementation:

```cpp
static char unroll_site;

__spork_unroll_loop(&unroll_site);
for (; i < loop_end;) {
  body(i, accumulator);
  ++i;
  sig_safe_i = i;
}

// In the asynchronous promotion callback:
auto increment = __spork_get_unroll_factor(&unroll_site);
```

The marker identifies the loop. The factor query identifies the corresponding
promotion calculation, even when inlining or outlining places it in a different
function. The pass removes both calls: the marker becomes a no-op and the factor
query becomes an integer constant.

The declarations are `noexcept` so Clang can represent them as non-unwinding
calls and retain the simple control-flow shape expected by the pass. No runtime
implementation of either function is needed after a successful plugin run.

The protocol is used by:

- `parfor.hpp`
- `spork-parlaylib/include/parlay/internal/spork_parfor.h`

## Transformation

The plugin runs at LLVM's optimizer-early extension point, after inlining and
before the normal loop and SLP vectorizers. It first runs `LoopSimplify` and
LCSSA formation, then processes the module as follows:

1. Count markers by site token. A token must identify exactly one static marker.
2. Match each marker to the loop reached through its canonical preheader.
3. Recognize the loop's induction variable, volatile progress store, volatile
   bound load, and signed or unsigned less-than exit test.
4. Ask LLVM's native target-aware unroll cost model for a factor.
5. Clone the original scalar loop for cleanup.
6. Use LLVM's `UnrollLoop` utility to create the checked clone chain.
7. Validate the resulting chain, remove intermediate volatile stores and bound
   checks, and add overflow-safe guards for complete batches.
8. Publish the effective factor by replacing every matching factor query.

The batch guard uses `sadd.with.overflow` or `uadd.with.overflow`, matching the
signedness of the original comparison. It also replays any integer cast chain
that originally connected the induction value or volatile bound load to the
exit comparison.

The cleanup loop starts either at the original initial value, when the first
full batch does not fit, or at the value following the last completed batch. It
retains per-iteration volatile progress and bound operations, so asynchronous
promotion remains valid near the end of the range.

## Accepted loop shape

The pass is deliberately conservative. Batching currently requires all of the
following:

- LoopSimplify form and LCSSA form.
- A unique global site token used by exactly one marker.
- A marker in the loop's canonical preheader, optionally separated from the
  header by one unconditional preheader bridge inserted by LoopSimplify.
- A single-block loop whose header is also its unique latch.
- A unique exit block with no PHI nodes.
- LLVM-recognized integer induction with a constant `+1` step.
- Exactly one volatile store in the latch, storing the next induction value.
- A conditional latch exit using signed or unsigned `<`.
- Exactly one volatile integer load feeding that exit condition, optionally
  through integer casts.
- Distinct stack slots for published progress and the mutable bound.
- No nested loops and no calls in the marked loop body.
- No ordinary reads of the progress slot in the loop or after the marker.
- An LLVM-selected unroll count of at least two, with no peeling decision.
- An unrolled clone chain whose progress stores and exits match the expected
  structure exactly.

These restrictions keep the transformation tied to Spork's concrete protocol
instead of treating arbitrary volatile loops as safe to batch.

## Safe fallback

Unsupported or ambiguous sites use factor one. For every recognized call, the
pass:

- removes the marker;
- replaces an unmatched or rejected factor query with `1`; and
- restores tagged progress stores to volatile if the final clone-chain
  validation fails.

Reusing one token for multiple markers is rejected. If multiple observations of
a token would produce different factors, the published factor is forced to one.
Thus a well-formed recognized protocol does not leave either compile-time-only
symbol for the linker.

A malformed call whose signature does not match the protocol is not recognized
and is intentionally left alone; that normally produces a link error instead
of silently changing unknown code.

## Building

The plugin must be compiled against the same LLVM build that will load it.
LLVM pass plugins are in-process C++ extensions and are not ABI-compatible
across different LLVM major versions—or necessarily across different revisions
of the same development version.

From the repository root:

```sh
cmake -S gempass -B gempass/build \
  -DLLVM_DIR="$PWD/llvm-project/build/lib/cmake/llvm"
cmake --build gempass/build -j8
```

The output is:

```text
gempass/build/SporkUnroll.so
```

If the build directory was previously configured against another LLVM checkout,
reconfigure it with the correct `LLVM_DIR` and verify the cached path:

```sh
rg '^LLVM_DIR' gempass/build/CMakeCache.txt
```

The LLVM headers in this repository use the LLVM 24 `CondBrInst`,
`UncondBrInst`, `OptionalPassInfoMixin`, and `computeUnrollCount` APIs.

## Loading with Clang

Load the plugin while compiling optimized C++:

```sh
llvm-project/build/bin/clang++ \
  -O3 \
  -fpass-plugin="$PWD/gempass/build/SporkUnroll.so" \
  source.cpp -o program
```

Spork code containing the marker calls should always be compiled with the
plugin. Without it, the compile-time-only symbols will ordinarily remain
undefined at link time.

## Running explicitly with `opt`

The registered pipeline name is `spork-unroll`:

```sh
llvm-project/build/bin/opt \
  -load-pass-plugin=gempass/build/SporkUnroll.so \
  -passes=spork-unroll \
  input.ll -S -o output.ll
```

The explicit pipeline also runs `LoopSimplify` and LCSSA before the module
transformation.

## Relevant files

- `SporkUnrollPass.cpp`: matching, unroll selection, CFG rewriting, fallback,
  and plugin registration.
- `CMakeLists.txt`: out-of-tree LLVM pass-plugin build.
- `../parfor.hpp`: standalone Spork `parfor` protocol.
- `../spork-parlaylib/include/parlay/internal/spork_parfor.h`: Parlay-integrated
  protocol used by PBBS.
