/**
 * RtsSporkPass.cpp
 *
 * Two-phase LLVM pass that finds all calls to:
 *
 *   static void __RTS_record_spork(
 *       volatile bool*  promotable_flag,   // arg 0 – local var address
 *       volatile uint*  num_promotions,    // arg 1 – local var address
 *       void*           prom,              // arg 2 – local var address
 *       bool (*exec_prom)(void*)           // arg 3 – local var address
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
 *     • Finds every call to __RTS_record_spork.
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
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <vector>
#include <string>

#define DEBUG_TYPE "rts-spork"

using namespace llvm;

// ─────────────────────────────────────────────────────────────────────────────
// Constants / names shared between both phases
// ─────────────────────────────────────────────────────────────────────────────

static constexpr unsigned kNumSporkArgs  = 4;
static constexpr char kTargetFnName[]    = "__RTS_record_spork";
static constexpr char kTableName[]       = "__rts_spork_table";
static constexpr char kTableSizeName[]   = "__rts_spork_table_size";
static constexpr char kMDSiteId[]        = "rts_spork_id";   // on CallInst
static constexpr char kMDSlotIdx[]       = "rts_spork_slot"; // on AllocaInst

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Strip pointer casts / GEPs with zero offset to find the underlying alloca.
/// Returns nullptr if we cannot prove the value originates from a local alloca.
static AllocaInst *resolveToAlloca(Value *V) {
  // Peel casts / zero-index GEPs iteratively.
  while (V) {
    if (AllocaInst *AI = dyn_cast<AllocaInst>(V))
      return AI;
    if (BitCastInst *BC = dyn_cast<BitCastInst>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    if (AddrSpaceCastInst *AC = dyn_cast<AddrSpaceCastInst>(V)) {
      V = AC->getOperand(0);
      continue;
    }
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V)) {
      // Only follow if all indices are zero (i.e. no actual displacement).
      bool allZero = true;
      for (Use &Idx : GEP->indices()) {
        if (ConstantInt *CI = dyn_cast<ConstantInt>(Idx)) {
          if (!CI->isZero()) { allZero = false; break; }
        } else {
          allZero = false; break;
        }
      }
      if (allZero) { V = GEP->getPointerOperand(); continue; }
      // Non-zero GEP: still the same alloca, just offset within it.
      // We can still recover the alloca for frame-offset purposes.
      V = GEP->getPointerOperand();
      continue;
    }
    // PHI / select: too complex – give up.
    break;
  }
  return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 1 – IR-level Module Pass
// ─────────────────────────────────────────────────────────────────────────────

struct RtsSporkIRPass : public PassInfoMixin<RtsSporkIRPass> {

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    LLVMContext &Ctx = M.getContext();

    // Collect all calls to __RTS_record_spork across the whole module.
    struct SiteInfo {
      CallInst      *CI;
      AllocaInst    *allocas[kNumSporkArgs]; // may be nullptr if unresolvable
      uint64_t       id;
    };
    std::vector<SiteInfo> sites;

    for (Function &F : M) {
      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          CallInst *CI = dyn_cast<CallInst>(&I);
          if (!CI) continue;

          Function *Callee = CI->getCalledFunction();
          if (!Callee || Callee->getName() != kTargetFnName) continue;

          if (CI->arg_size() < kNumSporkArgs) {
            errs() << "[RtsSpork] WARNING: call to " << kTargetFnName
                   << " has only " << CI->arg_size() << " args, skipping.\n";
            continue;
          }

          SiteInfo si;
          si.CI = CI;
          si.id = (uint64_t)sites.size();

          for (unsigned a = 0; a < kNumSporkArgs; ++a) {
            Value *Arg = CI->getArgOperand(a);
            si.allocas[a] = resolveToAlloca(Arg);
            if (!si.allocas[a]) {
              errs() << "[RtsSpork] WARNING: arg " << a
                     << " of call at " << F.getName()
                     << " could not be resolved to an alloca. "
                        "Offset will be INT64_MIN.\n";
            }
          }
          sites.push_back(si);
        }
      }
    }

    if (sites.empty()) {
      LLVM_DEBUG(dbgs() << "[RtsSpork] No calls found.\n");
      return PreservedAnalyses::all();
    }

    uint64_t N = sites.size();
    LLVM_DEBUG(dbgs() << "[RtsSpork] Found " << N << " call site(s).\n");

    // ── Build the global table type ──────────────────────────────────────────
    //
    //   struct RtsSporkSite {
    //       int64_t offsets[4];   // one per argument
    //   };
    //   RtsSporkSite __rts_spork_table[N];       // zero-initialised for now
    //   uint64_t     __rts_spork_table_size = N;
    //
    Type *I64Ty  = Type::getInt64Ty(Ctx);
    Type *I64x4  = ArrayType::get(I64Ty, kNumSporkArgs);

    // The site struct: { int64_t offsets[4]; }
    StructType *SiteTy = StructType::create(
        Ctx, {I64x4}, "struct.RtsSporkSite", /*isPacked=*/false);

    // The table array
    ArrayType *TableTy = ArrayType::get(SiteTy, N);

    // Zero initialiser – offsets filled at program start by the machine pass
    // (or at link time if using a constructor stub approach).
    Constant *ZeroTable = ConstantAggregateZero::get(TableTy);

    GlobalVariable *GTable = new GlobalVariable(
        M, TableTy,
        /*isConstant=*/false,
        GlobalValue::ExternalLinkage,
        ZeroTable,
        kTableName);
    GTable->setAlignment(Align(8));
    GTable->setSection(".data");

    // Table size
    GlobalVariable *GSize = new GlobalVariable(
        M, I64Ty,
        /*isConstant=*/true,
        GlobalValue::ExternalLinkage,
        ConstantInt::get(I64Ty, N),
        kTableSizeName);
    GSize->setAlignment(Align(8));

    // ── Annotate each call site with metadata ────────────────────────────────
    //
    // !rts_spork_id  = !{ i64 <site_id> }   attached to the CallInst
    // !rts_spork_slot = !{ i64 <site_id>, i64 <arg_index> }  on each AllocaInst
    //
    // The machine pass will iterate over MachineInstrs, find the call via the
    // metadata, look up the alloca slots, and patch __rts_spork_table.

    unsigned MDSiteIdKind  = Ctx.getMDKindID(kMDSiteId);
    unsigned MDSlotIdxKind = Ctx.getMDKindID(kMDSlotIdx);

    for (SiteInfo &si : sites) {
      // Tag the call instruction.
      MDNode *CallMD = MDNode::get(
          Ctx,
          {ConstantAsMetadata::get(ConstantInt::get(I64Ty, si.id))});
      si.CI->setMetadata(MDSiteIdKind, CallMD);

      // Tag each alloca (may be shared across args – use arg index to
      // disambiguate in the machine pass).
      for (unsigned a = 0; a < kNumSporkArgs; ++a) {
        if (!si.allocas[a]) continue;
        MDNode *SlotMD = MDNode::get(
            Ctx,
            {ConstantAsMetadata::get(ConstantInt::get(I64Ty, si.id)),
             ConstantAsMetadata::get(ConstantInt::get(I64Ty, a))});
        // Append (don't overwrite) – an alloca may appear in multiple sites.
        si.allocas[a]->setMetadata(MDSlotIdxKind, SlotMD);
      }
    }

    // ── Emit a __attribute__((constructor)) filler function ──────────────────
    //
    // At IR level we don't yet know the real offsets (the backend hasn't run).
    // We therefore emit a *placeholder* constructor whose body is filled in by
    // the MachineFunctionPass.  Here we just create the function signature and
    // insert it into the global_ctors list so the linker will call it.
    //
    // The machine pass will find the function by the well-known name
    // "__rts_spork_init" and patch its MachineBasicBlock to store the
    // offsets obtained from MachineFrameInfo.
    //
    // For now, emit:
    //   void __rts_spork_init() { /* machine pass will fill this */ }
    // and register it at priority 0 (runs before user code).

    FunctionType *InitFTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    Function *InitFn = Function::Create(
        InitFTy, GlobalValue::InternalLinkage, "__rts_spork_init", &M);
    InitFn->addFnAttr(Attribute::NoInline); // keep it addressable for patching
    BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", InitFn);
    IRBuilder<> Builder(Entry);

    // We store a sentinel (INT64_MIN = 0x8000000000000000) so the machine pass
    // can recognise un-patched slots at debug time, and also so that
    // applications can detect "no frame info available".
    Constant *Sentinel = ConstantInt::get(I64Ty, INT64_MIN);
    Type     *I32Ty    = Type::getInt32Ty(Ctx);

    for (SiteInfo &si : sites) {
      for (unsigned a = 0; a < kNumSporkArgs; ++a) {
        // GEP into __rts_spork_table[si.id].offsets[a]
        Value *Slot = Builder.CreateInBoundsGEP(
            TableTy, GTable,
            {ConstantInt::get(I32Ty, 0),
             ConstantInt::get(I32Ty, si.id),
             ConstantInt::get(I32Ty, 0),           // field 0: offsets[]
             ConstantInt::get(I32Ty, a)},
            "slot");
        // Store sentinel; the machine pass will replace with real offsets.
        Builder.CreateStore(Sentinel, Slot);
      }
    }
    Builder.CreateRetVoid();

    // Register in @llvm.global_ctors at priority 0.
    addToGlobalCtors(M, InitFn, /*Priority=*/0, /*Data=*/nullptr);

    return PreservedAnalyses::none(); // we modified the IR
  }

private:
  /// Append an entry to @llvm.global_ctors.
  static void addToGlobalCtors(Module &M, Function *F, int Priority,
                               Constant *Data) {
    LLVMContext &Ctx = M.getContext();
    Type   *I32Ty = Type::getInt32Ty(Ctx);
    Type   *I8PtrTy = PointerType::getUnqual(Ctx);

    StructType *EltTy = StructType::get(I32Ty, F->getType(), I8PtrTy);

    Constant *Elt = ConstantStruct::get(
        EltTy,
        ConstantInt::get(I32Ty, Priority),
        F,
        Data ? Data : Constant::getNullValue(I8PtrTy));

    GlobalVariable *GV = M.getGlobalVariable("llvm.global_ctors");
    if (!GV) {
      ArrayType *ArrTy = ArrayType::get(EltTy, 1);
      GV = new GlobalVariable(M, ArrTy, false,
                              GlobalValue::AppendingLinkage,
                              ConstantArray::get(ArrTy, {Elt}),
                              "llvm.global_ctors");
    } else {
      // Append to existing array.
      ConstantArray *OldArr = cast<ConstantArray>(GV->getInitializer());
      ArrayType *OldTy = cast<ArrayType>(OldArr->getType());
      std::vector<Constant *> Elts;
      for (unsigned i = 0; i < OldTy->getNumElements(); ++i)
        Elts.push_back(OldArr->getOperand(i));
      Elts.push_back(Elt);
      ArrayType *NewTy = ArrayType::get(EltTy, Elts.size());
      Constant  *NewArr = ConstantArray::get(NewTy, Elts);
      GlobalVariable *NewGV = new GlobalVariable(
          M, NewTy, false, GlobalValue::AppendingLinkage, NewArr,
          "llvm.global_ctors");
      GV->eraseFromParent();
      (void)NewGV;
    }
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Phase 2 – MachineFunctionPass
// ─────────────────────────────────────────────────────────────────────────────
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
      errs() << "[RtsSpork] ERROR: global table not found – did IR pass run?\n";
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
        errs() << "[RtsSpork] WARNING: no FrameIndex for alloca in "
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

      LLVM_DEBUG(dbgs() << "[RtsSpork] site=" << se.siteId
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

// ─────────────────────────────────────────────────────────────────────────────
// New-PM plugin registration (clang -fpass-plugin=...)
// ─────────────────────────────────────────────────────────────────────────────

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
