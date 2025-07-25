#ifndef _NOTDEC_PASSES_STACK_POINTER_FINDER_H_
#define _NOTDEC_PASSES_STACK_POINTER_FINDER_H_

#include <cassert>
#include <iostream>
#include <map>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Pass.h>

namespace notdec {

using namespace llvm;
void addSPMetadata(GlobalVariable *GV);

/// Match the following pattern:
/// store (add (load sp) space) sp
/// If specific_sp is not null, then specifically match sp as specific_sp.
/// For tail function, there is no store to the stack pointer.
struct StackPointerMatcher {
  Value *&sp;
  Value *&space;
  Instruction *&load;
  Instruction *&add;
  Value *specific_sp = nullptr;

  StackPointerMatcher(Value *&sp, Value *&space, Instruction *&load,
                      Instruction *&add)
      : sp(sp), space(space), load(load), add(add) {}

  StackPointerMatcher(Value *&sp, Value *&space, Instruction *&load,
                      Instruction *&add, Value *specific_sp)
      : sp(sp), space(space), load(load), add(add), specific_sp(specific_sp) {}

  bool match(Instruction *I) {
    using namespace llvm::PatternMatch;
    auto pat_alloc1 =
        m_Store(m_Add(m_Load(m_Value(sp)), m_Value(space)), m_Deferred(sp));
    auto pat_alloc2 =
        m_Store(m_Add(m_Value(space), m_Load(m_Value(sp))), m_Deferred(sp));
    auto pat_alloc_sub =
        m_Store(m_Sub(m_Load(m_Value(sp)), m_Value(space)), m_Deferred(sp));

    if (PatternMatch::match(I, pat_alloc1) ||
        PatternMatch::match(I, pat_alloc_sub)) {
      if (specific_sp != nullptr && sp != specific_sp) {
        return false;
      }
      add = cast<Instruction>(I->getOperand(0));
      load = cast<Instruction>(add->getOperand(0));
      return true;
    } else if (PatternMatch::match(I, pat_alloc2)) {
      if (isa<Constant>(space)) {
        assert(false && "instCombine should have moved the constant to the "
                        "second operand!");
      }
      if (specific_sp != nullptr && sp != specific_sp) {
        return false;
      }
      add = cast<Instruction>(I->getOperand(0));
      load = cast<Instruction>(add->getOperand(1));
      return true;
    }
    return false;
  }
};

struct StackPointerFinderResult {
  GlobalVariable *result;
  long direction; // 0 for down, 1 for up.
};

class StackPointerFinderAnalysis
    : public AnalysisInfoMixin<StackPointerFinderAnalysis> {
public:
  static const char *StackPointerNames[];

  explicit StackPointerFinderAnalysis() = default;
  ~StackPointerFinderAnalysis() = default;
  // Provide a unique key, i.e., memory address to be used by the LLVM's pass
  // infrastructure.
  static inline llvm::AnalysisKey Key; // NOLINT
  friend llvm::AnalysisInfoMixin<StackPointerFinderAnalysis>;

  // Specify the result type of this analysis pass.
  using Result = StackPointerFinderResult;

  // the number of functions that vote for the stack pointer.
  std::map<GlobalVariable *, size_t> sp_count;
  // the number of functions that vote for the stack direction.
  std::map<long, size_t> direction_count;
  static GlobalVariable *find_stack_ptr(Module &mod);
  // GlobalVariable *find_stack_ptr(Module &mod);
  GlobalVariable *find_stack_ptr(Function &f);
  GlobalVariable *find_stack_ptr(BasicBlock &entryBlock);

  Result run(llvm::Module &M);

  // Analyze the bitcode/IR in the given LLVM module.
  Result run(llvm::Module &M, llvm::ModuleAnalysisManager &) { return run(M); }
};
} // namespace notdec
#endif
