// karkas mode [api|mcp] [--fit] — where the answers of the agents' model come from (ADR 0024):
// "api", Karkas calls the models of [models.*]; "mcp", an MCP client (Claude Code) is their model
// and serves their turns. The same engine and protocol in both; each mode has its own model maps.
#include <fmt/format.h>

#include <iostream>

#include "app.hpp"
#include "kk/code/init.hpp"
#include "kk/core/config.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::cli {
namespace {

void print_maps(const core::Config& cfg) {
  auto tier_text = [&](const std::string& tier) {
    if (tier == "auto") return std::string("auto (routing by the size of the task)");
    const auto* t = cfg.tier(tier);
    if (t == nullptr) return tier + " (not in [models])";
    std::string s = tier + " = " + t->model;
    if (cfg.mcp()) s += t->claude_model().empty() ? "  (not a Claude model: no Claude Code subagent)" : "  → subagent " + core::subagent_name(tier);
    return s;
  };
  std::cout << fmt::format("mode {} — {}\n", cfg.agents_mode,
                           cfg.mcp() ? "an MCP client (Claude Code) is the agents' model: kk_task / kk_debug, then kk_agent_next / kk_agent_act"
                                     : "Karkas calls the models itself (keys from the environment)");
  std::string prefix = cfg.mcp() ? "mcp." : "";
  std::cout << "[" << prefix << "levels]\n";
  if (cfg.levels.empty()) std::cout << "  (none: roles and routing decide)\n";
  for (const auto& [lv, t] : cfg.levels) std::cout << fmt::format("  {} = {}\n", lv, tier_text(t));
  std::cout << "[" << prefix << "roles]\n";
  for (const auto& [role, t] : cfg.roles) std::cout << fmt::format("  {} = {}\n", role, tier_text(t));
  if (!cfg.block_tiers.empty()) {
    std::cout << "[" << prefix << "blocks]\n";
    for (const auto& [b, t] : cfg.block_tiers) std::cout << fmt::format("  {} = {}\n", b, tier_text(t));
  }
  std::cout << "escalation: " << (cfg.escalate.empty() ? std::string("local → cheap → medium → strong") : core::join(cfg.escalate, " → ")) << "\n";
}

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("mode", "where the agents' model answers come from: api (Karkas calls the models) | mcp (an MCP client "
                                         "such as Claude Code answers their turns)");
  static std::string mode;
  static bool fit = false;
  cmd->add_option("mode", mode, "api | mcp (none: show the mode and its models)")->check(CLI::IsMember({"api", "mcp"}));
  cmd->add_flag("--fit", fit, "fit the models of mode mcp to those of mode api: Claude tiers of the same strength per level, role and block");
  cmd->callback([&app] {
    auto root = app.root();
    if (!mode.empty()) {
      if (auto r = core::write_config_value(root, "agents", "mode", mode); !r) die(r.error().str());
    }
    if (fit) {
      auto cfg = core::load_config(root);
      if (!cfg) die(cfg.error().str());
      auto text = core::read_file(root / "karkas.toml");
      if (!text) die(text.error().str());
      auto out = core::write_mcp_maps(*text, core::mcp_maps_from_api(*cfg));
      if (!out) die(out.error().str());
      if (auto w = core::write_file(root / "karkas.toml", *out); !w) die(w.error().str());
      std::cout << "fitted [mcp.levels] [mcp.roles] [mcp.blocks] to the API models\n";
    }
    auto cfg = core::load_config(root);
    if (!cfg) die(cfg.error().str());
    print_maps(*cfg);
    // The subagents follow the tiers ([models.*]); written when missing.
    if (cfg->mcp() && !std::filesystem::exists(root / ".claude" / "agents")) {
      auto g = code::write_claude_agents(root, *cfg);
      if (g) std::cout << "wrote the Claude Code subagents of the tiers (.claude/agents/)\n";
    }
  });
});

}  // namespace
}  // namespace kk::cli
