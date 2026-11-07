#pragma once
// Benchmark harness (17.7): runs a scenario with a method in fresh copies, then builds and
// runs the hidden acceptance tests; `report` aggregates medians and spreads.
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "kk/agents/types.hpp"
#include "kk/core/config.hpp"

namespace kk::agents {

struct BenchOptions {
  std::string scenario;  // s1 | s2 | s3
  std::string method;    // A | B | C
  std::string model;     // A/B: --model; C: informational (tiers come from the config)
  int reps = 3;
  std::filesystem::path bench_dir;  // bench/wavkit
  std::filesystem::path reference;  // reference project for S2/S3 (tests/fixtures/wavkit)
  std::filesystem::path results;    // bench/results
  std::filesystem::path work;       // where fresh copies go (default: system temp)
  std::filesystem::path karkas;     // path of the karkas executable (method B, MCP)
  ProviderFactory providers;        // method C; default: real clients
};

struct BenchResult {
  std::string scenario, method, model;
  int rep = 0;
  std::string date;
  double cost_usd = 0;
  std::int64_t input_tokens = 0, output_tokens = 0, cache_write_tokens = 0, cache_read_tokens = 0;
  int model_calls = 0;
  double seconds = 0;
  int interventions = 0;
  bool build_ok = false;
  int hidden_pass = 0, hidden_total = 0;
  std::map<std::string, double> cost_by_model;
  std::map<std::string, double> cost_by_role;  // method C: where the tokens go (17.8)
  std::vector<std::string> notes;
  std::string workdir;
};

std::string bench_result_json(const BenchResult& r);
kk::Result<BenchResult> parse_bench_result(std::string_view json_text);

// Runs `reps` repetitions; results are written as they finish.
kk::Result<std::vector<BenchResult>> bench_run(const BenchOptions& opts, const core::Config& cfg,
                                               const std::function<void(const std::string&)>& progress);

// Reads results/**/*.json, writes report.csv, report.txt, report.html; returns the text table.
kk::Result<std::string> bench_report(const std::filesystem::path& results_dir);

// Applies a bug patch of bench/wavkit/bugs (exact old → new text).
kk::VoidResult apply_bug(const std::filesystem::path& project, const std::filesystem::path& bug_json);

// Parses `claude -p --output-format json` output into the result fields (17.5).
void parse_claude_json(std::string_view text, BenchResult& r);

}  // namespace kk::agents
