cmake_minimum_required(VERSION 3.20)
project(RtsSporkPass CXX C)

# ── Find LLVM ────────────────────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
include(HandleLLVMOptions)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

# ── Pass shared library (new PM / -fpass-plugin) ─────────────────────────────
add_llvm_library(RtsSporkPass MODULE
    RtsSporkPass.cpp
    PLUGIN_TOOL opt
)

set_target_properties(RtsSporkPass PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(RtsSporkPass PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fno-rtti   # match LLVM build flags
)

# ── Optional: link as a static library for in-tree use ───────────────────────
# add_llvm_library(RtsSporkPassStatic STATIC RtsSporkPass.cpp)

# ── Notes on machine pass registration ───────────────────────────────────────
#
# RtsSporkMachinePass must be injected into the target machine's post-regalloc
# pipeline.  There are two ways to do this:
#
# 1. In-tree (modify llvm/lib/Target/<Arch>/<Arch>TargetMachine.cpp):
#    In addPostRegAlloc(), call:
#        addPass(createRtsSporkMachinePass());
#    and expose createRtsSporkMachinePass() from a header in your pass dir.
#
# 2. Out-of-tree via TargetMachine callbacks (LLVM 17+):
#    Register a TargetMachineRegistrationCallback that inserts the pass
#    after PrologEpilogCodeInserter in the machine pass pipeline.
#    Example (in the plugin init lambda):
#
#    PB.registerTargetMachineCallback(
#        [](TargetMachine &TM) {
#            TM.addPostRegAlloc = [&](PassManagerBase &PM) {
#                PM.add(new RtsSporkMachinePass());
#            };
#        });
#
# See docs/WritingAnLLVMPass.rst for the full legacy PM injection API.