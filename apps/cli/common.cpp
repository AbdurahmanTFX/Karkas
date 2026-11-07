#include <iostream>

#include "app.hpp"
#include "kk/index/index.hpp"
#ifdef KK_HAVE_CODE
#include "kk/code/analyzer.hpp"
#endif

namespace kk::cli {

index::ICodeAnalyzer* analyzer() {
#ifdef KK_HAVE_CODE
  static code::CppAnalyzer a;
  return &a;
#else
  return nullptr;
#endif
}

std::unique_ptr<index::Index> open_index(const fs::path& root, bool update, bool full) {
  auto idx = index::Index::open(root);
  if (!idx) die(idx.error().str());
  if (update) {
    auto st = (*idx)->update({full, analyzer()});
    if (!st) die(st.error().str());
  }
  return std::move(*idx);
}

}  // namespace kk::cli
