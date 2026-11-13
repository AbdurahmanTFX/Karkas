#pragma once
// Model routing (11.2) and self-tuning from the runs table (11.4).
#include <string>
#include <vector>

#include "kk/core/config.hpp"
#include "kk/core/schema.hpp"
#include "kk/index/index.hpp"

namespace kk::agents {

struct RouteInput {
  std::string est;            // S | M | L
  bool crosses_threads = false;
  bool has_state = false;
  int impl_lines = 0;
  int failed_tickets_30d = 0;
  std::string difficulty;     // from the trail entry above: S | M | L
};

struct Route {
  int score = 0;
  std::string tier;     // cheap | medium | strong
  std::string reason;   // "est=M+1 cross+2 ..." for the journal
  std::string bucket;   // feature bucket for self-tuning
};

// bucket_runs / bucket_ok: first-attempt cheap runs of the same feature bucket (11.4).
Route route(const RouteInput& in, const core::Config& cfg, int bucket_runs = 0, int bucket_ok = 0);
RouteInput route_input(const core::Schema& schema, const index::Snapshot& snap, const std::string& block,
                       const std::string& difficulty, const std::vector<index::RunRow>& runs);
std::string next_tier(const std::string& tier);  // cheap → medium → strong → ""
// The next tier of [routing] escalate when it is set (the built-in chain otherwise).
std::string next_tier(const core::Config& cfg, const std::string& tier);

// route() of a leaf with the self-tuning statistics of its feature bucket taken from `runs`
// (first-attempt cheap worker runs, 11.4).
Route route_block(const core::Schema& schema, const index::Snapshot& snap, const std::vector<index::RunRow>& runs,
                  const std::string& block, const std::string& difficulty, const core::Config& cfg);

// The model for an agent working on a block (ADR 0011, 0013), as Karkas's own agents pick it:
// the block or its nearest ancestor in [blocks], then [levels], then the role ([roles]; a
// worker with role "auto" is routed by the size of the leaf, 11.2).
struct ModelChoice {
  std::string block;  // "" = the project
  int level = 0;
  std::string role;   // worker | tester | architect | ...
  std::string tier;   // strong | medium | cheap | a custom [models.<tier>]
  std::string by;     // "block game", "level 2", "role architect", "routing score=3 est=M+1"
  bool defined = false;  // the tier has a [models.<tier>] section
  core::ModelTier model;
  // Model id an external Claude Code session can run for this tier ("" when the tier is not an
  // Anthropic model), and the name of the generated subagent (.claude/agents/<name>.md).
  std::string claude_model;
  std::string subagent;
};
// role "" = worker for a block with code, architect for a group or the project.
kk::Result<ModelChoice> model_for(const core::Config& cfg, const core::Schema& schema, const index::Snapshot& snap,
                                  const std::vector<index::RunRow>& runs, std::string_view block, std::string role = "");
// The text of kk_model_for: tier, model and what an external agent should do with it.
std::string model_choice_text(const ModelChoice& c);
// One line of the same for kk_view: "model: tier cheap (...) -> subagent karkas-cheap (...)".
std::string model_choice_line(const ModelChoice& c);

}  // namespace kk::agents
