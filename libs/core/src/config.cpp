#include "kk/core/config.hpp"

#include <fmt/format.h>

#include <cstdlib>
#include <toml++/toml.hpp>

#include "kk/core/paths.hpp"

namespace kk::core {

const ModelTier* Config::tier(std::string_view name) const {
  auto it = models.find(std::string(name));
  return it == models.end() ? nullptr : &it->second;
}

namespace {

template <class T>
void get(const toml::table& t, std::string_view section, std::string_view key, T& out) {
  const auto* sec = t.get_as<toml::table>(section);
  if (sec == nullptr) return;
  if constexpr (std::is_same_v<T, std::string>) {
    if (auto v = (*sec)[key].value<std::string>()) out = *v;
  } else if constexpr (std::is_same_v<T, bool>) {
    if (auto v = (*sec)[key].value<bool>()) out = *v;
  } else if constexpr (std::is_floating_point_v<T>) {
    if (auto v = (*sec)[key].value<double>()) out = static_cast<T>(*v);
  } else {
    if (auto v = (*sec)[key].value<long long>()) out = static_cast<T>(*v);
  }
}

}  // namespace

VoidResult apply_config_text(Config& cfg, std::string_view text, std::string_view file) {
  toml::table t;
  try {
    t = toml::parse(text, file);
  } catch (const toml::parse_error& e) {
    return fail("config", std::string(e.description()),
                SourceLoc{std::string(file), static_cast<int>(e.source().begin.line),
                          static_cast<int>(e.source().begin.column)});
  }
  get(t, "project", "lang", cfg.lang);
  get(t, "project", "build", cfg.build);
  get(t, "project", "build_dir", cfg.build_dir);

  if (const auto* models = t.get_as<toml::table>("models")) {
    for (const auto& [name, node] : *models) {
      const auto* m = node.as_table();
      if (m == nullptr) continue;
      ModelTier& tier = cfg.models[std::string(name.str())];
      if (auto v = (*m)["provider"].value<std::string>()) tier.provider = *v;
      if (auto v = (*m)["model"].value<std::string>()) tier.model = *v;
      if (auto v = (*m)["base_url"].value<std::string>()) tier.base_url = *v;
    }
  }
  if (const auto* roles = t.get_as<toml::table>("roles")) {
    for (const auto& [name, node] : *roles) {
      if (auto v = node.value<std::string>()) cfg.roles[std::string(name.str())] = *v;
    }
  }
  get(t, "routing", "cheap_max", cfg.cheap_max);
  get(t, "routing", "medium_max", cfg.medium_max);
  get(t, "routing", "attempts_before_escalation", cfg.attempts_before_escalation);
  get(t, "routing", "self_tune", cfg.self_tune);

  get(t, "limits", "max_steps_worker", cfg.max_steps_worker);
  get(t, "limits", "max_steps_navigator", cfg.max_steps_navigator);
  get(t, "limits", "max_steps_orchestrator", cfg.max_steps_orchestrator);
  get(t, "limits", "max_steps_architect", cfg.max_steps_architect);
  get(t, "limits", "max_steps_tester", cfg.max_steps_tester);
  get(t, "limits", "max_tokens_per_run", cfg.max_tokens_per_run);
  get(t, "limits", "max_parallel_agents", cfg.max_parallel_agents);
  get(t, "limits", "max_parallel_builds", cfg.max_parallel_builds);
  get(t, "limits", "build_timeout_s", cfg.build_timeout_s);
  get(t, "limits", "test_timeout_s", cfg.test_timeout_s);
  get(t, "limits", "request_timeout_s", cfg.request_timeout_s);
  get(t, "limits", "run_timeout_s", cfg.run_timeout_s);
  get(t, "limits", "max_output_tokens", cfg.max_output_tokens);

  get(t, "cache", "ttl", cfg.cache_ttl);
  if (cfg.cache_ttl != "5m" && cfg.cache_ttl != "1h") {
    return fail("config", fmt::format("[cache] ttl must be \"5m\" or \"1h\", got \"{}\"", cfg.cache_ttl),
                SourceLoc{std::string(file), 0, 0});
  }

  if (const auto* prices = t.get_as<toml::table>("prices")) {
    for (const auto& [name, node] : *prices) {
      std::string key(name.str());
      if (const auto* p = node.as_table()) {
        Price& price = cfg.prices[key];
        if (auto v = (*p)["input"].value<double>()) price.input = *v;
        if (auto v = (*p)["output"].value<double>()) price.output = *v;
      } else if (auto v = node.value<double>()) {
        if (key == "cache_write_5m") cfg.cache_write_5m = *v;
        if (key == "cache_write_1h") cfg.cache_write_1h = *v;
        if (key == "cache_read") cfg.cache_read = *v;
      }
    }
  }
  get(t, "gui", "editor_command", cfg.editor_command);
  get(t, "gui", "font", cfg.font);
  get(t, "bench", "hidden_tests", cfg.bench_hidden_tests);
  get(t, "bench", "claude_command", cfg.claude_command);
  get(t, "bench", "max_budget_usd", cfg.bench_max_budget_usd);
  get(t, "bench", "max_turns", cfg.bench_max_turns);
  return {};
}

Result<Config> load_config(const std::filesystem::path& project_root) {
  Config cfg;
  std::vector<std::filesystem::path> files;
  std::filesystem::path user_dir;
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
    user_dir = xdg;
  } else if (const char* home = std::getenv("HOME"); home != nullptr) {
    user_dir = std::filesystem::path(home) / ".config";
  }
  // Lower precedence first: the project file overrides the user file.
  if (!user_dir.empty()) files.push_back(user_dir / "karkas" / "config.toml");
  if (!project_root.empty()) files.push_back(project_root / "karkas.toml");
  for (const auto& f : files) {
    if (!std::filesystem::exists(f)) continue;
    auto text = read_file(f);
    if (!text) return std::unexpected(text.error());
    if (auto r = apply_config_text(cfg, *text, f.string()); !r) return std::unexpected(r.error());
    cfg.sources.push_back(f.string());
  }
  return cfg;
}

}  // namespace kk::core
