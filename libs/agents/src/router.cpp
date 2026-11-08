#include "kk/agents/router.hpp"

#include <fmt/format.h>

#include <map>
#include <set>

#include "kk/core/text.hpp"

namespace kk::agents {

std::string next_tier(const std::string& tier) {
  if (tier == "local") return "cheap";
  if (tier == "cheap") return "medium";
  if (tier == "medium") return "strong";
  return {};
}

Route route(const RouteInput& in, const core::Config& cfg, int bucket_runs, int bucket_ok) {
  Route r;
  std::vector<std::string> why;
  auto add = [&](int pts, std::string what) {
    if (pts == 0) return;
    r.score += pts;
    why.push_back(fmt::format("{}+{}", what, pts));
  };
  add(in.est == "L" ? 3 : in.est == "M" ? 1 : 0, "est=" + in.est);
  add(in.crosses_threads ? 2 : 0, "cross");
  add(in.has_state ? 2 : 0, "state");
  add(in.impl_lines > 300 ? 1 : 0, "impl>300");
  add(std::min(in.failed_tickets_30d, 2), "failed");
  add(in.difficulty == "L" ? 3 : in.difficulty == "M" ? 1 : 0, "difficulty=" + in.difficulty);
  r.tier = r.score <= cfg.cheap_max ? "cheap" : r.score <= cfg.medium_max ? "medium" : "strong";
  r.bucket = fmt::format("est={} cross={} state={} big={}", in.est.empty() ? "S" : in.est, in.crosses_threads ? 1 : 0,
                         in.has_state ? 1 : 0, in.impl_lines > 300 ? 1 : 0);
  // Self-tuning (11.4): if cheap rarely succeeds at the first attempt for this kind of
  // leaf, start at medium.
  if (cfg.self_tune && r.tier == "cheap" && bucket_runs >= 5 &&
      static_cast<double>(bucket_ok) / static_cast<double>(bucket_runs) < 0.6) {
    r.tier = "medium";
    why.push_back(fmt::format("self-tune cheap {}/{}", bucket_ok, bucket_runs));
  }
  r.reason = fmt::format("score={} {}", r.score, core::join(why, " "));
  return r;
}

RouteInput route_input(const core::Schema& schema, const index::Snapshot& snap, const std::string& block,
                       const std::string& difficulty, const std::vector<index::RunRow>& runs) {
  RouteInput in;
  in.difficulty = difficulty;
  if (const auto* b = schema.find(block)) {
    in.est = b->est.value;
    in.has_state = !b->states.empty();
  }
  for (const auto& l : schema.links()) {
    if (l.kind == core::LinkKind::Use) continue;
    if (l.src_block != block && l.dst_block != block) continue;
    if (!l.thread_src.empty() && !l.thread_dst.empty() && l.thread_src != l.thread_dst) in.crosses_threads = true;
    if (l.kind == core::LinkKind::State) in.has_state = true;
  }
  if (const auto* row = snap.block(block)) in.impl_lines = row->impl_lines;
  auto month_ago = core::unix_now() - 30LL * 24 * 3600;
  std::set<std::string> failed;
  for (const auto& r : runs) {
    if (r.block == block && r.ts >= month_ago && (r.outcome == "failed" || r.outcome == "escalated") && !r.ticket.empty()) {
      failed.insert(r.ticket);
    }
  }
  in.failed_tickets_30d = static_cast<int>(failed.size());
  return in;
}

}  // namespace kk::agents

namespace kk::agents {

Route route_block(const core::Schema& schema, const index::Snapshot& snap, const std::vector<index::RunRow>& runs,
                  const std::string& block, const std::string& difficulty, const core::Config& cfg) {
  auto in = route_input(schema, snap, block, difficulty, runs);
  std::map<std::string, std::string> bucket_of;
  int n = 0;
  int ok = 0;
  auto mine = route(in, cfg).bucket;
  for (const auto& r : runs) {
    if (r.role != "worker" || r.attempt != 1 || r.tier != "cheap" || r.block.empty()) continue;
    if (!bucket_of.contains(r.block)) bucket_of[r.block] = route(route_input(schema, snap, r.block, "", runs), cfg).bucket;
    if (bucket_of[r.block] != mine) continue;
    ++n;
    ok += (r.outcome == "fixed" || r.outcome == "done") ? 1 : 0;
  }
  return route(in, cfg, n, ok);
}

kk::Result<ModelChoice> model_for(const core::Config& cfg, const core::Schema& schema, const index::Snapshot& snap,
                                  const std::vector<index::RunRow>& runs, std::string_view block_in, std::string role) {
  // Default tiers of the roles, as the engine uses them when [roles] does not name one.
  static const std::map<std::string, std::string, std::less<>> kDefaults = {
      {"worker", "cheap"},     {"tester", "cheap"},   {"integration_tester", "medium"}, {"architect", "strong"},
      {"orchestrator", "strong"}, {"navigator", "medium"}, {"composer", "medium"},          {"ask", "cheap"}};
  ModelChoice c;
  c.block = block_in == "root" ? std::string() : std::string(block_in);
  const core::Block* b = c.block.empty() ? nullptr : schema.find(c.block);
  if (!c.block.empty() && b == nullptr) return kk::fail("model", "no block " + c.block);
  if (role.empty()) role = b != nullptr && b->has_code() ? "worker" : "architect";
  auto def = kDefaults.find(role);
  if (def == kDefaults.end()) {
    std::vector<std::string> names;
    for (const auto& [n, _] : kDefaults) names.push_back(n);
    return kk::fail("model", fmt::format("unknown role {} (one of: {})", role, core::join(names, ", ")));
  }
  c.role = role;
  c.level = core::block_level(c.block);
  auto by_role = [&](const std::string& fallback, const std::string& why) {
    auto it = cfg.roles.find(role);
    if (it != cfg.roles.end() && it->second != "auto" && !it->second.empty()) {
      c.tier = it->second;
      c.by = "role " + role;
    } else {
      c.tier = fallback;
      c.by = why;
    }
  };
  if (auto p = cfg.pin(c.block); !p.tier.empty()) {
    c.tier = p.tier;
    c.by = "pinned by " + p.by;
  } else if (role == "worker" && b != nullptr && b->has_code()) {
    auto rt = route_block(schema, snap, runs, c.block, "", cfg);
    by_role(rt.tier, "routing " + std::string(core::trim(rt.reason)));
  } else if (role == "tester" && b != nullptr && b->est.value == "L") {
    c.tier = "medium";
    c.by = "tester of an est=L leaf";
  } else {
    by_role(def->second, "default of role " + role);
  }
  if (const auto* t = cfg.tier(c.tier)) {
    c.defined = true;
    c.model = *t;
    c.claude_model = t->claude_model();
    if (!c.claude_model.empty()) c.subagent = core::subagent_name(c.tier);
  }
  return c;
}

std::string model_choice_line(const ModelChoice& c) {
  if (!c.defined) return fmt::format("model: tier {} ({}) has no [models.{}]; work here\n", c.tier, c.by, c.tier);
  if (c.claude_model.empty()) {
    return fmt::format("model: tier {} ({}) = {} ({}), not a Claude model; work here\n", c.tier, c.by, c.model.model,
                       c.model.provider);
  }
  return fmt::format("model: tier {} ({}) -> subagent {} ({}); a session on another model hands the {} work to it\n",
                     c.tier, c.by, c.subagent, c.claude_model, c.role);
}

std::string model_choice_text(const ModelChoice& c) {
  std::string out = fmt::format("block {}  level {}  role {}\ntier {}  ({})\n", c.block.empty() ? "(project)" : c.block,
                                c.level, c.role, c.tier, c.by);
  if (!c.defined) {
    out += fmt::format("model: tier {} has no [models.{}] in karkas.toml; work in this session or ask the user to add it\n",
                       c.tier, c.tier);
    return out;
  }
  out += fmt::format("model {}  ({}{})\n", c.model.model, c.model.provider,
                     c.model.base_url.empty() ? "" : " at " + c.model.base_url);
  if (c.claude_model.empty()) {
    out += "claude code: this model is not a Claude model, a Claude Code subagent cannot run it. Do the work in this "
           "session, or let the user run Karkas's own agents (`karkas implement` / `karkas task`).\n";
  } else {
    out += fmt::format("claude code: if this session runs {}, do the work here. Otherwise hand it to the subagent {} "
                       "(Agent tool, subagent_type {}) with the block, the task and its inv lines, then review its diff "
                       "against the card before you accept it. Missing subagent: `karkas gen agents`.\n",
                       c.claude_model, c.subagent, c.subagent);
  }
  return out;
}

}  // namespace kk::agents
