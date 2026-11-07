#include "kk/agents/router.hpp"

#include <fmt/format.h>

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
