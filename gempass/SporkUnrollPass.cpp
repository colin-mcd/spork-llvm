#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/IR/Dominators.h"

using namespace llvm;

namespace {

// Determines if all indices of GEP are zero (i.e. no actual displacement).
static bool GEPAllZero(GetElementPtrInst *GEP) {
  for (Use &Idx : GEP->indices()) {
    ConstantInt *CI = dyn_cast<ConstantInt>(Idx);
    if (CI == nullptr || !CI->isZero()) return false;
  }
  return true;
}

// Strip pointer casts / GEPs with zero offset to find the underlying instruction.
// Returns nullptr if we cannot prove the value originates from an I.
template <typename I>
static I *resolveTo(Value *V) {
  if (BitCastInst *BC = dyn_cast<BitCastInst>(V)) {
    return resolveTo<I>(BC->getOperand(0));
  } else if (AddrSpaceCastInst *AC = dyn_cast<AddrSpaceCastInst>(V)) {
    return resolveTo<I>(AC->getOperand(0));
  } else if (CastInst *CI = dyn_cast<CastInst>(V)) {
    return resolveTo<I>(CI->getOperand(0));
  } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V)) {
    // Only follow if all indices are zero (i.e. no actual displacement).
    if (GEPAllZero(GEP)) return resolveTo<I>(GEP->getPointerOperand());
  } else if (LoadInst *LI = dyn_cast<LoadInst>(V)) {
    return resolveTo<I>(LI->getPointerOperand());
  } else if (I *i = dyn_cast<I>(V)) {
    return i;
  }
  // anything else: give up
  return nullptr;
}

// -----------------------------------------------------------------------------
// Helper: Find the underlying variable and strip/mark volatile memory access
// -----------------------------------------------------------------------------
bool stripAndMarkNonVolatile(Value *Arg, LLVMContext &Ctx) {
  // Resolve argument to its variable definition (AllocaInst or GlobalVariable)
  // Value *BaseVar = Arg->stripPointerCasts();
  Value *BaseVar = resolveTo<Value>(Arg);
  if (!BaseVar) return false;
  
  bool Changed = false;
  // Find all loads and stores to this variable
  for (User *U : BaseVar->users()) {
    if (auto *LI = dyn_cast<LoadInst>(U)) {
      if (LI->isVolatile()) {
        LI->setVolatile(false);
        outs() << "Marking " << *LI << " as nonvolatile\n";
        // Tag it so the post-pass knows to restore it
        LI->setMetadata("spork.volatile", MDNode::get(Ctx, {}));
        Changed = true;
      }
    } else if (auto *SI = dyn_cast<StoreInst>(U)) {
      if (SI->isVolatile()) {
        SI->setVolatile(false);
        outs() << "Marking " << *SI << " as nonvolatile\n";
        SI->setMetadata("spork.volatile", MDNode::get(Ctx, {}));
        Changed = true;
      }
    }
  }
  return Changed;
}

bool callIsSporkUnrollFactor(CallInst *Call) {
  return Call && Call->getCalledFunction() &&
    Call->getCalledFunction()->getName().contains("__spork_unroll_factor");
}

// -----------------------------------------------------------------------------
// Pre-Unroll Pass
// -----------------------------------------------------------------------------
struct SporkPreUnrollPass : public PassInfoMixin<SporkPreUnrollPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    LLVMContext &Ctx = F.getContext();
    bool Changed = false;

    // for (auto &BB : F) {
    //   for (auto &I : BB) {
    //     auto *Call = dyn_cast<CallInst>(&I);
    //     if (!callIsSporkUnrollFactor(Call)) continue;
        
    //     Value *SigSafeI = Call->getArgOperand(0);
    //     Value *LoopEnd = Call->getArgOperand(1);
        
    //     // 1. Resolve variables and strip volatile
    //     //Changed |= stripAndMarkNonVolatile(SigSafeI, Ctx);
    //     Changed |= stripAndMarkNonVolatile(LoopEnd, Ctx);
    //   }
    // }

    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F); // Use proper dominance
  
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *Call = dyn_cast<CallInst>(&I);
        if (!callIsSporkUnrollFactor(Call)) continue;
        
        Value *LoopBeg = resolveTo<Value>(Call->getArgOperand(0));
        Value *LoopEnd = resolveTo<Value>(Call->getArgOperand(1));
        
        
        // 2. Find the dominated loop using loop_end as a guard
        Loop *TargetLoop = nullptr;
        for (Loop *L : LI.getLoopsInPreorder()) {
          // Check if the call dominates the loop header
          if (!DT.dominates(Call, L->getHeader())) continue;

          PHINode *IndVar = L->getInductionVariable(SE);
          if (!IndVar) continue;
          bool indIsLoopBeg = false;
          for (Value *val : IndVar->operand_values()) {
            if (resolveTo<Value>(val) == LoopBeg) {
              indIsLoopBeg = true;
              break;
            }
          }
          if (!indIsLoopBeg) continue;

          // Look at the loop's exit conditions. Does it use LoopEnd?
          SmallVector<BasicBlock*, 4> ExitingBlocks;
          L->getExitingBlocks(ExitingBlocks);
          for (BasicBlock *EB : ExitingBlocks) {
            auto *BI = dyn_cast<BranchInst>(EB->getTerminator());
            if (BI && BI->isConditional()) {
              auto *Cmp = dyn_cast<CmpInst>(BI->getCondition());
              if (Cmp) {
                Value *Op0 = resolveTo<Value>(Cmp->getOperand(0));
                Value *Op1 = resolveTo<Value>(Cmp->getOperand(1));
                if (LoopEnd && ((Op0 == LoopEnd) || (Op1 == LoopEnd))
                    && L->getInductionVariable(SE)) {
                  TargetLoop = L;
                  break;
                }
              }
            }
          }
          if (TargetLoop) break;
        }
        if (!TargetLoop) continue;
      
        // 3. Extract original step size using SCEV
        // auto *IndVar = TargetLoop->getCanonicalInductionVariable();
        auto *IndVar = TargetLoop->getInductionVariable(SE);
        if (!IndVar) continue; // Requires a canonical induction variable
      
        const SCEV *IndVarSCEV = SE.getSCEV(IndVar);
        const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(IndVarSCEV);
        if (!AR) continue;
      
        const SCEVConstant *StepSCEV = dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE));
        if (!StepSCEV) continue; // Step must be a compile-time constant
        
        uint64_t OrigStep = StepSCEV->getValue()->getZExtValue();
        uint64_t OrigTripCount = SE.getSmallConstantTripCount(TargetLoop);
      
        // 4. Attach state to the CallInst via Metadata for the Post-Pass
        // Metadata array: [LoopHeader, OrigStep, OrigTripCount]
        Metadata *MDs[] = {
          ValueAsMetadata::get(TargetLoop->getHeader()),
          ConstantAsMetadata::get(ConstantInt::get(Type::getInt64Ty(Ctx), OrigStep)),
          ConstantAsMetadata::get(ConstantInt::get(Type::getInt64Ty(Ctx), OrigTripCount))
        };
        Call->setMetadata("spork.state", MDNode::get(Ctx, MDs));
        Changed = true;
      }
    }
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  // PreservedAnalyses run(LoopNest& nest, AnalysisManager<Loop, LoopStandardAnalysisResults&>& AM, LoopStandardAnalysisResults& LSAR, LPMUpdater updater) {
  //   //Loop* loop = nest.getInnermostLoop();
  //   Function *F = nest.getParent();
  //   FunctionAnalysisManager FAM = FunctionAnalysisManager();
  //   return run(*F, FAM);
  // }
};

// -----------------------------------------------------------------------------
// Post-Unroll Pass
// -----------------------------------------------------------------------------
struct SporkPostUnrollPass : public PassInfoMixin<SporkPostUnrollPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
    bool Changed = false;
    
    SmallVector<CallInst*, 4> CallsToRemove;
    
    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *Call = dyn_cast<CallInst>(&I);
        if (!callIsSporkUnrollFactor(Call)) continue;
        CallsToRemove.push_back(Call);
        if (!Call->hasMetadata("spork.state")) continue;

        MDNode *MD = Call->getMetadata("spork.state");
        outs() << "Call = " << *Call << " in function" << F.getName() << " in module " << F.getParent()->getName() << "\n";
        
        auto *HeaderMD = cast<ValueAsMetadata>(MD->getOperand(0));
        BasicBlock *LoopHeader = cast<BasicBlock>(HeaderMD->getValue());
        
        uint64_t OrigStep = cast<ConstantInt>(
            cast<ConstantAsMetadata>(MD->getOperand(1))->getValue())->getZExtValue();
        uint64_t OrigTripCount = cast<ConstantInt>(
            cast<ConstantAsMetadata>(MD->getOperand(2))->getValue())->getZExtValue();
        
        uint64_t UnrollFactor = 1; // Default
        
        // Check if the loop survived the unrolling pass
        Loop *CurrentLoop = LI.getLoopFor(LoopHeader);
        if (CurrentLoop) {
          outs() << "CurrentLoop = " << *CurrentLoop << "\n";
          // Loop still exists (Partial Unroll)
          auto *IndVar = CurrentLoop->getInductionVariable(SE);
          if (IndVar) {
            outs() << "IndVar = " << *IndVar << "\n";
            const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(IndVar));
            if (AR) {
              outs() << "AR = " << *AR << "\n";
              if (const SCEVConstant *NewStepSCEV =
                  dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE))) {
                uint64_t NewStep = NewStepSCEV->getValue()->getZExtValue();
                outs() << "new step = " << NewStep << " vs old step = " << OrigStep << "\n";
                UnrollFactor = NewStep / OrigStep;
                for (BasicBlock* BB : CurrentLoop->blocks()) {
                  outs() << *BB << "\n";
                }
              }
            }
          }
        } else {
          outs() << "Loop is gone!\n";
          // Loop is gone! It was fully unrolled. 
          // The unroll factor is effectively the original trip count.
          UnrollFactor = OrigTripCount;
        }
        
        // Replace the call with the calculated unroll factor
        IRBuilder<> Builder(Call);
        Value *FactorVal = Builder.getIntN(Call->getType()->getIntegerBitWidth(), UnrollFactor);
        Call->replaceAllUsesWith(FactorVal);
        
        // leaving this here in case I replace extern call with attribute in the future
        Changed = true;
        
      }
    }

    for (auto *Call : CallsToRemove) {
      Call->eraseFromParent();
      Changed = true;
    }

    // for (auto &BB : F) {
    //   for (auto &I : BB) {
    //     // Restore volatile qualifiers
    //     if (I.hasMetadata("spork.volatile")) {
    //       if (auto *Load = dyn_cast<LoadInst>(&I)) {
    //         Load->setVolatile(true);
    //         outs() << "Marking " << *Load << "as volatile\n";
    //       } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
    //         Store->setVolatile(true);
    //         outs() << "Marking " << *Store << "as volatile\n";
    //       }
    //       I.setMetadata("spork.volatile", nullptr); // Remove the tag
    //       Changed = true;
    //     }
    //   }
    // }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  // PreservedAnalyses run(LoopNest& nest, AnalysisManager<Loop, LoopStandardAnalysisResults&>& AM, LoopStandardAnalysisResults& LSAR, LPMUpdater updater) {
  //   //Loop* loop = nest.getInnermostLoop();
  //   Function *F = nest.getParent();
  //   FunctionAnalysisManager FAM = FunctionAnalysisManager();
  //   return run(*F, FAM);
  // }
};

} // end anonymous namespace

// -----------------------------------------------------------------------------
// Plugin Registration
// -----------------------------------------------------------------------------
// llvm::PassPluginLibraryInfo getSporkUnrollPluginInfo() {
//   return {LLVM_PLUGIN_API_VERSION, "SporkUnroll", LLVM_VERSION_STRING,
//           [](PassBuilder &PB) {
//             // Register passes so they can be called via command line string
//             PB.registerPipelineParsingCallback(
//                 [](StringRef Name, FunctionPassManager &FPM,
//                    ArrayRef<PassBuilder::PipelineElement>) {
//                   if (Name == "spork-pre-unroll") {
//                     FPM.addPass(SporkPreUnrollPass());
//                     return true;
//                   }
//                   if (Name == "spork-post-unroll") {
//                     FPM.addPass(SporkPostUnrollPass());
//                     return true;
//                   }
//                   return false;
//                 });
//           }};
// }

llvm::PassPluginLibraryInfo getSporkUnrollPluginInfo() {
  return
    {LLVM_PLUGIN_API_VERSION, "SporkUnroll", LLVM_VERSION_STRING,
     [](PassBuilder &PB) {
       // 1. Hook the Pre-Unroll pass BEFORE loop transformations begin
       // PB.registerLoopOptimizerEndEPCallback(
       //     [](LoopPassManager &LPM, OptimizationLevel Level) {
       //       // In some LLVM versions, this is a good place to catch 
       //       // the state before the final unrolling sweeps.
       //     });
     
       // 2. The most robust "Auto-Injection" for O3:
       PB.registerScalarOptimizerLateEPCallback(
         [](FunctionPassManager &FPM, OptimizationLevel Level) {
           FPM.addPass(SporkPreUnrollPass());
           
           // Re-trigger the standard Loop Unroll pass here to 
           // ensure it happens while our volatiles are stripped.
           LoopUnrollOptions opts = LoopUnrollOptions();
           FPM.addPass(LoopUnrollPass(opts));
           
           FPM.addPass(SporkPostUnrollPass());

           FPM.addPass(LoopUnrollPass(opts));
           });
     
       // 3. Keep the manual parsing (for 'opt' usage)
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
             return false;
           });
     }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getSporkUnrollPluginInfo();
}
