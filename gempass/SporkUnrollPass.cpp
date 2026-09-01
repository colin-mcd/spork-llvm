#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopPeel.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"

#include <algorithm>
#include <optional>

#define DEBUG_TYPE "spork-unroll"

using namespace llvm;

namespace {

static constexpr StringLiteral SporkUnrollLoopName("__spork_unroll_loop");
static constexpr StringLiteral
    SporkGetUnrollFactorName("__spork_get_unroll_factor");
static constexpr StringLiteral SporkProgressMetadata("spork.progress");

// Both compile-time-only calls use an exact C ABI contract. The pointer is a
// stable loop-site identity, allowing the marker and factor expression to be
// matched even when they end up in different outlined functions.
static bool isValidSporkLoopMarker(CallInst *Call) {
  if (!Call || Call->arg_size() != 1 || !Call->getType()->isVoidTy() ||
      !Call->getArgOperand(0)->getType()->isPointerTy())
    return false;

  Value *Called = Call->getCalledOperand()->stripPointerCasts();
  auto *Callee = dyn_cast<Function>(Called);
  return Callee && Callee->getName() == SporkUnrollLoopName &&
         Callee->isDeclaration();
}

static bool isValidSporkFactorCall(CallInst *Call) {
  if (!Call || Call->arg_size() != 1 || !Call->getType()->isIntegerTy() ||
      !Call->getArgOperand(0)->getType()->isPointerTy())
    return false;

  Value *Called = Call->getCalledOperand()->stripPointerCasts();
  auto *Callee = dyn_cast<Function>(Called);
  return Callee && Callee->getName() == SporkGetUnrollFactorName &&
         Callee->isDeclaration();
}

static GlobalValue *getSiteToken(CallInst *Call) {
  if (!Call)
    return nullptr;
  return dyn_cast<GlobalValue>(Call->getArgOperand(0)->stripPointerCasts());
}

static Value *stripIntegerCasts(Value *V) {
  while (auto *Cast = dyn_cast<CastInst>(V)) {
    if (!Cast->getSrcTy()->isIntegerTy() || !Cast->getDestTy()->isIntegerTy())
      break;
    V = Cast->getOperand(0);
  }
  return V;
}

static AllocaInst *resolveAlloca(Value *V) {
  SmallPtrSet<Value *, 8> Visited;
  while (V && Visited.insert(V).second) {
    V = V->stripPointerCasts();
    if (auto *AI = dyn_cast<AllocaInst>(V))
      return AI;
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
      if (!GEP->hasAllZeroIndices())
        return nullptr;
      V = GEP->getPointerOperand();
      continue;
    }
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      V = LI->getPointerOperand();
      continue;
    }
    return nullptr;
  }
  return nullptr;
}

// A marker denotes exactly the loop reached through its canonical preheader.
// General dominance is intentionally insufficient because it also captures
// nested loops and unrelated later loops.
static Loop *findMarkedLoop(CallInst *Call, LoopInfo &LI) {
  BasicBlock *MarkerBlock = Call->getParent();
  Loop *Found = nullptr;

  auto Consider = [&](BasicBlock *Preheader, BasicBlock *Header) {
    Loop *L = LI.getLoopFor(Header);
    if (!L || L->getHeader() != Header || L->getLoopPreheader() != Preheader)
      return true;
    if (Found && Found != L)
      return false;
    Found = L;
    return true;
  };

  for (BasicBlock *Successor : successors(MarkerBlock)) {
    if (!Consider(MarkerBlock, Successor))
      return nullptr;

    // If the successor was already the loop header, do not mistake an
    // unconditional header for a bridge block.
    if (Found && Found->getHeader() == Successor)
      continue;

    // LoopSimplify may split the marker-to-header edge when the source loop has
    // a zero-trip guard. Accept exactly that one canonical preheader hop.
    if (auto *Bridge = dyn_cast<BranchInst>(Successor->getTerminator());
        Bridge && Bridge->isUnconditional() &&
        !Consider(Successor, Bridge->getSuccessor(0)))
      return nullptr;
  }
  return Found;
}

struct ProgressStore {
  StoreInst *Store = nullptr;
  AllocaInst *Slot = nullptr;
};

static void collectDependentLoads(Value *V, SmallPtrSetImpl<Value *> &Visited,
                                  SmallVectorImpl<LoadInst *> &Loads) {
  if (!V || !Visited.insert(V).second)
    return;
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    Loads.push_back(LI);
    return;
  }
  if (auto *I = dyn_cast<Instruction>(V))
    for (Value *Operand : I->operands())
      collectDependentLoads(Operand, Visited, Loads);
}

// The only supported progress update is one unconditional volatile store in a
// unique latch, storing the latch value of LLVM's recognized induction PHI.
static std::optional<ProgressStore> findProgressStore(Loop *L,
                                                      ScalarEvolution &SE,
                                                      CallInst *Marker,
                                                      DominatorTree &DT) {
  BasicBlock *Latch = L->getLoopLatch();
  PHINode *Induction = L->getInductionVariable(SE);
  if (!Latch || !Induction)
    return std::nullopt;

  Value *NextInduction = Induction->getIncomingValueForBlock(Latch);
  if (!NextInduction)
    return std::nullopt;

  StoreInst *Candidate = nullptr;
  for (Instruction &I : *Latch) {
    auto *SI = dyn_cast<StoreInst>(&I);
    if (!SI || !SI->isVolatile())
      continue;
    if (Candidate)
      return std::nullopt;
    Candidate = SI;
  }
  if (!Candidate || stripIntegerCasts(Candidate->getValueOperand()) !=
                        stripIntegerCasts(NextInduction))
    return std::nullopt;

  auto *LatchBranch = dyn_cast<BranchInst>(Latch->getTerminator());
  if (!LatchBranch || !LatchBranch->isConditional())
    return std::nullopt;
  SmallPtrSet<Value *, 16> Visited;
  SmallVector<LoadInst *, 2> ExitLoads;
  collectDependentLoads(LatchBranch->getCondition(), Visited, ExitLoads);
  llvm::erase_if(ExitLoads, [](LoadInst *LI) { return !LI->isVolatile(); });
  if (ExitLoads.size() != 1 || !ExitLoads.front()->getType()->isIntegerTy())
    return std::nullopt;

  AllocaInst *ProgressSlot = resolveAlloca(Candidate->getPointerOperand());
  if (!ProgressSlot)
    return std::nullopt;

  // Normal code in the loop must not observe intermediate progress values.
  // The asynchronous promotion callback is the intended external observer.
  for (User *U : ProgressSlot->users()) {
    auto *LI = dyn_cast<LoadInst>(U);
    if (!LI)
      continue;
    // Eager promotion may read progress before entering the marked body. A
    // normal read in the loop or after it would make coalescing observable.
    if (L->contains(LI) || !DT.dominates(LI, Marker))
      return std::nullopt;
  }

  AllocaInst *BoundSlot = resolveAlloca(ExitLoads.front()->getPointerOperand());
  if (!BoundSlot || BoundSlot == ProgressSlot)
    return std::nullopt;

  return ProgressStore{Candidate, ProgressSlot};
}

struct UnrollDecision {
  unsigned Count = 0;
  TargetTransformInfo::UnrollingPreferences Preferences;
};

// Calls and nested loops can introduce work whose amount is not bounded by the
// marked loop's induction variable.  Spork publishes its factor to runtime
// code, so do not speculate about either: leave the loop intact and publish
// the factor-one fallback instead.
static bool hasPotentiallyUnboundedWork(const Loop *L) {
  if (!L->getSubLoops().empty())
    return true;

  for (const BasicBlock *BB : L->blocks())
    for (const Instruction &I : *BB)
      if (isa<CallBase>(I))
        return true;

  return false;
}

// Mirror the public, target-aware decision setup used by LoopUnrollPass, then
// delegate the actual count choice to LLVM's computeUnrollCount().
static std::optional<UnrollDecision>
computeNativeUnrollDecision(Loop *L, LoopInfo &LI, ScalarEvolution &SE,
                            DominatorTree &DT, AssumptionCache &AC,
                            const TargetTransformInfo &TTI,
                            OptimizationRemarkEmitter &ORE) {
  if (!L->isLoopSimplifyForm() || !L->isLCSSAForm(DT))
    return std::nullopt;

  TargetTransformInfo::UnrollingPreferences UP = gatherUnrollingPreferences(
      L, SE, TTI, /*BFI=*/nullptr, /*PSI=*/nullptr, ORE, /*OptLevel=*/3,
      /*UserThreshold=*/std::nullopt,
      /*UserCount=*/std::nullopt,
      /*UserAllowPartial=*/true,
      /*UserRuntime=*/true,
      /*UserUpperBound=*/std::nullopt,
      /*UserFullUnrollMaxCount=*/std::nullopt);
  TargetTransformInfo::PeelingPreferences PP =
      gatherPeelingPreferences(L, SE, TTI, /*UserAllowPeeling=*/false,
                               /*UserAllowProfileBasedPeeling=*/false,
                               /*UnrollingSpecficValues=*/true);

  if (UP.Threshold == 0 && (!UP.Partial || UP.PartialThreshold == 0))
    return std::nullopt;

  SmallPtrSet<const Value *, 32> EphValues;
  CodeMetrics::collectEphemeralValues(L, &AC, EphValues);
  UnrollCostEstimator UCE(L, TTI, EphValues, UP.BEInsns);
  if (!UCE.canUnroll() || UCE.NumInlineCandidates != 0)
    return std::nullopt;

  unsigned TripCount = 0;
  unsigned TripMultiple = 1;
  SmallVector<BasicBlock *, 8> ExitingBlocks;
  L->getExitingBlocks(ExitingBlocks);
  for (BasicBlock *Exiting : ExitingBlocks) {
    unsigned TC = SE.getSmallConstantTripCount(L, Exiting);
    if (TC && (!TripCount || TC < TripCount))
      TripCount = TripMultiple = TC;
  }

  if (!TripCount) {
    BasicBlock *Exiting = L->getLoopLatch();
    if (!Exiting || !L->isLoopExiting(Exiting))
      Exiting = L->getExitingBlock();
    if (Exiting)
      TripMultiple = SE.getSmallConstantTripMultiple(L, Exiting);
  }

  UP.AllowRemainder &= UCE.ConvergenceAllowsRuntime;
  unsigned MaxTripCount = 0;
  bool MaxOrZero = false;
  if (!TripCount) {
    MaxTripCount = SE.getSmallConstantMaxTripCount(L);
    MaxOrZero = SE.isBackedgeTakenCountMaxOrZero(L);
  }

  bool UseUpperBound = false;
  (void)computeUnrollCount(L, TTI, DT, &LI, &AC, SE, EphValues, &ORE, TripCount,
                           MaxTripCount, MaxOrZero, TripMultiple, UCE, UP, PP,
                           UseUpperBound);
  unsigned Count = UP.Count;
  if (PP.PeelCount || Count < 2)
    return std::nullopt;

  // UnrollLoop performs this same clamp internally. Do it here as well so the
  // factor published to parfor is the effective count, not merely the request.
  if (MaxTripCount && Count > MaxTripCount)
    Count = MaxTripCount;
  if (Count < 2)
    return std::nullopt;

  UP.Runtime &= UCE.ConvergenceAllowsRuntime;
  UP.Runtime &= TripCount == 0 && TripMultiple % Count != 0;
  return UnrollDecision{Count, UP};
}

static bool comesBeforeInUnrolledChain(StoreInst *A, StoreInst *B,
                                       DominatorTree &DT) {
  if (A->getParent() == B->getParent())
    return A->comesBefore(B);
  return DT.dominates(A, B);
}

// Coalesce only when the utility produced exactly Count sequential clones in
// the main unrolled body. Otherwise restore every store and keep factor 1.
static bool coalesceProgressStores(Function &F, Loop *MainLoop,
                                   Loop *RemainderLoop, MDNode *Tag,
                                   unsigned Count, DominatorTree &DT) {
  SmallVector<StoreInst *, 8> Tagged;
  SmallVector<StoreInst *, 8> MainStores;
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI || SI->getMetadata(SporkProgressMetadata) != Tag)
        continue;
      Tagged.push_back(SI);
      if (!RemainderLoop || !RemainderLoop->contains(SI)) {
        if (!MainLoop || MainLoop->contains(SI))
          MainStores.push_back(SI);
      }
    }
  }

  auto RestoreAll = [&]() {
    for (StoreInst *SI : Tagged) {
      SI->setVolatile(true);
      SI->setMetadata(SporkProgressMetadata, nullptr);
    }
  };

  if (MainStores.size() != Count) {
    RestoreAll();
    return false;
  }

  llvm::sort(MainStores, [&](StoreInst *A, StoreInst *B) {
    return comesBeforeInUnrolledChain(A, B, DT);
  });
  for (size_t I = 0; I + 1 < MainStores.size(); ++I) {
    if (!comesBeforeInUnrolledChain(MainStores[I], MainStores[I + 1], DT)) {
      RestoreAll();
      return false;
    }
  }

  StoreInst *Final = MainStores.back();
  for (StoreInst *SI : Tagged) {
    SI->setMetadata(SporkProgressMetadata, nullptr);
    SI->setVolatile(true);
  }
  for (StoreInst *SI : MainStores)
    if (SI != Final)
      SI->eraseFromParent();
  return true;
}

static bool
transformMarkedLoops(Function &F, FunctionAnalysisManager &FAM,
                     DenseMap<GlobalValue *, unsigned> &SiteFactors,
                     const DenseMap<GlobalValue *, unsigned> &MarkerCounts,
                     unsigned &SiteNumber) {
  SmallVector<CallInst *, 4> Markers;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *Call = dyn_cast<CallInst>(&I); isValidSporkLoopMarker(Call))
        Markers.push_back(Call);

  if (Markers.empty())
    return false;

  auto &LI = FAM.getResult<LoopAnalysis>(F);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &AC = FAM.getResult<AssumptionAnalysis>(F);
  auto &TTI = FAM.getResult<TargetIRAnalysis>(F);
  OptimizationRemarkEmitter ORE(&F);
  LLVMContext &Ctx = F.getContext();
  bool Changed = false;

  for (CallInst *Call : Markers) {
    GlobalValue *Token = getSiteToken(Call);
    Loop *L = findMarkedLoop(Call, LI);
    std::optional<ProgressStore> Progress =
        L ? findProgressStore(L, SE, Call, DT) : std::nullopt;

    std::optional<UnrollDecision> Decision;
    if (Token && MarkerCounts.lookup(Token) == 1 && L && Progress &&
        !hasPotentiallyUnboundedWork(L))
      Decision = computeNativeUnrollDecision(L, LI, SE, DT, AC, TTI, ORE);

    unsigned Factor = 1;

    if (Decision) {
      MDNode *Tag =
          MDNode::getDistinct(Ctx, ConstantAsMetadata::get(ConstantInt::get(
                                       Type::getInt32Ty(Ctx), SiteNumber++)));
      Progress->Store->setMetadata(SporkProgressMetadata, Tag);

      UnrollLoopOptions ULO;
      ULO.Count = Decision->Count;
      ULO.Force = true;
      // loop_end is asynchronously shortened by the promotion callback.
      // Runtime remainder generation would derive a fixed trip count and
      // could stop observing that volatile bound. Generic cloning retains a
      // bound check in every unrolled iteration.
      ULO.Runtime = false;
      ULO.AllowExpensiveTripCount =
          Decision->Preferences.AllowExpensiveTripCount;
      ULO.UnrollRemainder = Decision->Preferences.UnrollRemainder;
      ULO.ForgetAllSCEV = false;
      ULO.SCEVExpansionBudget = Decision->Preferences.SCEVExpansionBudget;
      ULO.RuntimeUnrollMultiExit = Decision->Preferences.RuntimeUnrollMultiExit;
      ULO.AddAdditionalAccumulators =
          Decision->Preferences.AddAdditionalAccumulators;

      Loop *RemainderLoop = nullptr;
      LoopUnrollResult Result =
          UnrollLoop(L, ULO, &LI, &SE, &DT, &AC, &TTI, &ORE,
                     /*PreserveLCSSA=*/true, &RemainderLoop);
      if (Result == LoopUnrollResult::Unmodified) {
        Progress->Store->setVolatile(true);
        Progress->Store->setMetadata(SporkProgressMetadata, nullptr);
      } else {
        Loop *MainLoop =
            Result == LoopUnrollResult::PartiallyUnrolled ? L : nullptr;
        if (coalesceProgressStores(F, MainLoop, RemainderLoop, Tag,
                                   Decision->Count, DT))
          Factor = Decision->Count;
        Changed = true;
      }
    }

    if (Token) {
      auto Inserted = SiteFactors.insert({Token, Factor});
      // A token is supposed to identify one static loop. If malformed IR
      // reuses it for sites with different decisions, publish the safe
      // single-iteration value for every matching factor expression.
      if (!Inserted.second && Inserted.first->second != Factor)
        Inserted.first->second = 1;
    }

    // The marker is compile-time-only. A recognized but unsupported site is
    // a safe no-op whose matching factor expression becomes 1.
    Call->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

static bool
replaceFactorCalls(Module &M,
                   const DenseMap<GlobalValue *, unsigned> &SiteFactors) {
  SmallVector<CallInst *, 8> Calls;
  for (Function &F : M)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *Call = dyn_cast<CallInst>(&I); isValidSporkFactorCall(Call))
          Calls.push_back(Call);

  for (CallInst *Call : Calls) {
    unsigned Factor = 1;
    if (GlobalValue *Token = getSiteToken(Call)) {
      auto It = SiteFactors.find(Token);
      if (It != SiteFactors.end())
        Factor = It->second;
    }
    Call->replaceAllUsesWith(
        ConstantInt::get(cast<IntegerType>(Call->getType()), Factor));
    Call->eraseFromParent();
  }
  return !Calls.empty();
}

struct SporkUnrollPass : PassInfoMixin<SporkUnrollPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    FunctionAnalysisManager &FAM =
        MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    DenseMap<GlobalValue *, unsigned> SiteFactors;
    DenseMap<GlobalValue *, unsigned> MarkerCounts;
    unsigned SiteNumber = 0;
    bool Changed = false;

    // A token must identify exactly one static marker. Count first so a reused
    // token is rejected before any loop using it can be transformed.
    for (Function &F : M)
      for (BasicBlock &BB : F)
        for (Instruction &I : BB)
          if (auto *Call = dyn_cast<CallInst>(&I); isValidSporkLoopMarker(Call))
            if (GlobalValue *Token = getSiteToken(Call))
              ++MarkerCounts[Token];

    for (Function &F : M)
      if (!F.isDeclaration())
        Changed |=
            transformMarkedLoops(F, FAM, SiteFactors, MarkerCounts, SiteNumber);

    // Replace every recognized placeholder, including unmatched ones. This
    // guarantees a factor-one fallback and prevents a compile-time-only call
    // from surviving to the linker.
    Changed |= replaceFactorCalls(M, SiteFactors);
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

} // namespace

llvm::PassPluginLibraryInfo getSporkUnrollPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "SporkUnroll", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        PB.registerOptimizerLastEPCallback(
            [](ModulePassManager &MPM, OptimizationLevel, ThinOrFullLTOPhase) {
              FunctionPassManager FPM;
              FPM.addPass(LoopSimplifyPass());
              FPM.addPass(LCSSAPass());
              MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
              MPM.addPass(SporkUnrollPass());
            });

        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &MPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name != "spork-unroll")
                return false;
              FunctionPassManager FPM;
              FPM.addPass(LoopSimplifyPass());
              FPM.addPass(LCSSAPass());
              MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
              MPM.addPass(SporkUnrollPass());
              return true;
            });
      }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getSporkUnrollPluginInfo();
}
