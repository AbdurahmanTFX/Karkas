#pragma once
// Delegation support for a session that hands a block to a smaller model (docs/adr/0018):
// the brief carries everything Karkas knows about the block, with the parts only the delegating
// model can write marked [fill]; the review gathers what has to be checked before accepting.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/config.hpp"
#include "kk/core/error.hpp"
#include "kk/core/schema.hpp"
#include "kk/index/index.hpp"

namespace kk::agents {

// The brief for the subagent of `block`: the subagent and its model, [fill] lines (task, spec
// with concrete numbers, done-when), the card (purpose, files, contract, guarantees), the api of
// the blocks it may use, what not to touch, project rules, how to build and test, the report.
// `task` (optional) is put into the task line as a start.
kk::Result<std::string> make_brief(const std::filesystem::path& root, const core::Schema& schema, const core::Config& cfg,
                                   const index::Snapshot& snap, const std::vector<index::RunRow>& runs,
                                   std::string_view block, std::string_view task = "");

// What to check before accepting the work on `block`: its guarantees as a checklist, the changed
// files (git; files outside the block flagged), the diff of the block's files (first lines), KSL
// and drift issues of the block, its test status.
kk::Result<std::string> make_review(const std::filesystem::path& root, const core::Schema& schema,
                                    const index::Snapshot& snap, std::string_view block);

}  // namespace kk::agents
