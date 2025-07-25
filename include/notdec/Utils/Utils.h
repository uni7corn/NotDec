#ifndef _NOTDEC_UTILS_H_
#define _NOTDEC_UTILS_H_

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <sstream>
#include <string>
#include <variant>

std::string getSuffix(std::string fname);

enum log_level {
  level_emergent = 0,
  level_alert = 1,
  level_critical = 2,
  level_error = 3,
  level_warning = 4,
  level_notice = 5,
  level_info = 6,
  level_debug = 7
};

extern llvm::cl::opt<log_level> logLevel;

namespace notdec {

std::string getFuncSetName(const std::set<llvm::Function *> &SCC);
std::string readFileToString(const char *path);

bool inline is_size_t(llvm::Type *ty, llvm::Module &M) {
  return ty->isIntegerTy(M.getDataLayout().getPointerSizeInBits());
}

bool inline is_size_t(llvm::Value *val, llvm::Module &M) {
  return is_size_t(val->getType(), M);
}

void inline assert_size_t(llvm::Type *ty, llvm::Module &M) {
  assert(is_size_t(ty, M));
}

void inline assert_size_t(llvm::Value *val, llvm::Module &M) {
  assert(is_size_t(val, M));
}

bool printModule(llvm::Module &M, const char *path);
bool equal(llvm::StringRef S1, const char* S2);

/// An ilist node that can access its parent list and has eraseFromParent impl.
///
/// Requires \c NodeTy to have \a getParent() to find the parent node, and the
/// \c ParentTy to have \a getSublistAccess() to get a reference to the list.
template <typename NodeTy, typename ParentTy, class... Options>
class node_with_erase
    : public llvm::ilist_node_with_parent<NodeTy, ParentTy, Options...> {
protected:
  ParentTy *Parent = nullptr;

public:
  inline ParentTy *getParent() { return Parent; }
  typename llvm::iplist<NodeTy>::iterator eraseFromParent() {
    auto &List = getParent()->*(ParentTy::getSublistAccess((NodeTy *)nullptr));
    return List.erase(this->getIterator());
  }

  node_with_erase(ParentTy &P) : Parent(&P) {}
};

template <typename ParentTy, typename TargetTy> class DSULink {
  using LinkTy = std::variant<ParentTy *, TargetTy>;

protected:
  LinkTy Link;

public:
  DSULink(TargetTy Val) : Link(Val) {}
  bool isForward() { return std::holds_alternative<ParentTy *>(Link); }
  ParentTy *getForward() { return std::get<ParentTy *>(Link); }
  TargetTy getTarget() { return std::get<TargetTy>(Link); }
  void setForward(ParentTy *Val) { Link = Val; }
  static ParentTy *getRepresent(ParentTy *N) {
    std::vector<ParentTy *> path;
    while (N->getLink().isForward()) {
      path.push_back(N);
      N = N->getLink().getForward();
    }
    for (auto *P : path) {
      P->getLink() = N;
    }
    return N;
  }
  static TargetTy lookup(ParentTy *N) {
    return getRepresent(N)->getLink().getTarget();
  }
};

std::string join(std::string path, std::string elem);

template <class result_t = std::chrono::milliseconds,
          class clock_t = std::chrono::steady_clock,
          class duration_t = std::chrono::milliseconds>
auto since(std::chrono::time_point<clock_t, duration_t> const &start) {
  return std::chrono::duration_cast<result_t>(clock_t::now() - start);
}

} // namespace notdec

#endif
