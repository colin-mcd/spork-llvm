/**
 * RtsSporkPass.cpp
 *
 * Two-phase LLVM pass that finds all calls to:
 *
 *   extern void __RECORD_SPORK(
 *     void* promotable_flag,  // arg 0: local var address
 *     void* num_promotions,   // arg 1: local var address
 *     void* prom,             // arg 2: local var address
 *     void* exec_prom,        // arg 3: global var address (constant)
 *     void* beg,              // arg 4: label address (constant)
 *     void* end               // arg 5: label address (constant)
 *   ) noexcept;
 *
 * and builds a static, global table that maps each call-site (identified by
 * a unique integer ID) to the stack-frame offsets of the four pointer
 * arguments.
 *
 * Because stack-frame offsets are only known after register allocation, the
 * work is split across two passes:
 *
 *   Phase 1 – RtsSporkIRPass  (ModulePass / FunctionPass at IR level)
 *     • Finds every call to __RECORD_SPORK.
 *     • Traces each pointer argument back to its AllocaInst.
 *     • Assigns a unique call-site ID (stored as !rts_spork_id metadata on
 *       the CallInst, and as !rts_spork_slot metadata on every referenced
 *       AllocaInst so the backend pass can recover the slot index).
 *     • Creates the global table:
 *         struct RtsSporkSite {
 *             int64_t offset[4];   // filled by phase 2
 *         };
 *         RtsSporkSite __rts_spork_table[N];
 *         uint64_t     __rts_spork_table_size = N;
 *
 *   Phase 2 – RtsSporkMachinePass  (MachineFunctionPass)
 *     • Iterates over MachineInstr-level call instructions.
 *     • For each call that carries !rts_spork_id metadata, looks up the four
 *       AllocaInsts (via !rts_spork_slot) in MachineFrameInfo to get their
 *       final byte offsets relative to the frame base (CFA).
 *     • Emits a small __attribute__((constructor)) stub (or uses a
 *       MachineFunction-level initialiser) to write those offsets into
 *       __rts_spork_table at program start.
 *
 * Build (out-of-tree, LLVM ≥ 17):
 *   clang++ -std=c++17 -fPIC -shared \
 *     $(llvm-config --cxxflags) \
 *     RtsSporkPass.cpp -o RtsSporkPass.so
 *
 *   clang -fpass-plugin=./RtsSporkPass.so  -O1  your_code.c  -o out
 *
 * The table is then available at link time via the extern declarations in
 * rts_spork_table.h (see companion header).
 */

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Target/TargetOptions.h"

#include <cstdint>
#include <vector>
#include <string>

#define DEBUG_TYPE "rts-spork"

using namespace llvm;

// name of extern function call to look for
static constexpr char SPORK_UNROLL_FACTOR_NAME[] = "__spork_unroll_factor";

/* Helpers */

// Determines if all indices of GEP are zero (i.e. no actual displacement).
static bool GEPAllZero(GetElementPtrInst *GEP) {
  for (Use &Idx : GEP->indices()) {
    ConstantInt *CI = dyn_cast<ConstantInt>(Idx);
    if (CI == nullptr || !CI->isZero()) {
      return false;
    }
  }
  return true;
}

// Strip pointer casts / GEPs with zero offset to find the underlying instruction.
// Returns nullptr if we cannot prove the value originates from an I.
template <typename I>
static I *resolveTo(Value *V) {
  if (I *i = dyn_cast<I>(V)) {
    return i;
  } else if (BitCastInst *BC = dyn_cast<BitCastInst>(V)) {
    return resolveTo<I>(BC->getOperand(0));
  } else if (AddrSpaceCastInst *AC = dyn_cast<AddrSpaceCastInst>(V)) {
    return resolveTo<I>(AC->getOperand(0));
  } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V)) {
    // Only follow if all indices are zero (i.e. no actual displacement).
    if (GEPAllZero(GEP)) return resolveTo<I>(GEP->getPointerOperand());
  }
  // anything else: give up
  return nullptr;
}

/* Phase 1 - IR-level Module Pass */

struct RtsSporkIRPass : public PassInfoMixin<RtsSporkIRPass> {

  struct SporkLoopInfo {
    CallInst *CI;
    uint64_t id;
    AllocaInst* i;
    AllocaInst* j;
    Loop* l;
  };
  
  using SporkNest = std::vector<SporkLoopInfo*>;

  void tryRecordSporkLoopInfo(const Instruction* I, std::vector<SporkLoopInfo>& sporks, const Function& F) {
    CallInst *CI = dyn_cast<CallInst>(&I);
    if (!CI) return;

    Function *Callee = CI->getCalledFunction();
    if (!Callee || Callee->getName() != SPORK_UNROLL_FACTOR_NAME) return;

    if (CI->arg_size() != 2) {
      errs() << "[spork] WARNING: call to " << SPORK_UNROLL_FACTOR_NAME
             << " has only " << CI->arg_size() << " args, skipping.\n";
      return;
    }

    SporkLoopInfo si;
    si.i = resolveTo<AllocaInst>(CI->getArgOperand(0));
    si.j = resolveTo<AllocaInst>(CI->getArgOperand(1));
    if (!si.i || !si.j) {
      errs() << "[spork] WARNING: arg(s) of call at " << F.getName()
             << " could not be resolved to a store, skipping.\n";
      return;
    }

    si.CI = CI;
    si.id = (uint64_t) sporks.size();
    sporks.push_back(si);
  }

  // Populates `sites` with the SporkInfo corresponding
  // to each `SPORK_UNROLL_FACTOR_NAME` function call in a module
  void populateSporkInfo(const Module &M, std::vector<SporkLoopInfo> &sites) {
    for (const Function &F : M)
      for (const BasicBlock &B : F)
        for (const Instruction &I : B)
          tryRecordSporkLoopInfo(&I, sites, F);
  }

  // void populateSporkNest(Module &M, const std::vector<SporkLoopInfo> &sites) {
  //   GlobalVariable *GA = M.getNamedGlobal("llvm.global.annotations");
  //   if (!GA) return;

  //   ConstantArray *CA = dyn_cast<ConstantArray>(GA->getOperand(0));
  //   for (unsigned i = 0; i < CA->getNumOperands(); ++i) {
  //     auto *CS = dyn_cast<ConstantStruct>(CA->getOperand(i));

  //     // Operand 1 = annotation string
  //     Constant *AnnoPtr = CS->getOperand(1);
  //     GlobalVariable *AnnoGV = cast<GlobalVariable>(cast<ConstantExpr>(AnnoPtr)->getOperand(0));
  //     ConstantDataArray *AnnoData = cast<ConstantDataArray>(AnnoGV->getInitializer());

  //     std::string AnnoStr = AnnoData->getAsCString().str();

  //     if (AnnoStr == "spork_unroll") {
  //       // Operand 0 = annotated entity
  //       Value *Annotated = CS->getOperand(0);

  //       // Instruction *I = dyn_cast<Instruction>(Annotated);
  //       // if (I == nullptr) continue;
        
  //       AllocaInst *AI = dyn_cast<AllocaInst>(Annotated);
  //       if (AI == nullptr) continue;
  //       BasicBlock *b = AI->getParent();

  //       // Operands 2 and 3 = file and line

  //       // Operand 4 = extra args (your labels)
  //       Value *Extra = CS->getOperand(4);

  //       // You'll need to peel this apart depending on how Clang encoded it
  //     }
  //   }
  //   // for (Function &F : M) {
  //   //   for (BasicBlock &B : F) {
  //   //     for (Instruction &I : B) {
  //   //       MDNode* meta = I.getMetadata("spork_range");
  //   //       for (auto mo = meta->op_begin(); mo < meta->op_end(); mo++) {
  //   //         Metadata *md = mo->get();
  //   //         md->
  //   //       }
  //   //       if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
            
  //   //       }
  //   //     }
  //   //   }
  //   // }
  // }

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    LLVMContext &Ctx = M.getContext();

    // Collect all calls to __RECORD_SPORK across the module
    std::vector<SporkLoopInfo> sites;
    populateSporkInfo(M, sites);
    // Now remove those calls
    //for (SporkLoopInfo sli : sites) sli.CI->removeFromParent();
    // TODO: replace each sli.CI with the unroll factor!

    if (sites.empty()) {
      LLVM_DEBUG(dbgs() << "[spork] No calls found.\n");
      return PreservedAnalyses::all();
    }

    uint64_t N = sites.size();
    LLVM_DEBUG(dbgs() << "[spork] Found " << N << " call site(s).\n");

    return PreservedAnalyses::none(); // we modified the IR
  }
};

// -----------------------------------------------------------------------------
// Phase 2 – MachineFunctionPass
// -----------------------------------------------------------------------------
//
// Runs after register allocation, when MachineFrameInfo has final slot offsets.
// For each function that contains annotated AllocaInsts:
//   1. Walk alloca instructions (via IR), read !rts_spork_slot metadata.
//   2. Map each alloca to its FrameIndex via MachineFrameInfo.
//   3. Ask TargetFrameLowering for the signed byte offset from the frame base.
//   4. Patch __rts_spork_init's MachineBasicBlock to store the real offset.
//
// NOTE: Mapping IR AllocaInst → MachineFrameInfo slot is done via the
//       FunctionLoweringInfo / SelectionDAGISel side-table.  We access it
//       through the standard "alloca frame index map" kept in
//       MachineFunction::getFrameInfo() — specifically by finding the
//       MachineFrameInfo entry whose isStaticAlloca() flag is true and whose
//       ordering matches the IR alloca (the canonical approach used by SROA,
//       StackColoring, etc.).
//
//       A simpler—and more robust—alternative used here is to emit a
//       llvm.dbg.addr / llvm.dbg.declare-style intrinsic that the backend
//       translates into a DBG_VALUE whose operand is already a FrameIndex.
//       We take that route via MachineFunction::getVariableDbgInfo().

struct RtsSporkMachinePass : public MachineFunctionPass {
  static char ID;
  RtsSporkMachinePass() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "RTS Spork Frame-Offset Recorder (Machine)";
  }

  // We need MachineFrameInfo to be finalised, so run after PrologEpilogInserter.
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    const Function &F = MF.getFunction();
    LLVMContext    &Ctx = F.getContext();

    //unsigned MDSiteIdKind  = Ctx.getMDKindID(kMDSiteId);
    unsigned MDSlotIdxKind = Ctx.getMDKindID(kMDSlotIdx);

    // Collect (site_id, arg_index, AllocaInst*) triples from this function's IR.
    struct SlotEntry {
      uint64_t    siteId;
      uint64_t    argIdx;
      const AllocaInst *AI;
    };
    SmallVector<SlotEntry, 16> slots;

    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const AllocaInst *AI = dyn_cast<AllocaInst>(&I);
        if (!AI) continue;
        MDNode *MD = AI->getMetadata(MDSlotIdxKind);
        if (!MD || MD->getNumOperands() < 2) continue;

        uint64_t siteId = mdconst::extract<ConstantInt>(
                              MD->getOperand(0))->getZExtValue();
        uint64_t argIdx = mdconst::extract<ConstantInt>(
                              MD->getOperand(1))->getZExtValue();
        slots.push_back({siteId, argIdx, AI});
      }
    }

    if (slots.empty()) return false;

    // Build a map from AllocaInst* → FrameIndex using the IR-to-FI mapping
    // stored in MachineFunction.  LLVM exposes this through
    // MachineFunction::getObjectIndexBegin() / getObjectIndexEnd() plus
    // MachineFrameInfo::getObjectAllocation().
    MachineFrameInfo &MFI = MF.getFrameInfo();
    const TargetFrameLowering *TFL =
        MF.getSubtarget().getFrameLowering();

    DenseMap<const AllocaInst *, int> AllocaToFI;
    for (int FI = MFI.getObjectIndexBegin(),
             FE = MFI.getObjectIndexEnd(); FI != FE; ++FI) {
      const AllocaInst *AI = MFI.getObjectAllocation(FI);
      if (AI) AllocaToFI[AI] = FI;
    }

    // Now compute byte offsets and stash them for the init function.
    // We can't easily patch MachineInstrs of a *different* MachineFunction
    // (the init function) from here, so instead we write the offsets into the
    // GlobalVariable directly as a ConstantArray update.
    //
    // That requires getting hold of the Module. We do so through the IR
    // Function reference we already have.
    Module *M = const_cast<Module *>(F.getParent());
    GlobalVariable *GTable = M->getGlobalVariable(kTableName);
    if (!GTable) {
      errs() << "[spork] ERROR: global table not found – did IR pass run?\n";
      return false;
    }

    // Read current initialiser (an array of structs of arrays).
    ConstantArray *OldInit = cast<ConstantArray>(GTable->getInitializer());
    // Make a mutable copy of the constant elements.
    uint64_t N = OldInit->getType()->getNumElements();

    // We represent each site as: { int64_t offsets[4] }
    // Decode → modify → re-encode.

    // Decode all current values.
    struct RawSite { int64_t off[kNumSporkArgs]; };
    std::vector<RawSite> rawSites(N);

    for (uint64_t s = 0; s < N; ++s) {
      ConstantStruct *SiteConst = cast<ConstantStruct>(OldInit->getOperand(s));
      ConstantArray  *OffArr    = cast<ConstantArray>(SiteConst->getOperand(0));
      for (unsigned a = 0; a < kNumSporkArgs; ++a) {
        rawSites[s].off[a] =
            cast<ConstantInt>(OffArr->getOperand(a))->getSExtValue();
      }
    }

    // Patch entries that belong to this function.
    bool changed = false;
    for (SlotEntry &se : slots) {
      if (se.siteId >= N) continue;
      auto it = AllocaToFI.find(se.AI);
      if (it == AllocaToFI.end()) {
        errs() << "[spork] WARNING: no FrameIndex for alloca in "
               << F.getName() << " (site " << se.siteId << " arg "
               << se.argIdx << ")\n";
        continue;
      }

      int FI = it->second;
      // getObjectOffset returns offset from the *stack pointer* at entry.
      // We want the offset from the frame base (CFA / frame pointer).
      // TargetFrameLowering::getFrameIndexReference gives us the correct
      // register + offset pair.
      StackOffset SO = TFL->getFrameIndexReference(MF, FI,
                           /*FrameReg=*/ *new Register());
      // SO.getFixed() is the fixed-size component in bytes.
      int64_t byteOffset = SO.getFixed();

      rawSites[se.siteId].off[se.argIdx] = byteOffset;
      changed = true;

      LLVM_DEBUG(dbgs() << "[spork] site=" << se.siteId
                        << " arg=" << se.argIdx
                        << " FI=" << FI
                        << " offset=" << byteOffset << "\n");
    }

    if (!changed) return false;

    // Re-encode back into a ConstantArray.
    LLVMContext &ICtx = M->getContext();
    Type *I64Ty = Type::getInt64Ty(ICtx);
    Type *I64x4 = ArrayType::get(I64Ty, kNumSporkArgs);
    StructType *SiteTy = cast<StructType>(cast<ArrayType>(GTable->getValueType())->getElementType());

    SmallVector<Constant *, 64> NewSites;
    for (uint64_t s = 0; s < N; ++s) {
      SmallVector<Constant *, 4> OffElts;
      for (unsigned a = 0; a < kNumSporkArgs; ++a)
        OffElts.push_back(ConstantInt::get(I64Ty, rawSites[s].off[a]));
      Constant *OffArr = ConstantArray::get(cast<ArrayType>(I64x4), OffElts);
      NewSites.push_back(ConstantStruct::get(SiteTy, {OffArr}));
    }

    Constant *NewInit = ConstantArray::get(
        cast<ArrayType>(GTable->getValueType()), NewSites);
    GTable->setInitializer(NewInit);

    return false; // we only changed a GlobalVariable, not MachineInstrs
  }
};

char RtsSporkMachinePass::ID = 0;

static RegisterPass<RtsSporkMachinePass> RegMP(
    "rts-spork-machine",
    "RTS Spork Frame-Offset Recorder (Machine)",
    /*CFGOnly=*/false,
    /*isAnalysis=*/false);

// -----------------------------------------------------------------------------
// New-PM plugin registration (clang -fpass-plugin=...)
// -----------------------------------------------------------------------------

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {
    LLVM_PLUGIN_API_VERSION, "RtsSporkPass", "0.1",
    [](PassBuilder &PB) {
      // Register the IR pass at the module level, early in the pipeline
      // so that allocas have not yet been SROA'd away.
      PB.registerPipelineStartEPCallback(
          [](ModulePassManager &MPM, OptimizationLevel) {
            // Run a function pass manager first so we can use FunctionPass
            // infra if desired; here we use the module pass directly.
            MPM.addPass(RtsSporkIRPass());
          });

      // The machine pass is registered separately via the legacy PM's
      // static registration above and injected by the target machine.
      // For new-PM integration, use:
      PB.registerOptimizerLastEPCallback(
          [](ModulePassManager &MPM, OptimizationLevel llvmOptimizationLevel, ThinOrFullLTOPhase phase) {
            // Placeholder: the machine pass cannot run in the new PM's
            // pre-ISel pipeline; it must be inserted into the
            // TargetMachine pass pipeline via TargetMachine::addPostRegAlloc.
            // See CMakeLists.txt notes.
            (void)MPM;
          });
    }
  };
}
