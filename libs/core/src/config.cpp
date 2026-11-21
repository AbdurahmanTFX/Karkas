#include "kk/core/config.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <functional>
#include <toml++/toml.hpp>

#include "kk/core/paths.hpp"
#include "kk/core/platform.hpp"
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

namespace {
bool leaf_role(std::string_view role) {
  return role == "worker" || role == "tester" || role == "integration_tester" || role == "composer";
}
}  // namespace

Pin Config::pin(std::string_view block, std::string_view role) const {
  if (block == "root") block = "";
  // The longest [blocks] path that is the block itself or one of its ancestors.
  std::string best;
  for (const auto& [path, t] : block_tiers) {
    bool covers = block == path || (block.starts_with(path) && block.size() > path.size() && block[path.size()] == '.');
    if (covers && path.size() >= best.size() && !t.empty()) best = path;
  }
  if (!best.empty()) return {block_tiers.at(best), "block " + best};
  if (leaf_role(role) && !levels_for_leaves) return {};
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

// [roles] [levels] [blocks] and [routing] escalate of `t` into `out` (`prefix` names the
// sections in messages: "" or "mcp.").
VoidResult parse_maps(const toml::table& t, ModelMaps& out, std::string_view prefix, std::string_view file) {
  if (const auto* roles = t.get_as<toml::table>("roles")) {
    for (const auto& [name, node] : *roles) {
      if (auto v = node.value<std::string>()) out.roles[std::string(name.str())] = *v;
    }
  }
  if (const auto* lv = t.get_as<toml::table>("levels")) {
    for (const auto& [name, node] : *lv) {
      std::string key(name.str());
      int n = -1;
      auto [p, ec] = std::from_chars(key.data(), key.data() + key.size(), n);
      if (ec != std::errc() || p != key.data() + key.size() || n < 0 || n > 16) {
        return fail("config", fmt::format("[{}levels] keys are tree levels 0..16, got \"{}\"", prefix, key), SourceLoc{std::string(file), 0, 0});
      }
      if (auto v = node.value<std::string>()) {
        if (v->empty()) out.levels.erase(n);
        else out.levels[n] = *v;
      }
    }
  }
  if (const auto* bl = t.get_as<toml::table>("blocks")) {
    for (const auto& [name, node] : *bl) {
      std::string key(name.str());
      if (!is_block_path(key)) {
        return fail("config", fmt::format("[{}blocks] keys are block paths, got \"{}\"", prefix, key), SourceLoc{std::string(file), 0, 0});
      }
      if (auto v = node.value<std::string>()) {
        if (v->empty()) out.block_tiers.erase(key);
        else out.block_tiers[key] = *v;
      }
    }
  }
  if (const auto* rt = t.get_as<toml::table>("routing")) {
    if (const auto* arr = rt->get_as<toml::array>("escalate")) {
      out.escalate.clear();
      for (const auto& n : *arr) {
        if (auto v = n.value<std::string>()) out.escalate.push_back(*v);
      }
    }
  }
  return {};
}

}  // namespace

void activate_mode(Config& cfg) {
  const ModelMaps& m = cfg.mcp() ? cfg.mcp_maps : cfg.api_maps;
  cfg.roles = m.roles;
  cfg.levels = m.levels;
  cfg.block_tiers = m.block_tiers;
  cfg.escalate = m.escalate;
}

std::string Config::map_section(std::string_view map) const { return (mcp() ? "mcp." : "") + std::string(map); }

ModelMaps mcp_maps_from_api(const Config& cfg) {
  // Claude tiers, weakest first: the standard names, then any other Anthropic tier.
  std::vector<std::string> claude;
  for (const char* n : {"cheap", "medium", "strong"}) {
    if (const auto* t = cfg.tier(n); t != nullptr && !t->claude_model().empty()) claude.emplace_back(n);
  }
  for (const auto& [name, t] : cfg.models) {
    if (!t.claude_model().empty() && std::find(claude.begin(), claude.end(), name) == claude.end()) claude.push_back(name);
  }
  ModelMaps out;
  if (claude.empty()) return out;
  std::vector<std::string> chain = cfg.api_maps.escalate;
  if (chain.empty()) {
    for (const char* n : {"local", "cheap", "medium", "strong"}) {
      if (cfg.tier(n) != nullptr) chain.emplace_back(n);
    }
  }
  auto fit = [&](const std::string& tier) -> std::string {
    if (tier.empty() || tier == "auto") return tier;
    if (const auto* t = cfg.tier(tier); t != nullptr && !t->claude_model().empty()) return tier;
    auto it = std::find(chain.begin(), chain.end(), tier);
    if (it == chain.end()) return claude.size() > 1 ? claude[claude.size() / 2] : claude.front();
    auto i = static_cast<double>(it - chain.begin());
    auto n = static_cast<double>(chain.size());
    auto k = static_cast<double>(claude.size());
    auto pos = n <= 1 ? k - 1 : std::round(i * (k - 1) / (n - 1));
    return claude[static_cast<std::size_t>(pos)];
  };
  for (const auto& [role, tier] : cfg.api_maps.roles) out.roles[role] = fit(tier);
  for (const auto& [level, tier] : cfg.api_maps.levels) out.levels[level] = fit(tier);
  for (const auto& [block, tier] : cfg.api_maps.block_tiers) out.block_tiers[block] = fit(tier);
  out.escalate = claude;
  return out;
}


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
  get(t, "project", "build_command", cfg.build_command);
  get(t, "project", "verify_command", cfg.verify_command);
  get(t, "project", "compile_commands", cfg.compile_commands);
  get(t, "project", "run_command", cfg.run_command);
  get(t, "test", "command", cfg.test_command);
  get(t, "test", "dir", cfg.test_dir);
  get(t, "test", "guide", cfg.test_guide);

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
  // The model maps: [roles] [levels] [blocks] [routing] escalate for the "api" mode, the same
  // under [mcp.*] for the "mcp" mode (ADR 0024).
  if (auto r = parse_maps(t, cfg.api_maps, "", file); !r) return r;
  if (const auto* mcp = t.get_as<toml::table>("mcp")) {
    if (auto r = parse_maps(*mcp, cfg.mcp_maps, "mcp.", file); !r) return r;
  }
  get(t, "routing", "cheap_max", cfg.cheap_max);
  get(t, "routing", "medium_max", cfg.medium_max);
  get(t, "routing", "attempts_before_escalation", cfg.attempts_before_escalation);
  get(t, "routing", "self_tune", cfg.self_tune);
  {
    std::string levels_for;
    get(t, "routing", "levels_for", levels_for);
    if (!levels_for.empty() && levels_for != "decisions" && levels_for != "all") {
      return fail("config", "[routing] levels_for must be \"decisions\" or \"all\"", SourceLoc{std::string(file), 0, 0});
    }
    if (!levels_for.empty()) cfg.levels_for_leaves = levels_for == "all";
  }
  if (const auto* ef = t.get_as<toml::table>("effort")) {
    for (const auto& [name, node] : *ef) {
      if (auto v = node.value<std::string>()) cfg.role_effort[std::string(name.str())] = *v;
    }
  }

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
  get(t, "limits", "max_output_tokens_routing", cfg.max_output_tokens_routing);

  get(t, "cache", "ttl", cfg.cache_ttl);
  get(t, "agents", "auto_approve", cfg.auto_approve);
  get(t, "agents", "mode", cfg.agents_mode);
  get(t, "agents", "mcp_wait_s", cfg.mcp_wait_s);
  get(t, "agents", "launch", cfg.mcp_launch);
  get(t, "agents", "launch_command", cfg.mcp_launch_command);
  get(t, "agents", "launch_budget_usd", cfg.mcp_launch_budget_usd);
  if (cfg.agents_mode != "api" && cfg.agents_mode != "mcp") {
    return fail("config", fmt::format("[agents] mode must be \"api\" or \"mcp\", got \"{}\"", cfg.agents_mode),
                SourceLoc{std::string(file), 0, 0});
  }
  // The GUI writes string values ("true"): accept them as well.
  if (auto v = t["agents"]["auto_approve"].value<std::string>()) cfg.auto_approve = *v == "true" || *v == "on" || *v == "1";
  get(t, "agents", "review_changes", cfg.review_changes);
  get(t, "agents", "done_needs_change", cfg.done_needs_change);
  if (auto v = t["agents"]["review_changes"].value<std::string>()) cfg.review_changes = *v == "true" || *v == "on" || *v == "1";
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
  activate_mode(cfg);
  return {};
}

Result<Config> load_config(const std::filesystem::path& project_root) {
  Config cfg;
  std::vector<std::filesystem::path> files;
  std::filesystem::path user_dir;
#ifdef _WIN32
  user_dir = config_home();  // %APPDATA% (docs/adr/0046)
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
    user_dir = xdg;
  } else if (const char* home = std::getenv("HOME"); home != nullptr) {
    user_dir = std::filesystem::path(home) / ".config";
  }
#endif
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

Result<std::string> write_mcp_maps(std::string_view text, const ModelMaps& maps) {
  std::string out(text);
  for (const char* sec : {"mcp.roles", "mcp.levels", "mcp.blocks", "mcp.routing"}) out = remove_config_section(out, sec);
  while (out.size() >= 2 && out.ends_with("\n\n")) out.pop_back();
  auto key = [](std::string_view k) { return bare_key(k) ? std::string(k) : toml_string(k); };
  out += "\n# Models of mode \"mcp\" (ADR 0024): Claude Code subagents karkas-<tier>.\n[mcp.roles]\n";
  for (const auto& [role, tier] : maps.roles) out += key(role) + " = " + toml_string(tier) + "\n";
  if (!maps.levels.empty()) {
    out += "\n[mcp.levels]\n";
    for (const auto& [level, tier] : maps.levels) out += std::to_string(level) + " = " + toml_string(tier) + "\n";
  }
  if (!maps.block_tiers.empty()) {
    out += "\n[mcp.blocks]\n";
    for (const auto& [block, tier] : maps.block_tiers) out += toml_string(block) + " = " + toml_string(tier) + "\n";
  }
  if (!maps.escalate.empty()) {
    std::vector<std::string> q;
    for (const auto& t : maps.escalate) q.push_back(toml_string(t));
    out += "\n[mcp.routing]\nescalate = [" + join(q, ", ") + "]\n";
  }
  Config probe;
  if (auto r = apply_config_text(probe, out, "karkas.toml"); !r) return std::unexpected(r.error());
  return out;
}

}  // namespace kk::core
