
#include <cassert>
#include <cmath>
#include <cstdint>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>
#include <type_traits>
#include <utility>
#include <vector>

#include "Passes/StackAlloca.h"
#include "Passes/StackPointerFinder.h"
#include "Utils/Utils.h"

namespace notdec {

const char *KIND_STACK_DIRECTION = "notdec.stack_direction";
const char *KIND_STACK_DIRECTION_NEGATIVE = "negative";

bool isGrowNegative(Instruction *Inst) {
  if (llvm::MDNode *MD = Inst->getMetadata(KIND_STACK_DIRECTION)) {
    // 验证元数据格式：应该包含一个MDString元素
    if (MD->getNumOperands() > 0) {
      if (auto *MDS = llvm::dyn_cast<llvm::MDString>(MD->getOperand(0))) {
        // 尝试将字符串转换为整数
        llvm::StringRef MDSRef = MDS->getString();
        if (MDSRef == KIND_STACK_DIRECTION_NEGATIVE) {
          return true;
        }
      }
    }
  }
  return false;
}

Instruction *createAllocaWithSize(IRBuilder<> &Builder, Value *Size,
                                  const Twine &Name) {
  auto i8 = IntegerType::get(Builder.getContext(), 8);
  if (auto C = dyn_cast<ConstantInt>(Size)) {
    auto si8 = ArrayType::get(i8, C->getSExtValue());
    return Builder.CreateAlloca(si8, nullptr, Name);
  }
  return Builder.CreateAlloca(i8, Size, Name);
}

void getPhiClosure(std::set<Value *> &Ret, PHINode *P) {
  if (!Ret.count(P)) {
    Ret.insert(P);
    for (auto &U : P->incoming_values()) {
      if (auto P = dyn_cast<PHINode>(U.get())) {
        getPhiClosure(Ret, P);
      } else {
        Ret.insert(U.get());
      }
    }
  }
}

// match dynamic stack allocation (optimized)
void LinearAllocationRecovery::matchDynamicAllocas(Function &F, Value *SP,
                                                   Instruction *LoadSP,
                                                   Instruction *add_load_sp,
                                                   Value *space,
                                                   bool isGrowNegative) {
  using namespace llvm::PatternMatch;
  Value *StackLoc = nullptr;
  Value *SizeVal = nullptr;

  // %sub = sub i32 %OldStackEnd, %Size
  // store i32 %sub, i32* @__stack_pointer
  // instcombine will convert sub to add generally.
  auto pat_alloca_sub =
      m_Store(m_Sub(m_Value(StackLoc), m_Value(SizeVal)), m_Specific(SP));

  // %sub = add i32 %OldStackEnd, %Size
  // store i32 %sub, i32* @__stack_pointer
  // sub的值可能是原始的sp,也可能是sub之后的sp.
  // 如果是原始的sp，就需要调整为相对于栈顶的大小
  auto pat_alloca_add =
      m_Store(m_Add(m_Value(StackLoc), m_Value(SizeVal)), m_Specific(SP));

  // replace %sub with alloca i8, %Size
  // remove store
  std::vector<llvm::Instruction *> toRemove;
  std::vector<std::pair<llvm::Instruction *, llvm::Instruction *>> toReplace;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (PatternMatch::match(&I, pat_alloca_add)) {
        auto Add = llvm::cast<Instruction>(I.getOperand(0));
        IRBuilder<> Builder(I.getParent());
        Builder.SetInsertPoint(&I);
        auto AllocaSize = SizeVal;
        std::set<Value *> PhiClosure;
        if (auto Phi = dyn_cast<PHINode>(StackLoc)) {
          getPhiClosure(PhiClosure, Phi);
        }
        if (StackLoc == LoadSP || PhiClosure.count(LoadSP)) {
          assert(!PhiClosure.count(add_load_sp));
          AllocaSize = Builder.CreateSub(AllocaSize, space);
        } else if (StackLoc == add_load_sp || PhiClosure.count(add_load_sp)) {
          // relative to the top of the stack
        } else {
          llvm::errs() << "Error: unrecognized sp modification: " << I << "\n";
          continue;
        }
        if (isGrowNegative) {
          AllocaSize = Builder.CreateNeg(AllocaSize);
        }
        Instruction *Alloca =
            createAllocaWithSize(Builder, AllocaSize, "alloc_mem");
        Alloca = llvm::cast<Instruction>(
            Builder.CreatePtrToInt(Alloca, Add->getType()));
        // create a alloca inst with arg Size.
        toRemove.push_back(&I);
        toReplace.push_back(std::make_pair(Add, Alloca));
      } else if (PatternMatch::match(&I, pat_alloca_sub)) {
        // todo check for
        auto Sub = llvm::cast<Instruction>(I.getOperand(0));
        IRBuilder<> Builder(I.getParent());
        Builder.SetInsertPoint(&I);
        auto AllocaSize = SizeVal;
        if (StackLoc == LoadSP) {
          AllocaSize = Builder.CreateSub(AllocaSize, space);
        } else if (StackLoc == add_load_sp) {
          // relative to the top of the stack
        } else {
          llvm::errs() << "Error: unrecognized sp modification: " << I << "\n";
          // continue;
          // assume as stack top for sub
        }
        if (!isGrowNegative) {
          AllocaSize = Builder.CreateNeg(AllocaSize);
        }
        // llvm::errs() << "stack alloca: " << *SizeVal << "\n";
        Instruction *Alloca =
            createAllocaWithSize(Builder, AllocaSize, "alloc_mem");
        Alloca = llvm::cast<Instruction>(
            Builder.CreatePtrToInt(Alloca, Sub->getType()));
        // create a alloca inst with arg Size.
        toRemove.push_back(&I);
        toReplace.push_back(std::make_pair(Sub, Alloca));
      }
    }
  }
  for (auto I : toRemove) {
    I->eraseFromParent();
  }
  for (auto P : toReplace) {
    P.first->replaceAllUsesWith(P.second);
  }
}

/// Scan for stack pointer subtraction and convert to alloca.
/// Uses ptrtoint to ensure type correctness.
///
/// Typical case:
/// entry:
/// %LoadSP = load i32, i32* @__stack_pointer, align 4
/// %add_load_sp = add i32 %0, -16 ; %space
/// store i32 %1, i32* @__stack_pointer, align 4 ; update the stack pointer
/// ... ; real function body
/// return_block:
/// store i32 %0, i32* @__stack_pointer, align 4 ; restore the stack pointer
/// ...; probably other instructions
/// ret i32 0
///
/// For tail functions, the update and the restore step can be omitted. and they
/// just address the stack directly.
/// TODO handle tail function with no stack space. two pattern will not match.
/// rely on canonical form of LLVM Instructions, So run instcombine first.
/// see:
/// https://www.npopov.com/2023/04/10/LLVM-Canonicalization-and-target-independence.html
///
/// Currently we only replace the stack pointer load and restore with alloca. No
/// add is converted to gep currently. If the stack grows negative, the offsets
/// will also be negative. But in the alloca, the size is still positive.
PreservedAnalyses LinearAllocationRecovery::run(Module &M,
                                                ModuleAnalysisManager &MAM) {
  errs() << " ============== LinearAllocationRecovery  ===============\n";
  auto DebugDir = std::getenv("NOTDEC_TYPE_RECOVERY_DEBUG_DIR");
  if (DebugDir) {
    printModule(M, notdec::join(DebugDir, "01-1-BeforeStackAlloca.ll").c_str());
  }

  auto sp_result = MAM.getResult<StackPointerFinderAnalysis>(M);
  auto sp = sp_result.result;
  if (sp == nullptr) {
    std::cerr << "ERROR: Stack pointer is not found!!";
    std::cerr << "LinearAllocationRecovery cannot proceed!\n";
    return PreservedAnalyses::all();
  }
  // iterate each use of sp, collect a list of functions to process.
  std::set<Function *> worklist;
  for (auto U : sp->users()) {
    if (Instruction *I = dyn_cast<Instruction>(U)) {
      auto *F = I->getFunction();
      worklist.emplace(F);
    }
  }

  if (worklist.empty()) {
    std::cerr << "ERROR: The stack pointer is not used in any function?";
    std::cerr << "LinearAllocationRecovery cannot proceed!\n";
    return PreservedAnalyses::all();
  }

  using namespace llvm::PatternMatch;
  Value *sp1;
  Instruction *LoadSP;
  Value *space;
  Instruction *add_load_sp;
  ConstantInt *offset;

  auto pat_alloc = StackPointerMatcher(sp1, space, LoadSP, add_load_sp, sp);
  auto pat_alloc_offset = m_Add(m_Load(m_Specific(sp)), m_ConstantInt(offset));

  // ======== 1. Matching patterns. ===========

  IRBuilder Builder(M.getContext());
  for (auto *F : worklist) {
    sp1 = nullptr;
    LoadSP = nullptr;
    space = nullptr;
    add_load_sp = nullptr;
    Instruction *prologue_store = nullptr;
    // 1. Match for stack allocation level.
    // level = 2 normal stack allocation.
    // level = 1 tail function stack allocation.
    // level = 0 cannot match stack allocation.
    int match_level = 0;
    auto *entry = &F->getEntryBlock();
    // 1.1 Match normal stack allocation in the entry block.
    for (auto &I : *entry) {
      if (PatternMatch::match(&I, pat_alloc)) {
        prologue_store = &I;
        match_level = 2;
        // llvm::errs() << "stack alloc: " << *space << "\n";
        break;
      }
    }
    // 1.2 Try to match tail function stack allocation if not matched.
    // There should be at least one Add(Load(sp), offset).
    if (match_level == 0) {
      bool has_load_sp = false;
      for (auto &BB : *F) {
        for (auto &I : BB) {
          if (PatternMatch::match(&I, pat_alloc_offset)) {
            LoadSP = cast<Instruction>(I.getOperand(0));
            match_level = 1;
            // llvm::errs() << "stack alloc: " << *space << "\n";
            break;
          }
          if (PatternMatch::match(&I, m_Load(m_Specific(sp)))) {
            has_load_sp = true;
          }
        }
      }
      if (match_level == 0 && has_load_sp) {
        llvm::errs() << "ERROR: No pattern matched but the stack pointer is "
                        "accessed in func: "
                     << F->getName() << "!\n";
      }
    }
    // 1.3 Failed to match any stack allocation.
    if (match_level == 0) {
      llvm::errs() << "ERROR: cannot find stack allocation in func: "
                   << F->getName() << "\n";
      continue;
    }
    // For normal stack allocation (level = 2): Remove epilogue that restore the
    // stack pointer
    if (match_level == 2) {
      assert(add_load_sp != nullptr && space != nullptr);
      assert(add_load_sp->getParent() == LoadSP->getParent());
      // find epilogue in exit blocks, and remove it.
      auto pat_restore = m_Store(m_Specific(LoadSP), m_Specific(sp));
      bool removed = false;
      for (auto &BB : *F) {
        if (BB.getTerminator()->getNumSuccessors() == 0) {
          for (auto &I : make_early_inc_range(BB)) {
            if (PatternMatch::match(&I, pat_restore)) {
              // llvm::errs() << "recover sp: " << I << "\n";
              I.eraseFromParent();
              removed = true;
              break;
            }
          }
        }
      }
      if (!removed) {
        llvm::errs() << "ERROR: Cannot find sp restore? func: " << F->getName()
                     << "\n";
        continue;
      }
    }

    // replace the stack allocation with alloca.
    bool grow_negative = sp_result.direction == 0;

    // remove prologue store in case matched by dynamic alloca matching
    if (match_level == 2) {
      prologue_store->eraseFromParent();
    }

    if (match_level == 2) {
      matchDynamicAllocas(*F, sp, LoadSP, add_load_sp, space, grow_negative);
    }

    auto SizeTy = IntegerType::get(M.getContext(),
                                   M.getDataLayout().getPointerSizeInBits());
    // TODO handle positive grow direction.
    assert(grow_negative == true);
    // When match_level == 1, Only LoadSP is not null.
    if (match_level == 1) {
      space = ConstantInt::getNullValue(SizeTy);
    }
    //  else if (grow_negative) {
    //   space = Builder.CreateNeg(space);
    // }
    Builder.SetInsertPoint(LoadSP);
    Value *alloc = createAllocaWithSize(
        Builder, grow_negative ? Builder.CreateNeg(space) : space, "stack");
    cast<Instruction>(alloc)->setMetadata(
        KIND_STACK_DIRECTION,
        MDNode::get(
            M.getContext(),
            MDString::get(M.getContext(), KIND_STACK_DIRECTION_NEGATIVE)));
    alloc = Builder.CreatePtrToInt(alloc, LoadSP->getType(), "stack_addr");
    Value *alloc_end = Builder.CreateAdd(alloc, space, "stack_end");

    // The name is incorrect. high_addr does not mean it will must
    Instruction *high_addr = add_load_sp;
    Instruction *low_addr = LoadSP;
    // if (grow_negative) {
    //   std::swap(high_addr, low_addr);
    // }
    // replace all uses of LoadSP with alloc_end.
    low_addr->replaceAllUsesWith(alloc);
    low_addr->eraseFromParent();
    if (high_addr != nullptr) {
      assert(match_level == 2);
      high_addr->replaceAllUsesWith(alloc_end);
      high_addr->eraseFromParent();
    }
  }
  // perform the transformation:

  return PreservedAnalyses::none();
}
} // namespace notdec
