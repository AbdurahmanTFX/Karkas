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

}  // namespace kk::agents
