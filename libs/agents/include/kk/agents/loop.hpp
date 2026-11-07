#pragma once
// The agent loop (9.4): request → tool_use → tool_result → ... until a terminal tool,
// the step limit, the token budget or the timeout. Every step goes to the run journal,
// the total to the runs table.
#include "kk/agents/types.hpp"
#include "kk/core/config.hpp"
#include "kk/index/index.hpp"

namespace kk::agents {

class AgentRunner {
 public:
  AgentRunner(fs::path project_root, const core::Config& cfg, ProviderFactory providers, index::Index* idx);
  AgentResult run(const AgentSpec& spec, IToolHost& host);

 private:
  fs::path root_;
  const core::Config& cfg_;
  ProviderFactory providers_;
  index::Index* idx_;
};

// The tier's model name and provider from the config ("" when the tier is missing).
const core::ModelTier* tier_config(const core::Config& cfg, const std::string& tier);

}  // namespace kk::agents
