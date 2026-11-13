#pragma once
// Token and cost accounting (16): run journals in .karkas/runs/*.jsonl, money from the
// [prices] table, reports by block / role / model / ticket.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/config.hpp"
#include "kk/index/index.hpp"

namespace kk::metrics {

namespace fs = std::filesystem;

struct Tokens {
  std::int64_t input = 0;
  std::int64_t output = 0;
  std::int64_t cache_write_5m = 0;
  std::int64_t cache_write_1h = 0;
  std::int64_t cache_read = 0;
  Tokens& operator+=(const Tokens& o);
  [[nodiscard]] std::int64_t cache_write() const { return cache_write_5m + cache_write_1h; }
};

// Local models (openai_compat) cost 0 unless priced; a cloud model without a price
// is an error, never zero (16.2).
bool is_local_provider(std::string_view provider);
kk::VoidResult check_price(const core::Config& cfg, std::string_view provider, std::string_view model);
//   cost = (input*in + w5m*in*1.25 + w1h*in*2 + read*in*0.1 + output*out) / 1e6
// with multipliers from [prices] and optional per-model absolute cache prices.
kk::Result<double> cost_usd(const core::Config& cfg, std::string_view provider, std::string_view model,
                            const Tokens& t);

struct RunInfo {
  std::string ticket, role, block, provider, model, tier;
  int attempt = 1;
};

struct ActiveRun {
  std::string role, block, model, ticket;
  std::int64_t started = 0;
  fs::path journal;
  int pid = 0;            // process running the agent (0: an old marker without it)
  bool stopping = false;  // a stop was requested and the agent has not ended yet
};

// One file per agent run: .karkas/runs/<time>-<role>-<block>.jsonl (9.4). While the
// run is active a marker in .karkas/runs/active/ lets the GUI show "agent working".
class RunJournal {
 public:
  static kk::Result<RunJournal> open(const fs::path& project_root, RunInfo info);
  RunJournal(RunJournal&&) noexcept = default;
  RunJournal& operator=(RunJournal&&) noexcept = default;
  ~RunJournal();

  // Appends {"type":"step","n":...,"kind":kind,"data":<payload JSON>}.
  kk::VoidResult step(std::string_view kind, std::string_view payload_json);
  // Writes the run_end line, removes the active marker and returns the runs row
  // (also inserted into the index when `idx` is given).
  kk::Result<index::RunRow> finish(const Tokens& t, double cost, std::int64_t duration_ms, int steps,
                                   std::string_view outcome, index::Index* idx = nullptr);
  [[nodiscard]] const fs::path& path() const { return path_; }
  // The GUI asked this run to stop (request_stop); the agent loop ends with "cancelled".
  [[nodiscard]] bool stop_requested() const;

 private:
  RunJournal() = default;
  fs::path path_, marker_, stop_;
  RunInfo info_;
  int n_ = 0;
  bool finished_ = false;
};

std::vector<ActiveRun> active_runs(const fs::path& project_root);

// Asks one active run to stop (ADR 0019): writes .karkas/runs/active/<journal stem>.stop,
// which the agent loop of that run checks between steps and while waiting for the model.
// `journal` is the journal path or its file name. Fails when the run is not active.
kk::VoidResult request_stop(const fs::path& project_root, const fs::path& journal);

// "YYYY-MM-DD" or a unix timestamp → unix seconds (UTC).
kk::Result<std::int64_t> parse_since(std::string_view s);

// karkas stats (ADR 0026). Without a ticket: per role and tier, runs, how many ended well (done,
// fixed, a decision, an answer), mean steps, cost and escalations — the data to route by. With a
// ticket: its runs in order, totals, and in mode mcp the traffic of its turns (whole turns, deltas,
// turns made shorter by `seen`) from <project>/.karkas/mcp/traffic.
std::string stats_report(const std::vector<index::RunRow>& runs, const std::string& ticket,
                         const std::filesystem::path& project_root);

// karkas cost [--by block|role|model|ticket] (16.3). Fails when a cloud model in the
// runs has no price in the config.
kk::Result<std::string> cost_report(const core::Config& cfg, const std::vector<index::RunRow>& runs,
                                    std::string_view by);

}  // namespace kk::metrics
