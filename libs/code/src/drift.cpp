#include "kk/code/drift.hpp"

#include "kk/code/analyzer.hpp"
#include "kk/core/paths.hpp"
#include "kk/index/index.hpp"

namespace kk::code {

std::vector<core::Issue> check_drift(const core::Schema& schema) {
  CppAnalyzer an;
  index::IndexFacts facts;
  facts.files = index::collect_code_files(schema);
  for (const auto& f : facts.files) {
    auto text = core::read_file(schema.root / f.path);
    if (text) facts.facts[f.path] = an.analyze(f, *text);
  }
  for (const auto& b : schema.blocks) {
    if (b.has_code()) facts.api_hash[b.id] = index::block_api_hash(facts.files, b.id);
  }
  auto state = index::StateStore(schema.root).load();
  if (state) facts.state = &*state;
  return an.check(schema, facts).issues;
}

}  // namespace kk::code
