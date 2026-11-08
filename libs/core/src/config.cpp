#include "kk/core/config.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <functional>
#include <toml++/toml.hpp>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::core {

const ModelTier* Config::tier(std::string_view name) const {
  auto it = models.find(std::string(name));
  return it == models.end() ? nullptr : &it->second;
}

std::string ModelTier::key_env() const {
  if (!api_key_env.empty()) return api_key_env;
  return provider == "openai_compat" ? "OPENAI_API_KEY" : "ANTHROPIC_API_KEY";
}

std::string ModelTier::claude_model() const { return provider == "anthropic" ? model : std::string(); }

std::string subagent_name(std::string_view tier) {
  std::string out = "karkas-";
  for (char c : tier) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '-';
  }
  return out;
}

int block_level(std::string_view block) {
  if (block.empty() || block == "root") return 0;
  return 1 + static_cast<int>(std::count(block.begin(), block.end(), '.'));
}

Pin Config::pin(std::string_view block) const {
  if (block == "root") block = "";
  // The longest [blocks] path that is the block itself or one of its ancestors.
  std::string best;
  for (const auto& [path, t] : block_tiers) {
    bool covers = block == path || (block.starts_with(path) && block.size() > path.size() && block[path.size()] == '.');
    if (covers && path.size() >= best.size() && !t.empty()) best = path;
  }
  if (!best.empty()) return {block_tiers.at(best), "block " + best};
  int lv = block_level(block);
  if (auto it = levels.find(lv); it != levels.end() && !it->second.empty()) return {it->second, fmt::format("level {}", lv)};
  return {};
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
  get(t, "project", "cmake_args", cfg.cmake_args);
  get(t, "test", "command", cfg.test_command);

  if (const auto* models = t.get_as<toml::table>("models")) {
    for (const auto& [name, node] : *models) {
      const auto* m = node.as_table();
      if (m == nullptr) continue;
      ModelTier& tier = cfg.models[std::string(name.str())];
      if (auto v = (*m)["provider"].value<std::string>()) tier.provider = *v;
      if (auto v = (*m)["model"].value<std::string>()) tier.model = *v;
      if (auto v = (*m)["base_url"].value<std::string>()) tier.base_url = *v;
      if (auto v = (*m)["effort"].value<std::string>()) tier.effort = *v;
      if (auto v = (*m)["fallbacks"].value<std::string>()) tier.fallbacks = *v;
      if (auto v = (*m)["api_key_env"].value<std::string>()) tier.api_key_env = *v;
    }
  }
  if (const auto* roles = t.get_as<toml::table>("roles")) {
    for (const auto& [name, node] : *roles) {
      if (auto v = node.value<std::string>()) cfg.roles[std::string(name.str())] = *v;
    }
  }
  if (const auto* lv = t.get_as<toml::table>("levels")) {
    for (const auto& [name, node] : *lv) {
      std::string key(name.str());
      int n = -1;
      auto [p, ec] = std::from_chars(key.data(), key.data() + key.size(), n);
      if (ec != std::errc() || p != key.data() + key.size() || n < 0 || n > 16) {
        return fail("config", fmt::format("[levels] keys are tree levels 0..16, got \"{}\"", key), SourceLoc{std::string(file), 0, 0});
      }
      if (auto v = node.value<std::string>()) {
        if (v->empty()) cfg.levels.erase(n);
        else cfg.levels[n] = *v;
      }
    }
  }
  if (const auto* bl = t.get_as<toml::table>("blocks")) {
    for (const auto& [name, node] : *bl) {
      std::string key(name.str());
      if (!is_block_path(key)) {
        return fail("config", fmt::format("[blocks] keys are block paths, got \"{}\"", key), SourceLoc{std::string(file), 0, 0});
      }
      if (auto v = node.value<std::string>()) {
        if (v->empty()) cfg.block_tiers.erase(key);
        else cfg.block_tiers[key] = *v;
      }
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
        if (auto v = (*p)["cache_read"].value<double>()) price.cache_read = *v;
        if (auto v = (*p)["cache_write_5m"].value<double>()) price.cache_write_5m = *v;
        if (auto v = (*p)["cache_write_1h"].value<double>()) price.cache_write_1h = *v;
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

// --- editing karkas.toml -----------------------------------------------------------------

namespace {

std::vector<std::string> split_lines(std::string_view text) {
  auto lines = split(text, '\n', false);
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  for (auto& l : lines) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
  }
  return lines;
}

bool bare_key(std::string_view k) {
  return !k.empty() && std::all_of(k.begin(), k.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

std::string toml_string(std::string_view v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out + "\"";
}

// "[name]" (comments and spaces allowed) → name; "" when the line is not a table header.
std::string header_of(std::string_view line) {
  auto t = trim(line);
  if (t.starts_with("[[") || !t.starts_with("[")) return "";
  auto close = t.find(']');
  if (close == std::string_view::npos) return "";
  std::string name;
  for (char c : t.substr(1, close - 1)) {
    if (c != ' ' && c != '\t') name += c;
  }
  return name;
}

// Key of a "key = value" line (bare or quoted); "" otherwise.
std::string key_of(std::string_view line) {
  auto t = trim(line);
  if (t.empty() || t.starts_with("#")) return "";
  if (t.starts_with("\"")) {
    auto end = t.find('"', 1);
    return end == std::string_view::npos ? "" : std::string(t.substr(1, end - 1));
  }
  auto eq = t.find('=');
  return eq == std::string_view::npos ? "" : std::string(trim(t.substr(0, eq)));
}

}  // namespace

Result<std::string> set_config_value(std::string_view text, std::string_view section, std::string_view key,
                                     const std::optional<std::string>& value) {
  auto lines = split_lines(text);
  std::string line = value ? fmt::format("{} = {}", bare_key(key) ? std::string(key) : toml_string(key), toml_string(*value)) : "";
  std::size_t start = lines.size();  // first line after the header
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (header_of(lines[i]) == section) {
      start = i + 1;
      break;
    }
  }
  if (start == lines.size() && (lines.empty() || header_of(lines.back()) != section)) {
    if (value) {
      if (!lines.empty() && !trim(lines.back()).empty()) lines.emplace_back("");
      lines.push_back(fmt::format("[{}]", section));
      lines.push_back(line);
    }
  } else {
    std::size_t end = start;
    while (end < lines.size() && header_of(lines[end]).empty()) ++end;
    std::size_t found = end;
    std::size_t last = start;  // after the last non-empty line of the section
    for (std::size_t i = start; i < end; ++i) {
      if (key_of(lines[i]) == key) found = i;
      if (!trim(lines[i]).empty()) last = i + 1;
    }
    if (found < end) {
      if (value) lines[found] = line;
      else lines.erase(lines.begin() + static_cast<long>(found));
    } else if (value) {
      lines.insert(lines.begin() + static_cast<long>(last), line);
    }
  }
  std::string out;
  for (const auto& l : lines) out += l + "\n";
  Config probe;
  if (auto r = apply_config_text(probe, out, "karkas.toml"); !r) return std::unexpected(r.error());
  return out;
}

std::string remove_config_section(std::string_view text, std::string_view section) {
  auto lines = split_lines(text);
  std::string out;
  bool skip = false;
  for (const auto& l : lines) {
    auto h = header_of(l);
    if (!h.empty()) skip = h == section;
    if (!skip) out += l + "\n";
  }
  return out;
}

namespace {

VoidResult rewrite(const std::filesystem::path& root, const std::function<Result<std::string>(const std::string&)>& f) {
  auto path = root / "karkas.toml";
  std::string text;
  if (std::filesystem::exists(path)) {
    auto t = read_file(path);
    if (!t) return std::unexpected(t.error());
    text = *t;
  }
  auto out = f(text);
  if (!out) return std::unexpected(out.error());
  return write_file(path, *out);
}

}  // namespace

VoidResult write_config_value(const std::filesystem::path& project_root, std::string_view section, std::string_view key,
                              const std::optional<std::string>& value) {
  return rewrite(project_root, [&](const std::string& text) { return set_config_value(text, section, key, value); });
}

VoidResult erase_config_section(const std::filesystem::path& project_root, std::string_view section) {
  return rewrite(project_root, [&](const std::string& text) -> Result<std::string> { return remove_config_section(text, section); });
}

}  // namespace kk::core
