#pragma once
// karkas.toml (appendix E). Search order: <project>/karkas.toml, then
// ~/.config/karkas/config.toml; values from the project file take precedence.
// The API key is never read from files: only ANTHROPIC_API_KEY (6.5).
#include <filesystem>
#include <map>
#include <optional>
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
  // Environment variable holding this API's key ("" = ANTHROPIC_API_KEY or OPENAI_API_KEY by
  // provider). Only the variable's name is stored, never the key (ADR 0011).
  std::string api_key_env;
  [[nodiscard]] std::string key_env() const;
};

// A model pinned for a part of the block tree (ADR 0011).
struct Pin {
  std::string tier;  // "" = nothing pinned: [roles] and routing decide
  std::string by;    // "block game", "level 2" (for the journal and the GUI)
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
  // [levels]: tier per level of the block tree (0 = the project: orchestrator, architect of
  // the top level; 1 = top-level blocks; 2 = their children; ...).
  std::map<int, std::string> levels;
  // [blocks]: tier per block, for the block and everything inside it (the longest path wins).
  std::map<std::string, std::string> block_tiers;
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
  // The model pinned for agents working on `block` ("" or "root" = the whole project):
  // [blocks] (longest prefix) first, then [levels] by the depth of the block.
  [[nodiscard]] Pin pin(std::string_view block) const;
};

// Level of a block in the tree: number of path segments ("" / "root" = 0, "game.flow" = 2).
int block_level(std::string_view block);

// Sets (value) or erases (nullopt) one string key of karkas.toml text, keeping everything
// else (comments, order). Creates the section when missing. Keys that are not bare TOML keys
// (block paths) are quoted. Fails if the result is not valid configuration.
Result<std::string> set_config_value(std::string_view text, std::string_view section, std::string_view key,
                                     const std::optional<std::string>& value);
// Removes a whole section ("models.local") from karkas.toml text.
std::string remove_config_section(std::string_view text, std::string_view section);
// Applies set_config_value to <project>/karkas.toml (created when missing).
VoidResult write_config_value(const std::filesystem::path& project_root, std::string_view section, std::string_view key,
                              const std::optional<std::string>& value);
VoidResult erase_config_section(const std::filesystem::path& project_root, std::string_view section);

// Parses TOML text into `cfg` (overriding the fields present).
VoidResult apply_config_text(Config& cfg, std::string_view toml_text, std::string_view file);
Result<Config> load_config(const std::filesystem::path& project_root);

}  // namespace kk::core
