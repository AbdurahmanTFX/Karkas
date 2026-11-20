#pragma once
// Subprocesses of the target project (9.5, 8): cmake, ninja, ctest — with timeouts and a
// cleaned environment (API keys and other secrets are never passed on).
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string_view>
#include <string>
#include <vector>

#include "kk/core/error.hpp"

namespace kk::runner {

namespace fs = std::filesystem;

// Called with each piece of a child's output as it comes (stdout + stderr merged), from the
// calling thread.
using OutputSink = std::function<void(std::string_view)>;

struct RunOptions {
  std::vector<std::string> argv;
  fs::path cwd;
  std::chrono::milliseconds timeout{300'000};
  std::map<std::string, std::string> env;  // added on top of the cleaned environment
  bool clean_env = true;
  // With clean_env = false: variables removed from the inherited environment (e.g. an API key
  // that would take precedence over a subscription login).
  std::vector<std::string> unset_env;
  std::size_t max_output = 8u << 20;  // bytes kept (stdout + stderr merged)
  // The output as it comes; it is kept in RunResult as well. "Build and run" streams its build
  // (ADR 0037).
  OutputSink on_output;
};

struct RunResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string output;
  std::chrono::milliseconds duration{0};
  [[nodiscard]] bool ok() const { return !timed_out && exit_code == 0; }
};

kk::Result<RunResult> run(const RunOptions& opts);

// Environment for child processes: a whitelist of the parent's variables, without
// anything that looks like a secret, plus `extra`.
std::map<std::string, std::string> clean_environment(const std::map<std::string, std::string>& extra = {});
bool is_secret_name(std::string_view name);

// Only error lines, deduplicated, paths relative to `root`, at most `max_lines` (stage 5).
std::string compress_build_log(std::string_view log, const fs::path& root, std::size_t max_lines = 40);
// Only compiler warning lines ("file:line:col: warning: ..."), deduplicated, paths relative to `root`.
std::string compress_build_warnings(std::string_view log, const fs::path& root, std::size_t max_lines = 40);

// --- build and test -------------------------------------------------------------------------
struct BuildOptions {
  fs::path source;                 // target project root
  fs::path build_dir;              // default <source>/build
  std::string doctest_include;     // passed as -DKARKAS_DOCTEST_DIR
  std::string target;              // "" = all
  std::chrono::seconds timeout{300};
  std::vector<std::string> cmake_args;
  OutputSink on_output;            // the output of configure and build as it comes
};

struct BuildResult {
  bool ok = false;
  bool timed_out = false;
  std::string summary;   // compressed errors
  std::string warnings;  // compressed compiler warnings (of the files compiled in this build)
  std::string log;       // full output
  std::chrono::milliseconds duration{0};
};

kk::Result<BuildResult> build(const BuildOptions& opts);
// A project's own build command (`[project] build_command`, ADR 0027), run by `sh -c` from `cwd`:
// nothing is configured by Karkas.
kk::Result<BuildResult> build_command(const std::string& command, const fs::path& cwd, std::chrono::seconds timeout,
                                      const OutputSink& on_output = {});

struct Counts {
  int pass = 0;
  int total = 0;
};

struct BlockTests {
  std::string block;
  Counts contract, unit, integration;
  bool build_failed = false;
  std::vector<std::string> failures;  // "contract: <test case> (file:line)"
};

struct TestOptions {
  fs::path source;
  fs::path build_dir;
  std::vector<std::string> blocks;  // labels; empty = all tests
  std::chrono::seconds timeout{120};
  fs::path probe_dir;               // KARKAS_PROBE_DIR for the tests
  std::string probe_run;            // KARKAS_PROBE_RUN
};

struct TestSummary {
  bool ok = false;
  std::vector<BlockTests> blocks;
  std::string text;  // compact summary for models and humans
  std::string log;
};

kk::Result<TestSummary> test(const TestOptions& opts);
// Parses `ctest -V` output (exposed for tests).
std::vector<BlockTests> parse_ctest_output(std::string_view out, const fs::path& root);
std::string summarize(const std::vector<BlockTests>& blocks);

}  // namespace kk::runner
