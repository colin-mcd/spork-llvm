#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <vector>

#define DEBUG_TYPE "spork-unroll"

using namespace llvm;

namespace {

// Determines if all indices of GEP are zero (i.e. no actual displacement).
static bool GEPAllZero(GetElementPtrInst *GEP) {
  for (Use &Idx : GEP->indices()) {
    ConstantInt *CI = dyn_cast<ConstantInt>(Idx);
    if (CI == nullptr || !CI->isZero())
      return false;
  }
  return true;
}

// Strip pointer casts / GEPs with zero offset to find the underlying instruction.
// Returns nullptr if we cannot prove the value originates from an I.
template <typename I>
static I *resolveTo(Value *V) {
  if (!V)
    return nullptr;
  if (I *i = dyn_cast<I>(V)) {
    return i;
  } else if (BitCastInst *BC = dyn_cast<BitCastInst>(V)) {
    return resolveTo<I>(BC->getOperand(0));
  } else if (AddrSpaceCastInst *AC = dyn_cast<AddrSpaceCastInst>(V)) {
    return resolveTo<I>(AC->getOperand(0));
  } else if (CastInst *CI = dyn_cast<CastInst>(V)) {
    return resolveTo<I>(CI->getOperand(0));
  } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V)) {
    if (GEPAllZero(GEP))
      return resolveTo<I>(GEP->getPointerOperand());
  } else if (LoadInst *LI = dyn_cast<LoadInst>(V)) {
    return resolveTo<I>(LI->getPointerOperand());
  }
  return nullptr;
}

static bool callIsSporkUnrollFactor(CallInst *Call) {
  if (!Call)
    return false;
  Function *Callee = Call->getCalledFunction();
  if (!Callee) {
    Callee = dyn_cast<Function>(Call->getCalledOperand()->stripPointerCasts());
  }
  return Callee && Callee->getName().contains("__spork_unroll_factor");
}

static bool isLoopStrictlyDominatedBy(Loop *L, CallInst *Call,
                                      DominatorTree &DT) {
  if (!Call || !L)
    return false;
  if (!DT.isReachableFromEntry(Call->getParent()))
    return false;
  if (L->contains(Call))
    return false;
  return DT.dominates(Call, L->getHeader());
}

static unsigned determineStandardUnrollCount(Loop *L,
                                             const TargetTransformInfo &TTI,
                                             ScalarEvolution &SE) {
  if (!L)
    return 1;

  MDNode *LoopID = L->getLoopID();
  if (LoopID) {
    for (unsigned i = 1, e = LoopID->getNumOperands(); i < e; ++i) {
      if (MDNode *MD = dyn_cast<MDNode>(LoopID->getOperand(i))) {
        if (MD->getNumOperands() > 0) {
          if (MDString *S = dyn_cast<MDString>(MD->getOperand(0))) {
            if (S->getString() == "llvm.loop.unroll.disable")
              return 1;
            if (S->getString() == "llvm.loop.unroll.count") {
              if (MD->getNumOperands() >= 2) {
                if (auto *CI =
                        mdconst::dyn_extract<ConstantInt>(MD->getOperand(1))) {
                  return CI->getZExtValue();
                }
              }
            }
          }
        }
      }
    }
  }

  OptimizationRemarkEmitter ORE(L->getHeader()->getParent());
  TargetTransformInfo::UnrollingPreferences UP = llvm::gatherUnrollingPreferences(
      L, SE, TTI, /*BFI=*/nullptr, /*PSI=*/nullptr, ORE, /*OptLevel=*/3,
      /*UserThreshold=*/std::nullopt, /*UserCount=*/std::nullopt,
      /*UserAllowPartial=*/true, /*UserRuntime=*/true,
      /*UserUpperBound=*/std::nullopt, /*UserFullUnrollMaxCount=*/std::nullopt);

  unsigned MaxThreshold = UP.PartialThreshold;
  if (MaxThreshold == 0)
    MaxThreshold = 150;

  unsigned DefaultCount = UP.DefaultUnrollRuntimeCount;
  if (DefaultCount == 0)
    DefaultCount = 8;
  if (UP.MaxCount > 0 && DefaultCount > UP.MaxCount)
    DefaultCount = UP.MaxCount;

  // Estimate loop size using instruction costs from TTI
  unsigned LoopSize = 0;
  for (BasicBlock *BB : L->getBlocks()) {
    for (Instruction &I : *BB) {
      if (!isa<DbgInfoIntrinsic>(&I) && !I.isTerminator()) {
        auto Cost =
            TTI.getInstructionCost(&I, TargetTransformInfo::TCK_CodeSize);
        if (Cost.isValid())
          LoopSize += Cost.getValue();
        else
          LoopSize += 1;
      }
    }
  }

  if (LoopSize == 0)
    LoopSize = 1;

  unsigned Count = DefaultCount;
  while (Count > 1 && (LoopSize * Count > MaxThreshold)) {
    Count >>= 1;
  }

  return Count;
}

static void enableLoopUnrolling(Loop *L, unsigned Count) {
  if (!L || Count <= 1)
    return;
  BasicBlock *Header = L->getHeader();
  if (!Header)
    return;
  LLVMContext &Ctx = Header->getContext();
  MDNode *LoopID = L->getLoopID();
  SmallVector<Metadata *, 4> MDs;
  MDs.push_back(nullptr); // placeholder for self-reference

  if (LoopID) {
    for (unsigned i = 1, e = LoopID->getNumOperands(); i < e; ++i) {
      if (MDNode *MD = dyn_cast<MDNode>(LoopID->getOperand(i))) {
        if (MD->getNumOperands() > 0) {
          if (MDString *S = dyn_cast<MDString>(MD->getOperand(0))) {
            if (S->getString() == "llvm.loop.unroll.disable" ||
                S->getString() == "llvm.loop.unroll.runtime.disable" ||
                S->getString() == "llvm.loop.unroll.count")
              continue;
          }
        }
        MDs.push_back(MD);
      }
    }
  }

  Metadata *CountArgs[] = {
      MDString::get(Ctx, "llvm.loop.unroll.count"),
      ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(Ctx), Count))};
  MDs.push_back(MDNode::get(Ctx, CountArgs));

  Metadata *EnableArgs[] = {MDString::get(Ctx, "llvm.loop.unroll.enable")};
  MDs.push_back(MDNode::get(Ctx, EnableArgs));

  MDNode *NewLoopID = MDNode::get(Ctx, MDs);
  NewLoopID->replaceOperandWith(0, NewLoopID);
  L->setLoopID(NewLoopID);
}

static uint64_t getLoopStep(Loop *L, ScalarEvolution &SE) {
  if (!L)
    return 0;
  BasicBlock *Header = L->getHeader();
  if (!Header)
    return 0;

  if (PHINode *IndVar = L->getInductionVariable(SE)) {
    if (const auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(IndVar))) {
      if (const auto *Step =
              dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE))) {
        int64_t StepVal = Step->getValue()->getSExtValue();
        return std::abs(StepVal);
      }
    }
  }

  for (PHINode &PN : Header->phis()) {
    if (const auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(&PN))) {
      if (AR->getLoop() == L) {
        if (const auto *Step =
                dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE))) {
          int64_t StepVal = Step->getValue()->getSExtValue();
          return std::abs(StepVal);
        }
      }
    }
  }

  return 0;
}

static void collectLoads(Value *V, SmallVectorImpl<LoadInst *> &Loads,
                         SmallPtrSetImpl<Value *> &Visited) {
  if (!V || !Visited.insert(V).second)
    return;

  for (User *U : V->users()) {
    if (auto *LI = dyn_cast<LoadInst>(U)) {
      Loads.push_back(LI);
    } else if (auto *BC = dyn_cast<BitCastInst>(U)) {
      collectLoads(BC, Loads, Visited);
    } else if (auto *AC = dyn_cast<AddrSpaceCastInst>(U)) {
      collectLoads(AC, Loads, Visited);
    } else if (auto *GEP = dyn_cast<GetElementPtrInst>(U)) {
      if (GEPAllZero(GEP))
        collectLoads(GEP, Loads, Visited);
    } else if (auto *SI = dyn_cast<StoreInst>(U)) {
      // If the variable pointer V is stored into another slot (e.g. a closure struct),
      // follow loads from that slot to find further loads of V.
      if (SI->getValueOperand() == V) {
        Value *PtrSlot = SI->getPointerOperand();
        for (User *SlotUser : PtrSlot->users()) {
          if (auto *SlotLoad = dyn_cast<LoadInst>(SlotUser)) {
            collectLoads(SlotLoad, Loads, Visited);
          }
        }
      }
    }
  }
}

static bool replaceUnrollFactorUses(CallInst *Call, Value *UnrollFactorArg,
                                    uint64_t UnrollFactor, LLVMContext &Ctx) {
  bool Changed = false;
  if (!UnrollFactorArg && !Call)
    return Changed;

  Type *IntTy = nullptr;
  if (UnrollFactorArg && UnrollFactorArg->getType()->isIntegerTy())
    IntTy = UnrollFactorArg->getType();
  else if (Call && Call->getType()->isIntegerTy())
    IntTy = Call->getType();
  else
    IntTy = Type::getInt32Ty(Ctx);

  ConstantInt *ConstFactor =
      ConstantInt::get(cast<IntegerType>(IntTy), UnrollFactor);

  // 1. Resolve to AllocaInst if any
  AllocaInst *AI = nullptr;
  if (UnrollFactorArg) {
    AI = resolveTo<AllocaInst>(UnrollFactorArg);
    if (!AI)
      AI = dyn_cast<AllocaInst>(UnrollFactorArg->stripPointerCasts());
    if (!AI) {
      if (auto *LI = dyn_cast<LoadInst>(UnrollFactorArg)) {
        AI = resolveTo<AllocaInst>(LI->getPointerOperand());
        if (!AI)
          AI = dyn_cast<AllocaInst>(
              LI->getPointerOperand()->stripPointerCasts());
      }
    }
  }

  if (AI) {
    // Store the constant factor into the alloca variable at the alloca site
    // so that any closures, eager promotion (which runs before the loop!),
    // and signal handlers see the initialized unroll factor from the beginning.
    BasicBlock::iterator InsertPt = AI->getNextNode()
                                        ? AI->getNextNode()->getIterator()
                                        : AI->getParent()->end();
    auto *SI = new StoreInst(ConstFactor, AI, InsertPt);
    SI->setVolatile(true);
    Changed = true;
  } else if (UnrollFactorArg && UnrollFactorArg->getType()->isPointerTy() &&
             Call && Call->getParent()) {
    auto *SI = new StoreInst(ConstFactor, UnrollFactorArg, Call->getIterator());
    SI->setVolatile(true);
    Changed = true;
  }

  if (AI) {
    SmallVector<LoadInst *, 8> Loads;
    SmallPtrSet<Value *, 8> Visited;
    collectLoads(AI, Loads, Visited);
    for (LoadInst *LI : Loads) {
      if (!LI->getParent())
        continue;
      Type *Ty = LI->getType();
      ConstantInt *CI =
          Ty->isIntegerTy()
              ? ConstantInt::get(cast<IntegerType>(Ty), UnrollFactor)
              : ConstFactor;
      LI->replaceAllUsesWith(CI);
      LI->eraseFromParent();
      Changed = true;
    }
  }

  // 2. If UnrollFactorArg itself is an integer instruction/value
  if (UnrollFactorArg) {
    if (auto *I = dyn_cast<Instruction>(UnrollFactorArg)) {
      if (I->getParent() != nullptr && I->getType()->isIntegerTy()) {
        I->replaceAllUsesWith(ConstantInt::get(
            cast<IntegerType>(I->getType()), UnrollFactor));
        Changed = true;
        if (isa<LoadInst>(I))
          I->eraseFromParent();
      }
    } else if (UnrollFactorArg->getType()->isIntegerTy() &&
               !isa<Constant>(UnrollFactorArg)) {
      UnrollFactorArg->replaceAllUsesWith(ConstFactor);
      Changed = true;
    }
  }

  // 3. If Call returned a value and was used
  if (Call && Call->getType()->isIntegerTy() && !Call->use_empty()) {
    Call->replaceAllUsesWith(
        ConstantInt::get(cast<IntegerType>(Call->getType()), UnrollFactor));
    Changed = true;
  }

  return Changed;
}

struct TrackedLoop {
  BasicBlock *Header = nullptr;
  uint64_t OrigStep = 0;
  uint64_t OrigTripCount = 0;
  std::vector<WeakVH> CachedLatchOps;
};

struct TrackedCall {
  CallInst *Call = nullptr;
  WeakVH UnrollFactorArg;
  std::vector<TrackedLoop> Loops;
};

static std::mutex StateMutex;
static DenseMap<Function *, std::vector<TrackedCall>> ActiveSporkCalls;

// -----------------------------------------------------------------------------
// Pre-Unroll Pass
// -----------------------------------------------------------------------------
struct SporkPreUnrollPass : public PassInfoMixin<SporkPreUnrollPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    LLVMContext &Ctx = F.getContext();
    bool Changed = false;

    // Collect all calls to __spork_unroll_factor
    SmallVector<CallInst *, 4> SporkCalls;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *Call = dyn_cast<CallInst>(&I)) {
          if (callIsSporkUnrollFactor(Call)) {
            SporkCalls.push_back(Call);
          }
        }
      }
    }

    if (SporkCalls.empty())
      return PreservedAnalyses::all();

    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &TTI = FAM.getResult<TargetIRAnalysis>(F);

    // (1) Immediately before LoopUnrollPass, identify all loops that are strictly
    // dominated by a call to the external function __spork_unroll_factor(unroll_factor)
    DenseMap<CallInst *, SmallVector<Loop *, 4>> CallToLoops;
    for (Loop *L : LI.getLoopsInPreorder()) {
      CallInst *NearestCall = nullptr;
      for (CallInst *Call : SporkCalls) {
        if (isLoopStrictlyDominatedBy(L, Call, DT)) {
          if (!NearestCall || DT.dominates(NearestCall, Call)) {
            NearestCall = Call;
          }
        }
      }
      if (NearestCall) {
        CallToLoops[NearestCall].push_back(L);
      }
    }

    std::vector<TrackedCall> TrackedCalls;

    for (CallInst *Call : SporkCalls) {
      TrackedCall TC;
      TC.Call = Call;
      if (Call->arg_size() >= 1)
        TC.UnrollFactorArg = Call->getArgOperand(0);

      auto It = CallToLoops.find(Call);
      if (It == CallToLoops.end() || It->second.empty()) {
        TrackedCalls.push_back(std::move(TC));
        continue;
      }

      SmallVector<Metadata *, 4> LoopMDList;

      for (Loop *TargetLoop : It->second) {
        TrackedLoop TL;
        TL.Header = TargetLoop->getHeader();
        TL.OrigStep = getLoopStep(TargetLoop, SE);
        TL.OrigTripCount = SE.getSmallConstantTripCount(TargetLoop);

        unsigned Count = determineStandardUnrollCount(TargetLoop, TTI, SE);
        if (Count > 1) {
          enableLoopUnrolling(TargetLoop, Count);
          Changed = true;
        }

        // (2) Each such loop should cache a list of volatile loads/stores that occur
        // in the loop latch, then mark those loads/stores as not volatile.
        SmallVector<BasicBlock *, 4> Latches;
        TargetLoop->getLoopLatches(Latches);
        if (Latches.empty()) {
          if (BasicBlock *Latch = TargetLoop->getLoopLatch())
            Latches.push_back(Latch);
        }

        for (BasicBlock *LatchBB : Latches) {
          for (Instruction &I : *LatchBB) {
            if (auto *LI = dyn_cast<LoadInst>(&I)) {
              if (LI->isVolatile()) {
                TL.CachedLatchOps.push_back(LI);
                LI->setVolatile(false);
                LI->setMetadata("spork.volatile", MDNode::get(Ctx, {}));
                Changed = true;
              }
            } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
              if (SI->isVolatile()) {
                TL.CachedLatchOps.push_back(SI);
                SI->setVolatile(false);
                SI->setMetadata("spork.volatile", MDNode::get(Ctx, {}));
                Changed = true;
              }
            }
          }
        }

        Metadata *MDs[] = {
          ValueAsMetadata::get(TL.Header),
          ConstantAsMetadata::get(
              ConstantInt::get(Type::getInt64Ty(Ctx), TL.OrigStep)),
          ConstantAsMetadata::get(
              ConstantInt::get(Type::getInt64Ty(Ctx), TL.OrigTripCount))
        };
        LoopMDList.push_back(MDNode::get(Ctx, MDs));

        TC.Loops.push_back(std::move(TL));
      }

      Call->setMetadata("spork.state", MDNode::get(Ctx, LoopMDList));
      TrackedCalls.push_back(std::move(TC));
    }

    {
      std::lock_guard<std::mutex> Lock(StateMutex);
      ActiveSporkCalls[&F] = std::move(TrackedCalls);
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

// -----------------------------------------------------------------------------
// Post-Unroll Pass
// -----------------------------------------------------------------------------
struct SporkPostUnrollPass : public PassInfoMixin<SporkPostUnrollPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    LLVMContext &Ctx = F.getContext();
    bool Changed = false;

    std::vector<TrackedCall> TrackedCalls;
    {
      std::lock_guard<std::mutex> Lock(StateMutex);
      auto It = ActiveSporkCalls.find(&F);
      if (It != ActiveSporkCalls.end()) {
        TrackedCalls = std::move(It->second);
        ActiveSporkCalls.erase(It);
      }
    }

    // Fallback: If not found in ActiveSporkCalls (e.g. separate opt invocations),
    // reconstruct from Call metadata.
    if (TrackedCalls.empty()) {
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (auto *Call = dyn_cast<CallInst>(&I)) {
            if (!callIsSporkUnrollFactor(Call))
              continue;
            TrackedCall TC;
            TC.Call = Call;
            if (Call->arg_size() >= 1)
              TC.UnrollFactorArg = Call->getArgOperand(0);

            if (Call->hasMetadata("spork.state")) {
              MDNode *MD = Call->getMetadata("spork.state");
              SmallVector<MDNode *, 4> LoopMDs;
              if (MD->getNumOperands() == 3 &&
                  isa<ValueAsMetadata>(MD->getOperand(0))) {
                LoopMDs.push_back(MD);
              } else {
                for (const auto &Op : MD->operands()) {
                  if (auto *LoopMD = dyn_cast<MDNode>(Op.get())) {
                    LoopMDs.push_back(LoopMD);
                  }
                }
              }
              for (MDNode *LoopMD : LoopMDs) {
                if (LoopMD->getNumOperands() >= 3) {
                  if (auto *HeaderMD =
                          dyn_cast<ValueAsMetadata>(LoopMD->getOperand(0))) {
                    if (auto *Header =
                            dyn_cast<BasicBlock>(HeaderMD->getValue())) {
                      TrackedLoop TL;
                      TL.Header = Header;
                      TL.OrigStep =
                          cast<ConstantInt>(
                              cast<ConstantAsMetadata>(LoopMD->getOperand(1))
                                  ->getValue())
                              ->getZExtValue();
                      TL.OrigTripCount =
                          cast<ConstantInt>(
                              cast<ConstantAsMetadata>(LoopMD->getOperand(2))
                                  ->getValue())
                              ->getZExtValue();
                      TC.Loops.push_back(std::move(TL));
                    }
                  }
                }
              }
            }
            TrackedCalls.push_back(std::move(TC));
          }
        }
      }
    }

    if (TrackedCalls.empty())
      return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();

    SmallVector<CallInst *, 4> CallsToRemove;

    for (TrackedCall &TC : TrackedCalls) {
      CallInst *Call = TC.Call;
      Value *UnrollFactorArg = TC.UnrollFactorArg;

      uint64_t MaxUnrollFactor = 1;
      bool AnyLoopUnrolled = false;

      // (3) After the loop unrolling pass, if such a loop was unrolled,
      // it should determine the unrolling factor of the loop and then replace
      // all uses of the unroll_factor variable with a constantint value
      // that is the actual unrolling factor of the loop.
      for (TrackedLoop &TL : TC.Loops) {
        uint64_t LoopFactor = 1;
        Loop *CurrentLoop = LI.getLoopFor(TL.Header);
        if (CurrentLoop && CurrentLoop->getHeader() == TL.Header) {
          // Loop still exists (partial unroll or not unrolled)
          uint64_t NewStep = getLoopStep(CurrentLoop, SE);
          if (TL.OrigStep > 0 && NewStep > TL.OrigStep) {
            LoopFactor = NewStep / TL.OrigStep;
          }

          // Coalesce latch updates: find cloned stores to latch pointer
          for (WeakVH &VH : TL.CachedLatchOps) {
            if (Value *V = VH) {
              if (auto *OrigSI = dyn_cast<StoreInst>(V)) {
                Value *Ptr = OrigSI->getPointerOperand();
                SmallVector<StoreInst *, 8> StoresToPtr;
                for (BasicBlock *BB : CurrentLoop->getBlocks()) {
                  for (Instruction &I : *BB) {
                    if (auto *SI = dyn_cast<StoreInst>(&I)) {
                      if (SI->getPointerOperand() == Ptr &&
                          (SI == OrigSI || SI->hasMetadata("spork.volatile"))) {
                        StoresToPtr.push_back(SI);
                      }
                    }
                  }
                }

                if (StoresToPtr.size() > 1) {
                  LoopFactor =
                      std::max(LoopFactor, (uint64_t)StoresToPtr.size());

                  // Sort stores by execution order using DominatorTree
                  llvm::sort(StoresToPtr, [&](StoreInst *A, StoreInst *B) {
                    if (A->getParent() == B->getParent())
                      return A->comesBefore(B);
                    if (DT.dominates(A->getParent(), B->getParent()))
                      return true;
                    if (DT.dominates(B->getParent(), A->getParent()))
                      return false;
                    return A->getParent() < B->getParent();
                  });

                  // Only coalesce if the stores are executed sequentially
                  bool CanCoalesce = true;
                  for (size_t i = 0; i + 1 < StoresToPtr.size(); ++i) {
                    BasicBlock *BBA = StoresToPtr[i]->getParent();
                    BasicBlock *BBB = StoresToPtr[i + 1]->getParent();
                    if (BBA != BBB && !DT.dominates(BBA, BBB)) {
                      CanCoalesce = false;
                      break;
                    }
                  }

                  if (CanCoalesce) {
                    // Coalesce: keep the final store, mark it volatile, and erase intermediate stores
                    StoreInst *FinalStore = StoresToPtr.back();
                    FinalStore->setVolatile(true);
                    FinalStore->setMetadata("spork.volatile", nullptr);

                    for (size_t i = 0; i + 1 < StoresToPtr.size(); ++i) {
                      StoresToPtr[i]->eraseFromParent();
                    }
                    Changed = true;
                  }
                }
              }
            }
          }
        } else {
          // Loop is gone! Fully unrolled.
          LoopFactor = TL.OrigTripCount;
        }

        if (LoopFactor > 1) {
          AnyLoopUnrolled = true;
          MaxUnrollFactor = std::max(MaxUnrollFactor, LoopFactor);
        }
      }

      uint64_t FinalFactor =
          (AnyLoopUnrolled && MaxUnrollFactor > 1) ? MaxUnrollFactor : 1;
      Changed |=
          replaceUnrollFactorUses(Call, UnrollFactorArg, FinalFactor, Ctx);

      // (4) Next, it should go back through each loop associated with a
      // __spork_unroll_factor(unroll_factor) call and for each load/store that was
      // turned from volatile into not volatile, if it has a parent still, mark it again as volatile.
      for (TrackedLoop &TL : TC.Loops) {
        for (WeakVH &VH : TL.CachedLatchOps) {
          if (Value *V = VH) {
            if (auto *I = dyn_cast<Instruction>(V)) {
              if (I->getParent() != nullptr) {
                if (auto *LI = dyn_cast<LoadInst>(I)) {
                  LI->setVolatile(true);
                  Changed = true;
                } else if (auto *SI = dyn_cast<StoreInst>(I)) {
                  SI->setVolatile(true);
                  Changed = true;
                }
                I->setMetadata("spork.volatile", nullptr);
              }
            }
          }
        }
      }

      if (Call)
        CallsToRemove.push_back(Call);
    }

    // Fallback: restore any remaining instructions with spork.volatile metadata
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.hasMetadata("spork.volatile")) {
          if (auto *LI = dyn_cast<LoadInst>(&I)) {
            LI->setVolatile(true);
            Changed = true;
          } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
            SI->setVolatile(true);
            Changed = true;
          }
          I.setMetadata("spork.volatile", nullptr);
        }
      }
    }

    for (CallInst *Call : CallsToRemove) {
      if (Call->getParent()) {
        if (!Call->use_empty() && Call->getType()->isIntegerTy()) {
          Call->replaceAllUsesWith(
              ConstantInt::get(cast<IntegerType>(Call->getType()), 1));
        }
        Call->eraseFromParent();
        Changed = true;
      }
    }

    // Ensure any remaining calls in this function are removed
    for (auto &BB : F) {
      for (auto &I : make_early_inc_range(BB)) {
        if (auto *Call = dyn_cast<CallInst>(&I)) {
          if (callIsSporkUnrollFactor(Call)) {
            if (!Call->use_empty() && Call->getType()->isIntegerTy()) {
              Call->replaceAllUsesWith(
                  ConstantInt::get(cast<IntegerType>(Call->getType()), 1));
            }
            Call->eraseFromParent();
            Changed = true;
          }
        }
      }
    }

    // (5) If __spork_unroll_factor has no remaining uses in the module,
    // remove its declaration from the program.
    Module *M = F.getParent();
    if (M) {
      SmallVector<Function *, 4> DeclsToRemove;
      for (Function &Func : *M) {
        if (Func.isDeclaration() &&
            Func.getName().contains("__spork_unroll_factor") &&
            Func.use_empty()) {
          DeclsToRemove.push_back(&Func);
        }
      }
      for (Function *Func : DeclsToRemove) {
        Func->eraseFromParent();
        Changed = true;
      }
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

} // end anonymous namespace

// -----------------------------------------------------------------------------
// Plugin Registration
// -----------------------------------------------------------------------------
llvm::PassPluginLibraryInfo getSporkUnrollPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "SporkUnroll", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        PB.registerScalarOptimizerLateEPCallback(
            [](FunctionPassManager &FPM, OptimizationLevel Level) {
              FPM.addPass(SporkPreUnrollPass());

              LoopUnrollOptions opts = LoopUnrollOptions(
                  Level.getSpeedupLevel(), /*OnlyWhenForced=*/false,
                  /*ForgetSCEV=*/false);
              opts.setPartial(true);
              opts.setRuntime(true);
              FPM.addPass(LoopUnrollPass(opts));

              FPM.addPass(SporkPostUnrollPass());
            });

        PB.registerPipelineParsingCallback(
            [](StringRef Name, FunctionPassManager &FPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name == "spork-pre-unroll") {
                FPM.addPass(SporkPreUnrollPass());
                return true;
              }
              if (Name == "spork-post-unroll") {
                FPM.addPass(SporkPostUnrollPass());
                return true;
              }
              if (Name == "spork-unroll") {
                FPM.addPass(SporkPreUnrollPass());
                LoopUnrollOptions opts = LoopUnrollOptions(
                    /*OptLevel=*/3, /*OnlyWhenForced=*/false,
                    /*ForgetSCEV=*/false);
                opts.setPartial(true);
                opts.setRuntime(true);
                FPM.addPass(LoopUnrollPass(opts));
                FPM.addPass(SporkPostUnrollPass());
                return true;
              }
              return false;
            });
      }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getSporkUnrollPluginInfo();
}
