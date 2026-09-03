# Spork unroll pass (probe-driven)

`fablepass/SporkUnrollPass.cpp` is an LLVM new-pass-manager plugin for the
interruptible loop in Spork's `parfor`. It is an alternative to `gempass`,
which asks LLVM's unroll cost model for a count and then rewrites LLVM's
unrolled clone chain by hand. That design cannot take advantage of
compiler-introduced vectorization. This pass instead lets LLVM's real
vectorizer decide, and then reuses LLVM's own output:

1. **Probe.** For each marked loop the pass outlines a *sequential* copy into
   a scratch function: the volatile progress store is dropped, the volatile
   bound becomes a loop-invariant parameter, and the induction is normalized
   to count from zero. It then runs the passes clang runs later
   (`LoopVectorize`, `LoopLoadElimination`, `InstCombine`, `SimplifyCFG`,
   `SLPVectorizer`, `VectorCombine`) on that function. Every loop that
   survives (main vector loop, epilogue vector loop, scalar remainder) is a
   *step loop* that advances the index by a constant step; the largest step
   is the batch `K`.
2. **Transplant.** If the probe vectorized, the probe *is* the new loop. Each
   step loop gets a fresh volatile bound check before it is entered and at
   its latch, publishes progress after every step, and every value LLVM
   derived from the trip-count snapshot (resume indices, "no remainder"
   tests) is replaced by the real exit index. The rewritten probe is called
   in place of the original loop and inlined. The batched loop is therefore
   exactly LLVM's sequential code plus the protocol: the same vector
   accumulators live across the whole range, and the horizontal reduction
   happens once at the end, not once per batch.
3. **Fallback.** If the probe only unrolled, the loop is restructured into an
   outer batch loop (one volatile store and one bound check per `K`
   iterations) around an inner loop with the constant trip count `K` that
   the real unroller then flattens, plus a scalar cleanup copy.
4. **Publish.** Every `__spork_get_unroll_factor(site)` call is replaced with
   the constant `K`.

Both rewrites maintain the invariant the promotion handler relies on: a step
of `s <= K` iterations is executed only if `i + s - 1 < loop_end` held on a
fresh volatile load after the previous progress store. The handler sets
`loop_end = progress + K`, so any mix of step sizes up to `K` is correct.

## Source-level protocol

Identical to `gempass` (see `../gempass/README.md`): a compile-time-only
marker `__spork_unroll_loop(&site)` immediately before the loop and a
`__spork_get_unroll_factor(&site)` query in the promotion handler, keyed by a
function-local `static char site`. Both calls are removed; the declarations
are dropped once unused. Used by `../parfor.hpp` and
`../spork-parlaylib/include/parlay/internal/spork_parfor.h`.

## Generated shape (transplant)

For `sum` with `-march=native` the probe gives, and the loop becomes:

```
entry:      end0 = load volatile bound; n = end0 - start (snapshot)
            min-iteration and runtime alias checks on the snapshot
main.ph:    guard: start + 63 < load volatile bound
main:       4 x <16 x i16> accumulators, step 64
            store volatile (i + 64) -> progress; end = load volatile bound
            continue if i + 64 + 63 < end
main.exit:  reduce once; i.exit replaces n.vec everywhere after
epilog.ph:  guard: i.exit + 7 < load volatile bound
epilog:     <8 x i16>, step 8, same protocol
scalar.ph:  guard: i < load volatile bound
scalar:     original body, per-iteration store/load/check
```

The snapshot-based checks LLVM emitted stay in place: they are conservative
(the handler can only shrink `loop_end`, and only to `progress + K`) and every
loop entry is re-guarded with a fresh load. Values LLVM computed from the
snapshot before a loop and used after it are recomputed after the exit from
the real exit index.

## Generated shape (fallback)

```
preheader:            br batch.header
batch.header:         o_k = phi [init_k, preheader], [latch_k, batch.latch]
                      fits = !overflow(o_i + K-1) && (o_i + K-1) < load volatile bound
                      br fits, header, cleanup.ph
header .. latch:      original blocks; j = 0..K-1 counter; no volatile ops
batch.latch:          store volatile (o_i + K) -> progress ; br batch.header
cleanup.ph:           ready = o_i < load volatile bound ; br ready, cleanup, exit
cleanup loop:         clone of the original loop (per-iteration volatile ops)
```

The batch guards use `sadd.with.overflow`/`uadd.with.overflow` matching the
signedness of the original comparison and replay any integer cast chains
that connected the induction value and the bound load to the exit compare.

## Placement

The pass registers at `registerOptimizerEarlyEPCallback`: after the function
simplification pipeline (inlining, SROA, LICM, IndVars, loop rotation) and
before the vector/unroll pipeline, so the marked loop already looks the way
the vectorizer will see it. `LoopSimplify` and `LCSSA` run before and after.
It is a module pass so that all markers sharing one site token are decided
together.

## Accepted loop shape

- Loop-simplify and LCSSA form, no nested loops.
- Reached from the marker block through barrier-free bridge blocks (the
  zero-trip guard and whatever loop unswitching left). Several loop copies
  under one marker are allowed if they use the same progress slot.
- Unique latch that is the only exiting block; unique exit block.
- LLVM-recognized integer induction with step `+1`.
- One volatile store in the latch publishing the next induction value
  (through integer casts) to a stack slot, and a `slt`/`ult` exit compare
  against a single-use volatile integer load (through integer casts) whose
  pointer is loop-invariant.
- No other volatile/atomic accesses, no calls except assume-like or
  memory-free intrinsics, nothing that may throw.
- Ordinary code never reads the progress slot inside or after the loop.
- Exit PHIs use only loop-invariant values or header-PHI latch values.
- Probe batch of at least 2 and at most `-spork-max-batch` (default 1024).

The transplant additionally requires every loop in the vectorized probe to
have one scalar integer induction of the index type, a single-use exit
compare of its next value against a loop-invariant limit, exit PHIs that are
header-PHI latch values, and a body that no longer depends on the trip-count
snapshot once limits are replaced (tail-folded loops are rejected). Loops
that fail these checks use the fallback rewrite with the same `K`.

All loop copies sharing a site token must produce the same batch, otherwise
the token publishes factor 1 and nothing is transformed.

## Options (`-mllvm ...`)

| Option | Meaning |
|---|---|
| `-spork-unroll-verbose` | Print the decision and reject reason per site |
| `-spork-unroll-dump-probe` | Print each probe after its pipeline, and rejected loops |
| `-spork-unroll-verify` | Run the IR verifier right after the pass |
| `-spork-force-batch=N` | Skip the probe; use the fallback rewrite with batch `N` |
| `-spork-inner-hints=false` | Fallback only: no width/interleave hints on the inner loop |
| `-spork-max-batch=N` | Largest batch accepted from the probe |

## Building and using

```sh
cmake -S fablepass -B fablepass/build \
  -DLLVM_DIR="$PWD/llvm-project/build/lib/cmake/llvm" -DCMAKE_BUILD_TYPE=Release
cmake --build fablepass/build -j8
llvm-project/build/bin/clang++ -O3 -fpass-plugin="$PWD/fablepass/build/SporkUnroll.so" ...
```

The repository `Makefile` uses this plugin by default (`make sum`); pass
`PASS=gempass` to build with the old one. With `opt` the pipeline name is
`spork-unroll`. `test/protocol_test.cpp` exercises the protocol standalone
(shared token across two inlined copies, unsigned index, cast-free index,
multi-block body, store-only body) and prints `ALL OK`.

## Results on `sum.cpp` (800M shorts, `-march=native`)

| Build | 1 worker | 20 workers |
|---|---|---|
| `seqsum` (sequential loop) | 63 ms | n/a |
| gempass | | ~30 ms |
| fablepass, fallback rewrite (batch 64) | 80 ms | 26 ms |
| fablepass, transplant (batch 64 = VF 16 x IC 4) | 64 ms | 26 ms |

The single-worker number is the one that matters for code quality: the
transplanted loop is LLVM's own vectorized loop, so it runs at the speed of
the sequential version. With 20 workers everything is DRAM-bandwidth bound.
