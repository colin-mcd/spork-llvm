#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/LoopPeel.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <algorithm>
#include <memory>
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
    if (auto *Bridge = dyn_cast<UncondBrInst>(Successor->getTerminator());
        Bridge && !Consider(Successor, Bridge->getSuccessor()))
      return nullptr;
  }
  return Found;
}

struct ProgressStore {
  StoreInst *Store = nullptr;
  AllocaInst *Slot = nullptr;
  AllocaInst *BoundSlot = nullptr;
  LoadInst *BoundLoad = nullptr;
  PHINode *Induction = nullptr;
  Value *NextInduction = nullptr;
  ICmpInst *ExitCompare = nullptr;
  CondBrInst *LatchBranch = nullptr;
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

  auto *LatchBranch = dyn_cast<CondBrInst>(Latch->getTerminator());
  if (!LatchBranch)
    return std::nullopt;
  auto *ExitCompare = dyn_cast<ICmpInst>(LatchBranch->getCondition());
  if (!ExitCompare ||
      (ExitCompare->getPredicate() != ICmpInst::ICMP_SLT &&
       ExitCompare->getPredicate() != ICmpInst::ICMP_ULT) ||
      stripIntegerCasts(ExitCompare->getOperand(0)) !=
          stripIntegerCasts(NextInduction))
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

  return ProgressStore{Candidate, ProgressSlot,  BoundSlot,   ExitLoads.front(),
                       Induction, NextInduction, ExitCompare, LatchBranch};
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

  unsigned Count =
      computeUnrollCount(L, TTI, DT, &LI, &AC, SE, EphValues, &ORE, TripCount,
                         MaxTripCount, MaxOrZero, TripMultiple, UCE, UP, PP);
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

struct ExitConditionParts {
  ICmpInst *Compare = nullptr;
  LoadInst *BoundLoad = nullptr;
  SmallVector<Instruction *, 4> BoundPath;
};

// Match the deliberately narrow latch condition accepted by
// findProgressStore(): next-induction < integer-casts(volatile bound load).
static std::optional<ExitConditionParts>
matchExitCondition(CondBrInst *Branch, AllocaInst *BoundSlot,
                   ICmpInst::Predicate Predicate) {
  if (!Branch)
    return std::nullopt;
  auto *Compare = dyn_cast<ICmpInst>(Branch->getCondition());
  if (!Compare || Compare->getPredicate() != Predicate || !Compare->hasOneUse())
    return std::nullopt;

  ExitConditionParts Parts;
  Parts.Compare = Compare;
  Value *V = Compare->getOperand(1);
  while (auto *Cast = dyn_cast<CastInst>(V)) {
    if (!Cast->hasOneUse() || !Cast->getSrcTy()->isIntegerTy() ||
        !Cast->getDestTy()->isIntegerTy())
      return std::nullopt;
    Parts.BoundPath.push_back(Cast);
    V = Cast->getOperand(0);
  }
  auto *Load = dyn_cast<LoadInst>(V);
  if (!Load || !Load->isVolatile() || !Load->hasOneUse() ||
      resolveAlloca(Load->getPointerOperand()) != BoundSlot)
    return std::nullopt;
  Parts.BoundLoad = Load;
  Parts.BoundPath.push_back(Load);
  return Parts;
}

static void eraseExitCondition(ExitConditionParts &Parts) {
  Parts.Compare->eraseFromParent();
  for (Instruction *I : Parts.BoundPath)
    I->eraseFromParent();
}

// Rebuild the consecutive integer casts between Root and Template around a
// replacement value. The outermost cast is encountered first while walking
// toward Root, so replay the chain in reverse order.
static Value *replayIntegerCastChain(IRBuilder<> &Builder, Value *Template,
                                     Value *Root, Value *Replacement,
                                     const Twine &Name) {
  SmallVector<CastInst *, 4> Casts;
  Value *V = Template;
  while (V != Root) {
    auto *Cast = dyn_cast<CastInst>(V);
    if (!Cast || !Cast->getSrcTy()->isIntegerTy() ||
        !Cast->getDestTy()->isIntegerTy())
      llvm_unreachable("validated exit condition lost its integer cast chain");
    Casts.push_back(Cast);
    V = Cast->getOperand(0);
  }

  Value *Result = Replacement;
  for (CastInst *Cast : llvm::reverse(Casts))
    Result = Builder.CreateCast(Cast->getOpcode(), Result, Cast->getDestTy(),
                                Name);
  return Result;
}

static Value *createBatchGuard(IRBuilder<> &Builder, Value *Base,
                               const ProgressStore &Progress, unsigned Count) {
  auto *BaseTy = cast<IntegerType>(Base->getType());
  LoadInst *End = Builder.CreateLoad(Progress.BoundLoad->getType(),
                                     Progress.BoundSlot, "spork.end");
  End->setVolatile(true);
  End->setAlignment(Progress.BoundLoad->getAlign());
  End->setOrdering(Progress.BoundLoad->getOrdering());
  End->setSyncScopeID(Progress.BoundLoad->getSyncScopeID());

  bool Signed = ICmpInst::isSigned(Progress.ExitCompare->getPredicate());
  Value *Offset = ConstantInt::get(BaseTy, Count - 1);
  Intrinsic::ID AddID =
      Signed ? Intrinsic::sadd_with_overflow : Intrinsic::uadd_with_overflow;
  Value *Add =
      Builder.CreateBinaryIntrinsic(AddID, Base, Offset, nullptr, "spork.last");
  Value *Last = Builder.CreateExtractValue(Add, 0, "spork.last.value");
  Value *Overflow = Builder.CreateExtractValue(Add, 1, "spork.last.overflow");

  Value *ComparedLast = replayIntegerCastChain(
      Builder, Progress.ExitCompare->getOperand(0), Progress.NextInduction,
      Last, "spork.last.cast");
  Value *ComparedEnd = replayIntegerCastChain(
      Builder, Progress.ExitCompare->getOperand(1), Progress.BoundLoad, End,
      "spork.end.cast");
  Value *InBounds = Builder.CreateICmp(Progress.ExitCompare->getPredicate(),
                                       ComparedLast, ComparedEnd,
                                       "spork.in.bounds");
  return Builder.CreateAnd(Builder.CreateNot(Overflow), InBounds,
                           "spork.batch.ready");
}

struct DetachedCleanupLoop {
  BasicBlock *Header = nullptr;
  BasicBlock *Exit = nullptr;
  ValueToValueMapTy VMap;
  SmallVector<PHINode *, 4> OriginalPHIs;
  SmallVector<Value *, 4> InitialValues;
  StoreInst *ProgressStore = nullptr;

  ~DetachedCleanupLoop() {
    if (Header && !Header->getParent())
      Header->deleteValue();
  }
};

static std::unique_ptr<DetachedCleanupLoop>
cloneCleanupLoop(Loop *L, const ProgressStore &Progress) {
  BasicBlock *Header = L->getHeader();
  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *Exit = L->getExitBlock();
  if (L->getNumBlocks() != 1 || Header != L->getLoopLatch() || !Preheader ||
      !Exit || !Exit->phis().empty())
    return nullptr;
  auto *PreheaderBranch = dyn_cast<UncondBrInst>(Preheader->getTerminator());
  if (!PreheaderBranch)
    return nullptr;

  Value *StrippedNext = stripIntegerCasts(Progress.NextInduction);
  auto *Increment = dyn_cast<BinaryOperator>(StrippedNext);
  if (!Increment || Increment->getOpcode() != Instruction::Add)
    return nullptr;
  Value *Step = nullptr;
  if (stripIntegerCasts(Increment->getOperand(0)) == Progress.Induction)
    Step = Increment->getOperand(1);
  else if (stripIntegerCasts(Increment->getOperand(1)) == Progress.Induction)
    Step = Increment->getOperand(0);
  auto *StepConstant = dyn_cast_or_null<ConstantInt>(Step);
  if (!StepConstant || !StepConstant->isOne())
    return nullptr;

  auto Cleanup = std::make_unique<DetachedCleanupLoop>();
  Cleanup->Header =
      CloneBasicBlock(Header, Cleanup->VMap, ".spork.cleanup", nullptr);
  Cleanup->Exit = Exit;
  Cleanup->VMap[Header] = Cleanup->Header;
  for (Instruction &I : *Cleanup->Header)
    RemapInstruction(&I, Cleanup->VMap,
                     RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
  for (PHINode &PN : Header->phis()) {
    Cleanup->OriginalPHIs.push_back(&PN);
    Cleanup->InitialValues.push_back(PN.getIncomingValueForBlock(Preheader));
  }
  Cleanup->ProgressStore =
      cast<StoreInst>(Cleanup->VMap.lookup(Progress.Store));
  return Cleanup;
}

// Turn LLVM's generic checked clone chain into a batch loop, and append the
// untouched original loop as a scalar cleanup. This intentionally supports
// only the single-block, unit-stride protocol validated above.
static bool
formBatchedLoopWithCleanup(Function &F, Loop *MainLoop,
                           const ProgressStore &Progress, MDNode *Tag,
                           unsigned Count, DominatorTree &DT,
                           std::unique_ptr<DetachedCleanupLoop> Cleanup) {
  if (!MainLoop || !Cleanup)
    return false;

  BasicBlock *Header = MainLoop->getHeader();
  BasicBlock *Preheader = MainLoop->getLoopPreheader();
  BasicBlock *Exit = Cleanup->Exit;
  if (!Preheader || !Exit || !Exit->phis().empty())
    return false;

  SmallVector<StoreInst *, 8> MainStores;
  for (BasicBlock *BB : MainLoop->blocks())
    for (Instruction &I : *BB)
      if (auto *SI = dyn_cast<StoreInst>(&I);
          SI && SI->getMetadata(SporkProgressMetadata) == Tag)
        MainStores.push_back(SI);
  if (MainStores.size() != Count)
    return false;

  llvm::sort(MainStores, [&](StoreInst *A, StoreInst *B) {
    return comesBeforeInUnrolledChain(A, B, DT);
  });
  for (size_t I = 0; I + 1 < MainStores.size(); ++I)
    if (!comesBeforeInUnrolledChain(MainStores[I], MainStores[I + 1], DT))
      return false;

  SmallVector<CondBrInst *, 8> LatchBranches;
  SmallVector<ExitConditionParts, 8> ExitConditions;
  for (StoreInst *Store : MainStores) {
    auto *Branch = dyn_cast<CondBrInst>(Store->getParent()->getTerminator());
    auto Parts = matchExitCondition(Branch, Progress.BoundSlot,
                                    Progress.ExitCompare->getPredicate());
    if (!Parts)
      return false;
    unsigned InLoopSuccessors = MainLoop->contains(Branch->getSuccessor(0)) +
                                MainLoop->contains(Branch->getSuccessor(1));
    if (InLoopSuccessors != 1)
      return false;
    LatchBranches.push_back(Branch);
    ExitConditions.push_back(std::move(*Parts));
  }

  BasicBlock *FinalLatch = MainStores.back()->getParent();
  SmallVector<Value *, 4> FinalValues;
  for (PHINode *PN : Cleanup->OriginalPHIs) {
    Value *V = PN->getIncomingValueForBlock(FinalLatch);
    if (!V)
      return false;
    FinalValues.push_back(V);
  }
  auto InductionIt = llvm::find(Cleanup->OriginalPHIs, Progress.Induction);
  if (InductionIt == Cleanup->OriginalPHIs.end())
    return false;
  size_t InductionIndex = InductionIt - Cleanup->OriginalPHIs.begin();

  // All validation is complete. From here onward the rewrite cannot fall back
  // without undoing CFG changes.
  BasicBlock *CleanupPreheader = BasicBlock::Create(
      F.getContext(), Header->getName() + ".spork.cleanup.ph", &F, Exit);
  Cleanup->Header->insertInto(&F, Exit);
  for (Instruction &I : *Cleanup->Header)
    RemapDbgRecordRange(F.getParent(), I.getDbgRecordRange(), Cleanup->VMap,
                        RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
  Cleanup->ProgressStore->setMetadata(SporkProgressMetadata, nullptr);
  Cleanup->ProgressStore->setVolatile(true);

  IRBuilder<> CleanupBuilder(CleanupPreheader);
  SmallVector<PHINode *, 4> CleanupStarts;
  for (size_t I = 0; I < Cleanup->OriginalPHIs.size(); ++I) {
    PHINode *Original = Cleanup->OriginalPHIs[I];
    auto *Start = CleanupBuilder.CreatePHI(
        Original->getType(), 2, Original->getName() + ".spork.cleanup.start");
    Start->addIncoming(Cleanup->InitialValues[I], Preheader);
    Start->addIncoming(FinalValues[I], FinalLatch);
    CleanupStarts.push_back(Start);

    auto *CleanupPN = cast<PHINode>(Cleanup->VMap.lookup(Original));
    int PreheaderIndex = CleanupPN->getBasicBlockIndex(Preheader);
    if (PreheaderIndex < 0)
      llvm_unreachable("validated cleanup PHI lost its preheader input");
    CleanupPN->setIncomingBlock(PreheaderIndex, CleanupPreheader);
    CleanupPN->setIncomingValue(PreheaderIndex, Start);
  }
  Value *CleanupReady = createBatchGuard(
      CleanupBuilder, CleanupStarts[InductionIndex], Progress, 1);
  CleanupBuilder.CreateCondBr(CleanupReady, Cleanup->Header, Exit);

  // Guard initial entry with a current volatile bound snapshot.
  auto *OldPreheaderBranch = cast<UncondBrInst>(Preheader->getTerminator());
  IRBuilder<> EntryBuilder(OldPreheaderBranch);
  Value *InitialBase = Progress.Induction->getIncomingValueForBlock(Preheader);
  Value *EntryReady =
      createBatchGuard(EntryBuilder, InitialBase, Progress, Count);
  EntryBuilder.CreateCondBr(EntryReady, Header, CleanupPreheader);
  OldPreheaderBranch->eraseFromParent();

  // Materialize the final guard before deleting the first clone's matched
  // condition, whose instructions supply the protocol's type information.
  CondBrInst *OldFinalBranch = LatchBranches.back();
  Value *NextBase = Progress.Induction->getIncomingValueForBlock(FinalLatch);
  IRBuilder<> FinalBuilder(OldFinalBranch);
  Value *NextReady = createBatchGuard(FinalBuilder, NextBase, Progress, Count);

  // Remove the first Count-1 checks. Their volatile loads are deliberately
  // removed as part of the batching protocol.
  for (unsigned I = 0; I + 1 < Count; ++I) {
    CondBrInst *Old = LatchBranches[I];
    BasicBlock *Next = MainLoop->contains(Old->getSuccessor(0))
                           ? Old->getSuccessor(0)
                           : Old->getSuccessor(1);
    UncondBrInst::Create(Next, Old->getIterator());
    Old->eraseFromParent();
    eraseExitCondition(ExitConditions[I]);
  }

  // The final latch checks whether the *next* complete batch fits.
  FinalBuilder.CreateCondBr(NextReady, Header, CleanupPreheader);
  OldFinalBranch->eraseFromParent();
  eraseExitCondition(ExitConditions.back());

  for (StoreInst *Store : MainStores) {
    Store->setMetadata(SporkProgressMetadata, nullptr);
    Store->setVolatile(true);
  }
  for (StoreInst *Store : ArrayRef(MainStores).drop_back())
    Store->eraseFromParent();
  return true;
}

static void restoreTaggedProgressStores(Function &F, MDNode *Tag) {
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *Store = dyn_cast<StoreInst>(&I);
          Store && Store->getMetadata(SporkProgressMetadata) == Tag) {
        Store->setMetadata(SporkProgressMetadata, nullptr);
        Store->setVolatile(true);
      }
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

      std::unique_ptr<DetachedCleanupLoop> Cleanup =
          cloneCleanupLoop(L, *Progress);
      if (!Cleanup) {
        Progress->Store->setMetadata(SporkProgressMetadata, nullptr);
        Progress->Store->setVolatile(true);
        Decision.reset();
      }

      if (Decision) {
        UnrollLoopOptions ULO;
        ULO.Count = Decision->Count;
        ULO.Force = true;
        // First form LLVM's checked clone chain. Once its exact shape has been
        // validated, replace the intermediate checks with one up-front batch
        // guard and append the untouched scalar cleanup loop.
        ULO.Runtime = false;
        ULO.AllowExpensiveTripCount =
            Decision->Preferences.AllowExpensiveTripCount;
        ULO.UnrollRemainder = Decision->Preferences.UnrollRemainder;
        ULO.ForgetAllSCEV = false;
        ULO.SCEVExpansionBudget = Decision->Preferences.SCEVExpansionBudget;
        ULO.RuntimeUnrollMultiExit =
            Decision->Preferences.RuntimeUnrollMultiExit;
        ULO.AddAdditionalAccumulators =
            Decision->Preferences.AddAdditionalAccumulators;

        Loop *RemainderLoop = nullptr;
        LoopUnrollResult Result =
            UnrollLoop(L, ULO, &LI, &SE, &DT, &AC, &TTI, &ORE,
                       /*PreserveLCSSA=*/true, &RemainderLoop);
        if (Result == LoopUnrollResult::Unmodified) {
          restoreTaggedProgressStores(F, Tag);
        } else {
          Loop *MainLoop =
              Result == LoopUnrollResult::PartiallyUnrolled ? L : nullptr;
          if (formBatchedLoopWithCleanup(F, MainLoop, *Progress, Tag,
                                         Decision->Count, DT,
                                         std::move(Cleanup))) {
            Factor = Decision->Count;
            SE.forgetAllLoops();
            DT.recalculate(F);
            LI.releaseMemory();
            LI.analyze(DT);
          } else {
            restoreTaggedProgressStores(F, Tag);
          }
          Changed = true;
        }
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

struct SporkUnrollPass : OptionalPassInfoMixin<SporkUnrollPass> {
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
        // Run after inlining but before LLVM's loop and SLP vectorizers. The
        // batched body is straight-line code by this point, so the normal
        // vector pipeline can form SIMD operations without crossing volatile
        // progress/boundary updates.
        PB.registerOptimizerEarlyEPCallback(
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
