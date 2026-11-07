#pragma once
// karkas.toml (appendix E). Search order: <project>/karkas.toml, then
// ~/.config/karkas/config.toml; values from the project file take precedence.
// The API key is never read from files: only ANTHROPIC_API_KEY (6.5).
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "kk/core/error.hpp"

namespace kk::core {

struct ModelTier {
  std::string provider = "anthropic";  // anthropic | openai_compat
  std::string model;
  std::string base_url;  // openai_compat; anthropic default https://api.anthropic.com
  std::string effort;    // output_config.effort ("" = model default)
  std::string fallbacks = "auto";  // "default" | "off" | "auto" (on where the model supports it)
};

struct Price {
  double input = 0.0;   // USD per 1M tokens
  double output = 0.0;  // USD per 1M tokens
  // Optional absolute overrides (USD per 1M tokens) when a model's cache prices are
  // not the standard multiples of the input price (e.g. cache reads at 0.05x).
  double cache_read = -1.0;
  double cache_write_5m = -1.0;
  double cache_write_1h = -1.0;
};

struct Config {
  // [project]
  std::string lang = "cpp";
  std::string build = "cmake-ninja";
  std::string build_dir = "build";
  // [models.*]
  std::map<std::string, ModelTier> models;  // strong | medium | cheap | local
  // [roles]: role → tier or "auto"
  std::map<std::string, std::string> roles = {{"architect", "strong"},
                                              {"orchestrator", "strong"},
                                              {"navigator", "medium"},
                                              {"worker", "auto"},
                                              {"tester", "cheap"}};
  // [routing]
  int cheap_max = 3;
  int medium_max = 6;
  int attempts_before_escalation = 2;
  bool self_tune = true;
  // [limits]
  int max_steps_worker = 25;
  int max_steps_navigator = 6;
  int max_steps_orchestrator = 8;
  int max_steps_architect = 30;
  int max_steps_tester = 25;
  long long max_tokens_per_run = 150000;
  int max_parallel_agents = 2;
  int max_parallel_builds = 1;
  int build_timeout_s = 300;
  int test_timeout_s = 120;
  int request_timeout_s = 300;
  int run_timeout_s = 1800;
  int max_output_tokens = 8192;
  // [cache]
  std::string cache_ttl = "5m";  // "5m" | "1h"
  // [prices]
  std::map<std::string, Price> prices;
  double cache_write_5m = 1.25;
  double cache_write_1h = 2.0;
  double cache_read = 0.1;
  // [gui]
  std::string editor_command = "xdg-open {file}";
  std::string font;
  // [bench]
  std::string bench_hidden_tests;  // directory outside the repository (17.6)
  std::string claude_command = "claude";
  double bench_max_budget_usd = 5.0;
  int bench_max_turns = 60;

  std::vector<std::string> sources;  // files that were loaded

  [[nodiscard]] const ModelTier* tier(std::string_view name) const;
};

// Parses TOML text into `cfg` (overriding the fields present).
VoidResult apply_config_text(Config& cfg, std::string_view toml_text, std::string_view file);
Result<Config> load_config(const std::filesystem::path& project_root);

}  // namespace kk::core
