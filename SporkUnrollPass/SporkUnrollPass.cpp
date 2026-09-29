//===- SporkUnrollPass.cpp - probe-driven batching for Spork parfor loops -===//
//
// This plugin batches the interruptible loop used by Spork's `parfor` so that
// LLVM's ordinary vectorizer and unroller can operate on it.
//
// The loop publishes its progress through a volatile store after every
// iteration and re-reads a volatile bound that an asynchronous promotion
// handler may shorten.  Those two operations pin every iteration in place, so
// LLVM will neither unroll nor vectorize the loop as written.
//
// Instead of guessing a factor, the pass asks LLVM directly:
//
//   1. It outlines a *sequential* copy of the marked loop into a scratch
//      function: no progress store, a loop-invariant bound parameter, and an
//      induction normalized to count from zero.
//   2. It runs the same vectorizer passes clang runs later on that function.
//      Every loop that survives (main vector loop, epilogue vector loop,
//      scalar remainder) is a "step" loop that advances the original index by
//      a constant step; the largest step is the batch K.
//   3. If the probe vectorized, the probe *is* the new loop: each step loop
//      gets a fresh volatile bound check before it is entered and at its
//      latch, publishes progress after every step, and every value that LLVM
//      derived from the trip-count snapshot (resume indices, "no remainder"
//      tests) is replaced by the real exit index.  The rewritten probe is
//      called in place of the original loop and inlined, so the batched loop
//      is exactly LLVM's sequential code plus the protocol.
//   4. If the probe only unrolled, the loop is instead restructured into an
//      outer batch loop around an inner constant-K loop, which the real
//      unroller then flattens.
//   5. K is published to the promotion handler by replacing every
//      `__spork_get_unroll_factor(site)` call with the constant.
//
// The protocol tolerates any step size up to K: a step of s iterations is
// legal iff `i + s - 1 < loop_end` held on a fresh volatile load after the
// previous progress store.  Both rewrites maintain exactly that invariant.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/LoopLoadElimination.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LCSSA.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Transforms/Vectorize/LoopVectorize.h"
#include "llvm/Transforms/Vectorize/SLPVectorizer.h"
#include "llvm/Transforms/Vectorize/VectorCombine.h"

#include <optional>

#define DEBUG_TYPE "spork-unroll"

using namespace llvm;

static cl::opt<unsigned> SporkMaxBatch(
    "spork-max-batch", cl::init(1024), cl::Hidden,
    cl::desc("Largest batch size the Spork unroll pass will accept from the "
             "probe; larger probe results fall back to factor one"));

static cl::opt<unsigned> SporkForceBatch(
    "spork-force-batch", cl::init(0), cl::Hidden,
    cl::desc("Skip the probe and batch every supported Spork loop by this "
             "factor (0 = ask the probe)"));

static cl::opt<bool> SporkInnerHints(
    "spork-inner-hints", cl::init(true), cl::Hidden,
    cl::desc("Attach vectorize.width/interleave.count hints, taken from the "
             "probe, to the inner batch loop"));

static cl::opt<bool> SporkVerify(
    "spork-unroll-verify", cl::init(false), cl::Hidden,
    cl::desc("Run the IR verifier immediately after the Spork unroll pass"));

static cl::opt<bool> SporkDumpProbe(
    "spork-unroll-dump-probe", cl::init(false), cl::Hidden,
    cl::desc("Print each probe function after the vector/unroll pipeline"));

static cl::opt<bool> SporkVerbose(
    "spork-unroll-verbose", cl::init(false), cl::Hidden,
    cl::desc("Print the batch decision made for every Spork loop site"));

namespace {

static constexpr StringLiteral SporkUnrollLoopName("__spork_unroll_loop");
static constexpr StringLiteral
    SporkGetUnrollFactorName("__spork_get_unroll_factor");

//===----------------------------------------------------------------------===//
// Protocol recognition
//===----------------------------------------------------------------------===//

static bool isValidSporkLoopMarker(CallInst *Call) {
  if (!Call || Call->arg_size() != 1 || !Call->getType()->isVoidTy() ||
      !Call->getArgOperand(0)->getType()->isPointerTy())
    return false;
  auto *Callee =
      dyn_cast<Function>(Call->getCalledOperand()->stripPointerCasts());
  return Callee && Callee->getName() == SporkUnrollLoopName &&
         Callee->isDeclaration();
}

static bool isValidSporkFactorCall(CallInst *Call) {
  if (!Call || Call->arg_size() != 1 || !Call->getType()->isIntegerTy() ||
      !Call->getArgOperand(0)->getType()->isPointerTy())
    return false;
  auto *Callee =
      dyn_cast<Function>(Call->getCalledOperand()->stripPointerCasts());
  return Callee && Callee->getName() == SporkGetUnrollFactorName &&
         Callee->isDeclaration();
}

static GlobalValue *getSiteToken(CallInst *Call) {
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

static bool definedInLoop(const Loop *L, const Value *V) {
  auto *I = dyn_cast<Instruction>(V);
  return I && L->contains(I);
}

static AllocaInst *resolveAlloca(Value *V) {
  SmallPtrSet<Value *, 8> Visited;
  while (V && Visited.insert(V).second) {
    V = V->stripPointerCasts();
    if (auto *AI = dyn_cast<AllocaInst>(V))
      return AI;
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V);
        GEP && GEP->hasAllZeroIndices()) {
      V = GEP->getPointerOperand();
      continue;
    }
    return nullptr;
  }
  return nullptr;
}

static bool isHarmlessCall(const CallBase &CB) {
  auto *II = dyn_cast<IntrinsicInst>(&CB);
  if (!II)
    return false;
  return II->isAssumeLikeIntrinsic() || !II->mayReadOrWriteMemory();
}

// Blocks between the marker and the loop(s) it denotes are the zero-trip
// guard and whatever LLVM's loop unswitching left behind: cheap straight-line
// code.  Anything that looks like the loop's epilogue (atomic/volatile stores,
// fences, real calls, or another marker) ends the walk.
static bool isWalkBarrier(BasicBlock *BB) {
  for (Instruction &I : *BB) {
    if (auto *Call = dyn_cast<CallInst>(&I)) {
      if (isValidSporkLoopMarker(Call) || !isHarmlessCall(*Call))
        return true;
    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (SI->isVolatile() || SI->isAtomic())
        return true;
    } else if (isa<InvokeInst>(I) || isa<FenceInst>(I) ||
               isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I)) {
      return true;
    }
  }
  return false;
}

static constexpr unsigned MaxMarkerWalkDepth = 8;

// Collect the loops a marker denotes: every loop entered through its canonical
// preheader from the marker block via barrier-free bridge blocks that sit at
// the marker's own loop depth.  Unswitching may have produced several copies;
// they are all the marked loop.  Returns false if the shape is ambiguous.
static bool findMarkedLoops(CallInst *Marker, LoopInfo &LI,
                            SmallVectorImpl<Loop *> &Loops) {
  BasicBlock *Start = Marker->getParent();
  Loop *Enclosing = LI.getLoopFor(Start);
  SmallPtrSet<BasicBlock *, 16> Visited{Start};
  SmallVector<std::pair<BasicBlock *, unsigned>, 16> Work{{Start, 0}};

  while (!Work.empty()) {
    auto [BB, Depth] = Work.pop_back_val();
    for (BasicBlock *S : successors(BB)) {
      Loop *LS = LI.getLoopFor(S);
      if (LS != Enclosing) {
        // Leaving the enclosing loop is not a path to the marked loop.
        if (!LS || (Enclosing && !Enclosing->contains(LS)))
          continue;
        if (LS->getHeader() != S || LS->getParentLoop() != Enclosing ||
            LS->getLoopPreheader() != BB)
          return false;
        if (!llvm::is_contained(Loops, LS))
          Loops.push_back(LS);
        continue;
      }
      if (!Visited.insert(S).second || isWalkBarrier(S))
        continue;
      if (Depth + 1 > MaxMarkerWalkDepth)
        return false;
      Work.push_back({S, Depth + 1});
    }
  }
  return !Loops.empty();
}

// A chain of integer casts between a root value and a derived value, kept as
// (opcode, destination type) so it can be replayed on a replacement root even
// after the original instructions are deleted.
struct CastChain {
  SmallVector<std::pair<Instruction::CastOps, Type *>, 4> Steps;

  static std::optional<CastChain> capture(Value *Template, Value *Root) {
    CastChain Chain;
    Value *V = Template;
    while (V != Root) {
      auto *Cast = dyn_cast<CastInst>(V);
      if (!Cast || !Cast->getSrcTy()->isIntegerTy() ||
          !Cast->getDestTy()->isIntegerTy())
        return std::nullopt;
      Chain.Steps.push_back({Cast->getOpcode(), Cast->getDestTy()});
      V = Cast->getOperand(0);
    }
    std::reverse(Chain.Steps.begin(), Chain.Steps.end());
    return Chain;
  }

  Value *replay(IRBuilder<> &Builder, Value *Root, const Twine &Name) const {
    Value *Result = Root;
    for (auto &[Op, Ty] : Steps)
      Result = Builder.CreateCast(Op, Result, Ty, Name);
    return Result;
  }
};

struct HeaderPhi {
  PHINode *PN = nullptr;
  Value *Init = nullptr;     // incoming from the preheader
  Value *LatchVal = nullptr; // incoming from the latch
};

// Everything the transformation needs to know about one marked loop.  All
// validation happens while this is built, so the rewrite itself cannot fail.
struct LoopProtocol {
  Function *F = nullptr;
  Loop *L = nullptr;
  BasicBlock *Preheader = nullptr;
  BasicBlock *Header = nullptr;
  BasicBlock *Latch = nullptr;
  BasicBlock *Exit = nullptr;
  CondBrInst *LatchBranch = nullptr;

  PHINode *Induction = nullptr;
  Instruction *NextInduction = nullptr;
  unsigned InductionIndex = 0;
  SmallVector<HeaderPhi, 4> Phis;

  StoreInst *ProgressStore = nullptr;
  CastChain ProgressCasts; // NextInduction -> stored value
  Value *ProgressPtr = nullptr;
  Align ProgressAlign;
  AtomicOrdering ProgressOrdering = AtomicOrdering::NotAtomic;
  SyncScope::ID ProgressScope = SyncScope::System;

  LoadInst *BoundLoad = nullptr;
  ICmpInst *ExitCompare = nullptr;
  ICmpInst::Predicate Predicate = ICmpInst::BAD_ICMP_PREDICATE;
  CastChain NextCasts;  // NextInduction -> compare operand 0
  CastChain BoundCasts; // BoundLoad -> compare operand 1
  SmallVector<Instruction *, 4> ExitConditionInsts; // compare, casts, load

  // Captured from BoundLoad so guards can be built after it is deleted.
  Type *BoundTy = nullptr;
  Value *BoundPtr = nullptr;
  Align BoundAlign;
  AtomicOrdering BoundOrdering = AtomicOrdering::NotAtomic;
  SyncScope::ID BoundScope = SyncScope::System;
};

static std::optional<LoopProtocol>
matchLoopProtocol(Function &F, Loop *L, CallInst *Marker, ScalarEvolution &SE,
                  DominatorTree &DT, const char *&Reason) {
#define SPORK_REJECT(Why)                                                      \
  do {                                                                         \
    Reason = Why;                                                              \
    return std::nullopt;                                                       \
  } while (0)
  LoopProtocol P;
  P.F = &F;
  P.L = L;
  if (!L->isLoopSimplifyForm() || !L->isLCSSAForm(DT) ||
      !L->getSubLoops().empty())
    SPORK_REJECT("not in loop-simplify/LCSSA form or has nested loops");

  P.Preheader = L->getLoopPreheader();
  P.Header = L->getHeader();
  P.Latch = L->getLoopLatch();
  P.Exit = L->getExitBlock();
  if (!P.Preheader || !P.Latch || !P.Exit || L->getExitingBlock() != P.Latch)
    SPORK_REJECT("needs a unique latch that is the only exiting block and a unique exit");
  if (!isa<UncondBrInst>(P.Preheader->getTerminator()))
    SPORK_REJECT("preheader does not end in an unconditional branch");

  P.LatchBranch = dyn_cast<CondBrInst>(P.Latch->getTerminator());
  if (!P.LatchBranch || P.LatchBranch->getSuccessor(0) != P.Header ||
      P.LatchBranch->getSuccessor(1) != P.Exit)
    SPORK_REJECT("latch branch is not 'continue-to-header else exit'");

  // Induction: LLVM-recognized, unit positive step.
  P.Induction = L->getInductionVariable(SE);
  if (!P.Induction || !P.Induction->getType()->isIntegerTy())
    SPORK_REJECT("no LLVM-recognized integer induction variable");
  auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(P.Induction));
  if (!AR || AR->getLoop() != L || !AR->getStepRecurrence(SE)->isOne())
    SPORK_REJECT("induction step is not +1");
  P.NextInduction =
      dyn_cast<Instruction>(P.Induction->getIncomingValueForBlock(P.Latch));
  if (!P.NextInduction || !L->contains(P.NextInduction) ||
      isa<CastInst>(P.NextInduction))
    SPORK_REJECT("next induction value is not a plain in-loop instruction");

  for (PHINode &PN : P.Header->phis()) {
    HeaderPhi HP;
    HP.PN = &PN;
    HP.Init = PN.getIncomingValueForBlock(P.Preheader);
    HP.LatchVal = PN.getIncomingValueForBlock(P.Latch);
    if (!HP.Init || !HP.LatchVal)
      SPORK_REJECT("header PHI lacks preheader or latch input");
    if (&PN == P.Induction)
      P.InductionIndex = P.Phis.size();
    P.Phis.push_back(HP);
  }

  // Exactly one volatile store in the latch, storing the next induction value.
  for (Instruction &I : *P.Latch) {
    auto *SI = dyn_cast<StoreInst>(&I);
    if (!SI || !SI->isVolatile())
      continue;
    if (P.ProgressStore)
      SPORK_REJECT("more than one volatile store in the latch");
    P.ProgressStore = SI;
  }
  if (!P.ProgressStore ||
      stripIntegerCasts(P.ProgressStore->getValueOperand()) !=
          P.NextInduction ||
      definedInLoop(L, P.ProgressStore->getPointerOperand()))
    SPORK_REJECT("latch volatile store does not publish the next induction value");
  auto ProgressCasts =
      CastChain::capture(P.ProgressStore->getValueOperand(), P.NextInduction);
  if (!ProgressCasts)
    SPORK_REJECT("stored progress value is not a cast chain of the next induction");
  P.ProgressCasts = *ProgressCasts;
  P.ProgressPtr = P.ProgressStore->getPointerOperand();
  P.ProgressAlign = P.ProgressStore->getAlign();
  P.ProgressOrdering = P.ProgressStore->getOrdering();
  P.ProgressScope = P.ProgressStore->getSyncScopeID();

  // Exit condition: casts(next) <s/u casts(volatile load bound).
  P.ExitCompare = dyn_cast<ICmpInst>(P.LatchBranch->getCondition());
  if (!P.ExitCompare || !P.ExitCompare->hasOneUse())
    SPORK_REJECT("latch condition is not a single-use icmp");
  P.Predicate = P.ExitCompare->getPredicate();
  if (P.Predicate != ICmpInst::ICMP_SLT && P.Predicate != ICmpInst::ICMP_ULT)
    SPORK_REJECT("latch condition is not signed/unsigned less-than");
  auto NextCasts =
      CastChain::capture(P.ExitCompare->getOperand(0), P.NextInduction);
  if (!NextCasts)
    SPORK_REJECT("compare operand is not a cast chain of the next induction");
  P.NextCasts = *NextCasts;

  P.ExitConditionInsts.push_back(P.ExitCompare);
  Value *V = P.ExitCompare->getOperand(1);
  while (auto *Cast = dyn_cast<CastInst>(V)) {
    if (!Cast->hasOneUse() || !Cast->getSrcTy()->isIntegerTy() ||
        !Cast->getDestTy()->isIntegerTy())
      SPORK_REJECT("bound cast chain has extra uses");
    P.ExitConditionInsts.push_back(Cast);
    V = Cast->getOperand(0);
  }
  P.BoundLoad = dyn_cast<LoadInst>(V);
  if (!P.BoundLoad || !P.BoundLoad->isVolatile() || !P.BoundLoad->hasOneUse() ||
      !P.BoundLoad->getType()->isIntegerTy() ||
      P.BoundLoad->getParent() != P.Latch ||
      definedInLoop(L, P.BoundLoad->getPointerOperand()))
    SPORK_REJECT("bound is not a single-use volatile integer load in the latch");
  P.ExitConditionInsts.push_back(P.BoundLoad);
  P.BoundCasts = *CastChain::capture(P.ExitCompare->getOperand(1), P.BoundLoad);
  P.BoundTy = P.BoundLoad->getType();
  P.BoundPtr = P.BoundLoad->getPointerOperand();
  P.BoundAlign = P.BoundLoad->getAlign();
  P.BoundOrdering = P.BoundLoad->getOrdering();
  P.BoundScope = P.BoundLoad->getSyncScopeID();

  // Progress and bound must be distinct stack slots, and ordinary code must
  // never observe intermediate progress values (only the asynchronous handler
  // may read the slot once the loop has started).
  AllocaInst *ProgressSlot = resolveAlloca(P.ProgressStore->getPointerOperand());
  if (!ProgressSlot || resolveAlloca(P.BoundPtr) == ProgressSlot)
    SPORK_REJECT("progress/bound slots are not distinct stack slots");
  for (User *U : ProgressSlot->users()) {
    auto *LI = dyn_cast<LoadInst>(U);
    if (!LI)
      continue;
    if (L->contains(LI) || !DT.dominates(LI, Marker))
      SPORK_REJECT("progress slot is read by ordinary code in or after the loop");
  }

  // The body may contain no other volatile/atomic accesses, no calls with
  // side effects, and nothing that may throw.
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if ((LI->isVolatile() || LI->isAtomic()) && LI != P.BoundLoad)
          SPORK_REJECT("body contains another volatile/atomic load");
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if ((SI->isVolatile() || SI->isAtomic()) && SI != P.ProgressStore)
          SPORK_REJECT("body contains another volatile/atomic store");
      } else if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (!isHarmlessCall(*CB))
          SPORK_REJECT("body contains a call");
      } else if (isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I) ||
                 isa<FenceInst>(I)) {
        SPORK_REJECT("body contains an atomic/fence");
      }
      if (I.mayThrow())
        SPORK_REJECT("body contains an instruction that may throw");
    }

  // Exit values must be reconstructible at a batch boundary: loop-invariant,
  // or the latch value of a header PHI.
  for (PHINode &PN : P.Exit->phis()) {
    Value *V = PN.getIncomingValueForBlock(P.Latch);
    if (!V)
      SPORK_REJECT("exit PHI lacks a latch input");
    auto *I = dyn_cast<Instruction>(V);
    if (!I || !L->contains(I))
      continue;
    if (llvm::none_of(P.Phis, [&](const HeaderPhi &HP) { return HP.LatchVal == V; }))
      SPORK_REJECT("exit PHI uses a loop value that is not a header PHI's latch value");
  }

  Reason = nullptr;
  return P;
#undef SPORK_REJECT
}

//===----------------------------------------------------------------------===//
// Probe: outline a sequential copy and ask LLVM what it would do
//===----------------------------------------------------------------------===//

static bool isForwardableParamAttr(const Attribute &A) {
  if (!A.isEnumAttribute() && !A.isIntAttribute())
    return false;
  switch (A.getKindAsEnum()) {
  case Attribute::NoAlias:
  case Attribute::NonNull:
  case Attribute::Dereferenceable:
  case Attribute::DereferenceableOrNull:
  case Attribute::Alignment:
  case Attribute::ReadOnly:
  case Attribute::ReadNone:
  case Attribute::NoUndef:
    return true;
  default:
    return false;
  }
}

struct ProbeResult {
  unsigned Batch = 1;
  unsigned VF = 0; // vector width observed in the probe, 0 if not vectorized
};

// A scratch function holding LLVM's optimized sequential version of the loop.
struct ProbeInfo {
  Function *Probe = nullptr;
  ProbeResult Result;
  bool Vectorized = false;
  Value *Start = nullptr;            // probe-side initial induction value
  Argument *ProgressPtrArg = nullptr;
  Argument *BoundPtrArg = nullptr;
  SmallVector<Value *, 8> Inputs;    // caller-side values, parameter order
  SmallVector<Value *, 4> LiveOuts;  // caller-side loop values it returns
};

static void createProgressStore(IRBuilder<> &B, Value *INext, Value *Ptr,
                                const LoopProtocol &P) {
  Value *Stored = P.ProgressCasts.replay(B, INext, "spork.progress");
  StoreInst *S = B.CreateStore(Stored, Ptr);
  S->setVolatile(true);
  S->setAlignment(P.ProgressAlign);
  S->setOrdering(P.ProgressOrdering);
  S->setSyncScopeID(P.ProgressScope);
}

static LoadInst *createBoundLoad(IRBuilder<> &B, Value *Ptr,
                                 const LoopProtocol &P) {
  LoadInst *End = B.CreateLoad(P.BoundTy, Ptr, "spork.end");
  End->setVolatile(true);
  End->setAlignment(P.BoundAlign);
  End->setOrdering(P.BoundOrdering);
  End->setSyncScopeID(P.BoundScope);
  return End;
}

// !(overflow(base + (count-1))) && casts(base + count - 1) < casts(bound)
static Value *createBatchGuard(IRBuilder<> &Builder, Value *Base,
                               const LoopProtocol &P, unsigned Count,
                               Value *BoundPtr) {
  auto *BaseTy = cast<IntegerType>(Base->getType());
  LoadInst *End = createBoundLoad(Builder, BoundPtr, P);

  Value *Last = Base;
  Value *Overflow = nullptr;
  if (Count > 1) {
    bool Signed = ICmpInst::isSigned(P.Predicate);
    Value *Offset = ConstantInt::get(BaseTy, Count - 1);
    Intrinsic::ID AddID =
        Signed ? Intrinsic::sadd_with_overflow : Intrinsic::uadd_with_overflow;
    Value *Add = Builder.CreateBinaryIntrinsic(AddID, Base, Offset, nullptr,
                                               "spork.last");
    Last = Builder.CreateExtractValue(Add, 0, "spork.last.value");
    Overflow = Builder.CreateExtractValue(Add, 1, "spork.last.overflow");
  }
  Value *ComparedLast = P.NextCasts.replay(Builder, Last, "spork.last.cast");
  Value *ComparedEnd = P.BoundCasts.replay(Builder, End, "spork.end.cast");
  Value *InBounds = Builder.CreateICmp(P.Predicate, ComparedLast, ComparedEnd,
                                       "spork.in.bounds");
  if (!Overflow)
    return InBounds;
  return Builder.CreateAnd(Builder.CreateNot(Overflow), InBounds,
                           "spork.batch.ready");
}

static bool hasVectorLoop(LoopInfo &LI) {
  for (Loop *L : LI.getLoopsInPreorder())
    for (BasicBlock *BB : L->blocks())
      for (Instruction &I : *BB)
        if (isa<FixedVectorType>(I.getType()))
          return true;
  return false;
}

// Every loop left in the probe descends from the marked loop.  The step of
// the slowest-moving integer induction in a loop is the number of original
// iterations one trip of that loop performs; the largest such step is the
// batch LLVM formed.
static ProbeResult measureProbe(Function &Probe, FunctionAnalysisManager &FAM) {
  ProbeResult Result;
  auto &PLI = FAM.getResult<LoopAnalysis>(Probe);
  auto &PSE = FAM.getResult<ScalarEvolutionAnalysis>(Probe);
  for (Loop *PL : PLI.getLoopsInPreorder()) {
    uint64_t MinStep = 0;
    unsigned MinVF = 0;
    for (PHINode &PN : PL->getHeader()->phis()) {
      if (!PN.getType()->isIntegerTy())
        continue;
      auto *AR = dyn_cast<SCEVAddRecExpr>(PSE.getSCEV(&PN));
      if (!AR || AR->getLoop() != PL)
        continue;
      auto *Step = dyn_cast<SCEVConstant>(AR->getStepRecurrence(PSE));
      if (!Step)
        continue;
      uint64_t S = Step->getAPInt().abs().getLimitedValue();
      if (S && (!MinStep || S < MinStep))
        MinStep = S;
    }
    for (BasicBlock *BB : PL->blocks())
      for (Instruction &I : *BB)
        if (auto *VT = dyn_cast<FixedVectorType>(I.getType()))
          if (!MinVF || VT->getNumElements() < MinVF)
            MinVF = VT->getNumElements();
    if (MinStep > Result.Batch) {
      Result.Batch = MinStep;
      Result.VF = MinVF;
    }
  }
  return Result;
}

static void eraseProbe(ProbeInfo &PI, FunctionAnalysisManager &FAM) {
  if (!PI.Probe)
    return;
  FAM.clear(*PI.Probe, PI.Probe->getName());
  PI.Probe->eraseFromParent();
  PI.Probe = nullptr;
}

// Outline the sequential copy:
//
//   define <liveouts> @probe(inputs..., BoundTy %bound, ptr %progress,
//                            ptr %boundptr)
//     entry:   n = casts(bound) - casts(start)
//     header:  idx = phi [0, entry], [idx.next, latch]; i = start + idx
//     ...      original body
//     latch:   idx.next = idx + 1; br (casts(idx.next) < n), header, exit
//     exit:    ret liveouts
//
// Then run clang's vector pipeline on it and measure the batch.
static ProbeInfo buildProbe(Module &M, const LoopProtocol &P,
                            FunctionAnalysisManager &FAM, unsigned OptLevel) {
  Function &F = *P.F;
  Loop *L = P.L;
  LLVMContext &Ctx = M.getContext();
  ProbeInfo PI;

  // Values defined outside the loop become parameters.
  SetVector<Value *> Inputs;
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB)
      for (Value *Op : I.operands()) {
        if (auto *OpI = dyn_cast<Instruction>(Op)) {
          if (!L->contains(OpI))
            Inputs.insert(OpI);
        } else if (isa<Argument>(Op)) {
          Inputs.insert(Op);
        }
      }
  PI.Inputs.assign(Inputs.begin(), Inputs.end());

  // Values that flow out of the loop (its LCSSA exit PHIs) are returned.
  for (PHINode &PN : P.Exit->phis()) {
    Value *V = PN.getIncomingValueForBlock(P.Latch);
    if (V && definedInLoop(L, V) && !llvm::is_contained(PI.LiveOuts, V))
      PI.LiveOuts.push_back(V);
  }

  SmallVector<Type *, 16> ParamTys;
  for (Value *V : PI.Inputs)
    ParamTys.push_back(V->getType());
  ParamTys.push_back(P.BoundTy);
  ParamTys.push_back(PointerType::getUnqual(Ctx));
  ParamTys.push_back(PointerType::getUnqual(Ctx));

  Type *RetTy = Type::getVoidTy(Ctx);
  if (PI.LiveOuts.size() == 1) {
    RetTy = PI.LiveOuts.front()->getType();
  } else if (!PI.LiveOuts.empty()) {
    SmallVector<Type *, 4> Tys;
    for (Value *V : PI.LiveOuts)
      Tys.push_back(V->getType());
    RetTy = StructType::get(Ctx, Tys);
  }

  auto *FT = FunctionType::get(RetTy, ParamTys, false);
  Function *Probe = Function::Create(FT, GlobalValue::InternalLinkage,
                                     F.getName() + ".spork.probe", &M);
  PI.Probe = Probe;
  Probe->addFnAttrs(AttrBuilder(Ctx, F.getAttributes().getFnAttrs()));
  Probe->removeFnAttr(Attribute::NoInline);
  Probe->removeFnAttr(Attribute::AlwaysInline);
  Probe->setCallingConv(F.getCallingConv());
  for (auto [Idx, V] : llvm::enumerate(PI.Inputs)) {
    if (auto *A = dyn_cast<Argument>(V)) {
      AttributeSet PA = F.getAttributes().getParamAttrs(A->getArgNo());
      AttrBuilder B(Ctx);
      for (const Attribute &Attr : PA)
        if (isForwardableParamAttr(Attr))
          B.addAttribute(Attr);
      Probe->addParamAttrs(Idx, B);
    }
  }
  unsigned NumInputs = PI.Inputs.size();
  Argument *BoundArg = Probe->getArg(NumInputs);
  PI.ProgressPtrArg = Probe->getArg(NumInputs + 1);
  PI.BoundPtrArg = Probe->getArg(NumInputs + 2);
  BoundArg->setName("spork.bound");
  PI.ProgressPtrArg->setName("spork.progress.ptr");
  PI.BoundPtrArg->setName("spork.bound.ptr");
  Probe->addParamAttr(NumInputs + 1, Attribute::NoAlias);
  Probe->addParamAttr(NumInputs + 2, Attribute::NoAlias);

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Probe);
  BasicBlock *ExitBB = BasicBlock::Create(Ctx, "exit", Probe);

  ValueToValueMapTy VMap;
  for (auto [Idx, V] : llvm::enumerate(PI.Inputs))
    VMap[V] = Probe->getArg(Idx);
  VMap[P.Preheader] = Entry;
  VMap[P.Exit] = ExitBB;

  SmallVector<BasicBlock *, 8> NewBlocks;
  for (BasicBlock *BB : L->blocks()) {
    BasicBlock *NewBB = CloneBasicBlock(BB, VMap, ".probe", Probe);
    VMap[BB] = NewBB;
    NewBlocks.push_back(NewBB);
  }
  ExitBB->moveAfter(NewBlocks.back());
  for (BasicBlock *BB : NewBlocks)
    for (Instruction &I : *BB) {
      I.dropDbgRecords();
      RemapInstruction(&I, VMap,
                       RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
    }

  auto *Header = cast<BasicBlock>(VMap[P.Header]);
  auto *Latch = cast<BasicBlock>(VMap[P.Latch]);
  auto *LatchBr = cast<CondBrInst>(VMap[P.LatchBranch]);
  auto *IndPhi = cast<PHINode>(VMap[P.Induction]);
  Value *Init = P.Induction->getIncomingValueForBlock(P.Preheader);
  Value *Start = VMap.lookup(Init);
  if (!Start)
    Start = Init; // constant
  PI.Start = Start;
  Type *IdxTy = P.Induction->getType();
  bool NSW = false, NUW = false;
  if (auto *BO = dyn_cast<BinaryOperator>(P.NextInduction)) {
    NSW = BO->hasNoSignedWrap();
    NUW = BO->hasNoUnsignedWrap();
  }
  bool Signed = ICmpInst::isSigned(P.Predicate);

  // entry: n = casts(bound) - casts(start); br header
  IRBuilder<> EB(Entry);
  Value *StartC = P.NextCasts.replay(EB, Start, "spork.start.cast");
  Value *BoundC = P.BoundCasts.replay(EB, BoundArg, "spork.bound.cast");
  Value *N = EB.CreateSub(BoundC, StartC, "spork.n", /*NUW=*/!Signed,
                          /*NSW=*/Signed);
  EB.CreateBr(Header);

  // header: idx = phi [0, entry], [idx.next, latch]; i = start + idx
  PHINode *Idx = PHINode::Create(IdxTy, 2, "spork.idx", Header->begin());
  Idx->addIncoming(ConstantInt::get(IdxTy, 0), Entry);
  IRBuilder<> HB(Header, Header->getFirstNonPHIIt());
  Value *IVal = HB.CreateAdd(Start, Idx, "spork.i", NUW, NSW);
  IndPhi->replaceAllUsesWith(IVal);
  IndPhi->eraseFromParent();

  // latch: idx.next = idx + 1; br casts(idx.next) < n
  IRBuilder<> LB(LatchBr);
  Value *IdxNext = LB.CreateAdd(Idx, ConstantInt::get(IdxTy, 1),
                                "spork.idx.next", /*NUW=*/true, /*NSW=*/true);
  Idx->addIncoming(IdxNext, Latch);
  Value *IdxNextC = P.NextCasts.replay(LB, IdxNext, "spork.idx.next.cast");
  Value *Cond = LB.CreateICmp(P.Predicate, IdxNextC, N, "spork.cont");
  LatchBr->setCondition(Cond);
  LatchBr->setMetadata(LLVMContext::MD_loop, nullptr);
  for (Instruction *I : P.ExitConditionInsts)
    cast<Instruction>(VMap[I])->eraseFromParent();
  cast<StoreInst>(VMap[P.ProgressStore])->eraseFromParent();

  // exit: ret liveouts (through LCSSA PHIs)
  IRBuilder<> XB(ExitBB);
  Value *Ret = nullptr;
  if (PI.LiveOuts.size() == 1) {
    PHINode *PN = XB.CreatePHI(RetTy, 1, "spork.liveout");
    PN->addIncoming(VMap[PI.LiveOuts.front()], Latch);
    Ret = PN;
  } else if (!PI.LiveOuts.empty()) {
    SmallVector<Value *, 4> Parts;
    for (Value *V : PI.LiveOuts) {
      PHINode *PN = XB.CreatePHI(V->getType(), 1, "spork.liveout");
      PN->addIncoming(VMap[V], Latch);
      Parts.push_back(PN);
    }
    Ret = PoisonValue::get(RetTy);
    for (auto [I, V] : llvm::enumerate(Parts))
      Ret = XB.CreateInsertValue(Ret, V, I);
  }
  if (Ret)
    XB.CreateRet(Ret);
  else
    XB.CreateRetVoid();

  if (verifyFunction(*Probe, &errs())) {
    errs() << "spork-unroll: probe for " << F.getName()
           << " failed verification; using factor 1\n";
    eraseProbe(PI, FAM);
    return PI;
  }

  // Mirror the vector part of clang's optimization pipeline.
  {
    FunctionPassManager FPM;
    FPM.addPass(LoopVectorizePass());
    FPM.addPass(LoopLoadEliminationPass());
    FPM.addPass(InstCombinePass());
    FPM.addPass(SimplifyCFGPass(SimplifyCFGOptions()
                                    .forwardSwitchCondToPhi(true)
                                    .convertSwitchRangeToICmp(true)
                                    .convertSwitchToLookupTable(true)
                                    .needCanonicalLoops(false)
                                    .hoistCommonInsts(true)
                                    .sinkCommonInsts(true)));
    FPM.addPass(SLPVectorizerPass());
    FPM.addPass(VectorCombinePass());
    FPM.addPass(InstCombinePass());
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(LCSSAPass());
    FPM.run(*Probe, FAM);
  }
  PI.Vectorized = hasVectorLoop(FAM.getResult<LoopAnalysis>(*Probe));
  if (!PI.Vectorized) {
    // Not vectorized: see what the unroller would do instead.
    FunctionPassManager FPM;
    FPM.addPass(LoopUnrollPass(LoopUnrollOptions(OptLevel)));
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(LCSSAPass());
    FPM.run(*Probe, FAM);
  }
  if (SporkDumpProbe)
    Probe->print(errs());
  PI.Result = measureProbe(*Probe, FAM);
  if (PI.Result.Batch > SporkMaxBatch)
    PI.Result = ProbeResult();
  return PI;
}

//===----------------------------------------------------------------------===//
// Transplant: turn the vectorized probe into the batched loop
//===----------------------------------------------------------------------===//

struct StepLoop {
  Loop *L = nullptr;
  BasicBlock *Preheader = nullptr;
  BasicBlock *Header = nullptr;
  BasicBlock *Latch = nullptr;
  BasicBlock *Exit = nullptr;
  CondBrInst *Branch = nullptr;
  ICmpInst *Compare = nullptr;
  PHINode *IV = nullptr;      // index-domain induction (== original idx)
  Value *IVNext = nullptr;    // IV's latch value
  Value *IVEntry = nullptr;   // IV's preheader value
  Value *Limit = nullptr;     // loop-invariant snapshot the exit compared to
  unsigned Step = 0;
};

// Does the loop exit block dominate this use?  PHI uses are attributed to
// their incoming block; a use inside the exit block itself counts.
static bool exitDominatesUse(BasicBlock *Exit, const Use &U,
                             DominatorTree &DT) {
  auto *UI = dyn_cast<Instruction>(U.getUser());
  if (!UI)
    return false;
  if (auto *PN = dyn_cast<PHINode>(UI))
    return DT.dominates(Exit, PN->getIncomingBlock(U));
  return DT.dominates(Exit, UI->getParent());
}

// Code after a step loop's exit may only assume the loop stopped at the real
// exit index, never at the snapshot limit LLVM computed.  This decides, for
// the transitive pure users of the limit, which ones feed a use after the
// exit (and so must be recomputed there) and rejects anything that could
// carry the snapshot past the exit through memory or a call.
struct SinkPlan {
  BasicBlock *Exit;
  DominatorTree &DT;
  Instruction *Skip;
  DenseMap<Instruction *, bool> Needs;
  const char *Failure = nullptr;

  bool needs(Instruction *I) {
    auto It = Needs.find(I);
    if (It != Needs.end())
      return It->second;
    Needs[I] = false; // provisional; pure DAGs have no cycles anyway
    bool Result = false;
    for (Use &U : I->uses())
      if (visitUse(U))
        Result = true;
    Needs[I] = Result;
    return Result;
  }

  // Returns true if this use, or something it feeds, sits after the exit.
  bool visitUse(Use &U) {
    User *Usr = U.getUser();
    if (Usr == Skip)
      return false;
    if (exitDominatesUse(Exit, U, DT))
      return true;
    auto *UI = dyn_cast<Instruction>(Usr);
    if (!UI) {
      Failure = "snapshot limit has a non-instruction user";
      return false;
    }
    if (isa<PHINode>(UI) || UI->isTerminator())
      return false; // a pre-exit merge or decision; the snapshot is fine there
    if (UI->mayReadOrWriteMemory() || UI->mayHaveSideEffects()) {
      Failure = "snapshot limit reaches memory or a call before the exit";
      return false;
    }
    return needs(UI);
  }

  bool plan(Value *Root) {
    for (Use &U : Root->uses())
      visitUse(U);
    return !Failure;
  }
};

// Replace every use of Orig with Repl where the exit dominates the use, and
// clone the pre-exit users that feed such uses into the exit block.
static void replaceOrSinkPastExit(Value *Orig, Value *Repl, SinkPlan &Plan) {
  BasicBlock *Exit = Plan.Exit;
  SmallVector<std::pair<Value *, Value *>, 8> Work{{Orig, Repl}};
  DenseMap<Instruction *, Instruction *> Clones;
  BasicBlock::iterator InsertPt = Exit->getFirstNonPHIIt();
  while (!Work.empty()) {
    auto [O, R] = Work.pop_back_val();
    for (Use &U : llvm::make_early_inc_range(O->uses())) {
      User *Usr = U.getUser();
      if (Usr == Plan.Skip)
        continue;
      if (exitDominatesUse(Exit, U, Plan.DT)) {
        U.set(R);
        continue;
      }
      auto *UI = dyn_cast<Instruction>(Usr);
      if (!UI || !Plan.Needs.lookup(UI))
        continue;
      Instruction *&C = Clones[UI];
      if (!C) {
        C = UI->clone();
        C->setName(UI->getName() + ".spork.sunk");
        C->insertBefore(*Exit, InsertPt);
        Work.push_back({UI, C});
      }
      for (Use &CU : C->operands())
        if (CU.get() == O)
          CU.set(R);
    }
  }
}

static std::optional<StepLoop>
matchStepLoop(Loop *DL, const LoopProtocol &P, ScalarEvolution &SE,
              DominatorTree &DT, const char *&Reason) {
#define SPORK_REJECT(Why)                                                      \
  do {                                                                         \
    Reason = Why;                                                              \
    return std::nullopt;                                                       \
  } while (0)
  StepLoop S;
  S.L = DL;
  if (!DL->getSubLoops().empty() || !DL->isLoopSimplifyForm())
    SPORK_REJECT("probe loop is nested or not in simplify form");
  S.Preheader = DL->getLoopPreheader();
  S.Header = DL->getHeader();
  S.Latch = DL->getLoopLatch();
  S.Exit = DL->getExitBlock();
  if (!S.Preheader || !S.Latch || !S.Exit || DL->getExitingBlock() != S.Latch)
    SPORK_REJECT("probe loop needs a single latch exit and a unique exit");
  if (!isa<UncondBrInst>(S.Preheader->getTerminator()))
    SPORK_REJECT("probe loop preheader has a conditional terminator");
  S.Branch = dyn_cast<CondBrInst>(S.Latch->getTerminator());
  if (!S.Branch)
    SPORK_REJECT("probe loop latch is not a conditional branch");
  bool HeaderFirst = S.Branch->getSuccessor(0) == S.Header;
  if (S.Branch->getSuccessor(HeaderFirst ? 1 : 0) != S.Exit ||
      S.Branch->getSuccessor(HeaderFirst ? 0 : 1) != S.Header)
    SPORK_REJECT("probe loop latch does not branch header/exit");
  S.Compare = dyn_cast<ICmpInst>(S.Branch->getCondition());
  if (!S.Compare || !S.Compare->hasOneUse())
    SPORK_REJECT("probe loop exit condition is not a single-use icmp");

  // Exactly one scalar integer induction, of the original index type.
  Type *IdxTy = P.Induction->getType();
  for (PHINode &PN : S.Header->phis()) {
    if (!PN.getType()->isIntegerTy())
      continue;
    auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(&PN));
    if (!AR || AR->getLoop() != DL)
      continue;
    auto *StepC = dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE));
    if (!StepC || !StepC->getAPInt().isStrictlyPositive())
      SPORK_REJECT("probe loop induction has a non-constant step");
    if (S.IV)
      SPORK_REJECT("probe loop has several integer inductions");
    S.IV = &PN;
    S.Step = StepC->getAPInt().getLimitedValue();
  }
  if (!S.IV || S.IV->getType() != IdxTy || S.Step == 0)
    SPORK_REJECT("probe loop has no induction of the index type");
  S.IVNext = S.IV->getIncomingValueForBlock(S.Latch);
  S.IVEntry = S.IV->getIncomingValueForBlock(S.Preheader);

  // exit compare: casts(IV.next) <pred> Limit  (either operand order)
  unsigned IVIdx = 2;
  for (unsigned I = 0; I < 2; ++I)
    if (stripIntegerCasts(S.Compare->getOperand(I)) == S.IVNext)
      IVIdx = I;
  if (IVIdx == 2)
    SPORK_REJECT("probe loop exit does not compare the next induction");
  S.Limit = S.Compare->getOperand(1 - IVIdx);
  if (definedInLoop(DL, S.Limit) || isa<Constant>(S.Limit))
    SPORK_REJECT("probe loop exit limit is not a loop-invariant value");
  {
    SinkPlan Plan{S.Exit, DT, S.Compare};
    if (!Plan.plan(S.Limit))
      SPORK_REJECT(Plan.Failure);
  }

  // Exit values must be reconstructible when the loop is skipped.
  for (PHINode &PN : S.Exit->phis()) {
    Value *V = PN.getIncomingValueForBlock(S.Latch);
    if (!V)
      SPORK_REJECT("probe exit PHI lacks a latch input");
    if (!definedInLoop(DL, V) || V == S.IVNext)
      continue;
    bool Found = false;
    for (PHINode &HP : S.Header->phis())
      Found |= HP.getIncomingValueForBlock(S.Latch) == V;
    if (!Found)
      SPORK_REJECT("probe exit PHI uses a non-PHI loop value");
  }
  Reason = nullptr;
  return S;
#undef SPORK_REJECT
}

static void rewriteStepLoop(StepLoop &S, const LoopProtocol &P, ProbeInfo &PI,
                            DominatorTree &DT) {
  LLVMContext &Ctx = PI.Probe->getContext();
  bool NSW = false, NUW = false;
  if (auto *BO = dyn_cast<BinaryOperator>(P.NextInduction)) {
    NSW = BO->hasNoSignedWrap();
    NUW = BO->hasNoUnsignedWrap();
  }

  // Real exit index, replacing the snapshot limit everywhere after the exit.
  PHINode *IVExit = PHINode::Create(S.IV->getType(), 2,
                                    "spork.idx.exit", S.Exit->begin());
  IVExit->addIncoming(S.IVNext, S.Latch);
  SinkPlan Plan{S.Exit, DT, S.Compare};
  Plan.plan(S.Limit);
  replaceOrSinkPastExit(S.Limit, IVExit, Plan);

  // Latch: publish progress, then check that the next step fits.
  MDNode *LoopID = S.Branch->getMetadata(LLVMContext::MD_loop);
  IRBuilder<> LB(S.Branch);
  Value *INext = LB.CreateAdd(PI.Start, S.IVNext, "spork.i.next", NUW, NSW);
  createProgressStore(LB, INext, PI.ProgressPtrArg, P);
  Value *Ready =
      createBatchGuard(LB, INext, P, S.Step, PI.BoundPtrArg);
  Instruction *NewBr = LB.CreateCondBr(Ready, S.Header, S.Exit);
  NewBr->setMetadata(LLVMContext::MD_loop, LoopID);
  S.Branch->eraseFromParent();
  if (S.Compare->use_empty())
    S.Compare->eraseFromParent();

  // Preheader: the loop is rotated, so guard its first step too.
  Instruction *PHBr = S.Preheader->getTerminator();
  IRBuilder<> PB(PHBr);
  Value *IEntry = PB.CreateAdd(PI.Start, S.IVEntry, "spork.i.entry", NUW, NSW);
  Value *Ready0 = createBatchGuard(PB, IEntry, P, S.Step, PI.BoundPtrArg);
  PB.CreateCondBr(Ready0, S.Header, S.Exit);
  PHBr->eraseFromParent();

  for (PHINode &PN : S.Exit->phis()) {
    Value *V = PN.getIncomingValueForBlock(S.Latch);
    Value *FromPH = V;
    if (V == S.IVNext) {
      FromPH = S.IVEntry;
    } else if (definedInLoop(S.L, V)) {
      for (PHINode &HP : S.Header->phis())
        if (HP.getIncomingValueForBlock(S.Latch) == V)
          FromPH = HP.getIncomingValueForBlock(S.Preheader);
    }
    PN.addIncoming(FromPH, S.Preheader);
  }
  (void)Ctx;
}

// Rewrite the vectorized probe into the batched loop and splice it into the
// parent function in place of the original loop.  Returns false, leaving the
// parent untouched, if the probe's shape is not understood.
static bool transplantProbe(LoopProtocol &P, ProbeInfo &PI,
                            FunctionAnalysisManager &FAM, const char *&Reason) {
  if (!PI.Probe || !PI.Vectorized) {
    Reason = "probe did not vectorize";
    return false;
  }
  Function &Probe = *PI.Probe;
  auto &LI = FAM.getResult<LoopAnalysis>(Probe);
  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(Probe);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(Probe);

  SmallVector<StepLoop, 4> Steps;
  for (Loop *DL : LI.getLoopsInPreorder()) {
    std::optional<StepLoop> S = matchStepLoop(DL, P, SE, DT, Reason);
    if (!S)
      return false;
    Steps.push_back(*S);
  }
  if (Steps.empty()) {
    Reason = "probe has no loops";
    return false;
  }

  for (StepLoop &S : Steps)
    rewriteStepLoop(S, P, PI, DT);
  if (verifyFunction(Probe, &errs())) {
    Reason = "rewritten probe failed verification";
    return false;
  }

  // Post-condition: once every snapshot limit has been replaced by the real
  // exit index, no loop may still depend on the trip-count snapshot.  That
  // catches tail folding, masked steps, or any shape the rewrite missed.
  {
    SmallPtrSet<Instruction *, 32> Snapshot;
    SmallVector<Value *, 8> Work{Probe.getArg(PI.Inputs.size())};
    while (!Work.empty()) {
      Value *V = Work.pop_back_val();
      for (User *U : V->users())
        if (auto *I = dyn_cast<Instruction>(U); I && Snapshot.insert(I).second)
          Work.push_back(I);
    }
    for (StepLoop &S : Steps)
      for (BasicBlock *BB : S.L->blocks())
        for (Instruction &I : *BB)
          if (Snapshot.count(&I)) {
            Reason = "a probe loop still depends on the trip-count snapshot";
            return false;
          }
  }

  // Splice: preheader -> call block -> exit; the old loop dies.
  Function &F = *P.F;
  LLVMContext &Ctx = F.getContext();
  BasicBlock *CallBB = BasicBlock::Create(
      Ctx, P.Header->getName() + ".spork.call", &F, P.Exit);
  P.Preheader->getTerminator()->replaceSuccessorWith(P.Header, CallBB);

  IRBuilder<> CB(CallBB);
  CB.SetCurrentDebugLocation(P.LatchBranch->getDebugLoc());
  SmallVector<Value *, 16> Args(PI.Inputs.begin(), PI.Inputs.end());
  Args.push_back(createBoundLoad(CB, P.BoundPtr, P));
  Args.push_back(P.ProgressPtr);
  Args.push_back(P.BoundPtr);
  CallInst *Call = CB.CreateCall(&Probe, Args);
  Call->setCallingConv(Probe.getCallingConv());
  DenseMap<Value *, Value *> LiveOutMap;
  if (PI.LiveOuts.size() == 1)
    LiveOutMap[PI.LiveOuts.front()] = Call;
  else
    for (auto [I, V] : llvm::enumerate(PI.LiveOuts))
      LiveOutMap[V] = CB.CreateExtractValue(Call, I, "spork.liveout");
  CB.CreateBr(P.Exit);

  for (PHINode &PN : P.Exit->phis()) {
    int Idx = PN.getBasicBlockIndex(P.Latch);
    assert(Idx >= 0 && "exit PHI lost its latch input");
    Value *V = PN.getIncomingValue(Idx);
    if (auto It = LiveOutMap.find(V); It != LiveOutMap.end())
      V = It->second;
    PN.setIncomingBlock(Idx, CallBB);
    PN.setIncomingValue(Idx, V);
  }

  SmallVector<BasicBlock *, 8> Dead(P.L->blocks().begin(), P.L->blocks().end());
  for (BasicBlock *BB : Dead)
    BB->dropAllReferences();
  for (BasicBlock *BB : Dead)
    BB->eraseFromParent();

  InlineFunctionInfo IFI;
  InlineResult IR = InlineFunction(*Call, IFI);
  if (!IR.isSuccess())
    errs() << "spork-unroll: could not inline probe into " << F.getName()
           << ": " << IR.getFailureReason() << " (left as a call)\n";
  return true;
}

//===----------------------------------------------------------------------===//
// Transformation
//===----------------------------------------------------------------------===//

static MDNode *makeLoopID(LLVMContext &Ctx, MDNode *Existing,
                          ArrayRef<Metadata *> Extra) {
  SmallVector<Metadata *, 4> Ops;
  Ops.push_back(nullptr);
  if (Existing)
    for (unsigned I = 1; I < Existing->getNumOperands(); ++I)
      Ops.push_back(Existing->getOperand(I));
  Ops.append(Extra.begin(), Extra.end());
  MDNode *ID = MDNode::getDistinct(Ctx, Ops);
  ID->replaceOperandWith(0, ID);
  return ID;
}

static Metadata *loopHint(LLVMContext &Ctx, StringRef Name,
                          std::optional<unsigned> Value = std::nullopt) {
  SmallVector<Metadata *, 2> Ops{MDString::get(Ctx, Name)};
  if (Value)
    Ops.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), *Value)));
  return MDNode::get(Ctx, Ops);
}

// Rewrite the validated loop into
//
//   preheader -> batch header (volatile bound check for a full batch)
//       -> inner loop (original blocks, constant trip count K, no volatile ops)
//       -> batch latch (one volatile progress store) -> batch header
//     batch header -> cleanup preheader (bound check for one iteration)
//       -> cleanup loop (untouched copy of the original) -> exit
//
// Validation is complete, so this cannot fail.
static void formBatchedLoop(LoopProtocol &P, unsigned K,
                            const ProbeResult &Probe) {
  Function &F = *P.F;
  Loop *L = P.L;
  LLVMContext &Ctx = F.getContext();
  BasicBlock *Preheader = P.Preheader;
  BasicBlock *Header = P.Header;
  BasicBlock *Latch = P.Latch;
  BasicBlock *Exit = P.Exit;
  DebugLoc LatchLoc = P.LatchBranch->getDebugLoc();
  MDNode *OrigLoopID = P.LatchBranch->getMetadata(LLVMContext::MD_loop);

  // 1. Clone the untouched loop as the scalar cleanup loop.
  BasicBlock *CleanupPH = BasicBlock::Create(
      Ctx, Header->getName() + ".spork.cleanup.ph", &F, Exit);
  ValueToValueMapTy VMapC;
  VMapC[Preheader] = CleanupPH;
  SmallVector<BasicBlock *, 8> CleanupBlocks;
  for (BasicBlock *BB : L->blocks()) {
    BasicBlock *NewBB = CloneBasicBlock(BB, VMapC, ".spork.cleanup", &F);
    VMapC[BB] = NewBB;
    NewBB->moveBefore(Exit);
    CleanupBlocks.push_back(NewBB);
  }
  for (BasicBlock *BB : CleanupBlocks)
    for (Instruction &I : *BB) {
      RemapInstruction(&I, VMapC,
                       RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
      RemapDbgRecordRange(F.getParent(), I.getDbgRecordRange(), VMapC,
                          RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);
    }
  auto *CleanupHeader = cast<BasicBlock>(VMapC[Header]);
  auto *CleanupLatch = cast<BasicBlock>(VMapC[Latch]);
  // Give the cleanup loop its own ID; it must never be vectorized (it cannot
  // be, because of the volatile operations) and unrolling it only adds code.
  CleanupLatch->getTerminator()->setMetadata(
      LLVMContext::MD_loop,
      makeLoopID(Ctx, OrigLoopID,
                 {loopHint(Ctx, "llvm.loop.unroll.disable"),
                  loopHint(Ctx, "llvm.loop.vectorize.disable")}));

  // 2. Batch header with one outer PHI per header PHI and the batch guard.
  BasicBlock *BatchHeader =
      BasicBlock::Create(Ctx, Header->getName() + ".spork.batch", &F, Header);
  BasicBlock *BatchLatch = BasicBlock::Create(
      Ctx, Header->getName() + ".spork.batch.latch", &F, CleanupPH);

  SmallVector<PHINode *, 4> Outer;
  IRBuilder<> HB(BatchHeader);
  HB.SetCurrentDebugLocation(LatchLoc);
  for (HeaderPhi &HP : P.Phis) {
    PHINode *O = HB.CreatePHI(HP.PN->getType(), 2,
                              HP.PN->getName() + ".spork.outer");
    O->addIncoming(HP.Init, Preheader);
    Outer.push_back(O);
  }
  Value *Fits = createBatchGuard(HB, Outer[P.InductionIndex], P, K, P.BoundPtr);
  HB.CreateCondBr(Fits, Header, CleanupPH);

  // 3. Cleanup preheader: LCSSA copies of the outer PHIs and a one-iteration
  // guard (the original loop is rotated, so its header does not check).
  SmallVector<PHINode *, 4> Resume;
  IRBuilder<> CB(CleanupPH);
  CB.SetCurrentDebugLocation(LatchLoc);
  for (auto [HP, O] : llvm::zip(P.Phis, Outer)) {
    PHINode *R = CB.CreatePHI(O->getType(), 1, HP.PN->getName() + ".spork.resume");
    R->addIncoming(O, BatchHeader);
    Resume.push_back(R);
  }
  Value *Ready = createBatchGuard(CB, Resume[P.InductionIndex], P, 1, P.BoundPtr);
  CB.CreateCondBr(Ready, CleanupHeader, Exit);

  for (auto [HP, R] : llvm::zip(P.Phis, Resume)) {
    auto *PNc = cast<PHINode>(VMapC[HP.PN]);
    int Idx = PNc->getBasicBlockIndex(CleanupPH);
    assert(Idx >= 0 && "cleanup PHI lost its preheader input");
    PNc->setIncomingValue(Idx, R);
  }

  // 4. Exit PHIs now come from the cleanup latch or the cleanup guard.
  for (PHINode &PN : Exit->phis()) {
    int Idx = PN.getBasicBlockIndex(Latch);
    assert(Idx >= 0 && "exit PHI lost its latch input");
    Value *V = PN.getIncomingValue(Idx);
    Value *FromCleanup = V;
    if (auto *I = dyn_cast<Instruction>(V); I && L->contains(I))
      FromCleanup = VMapC[V];
    PN.setIncomingBlock(Idx, CleanupLatch);
    PN.setIncomingValue(Idx, FromCleanup);

    Value *FromGuard = V;
    if (auto *I = dyn_cast<Instruction>(V); I && L->contains(I)) {
      auto It = llvm::find_if(
          P.Phis, [&](const HeaderPhi &HP) { return HP.LatchVal == V; });
      assert(It != P.Phis.end() && "validated exit value is not a latch value");
      FromGuard = Resume[It - P.Phis.begin()];
    }
    PN.addIncoming(FromGuard, CleanupPH);
  }

  // 5. Enter the inner loop from the batch header.
  Preheader->getTerminator()->replaceSuccessorWith(Header, BatchHeader);
  for (auto [HP, O] : llvm::zip(P.Phis, Outer)) {
    int Idx = HP.PN->getBasicBlockIndex(Preheader);
    assert(Idx >= 0 && "header PHI lost its preheader input");
    HP.PN->setIncomingBlock(Idx, BatchHeader);
    HP.PN->setIncomingValue(Idx, O);
  }

  // 6. Constant-trip-count inner latch.
  Type *CounterTy = Type::getInt32Ty(Ctx);
  PHINode *Counter =
      PHINode::Create(CounterTy, 2, "spork.j", Header->begin());
  Counter->addIncoming(ConstantInt::get(CounterTy, 0), BatchHeader);
  IRBuilder<> LB(P.LatchBranch);
  LB.SetCurrentDebugLocation(LatchLoc);
  Value *CounterNext = LB.CreateAdd(Counter, ConstantInt::get(CounterTy, 1),
                                    "spork.j.next", /*NUW=*/true, /*NSW=*/true);
  Counter->addIncoming(CounterNext, Latch);
  Value *Continue = LB.CreateICmpULT(CounterNext, ConstantInt::get(CounterTy, K),
                                     "spork.j.continue");
  Instruction *InnerBranch = LB.CreateCondBr(Continue, Header, BatchLatch);

  SmallVector<Metadata *, 4> InnerHints;
  if (SporkInnerHints && Probe.VF > 1 && K % Probe.VF == 0) {
    InnerHints.push_back(loopHint(Ctx, "llvm.loop.vectorize.enable"));
    InnerHints.push_back(loopHint(Ctx, "llvm.loop.vectorize.width", Probe.VF));
    InnerHints.push_back(
        loopHint(Ctx, "llvm.loop.interleave.count", K / Probe.VF));
  }
  InnerBranch->setMetadata(LLVMContext::MD_loop,
                           makeLoopID(Ctx, OrigLoopID, InnerHints));

  P.LatchBranch->eraseFromParent();
  for (Instruction *I : P.ExitConditionInsts)
    I->eraseFromParent();

  // 7. Batch latch: LCSSA PHIs, one volatile progress store, back edge.
  IRBuilder<> BLB(BatchLatch);
  BLB.SetCurrentDebugLocation(LatchLoc);
  SmallVector<Value *, 4> LcssaLatch;
  for (HeaderPhi &HP : P.Phis) {
    Value *V = HP.LatchVal;
    if (auto *I = dyn_cast<Instruction>(V); I && L->contains(I)) {
      PHINode *Lc = BLB.CreatePHI(V->getType(), 1, V->getName() + ".spork.lcssa");
      Lc->addIncoming(V, Latch);
      V = Lc;
    }
    LcssaLatch.push_back(V);
  }
  for (auto [O, V] : llvm::zip(Outer, LcssaLatch))
    O->addIncoming(V, BatchLatch);

  Value *OldStored = P.ProgressStore->getValueOperand();
  Value *NewStored = P.ProgressCasts.replay(
      BLB, LcssaLatch[P.InductionIndex], "spork.progress");
  P.ProgressStore->moveBefore(*BatchLatch, BatchLatch->end());
  P.ProgressStore->setOperand(0, NewStored);
  if (OldStored != P.NextInduction)
    RecursivelyDeleteTriviallyDeadInstructions(OldStored);

  Instruction *OuterBranch = BLB.CreateBr(BatchHeader);
  OuterBranch->setMetadata(
      LLVMContext::MD_loop,
      makeLoopID(Ctx, nullptr,
                 {loopHint(Ctx, "llvm.loop.unroll.disable"),
                  loopHint(Ctx, "llvm.loop.vectorize.disable")}));
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

struct SporkUnrollPass : OptionalPassInfoMixin<SporkUnrollPass> {
  unsigned OptLevel;
  explicit SporkUnrollPass(unsigned OptLevel = 3) : OptLevel(OptLevel) {}

  struct Plan {
    LoopProtocol Proto;
    ProbeInfo Probe;
  };

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    FunctionAnalysisManager &FAM =
        MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

    // Group markers by site token.  Every marker sharing a token must batch
    // by the same factor, because the handler query cannot tell them apart.
    MapVector<GlobalValue *, SmallVector<CallInst *, 2>> Sites;
    SmallVector<CallInst *, 4> UntokenedMarkers;
    for (Function &F : M)
      for (BasicBlock &BB : F)
        for (Instruction &I : BB)
          if (auto *Call = dyn_cast<CallInst>(&I); isValidSporkLoopMarker(Call)) {
            if (GlobalValue *Token = getSiteToken(Call))
              Sites[Token].push_back(Call);
            else
              UntokenedMarkers.push_back(Call);
          }

    DenseMap<GlobalValue *, unsigned> SiteFactors;
    bool Changed = false;

    for (auto &[Token, Markers] : Sites) {
      unsigned Factor = 1;
      SmallVector<Plan, 2> Plans;
      unsigned Batch = 0;
      bool Ok = true;
      for (CallInst *Marker : Markers) {
        Function &F = *Marker->getFunction();
        if (F.hasOptNone()) {
          Ok = false;
          break;
        }
        auto &LI = FAM.getResult<LoopAnalysis>(F);
        auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
        auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
        SmallVector<Loop *, 2> Loops;
        if (!findMarkedLoops(Marker, LI, Loops)) {
          if (SporkVerbose) {
            errs() << "spork-unroll: " << Token->getName() << " in "
                   << F.getName()
                   << ": not batched: no loop follows the marker\n";
            if (SporkDumpProbe) {
              Marker->getParent()->print(errs());
              for (BasicBlock *Succ : successors(Marker->getParent()))
                Succ->print(errs());
            }
          }
          Ok = false;
          break;
        }
        Value *MarkerProgressPtr = nullptr;
        for (Loop *L : Loops) {
          const char *Reason = nullptr;
          std::optional<LoopProtocol> Proto =
              matchLoopProtocol(F, L, Marker, SE, DT, Reason);
          if (Proto && MarkerProgressPtr &&
              Proto->ProgressPtr != MarkerProgressPtr) {
            Proto.reset();
            Reason = "loop copies under one marker use different progress slots";
          }
          if (!Proto) {
            if (SporkVerbose) {
              errs() << "spork-unroll: " << Token->getName() << " in "
                     << F.getName() << ": not batched: " << Reason << "\n";
              if (SporkDumpProbe)
                for (BasicBlock *BB : L->blocks())
                  BB->print(errs());
            }
            Ok = false;
            break;
          }
          MarkerProgressPtr = Proto->ProgressPtr;
          Plan Pl;
          Pl.Proto = std::move(*Proto);
          if (SporkForceBatch)
            Pl.Probe.Result.Batch = SporkForceBatch;
          else
            Pl.Probe = buildProbe(M, Pl.Proto, FAM, OptLevel);
          unsigned B = Pl.Probe.Result.Batch;
          if (SporkVerbose)
            errs() << "spork-unroll: " << Token->getName() << " in "
                   << F.getName() << ": probe batch " << B << " (vf "
                   << Pl.Probe.Result.VF << ")\n";
          if (B < 2 || (Batch && B != Batch)) {
            eraseProbe(Pl.Probe, FAM);
            Ok = false;
            break;
          }
          Batch = B;
          Plans.push_back(std::move(Pl));
        }
        if (!Ok)
          break;
      }

      if (Ok) {
        SmallPtrSet<Function *, 2> Touched;
        for (Plan &Pl : Plans) {
          const char *Reason = nullptr;
          bool Transplanted =
              !SporkForceBatch && transplantProbe(Pl.Proto, Pl.Probe, FAM, Reason);
          if (!Transplanted)
            formBatchedLoop(Pl.Proto, Batch, Pl.Probe.Result);
          if (SporkVerbose)
            errs() << "spork-unroll: " << Token->getName() << ": "
                   << (Transplanted ? "transplanted LLVM's vectorized loop"
                                    : "outer/inner batch loop")
                   << " with batch " << Batch
                   << (Reason ? StringRef(" (") : StringRef(""))
                   << (Reason ? Reason : "") << (Reason ? ")" : "") << "\n";
          Touched.insert(Pl.Proto.F);
        }
        for (Function *F : Touched)
          FAM.invalidate(*F, PreservedAnalyses::none());
        Factor = Batch;
        Changed = true;
      }
      for (Plan &Pl : Plans)
        if (Pl.Probe.Probe && Pl.Probe.Probe->use_empty())
          eraseProbe(Pl.Probe, FAM);

      SiteFactors[Token] = Factor;
      for (CallInst *Marker : Markers)
        Marker->eraseFromParent();
      Changed = true;
    }
    for (CallInst *Marker : UntokenedMarkers) {
      Marker->eraseFromParent();
      Changed = true;
    }

    // Replace every factor query, including unmatched ones, so neither
    // compile-time-only symbol survives to the linker.
    SmallVector<CallInst *, 8> FactorCalls;
    for (Function &F : M)
      for (BasicBlock &BB : F)
        for (Instruction &I : BB)
          if (auto *Call = dyn_cast<CallInst>(&I); isValidSporkFactorCall(Call))
            FactorCalls.push_back(Call);
    for (CallInst *Call : FactorCalls) {
      unsigned Factor = 1;
      if (GlobalValue *Token = getSiteToken(Call))
        Factor = SiteFactors.lookup(Token) ? SiteFactors.lookup(Token) : 1;
      Call->replaceAllUsesWith(
          ConstantInt::get(cast<IntegerType>(Call->getType()), Factor));
      Call->eraseFromParent();
      Changed = true;
    }

    // Drop the now-unused compile-time-only declarations.
    for (StringRef Name : {SporkUnrollLoopName, SporkGetUnrollFactorName})
      if (Function *Decl = M.getFunction(Name);
          Decl && Decl->isDeclaration() && Decl->use_empty()) {
        Decl->eraseFromParent();
        Changed = true;
      }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

static void addSporkPipeline(ModulePassManager &MPM, unsigned OptLevel) {
  FunctionPassManager Pre;
  Pre.addPass(LoopSimplifyPass());
  Pre.addPass(LCSSAPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(Pre)));
  MPM.addPass(SporkUnrollPass(OptLevel));
  if (SporkVerify)
    MPM.addPass(VerifierPass());
  FunctionPassManager Post;
  Post.addPass(LoopSimplifyPass());
  Post.addPass(LCSSAPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(std::move(Post)));
}

} // namespace

llvm::PassPluginLibraryInfo getSporkUnrollPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "SporkUnroll", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        // Runs after the function simplification pipeline (inlining, SROA,
        // LICM, IndVars, ...) and before the vector/unroll pipeline, so the
        // marked loop looks the way the vectorizer will see it.
        PB.registerOptimizerEarlyEPCallback(
            [](ModulePassManager &MPM, OptimizationLevel Level,
               ThinOrFullLTOPhase) {
              addSporkPipeline(MPM, static_cast<unsigned>(Level));
            });
        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &MPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name != "spork-unroll")
                return false;
              addSporkPipeline(MPM, 3);
              return true;
            });
      }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getSporkUnrollPluginInfo();
}
