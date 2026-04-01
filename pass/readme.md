# RtsSporkPass — LLVM Pass: Stack-Frame Offset Table for `__RTS_record_spork`

## Overview

This out-of-tree LLVM pass finds every call to

```cpp
static void __RTS_record_spork(
    volatile bool*  promotable_flag,   // arg 0
    volatile uint*  num_promotions,    // arg 1
    void*           prom,              // arg 2
    bool (*exec_prom)(void*)           // arg 3
) noexcept;
```

and records the **signed byte offsets of each pointer argument relative to
the calling function's frame base** (CFA / `%rbp` on x86-64) into a
statically-allocated global table that is available at link time and at
runtime.

---

## Design

Stack-frame offsets are not known until the backend has performed register
allocation and prologue/epilogue insertion. The pass therefore works in two
phases.

### Phase 1 — IR-level `ModulePass` (`RtsSporkIRPass`)

Runs early, before SROA and alias analysis, while `alloca` instructions are
still clearly visible.

1. Walks every `CallInst` in the module looking for `__RTS_record_spork`.
2. For each call, traces each of the four pointer arguments back through
   `bitcast` / `addrspacecast` / zero-offset `getelementptr` chains to the
   underlying `AllocaInst`.
3. Assigns a monotonically-increasing **call-site ID** (`uint64_t`).
4. Annotates the `CallInst` with `!rts_spork_id = !{i64 <id>}`.
5. Annotates each `AllocaInst` with
   `!rts_spork_slot = !{i64 <site_id>, i64 <arg_index>}`.
6. Emits two globals:
   ```c
   struct RtsSporkSite { int64_t offsets[4]; };
   RtsSporkSite __rts_spork_table[N];      // zero-initialised sentinel
   const uint64_t __rts_spork_table_size = N;
   ```
7. Emits `__rts_spork_init()` and registers it as a
   `@llvm.global_ctors` priority-0 constructor.  The function stores
   `INT64_MIN` (sentinel) into every slot so that partially-patched tables
   are detectable.

### Phase 2 — `MachineFunctionPass` (`RtsSporkMachinePass`)

Runs **after** `PrologEpilogCodeInserter` (i.e., after register allocation),
when `MachineFrameInfo` has final slot assignments.

1. Iterates over IR `AllocaInst`s in the current function, reading
   `!rts_spork_slot` metadata.
2. Looks up each `AllocaInst` in `MachineFrameInfo::getObjectAllocation()`
   to obtain the `FrameIndex`.
3. Calls `TargetFrameLowering::getFrameIndexReference()` to get the
   signed byte offset from the frame base register.
4. Patches `__rts_spork_table[site_id].offsets[arg_idx]` in-place by
   updating the `GlobalVariable`'s `ConstantArray` initializer.

Because the table is a writable global, these updates are reflected both in
the binary (`.data` section) and at runtime through the `__rts_spork_init`
constructor.

---

## File Layout

```
RtsSporkPass/
├── RtsSporkPass.cpp      # Pass implementation (both phases)
├── rts_spork_table.h     # Public C/C++ header for table consumers
├── test_spork.c          # Smoke-test / example
├── CMakeLists.txt        # Out-of-tree build
└── README.md             # This file
```

---

## Building

### Prerequisites

- LLVM ≥ 17 (built with CMake, `llvm-config` on `$PATH`)
- CMake ≥ 3.20
- A C++17-capable compiler

### Steps

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DLLVM_DIR=$(llvm-config --cmakedir)
make -j$(nproc)
# Produces: build/RtsSporkPass.so  (Linux) or build/RtsSporkPass.dylib (macOS)
```

---

## Usage

```bash
# Compile your code with the pass injected
clang -fpass-plugin=./build/RtsSporkPass.so \
      -O1 -g \
      your_code.c -o your_program

# The table is populated before main() via the constructor.
# Include the header to access it:
```

```c
#include "rts_spork_table.h"

// At any point after program startup:
for (uint64_t i = 0; i < __rts_spork_table_size; ++i) {
    printf("site %llu: flag_offset=%lld\n",
           (unsigned long long)i,
           (long long)__rts_spork_table[i].offsets[0]);
}
```

---

## Machine Pass Integration

The `RtsSporkMachinePass` must be inserted into the backend pipeline **after**
`PrologEpilogCodeInserter`. There are two ways:

### 1. In-tree (modify the target machine)

In `llvm/lib/Target/<Arch>/<Arch>TargetMachine.cpp`, inside `addPostRegAlloc`:

```cpp
bool <Arch>PassConfig::addPostRegAlloc() {
    addPass(createRtsSporkMachinePass());   // ← add this
    return TargetPassConfig::addPostRegAlloc();
}
```

Expose `createRtsSporkMachinePass()` from a header, e.g.:

```cpp
// RtsSporkMachinePass.h
#pragma once
#include "llvm/CodeGen/MachineFunctionPass.h"
namespace llvm {
  MachineFunctionPass *createRtsSporkMachinePass();
}
```

### 2. Out-of-tree via `registerTargetMachineCallback` (LLVM ≥ 17)

Add to the plugin's `PassBuilder` callback:

```cpp
PB.registerTargetMachineCallback([](TargetMachine &TM) {
    TM.addPostRegAlloc([](legacy::PassManagerBase &PM) {
        PM.add(new RtsSporkMachinePass());
    });
});
```

---

## Metadata Protocol

| Metadata kind     | Attached to  | Format                          |
|-------------------|--------------|---------------------------------|
| `rts_spork_id`    | `CallInst`   | `!{ i64 <site_id> }`            |
| `rts_spork_slot`  | `AllocaInst` | `!{ i64 <site_id>, i64 <arg> }` |

---

## Known Limitations / Future Work

| Issue | Notes |
|---|---|
| Non-alloca arguments | If an argument is not directly traceable to a static `alloca` (e.g., heap pointer, function parameter), the offset is set to `INT64_MIN`. A warning is printed. |
| SROA interaction | Run Phase 1 **before** SROA (pipeline start EP) to ensure allocas are still whole. |
| Shared allocas | If the same `alloca` is passed to multiple call sites with different `arg_index` values, only the last `!rts_spork_slot` annotation is preserved. Fix: use a list-typed metadata node. |
| Dynamic allocas | `alloca` with non-constant size has no fixed frame offset; treated as unknown. |
| GEP with non-zero offset | The pass follows all GEPs to the base alloca but records the *alloca's* frame offset, not the field's. If you need the exact field offset, extend `resolveToAlloca` to accumulate the GEP byte delta. |
| Cross-function sharing | The machine pass runs per-function; global table updates are serialised by the constant-initializer mechanism, which is not thread-safe during compilation. This is fine for single-threaded compilation. |

---

## Table Layout in Memory

```
__rts_spork_table:
  [0] { offsets: [ -8, -16, -24, -32 ] }   ← site 0 (do_spork_a)
  [1] { offsets: [ -8, -16, -16, -24 ] }   ← site 1 (do_spork_b)
  ...
__rts_spork_table_size: 2
```

All offsets are **signed 64-bit integers in bytes**, measured from the
frame base.  Negative means below `%rbp` (typical for locals on x86-64).
