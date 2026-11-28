#include "kk/agents/engine.hpp"

#include "kk/agents/api_edit.hpp"
#include "kk/agents/build_files.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <deque>
#include <future>
#include <map>
#include <regex>
#include <nlohmann/json.hpp>
#include <semaphore>
#include <set>

#include "kk/agents/changes.hpp"
#include "kk/agents/launcher.hpp"
#include "kk/agents/notes_agent.hpp"
#include "kk/agents/sandbox.hpp"
#include "kk/agents/tools.hpp"
#include "kk/code/analyzer.hpp"
#include "kk/code/gen.hpp"
#include "kk/core/cancel.hpp"
#include "kk/core/notes.hpp"
#include "kk/core/lang.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/index/state.hpp"
#include "kk/llm/mcp_bridge.hpp"
#include "kk/llm/provider.hpp"
#include "kk/templates.hpp"

namespace kk::agents {

using json = nlohmann::json;

namespace {

// The fns a change names in a note of a block ("fn ships_destroyed() -> int64_t") without declaring
// them: no "+ fn ships_destroyed(" line for that block and none in its card (ADR 0029). One line per gap,
// for the orchestrator that amends the change.
std::vector<std::string> fns_only_in_notes(const core::Change& c, const core::Schema& s) {
  static const std::regex named(R"(\bfn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
  static const std::regex declared(R"(^fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
  std::map<std::string, std::set<std::string>> fns;                 // block -> fns the change declares
  std::map<std::string, std::vector<std::string>> in_notes;         // block -> fns its notes name
  std::string at;
  for (const auto& d : c.diff) {
    if (d.text.starts_with("@block ")) {
      at = std::string(core::trim(std::string_view(d.text).substr(7)));
      continue;
    }
    if (at.empty() || d.op != '+') continue;
    std::smatch m;
    if (std::regex_search(d.text, m, declared)) fns[at].insert(m[1].str());
    if (d.text.starts_with("note")) {
      for (auto it = std::sregex_iterator(d.text.begin(), d.text.end(), named); it != std::sregex_iterator(); ++it) {
        in_notes[at].push_back((*it)[1].str());
      }
    }
  }
  std::vector<std::string> gaps;
  for (const auto& [block, names] : in_notes) {
    const auto* b = s.find(block);
    for (const auto& n : names) {
      if (fns[block].contains(n) || (b != nullptr && b->port(n) != nullptr)) continue;
      gaps.push_back(fmt::format("{}: a note names fn {}() but the change declares no \"+ fn {}(...)\" line for it; add it, and a "
                                 "\"+ call {}:{}\" line to every block that will call it",
                                 block, n, n, block, n));
    }
  }
  return gaps;
}

std::string sget(const json& j, const char* k) {
  if (!j.contains(k) || j[k].is_null()) return {};
  if (j[k].is_string()) return j[k].get<std::string>();
  if (j[k].is_array()) {
    std::vector<std::string> parts;
    for (const auto& x : j[k]) parts.push_back(x.is_string() ? x.get<std::string>() : x.dump());
    return core::join(parts, ", ");
  }
  return j[k].dump();
}

std::vector<std::string> sarr(const json& j, const char* k) {
  std::vector<std::string> out;
  if (j.contains(k) && j[k].is_array()) {
    for (const auto& x : j[k]) {
      if (x.is_string()) out.push_back(x.get<std::string>());
    }
  }
  return out;
}

std::string shorten(std::string s, std::size_t n) {
  s = core::replace_all(std::move(s), "\n", " ");
  return s.size() <= n ? s : core::truncate_utf8(s, n - 3) + "...";
}

// The block whose listed file the first error line of a build names ("source/X.cpp:3:1: error: ..."), or ""
// (an imported project: the code lives in the files of the cards; ADR 0028).
// A linker error names no file ("<artificial>:(.text+0x9): undefined reference to `Panel::Panel(Player&)'"): its
// symbol's class or function is looked up as the stem of a block's file (source/Panel.cpp), as an imported
// project names them (docs/adr/0030; Endless Sky's round 3 escalated on such a line).
std::string block_of_build_error(const core::Schema& s, const std::string& summary) {
  std::map<std::string, std::string> owner;
  std::map<std::string, std::string> by_stem;
  for (const auto& b : s.blocks) {
    for (const auto& f : b.file_list()) {
      owner[f] = b.id;
      if (core::is_code_file(fs::path(f))) by_stem.emplace(core::to_lower(fs::path(f).stem().string()), b.id);
    }
  }
  for (const auto& line : core::split(summary, '\n')) {
    if (!core::contains(line, "error") && !core::contains(line, "undefined") && !core::contains(line, "multiple definition")) {
      continue;
    }
    auto colon = line.find(':');
    if (colon != std::string::npos && colon != 0) {
      if (auto it = owner.find(line.substr(0, colon)); it != owner.end()) return it->second;
    }
    for (const auto& name : link_error_names(line)) {
      if (auto it = by_stem.find(core::to_lower(name)); it != by_stem.end()) return it->second;
    }
  }
  return {};
}

// The source files of an imported block's card (headers are compiled through them).
std::vector<std::string> own_sources(const core::Block& b) {
  std::vector<std::string> out;
  for (const auto& f : b.file_list()) {
    fs::path p(f);
    if (core::language_of(p) != nullptr && core::language_of(p)->id == "cpp" && !core::is_interface_file(p)) out.push_back(f);
  }
  return out;
}

// The project's tests (files under [test] dir that no card lists) a failed test run names: no worker may
// change them, so a worker sent again for them only climbs the tiers (bugs.txt 26).
std::vector<std::string> tests_of_no_block(const core::Schema& s, const std::string& test_dir, const std::string& text) {
  std::vector<std::string> out;
  if (test_dir.empty()) return out;
  std::set<std::string> listed;
  for (const auto& b : s.blocks) {
    for (const auto& f : b.file_list()) listed.insert(f);
  }
  auto strip = [](std::string w, std::string_view chars) {
    while (!w.empty() && chars.find(w.front()) != std::string_view::npos) w.erase(0, 1);
    while (!w.empty() && chars.find(w.back()) != std::string_view::npos) w.pop_back();
    return w;
  };
  const std::string prefix = strip(test_dir, "/") + "/";
  std::string word;
  auto take = [&] {
    auto w = strip(word, "\"'`()[]{}<>:;,");
    word.clear();
    if (w.starts_with("./")) w = w.substr(2);
    w = strip(w, ".:;,");
    if (!w.starts_with(prefix) || listed.contains(w) || std::find(out.begin(), out.end(), w) != out.end()) return;
    std::error_code ec;
    if (fs::is_regular_file(s.root / w, ec)) out.push_back(w);
  };
  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      take();
    } else {
      word += c;
    }
  }
  take();
  return out;
}

// The inv lines a block gains by the approved changes of a ticket ("+ inv" under its "@block").
std::vector<std::string> ticket_invariants(const ChangeStore& changes, const std::string& tid, const std::string& block) {
  std::vector<std::string> out;
  auto cs = changes.list();
  if (!cs) return out;
  for (const auto& c : *cs) {
    if (c.status.value != "approved") continue;
    auto from = core::split_ws(c.from.value);
    if (std::find(from.begin(), from.end(), tid) == from.end()) continue;
    std::string at;
    for (const auto& d : c.diff) {
      if (d.text.starts_with("@block ")) {
        at = std::string(core::trim(std::string_view(d.text).substr(7)));
        continue;
      }
      if (at == block && d.op == '+' && d.text.starts_with("inv ")) out.emplace_back(core::trim(std::string_view(d.text).substr(4)));
    }
  }
  return out;
}

// Whether an approved change of the ticket changed the card of `block` (it unfreezes that block's api).
bool ticket_changed_block(const ChangeStore& changes, const std::string& tid, const std::string& block) {
  if (tid.empty()) return false;
  auto cs = changes.list();
  if (!cs) return false;
  for (const auto& c : *cs) {
    if (c.status.value != "approved") continue;
    auto from = core::split_ws(c.from.value);
    if (std::find(from.begin(), from.end(), tid) == from.end()) continue;
    for (const auto& d : c.diff) {
      if (d.text.starts_with("@block ") && core::trim(std::string_view(d.text).substr(7)) == block) return true;
    }
  }
  return false;
}

core::TrailEntry level_entry(int level, std::string role, std::vector<core::Attr> attrs) {
  core::TrailEntry e;
  e.type = core::TrailEntry::Type::Level;
  e.level = level;
  e.role = std::move(role);
  for (auto& a : attrs) {
    if (!a.value.empty()) e.attrs.push_back(std::move(a));
  }
  return e;
}

// What the files of a block hold (path -> hash; "-" when missing): the files of its card for an imported block,
// the files of its folder but its card otherwise. A worker's done with none of them changed did nothing
// (docs/adr/0031, bugs.txt 14).
std::map<std::string, std::string> block_files_state(const fs::path& root, const core::Schema& s, const core::Block& b) {
  std::map<std::string, std::string> out;
  auto put = [&](const fs::path& rel) {
    auto text = core::read_file(root / rel);
    out[rel.generic_string()] = text ? fmt::format("{:016x}", core::fnv1a64(*text)) : "-";
  };
  if (b.external()) {
    for (const auto& f : b.file_list()) put(fs::path(f));
    return out;
  }
  std::error_code ec;
  const auto dir = s.block_dir(b.id);
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec) || it->path().filename() == "block.ksl") continue;
    put(fs::relative(it->path(), root, ec));
  }
  return out;
}

// The cost of a ticket's runs since `since` (docs/adr/0031): a run whose turns a Claude Code session Karkas
// launched served costs what that session reported (.karkas/mcp/launch/costs.jsonl, at list prices, every
// try of it counted: a refused one too); the others what their journal holds (the estimate from the turns).
struct TicketCost {
  int runs = 0;
  int sessions = 0;  // runs priced by their session's own cost
  std::int64_t input = 0, output = 0, cache_write = 0, cache_read = 0;
  double usd = 0;
};

TicketCost ticket_cost(const fs::path& root, const std::string& ticket, std::int64_t since) {
  struct Row {
    double usd = 0;
    std::int64_t in = 0, out = 0, cw = 0, cr = 0;
  };
  std::map<std::string, Row> session;
  for (const auto& line : core::split(core::read_file(root / ".karkas/mcp/launch/costs.jsonl").value_or(""), '\n')) {
    auto j = json::parse(line, nullptr, false);
    if (!j.is_object() || !j.contains("run") || !j["run"].is_string()) continue;
    auto& r = session[j["run"].get<std::string>()];
    r.usd += j.value("cost_usd", 0.0);
    if (j.contains("usage") && j["usage"].is_object()) {
      r.in += j["usage"].value("input_tokens", std::int64_t{0});
      r.out += j["usage"].value("output_tokens", std::int64_t{0});
      r.cw += j["usage"].value("cache_creation_input_tokens", std::int64_t{0});
      r.cr += j["usage"].value("cache_read_input_tokens", std::int64_t{0});
    }
  }
  TicketCost tc;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(root / ".karkas" / "runs", ec)) {
    if (!e.is_regular_file(ec) || e.path().extension() != ".jsonl") continue;
    auto text = core::read_file(e.path());
    if (!text) continue;
    auto lines = core::split(*text, '\n');
    if (lines.empty()) continue;
    auto start = json::parse(lines.front(), nullptr, false);
    if (!start.is_object() || start.value("type", "") != "run_start" || start.value("ticket", "") != ticket ||
        start.value("ts", std::int64_t{0}) < since) {
      continue;
    }
    ++tc.runs;
    const auto name = e.path().filename().string();
    if (auto s = session.find(name); s != session.end()) {
      ++tc.sessions;
      tc.usd += s->second.usd;
      tc.input += s->second.in;
      tc.output += s->second.out;
      tc.cache_write += s->second.cw;
      tc.cache_read += s->second.cr;
      continue;
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
      auto end = json::parse(*it, nullptr, false);
      if (!end.is_object() || end.value("type", "") != "run_end") continue;
      tc.usd += end.value("cost_usd", 0.0);
      tc.input += end.value("input_tokens", std::int64_t{0});
      tc.output += end.value("output_tokens", std::int64_t{0});
      tc.cache_write += end.value("cache_write_tokens", std::int64_t{0});
      tc.cache_read += end.value("cache_read_tokens", std::int64_t{0});
      break;
    }
  }
  return tc;
}

}  // namespace

// The tester writes Karkas's contract tests; an imported block has none: the project's own build and
// tests check it (ADR 0016, 0027).
constexpr const char* kImportedNoTester =
    "block is imported code (files): it has no Karkas contract tests, the project's own build and tests check it (docs/adr/0027)";

struct Engine::Impl {
  fs::path root;
  core::Config& cfg;
  EngineOptions opts;
  // Mode mcp with [agents] launch = "claude": the sessions that serve the runs of this engine (ADR 0029).
  std::unique_ptr<RunLauncher> launcher;
  std::unique_ptr<llm::IHttpTransport> http;
  std::unique_ptr<ITestService> own_tests;
  ITestService* tests = nullptr;
  std::unique_ptr<index::Index> idx;
  std::unique_ptr<code::CppAnalyzer> own_analyzer;
  index::ICodeAnalyzer* analyzer = nullptr;
  std::recursive_mutex mu;
  core::TicketStore tickets;
  ChangeStore changes;
  ProviderFactory providers;
  std::map<std::string, std::string> navigator_tier;  // scope → tier after escalations

  Impl(fs::path r, core::Config& c, EngineOptions o) : root(std::move(r)), cfg(c), opts(std::move(o)), tickets(root), changes(root) {}

  void say(const std::string& m) {
    spdlog::info("{}", m);
    if (opts.progress) opts.progress(m);
  }

  void reindex() {
    std::lock_guard lk(mu);
    if (auto r = idx->update({false, analyzer}); !r) spdlog::warn("index: {}", r.error().str());
  }

  kk::Result<core::Schema> schema() { return core::load_schema(root); }

  std::string prompt(const std::string& name, std::vector<std::pair<std::string, std::string>> vars) {
    std::string text;
    auto override_path = root / ".karkas" / "prompts" / (name + ".txt");
    if (fs::exists(override_path)) {
      text = core::read_file(override_path).value_or("");
    } else {
      text = std::string(templates::get("prompts/" + name + ".txt").value_or(""));
    }
    vars.emplace_back("ksl_reference", std::string(templates::get("prompts/ksl_reference.txt").value_or("")));
    return core::render_template(text, vars);
  }

  AgentRunner runner() { return AgentRunner(root, cfg, providers, nullptr); }

  // A fingerprint of everything a human changes to unblock a ticket: every card, every api/
  // and the frozen set. The same fingerprint means nothing was done since the last run.
  std::string state_hash() {
    auto s = schema();
    if (!s) return "";
    auto files = index::collect_code_files(*s);
    std::string acc;
    for (const auto& b : s->blocks) {
      acc += b.id + "\n" + index::card_text_without_comments(b) + "\n" + index::block_api_hash(files, b.id) + "\n";
    }
    if (auto st = index::StateStore(root).load()) {
      for (const auto& [id, h] : st->frozen_api) acc += "frozen " + id + " " + h + "\n";
    }
    return fmt::format("{:016x}", core::fnv1a64(acc));
  }

  // Why running the ticket again would only repeat its last run (empty if it may run): a
  // change of the ticket still waits for approval, or the last run stopped on something only
  // a human resolves and nothing a human changes (cards, api/, approvals) changed since.
  std::string blocked_reason(const core::Ticket& t) {
    if (auto cs = changes.list()) {
      for (const auto& c : *cs) {
        if (c.status.value != "pending") continue;
        for (const auto& part : core::split_ws(c.from.value)) {
          if (part == t.id) {
            return fmt::format("{} waits for {}: approve it (karkas approve {}) or reject it (karkas change reject {}) first",
                               t.id, c.id, c.id, c.id);
          }
        }
      }
    }
    std::size_t start = t.result.size();
    for (std::size_t i = 0; i < t.result.size(); ++i) {
      if (t.result[i].key == "status") start = i;
    }
    std::string status, state, cause, note;
    for (std::size_t i = start; i < t.result.size(); ++i) {
      const auto& r = t.result[i];
      if (r.key == "status") status = r.text;
      if (r.key == "state") state = r.text;
      if (r.key == "cause" && cause.empty()) cause = r.text;
      if (r.key == "note") note = r.text;
    }
    static const std::set<std::string> blocking = {"needs_contract_change", "escalated", "needs_tester"};
    if (!blocking.contains(status) || state.empty()) return "";
    // A model or network failure is not an answer: running again is the remedy for it.
    for (const char* w : {"failed: error", "failed: timeout", "failed: budget", "failed: max_steps", "failed: refusal",
                          "failed: end_turn"}) {
      if (core::contains(note, w)) return "";
    }
    if (state != state_hash()) return "";
    return fmt::format("{} ended with {} and nothing changed since (cards, api/, approvals): running it again would only "
                       "repeat that run.\n{}{}Act on it first, or run again with --force.",
                       t.id, status, cause.empty() ? "" : "cause: " + cause + "\n", note.empty() ? "" : "note: " + note + "\n");
  }

  std::string tier_for(const std::string& role, const std::string& fallback) {
    auto it = cfg.roles.find(role);
    std::string t = it == cfg.roles.end() ? fallback : it->second;
    return t == "auto" ? fallback : t;
  }
  // A model pinned for the block ([blocks], [levels]; ADR 0011) beats the role's tier.
  std::string tier_for(const std::string& role, const std::string& fallback, std::string_view block) {
    if (auto p = cfg.pin(block, role); !p.tier.empty()) return p.tier;
    return tier_for(role, fallback);
  }

  void trail(const std::string& tid, const core::TrailEntry& e, const std::vector<std::string>& facts = {}) {
    if (tid.empty()) return;
    std::vector<std::string> f(facts.begin(), facts.begin() + static_cast<long>(std::min<std::size_t>(facts.size(), 2)));
    if (auto r = tickets.append(tid, e, f); !r) spdlog::warn("ticket {}: {}", tid, r.error().message);
  }

  void note(const std::string& tid, const std::string& text) {
    core::TrailEntry e;
    e.type = core::TrailEntry::Type::Note;
    e.text = text;
    trail(tid, e);
  }

  void set_status(const std::string& tid, core::TicketStatus s) {
    if (!tid.empty()) (void)tickets.set_status(tid, s);
  }

  // --- contexts (9.2) ----------------------------------------------------------------------

  static void add_files(std::string& out, const fs::path& dir, const std::string& label, std::size_t inline_max,
                        std::size_t& budget, bool list_only = false) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
      if (e.is_regular_file(ec) && core::is_code_file(e.path())) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      auto name = label + core::rel_str(dir, f);
      auto size = fs::file_size(f, ec);
      if (list_only || size > inline_max || size > budget) {
        out += fmt::format("=== {} ({} bytes, use read_file)\n", name, size);
        continue;
      }
      auto text = core::read_file(f).value_or("");
      budget -= std::min(budget, text.size());
      out += fmt::format("=== {}\n{}{}", name, text, text.ends_with("\n") ? "" : "\n");
    }
  }

  // Identifiers a block uses (its card, api/, tests and, for a worker, impl/): the declarations of
  // a big neighbor header an agent sees are these, the rest is one read_file away (ADR 0026).
  static std::set<std::string> used_identifiers(const core::Schema& s, const std::string& block, Role role,
                                                const std::string& card) {
    static const std::set<std::string> skip = {
        "const", "constexpr", "inline", "static", "struct", "class", "enum", "namespace", "using", "return", "void", "bool",
        "char", "int", "unsigned", "long", "short", "float", "double", "auto", "true", "false", "nullptr", "include", "pragma",
        "once", "std", "virtual", "override", "public", "private", "protected", "template", "typename", "default", "this",
        "for", "while", "if", "else", "switch", "case", "break", "continue", "new", "delete", "size_t", "the", "and", "of"};
    static const std::regex ident(R"([A-Za-z_][A-Za-z0-9_]{2,})");
    std::set<std::string> out;
    auto take = [&](const std::string& text) {
      for (auto it = std::sregex_iterator(text.begin(), text.end(), ident); it != std::sregex_iterator(); ++it) {
        if (!skip.contains(it->str())) out.insert(it->str());
      }
    };
    take(card);
    // An imported block: the code of its card (headers only for the tester).
    if (const auto* b = s.find(block); b != nullptr && b->external()) {
      for (const auto& f : b->file_list()) {
        fs::path p = s.root / f;
        if (!core::is_code_file(p)) continue;
        if (role != Role::Worker && p.extension().string().starts_with(".c")) continue;  // a source
        take(core::read_file(p).value_or(""));
      }
      return out;
    }
    const auto& dirs = s.project.dirs;
    std::vector<fs::path> dirs_to_read = {s.block_dir(block) / dirs.api, s.block_dir(block) / dirs.tests, s.block_dir(block) / dirs.mocks};
    if (role == Role::Worker) dirs_to_read.push_back(s.block_dir(block) / dirs.impl);
    std::error_code ec;
    for (const auto& d : dirs_to_read) {
      if (!fs::is_directory(d, ec)) continue;
      for (const auto& e : fs::recursive_directory_iterator(d, ec)) {
        if (e.is_regular_file(ec) && core::is_code_file(e.path())) take(core::read_file(e.path()).value_or(""));
      }
    }
    return out;
  }

  // The functions of an imported block its task names — by the card's fn names or their C++ names
  // (handle_event or HandleEvent, save) — and the identifiers of their bodies: what the worker of this
  // task needs from the linked headers, not what the whole 6 000-line file uses (ADR 0029). Empty
  // when the task names none of them or no body is found: the caller then takes the whole block.
  static std::set<std::string> focus_identifiers(const core::Schema& s, const std::string& block, const std::string& task,
                                                 std::vector<std::string>* names) {
    const auto* b = s.find(block);
    if (b == nullptr || !b->external() || task.empty()) return {};
    auto norm = [](std::string_view w) {
      std::string o;
      for (char c : w) {
        if (c != '_') o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return o;
    };
    auto camel = [](const std::string& snake) {
      std::string o;
      bool up = true;
      for (char c : snake) {
        if (c == '_') {
          up = true;
          continue;
        }
        o += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        up = false;
      }
      return o;
    };
    static const std::regex word(R"([A-Za-z_][A-Za-z0-9_]{2,})");
    std::set<std::string> words;
    for (auto it = std::sregex_iterator(task.begin(), task.end(), word); it != std::sregex_iterator(); ++it) words.insert(norm(it->str()));
    std::vector<std::string> wanted;  // C++ names of the named functions, card order
    for (const auto& p : b->ports) {
      if (p.dir != core::PortDir::Fn || !words.contains(norm(p.name))) continue;
      auto cpp = camel(p.name);
      if (std::find(wanted.begin(), wanted.end(), cpp) == wanted.end()) wanted.push_back(cpp);
      if (wanted.size() >= 8) break;
    }
    if (wanted.empty()) return {};
    static const std::regex ident(R"([A-Za-z_][A-Za-z0-9_]{2,})");
    std::set<std::string> out;
    for (const auto& f : b->file_list()) {
      fs::path p = s.root / f;
      if (core::language_of(p) == nullptr || core::is_contract_file(p)) continue;  // the sources: C/C++ files that are no header
      auto text = core::read_file(p);
      if (!text) continue;
      for (const auto& fn : wanted) {
        // A definition: "::Name(" with its body, the braces counted from the first "{" after it.
        for (std::size_t at = text->find("::" + fn + "("); at != std::string::npos; at = text->find("::" + fn + "(", at + 1)) {
          auto open = text->find_first_of("{;", at);
          if (open == std::string::npos || (*text)[open] == ';') continue;
          int depth = 0;
          std::size_t end = open;
          for (; end < text->size(); ++end) {
            if ((*text)[end] == '{') ++depth;
            if ((*text)[end] == '}' && --depth == 0) break;
          }
          std::string body = text->substr(at, std::min<std::size_t>(end - at + 1, 24 * 1024));
          for (auto it = std::sregex_iterator(body.begin(), body.end(), ident); it != std::sregex_iterator(); ++it) out.insert(it->str());
          if (names != nullptr && std::find(names->begin(), names->end(), fn) == names->end()) names->push_back(fn);
          break;
        }
      }
    }
    if (out.empty()) return {};
    for (auto it = std::sregex_iterator(task.begin(), task.end(), ident); it != std::sregex_iterator(); ++it) out.insert(it->str());
    return out;
  }

  // A neighbor's api/: a small header whole, a big one as the declarations the block uses.
  static void add_neighbor_api(std::string& out, const fs::path& dir, const std::string& label, const std::set<std::string>& used,
                               std::size_t& budget) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
      if (e.is_regular_file(ec) && core::is_code_file(e.path())) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) add_neighbor_file(out, f, label + core::rel_str(dir, f), used, budget);
  }

  // The files of an imported block's card (ADR 0027): small headers and sources whole, big ones by
  // size (read by offset/limit or grep), data files by size, files of the card not written yet.
  static void add_listed(std::string& out, const fs::path& root, const std::vector<std::string>& files, std::size_t& budget) {
    constexpr std::size_t kHeader = 16384;
    constexpr std::size_t kSource = 8192;
    for (const auto& rel : files) {
      fs::path p = root / rel;
      std::error_code ec;
      if (!fs::exists(p, ec)) {
        out += fmt::format("=== {} (listed in the card, does not exist yet: create it)\n", rel);
        continue;
      }
      auto text = core::read_file(p).value_or("");
      auto lines = std::count(text.begin(), text.end(), '\n');
      if (!core::is_code_file(p)) {
        out += fmt::format("=== {} ({} bytes, {} lines; data of this block: read_file with offset/limit, grep)\n", rel, text.size(), lines);
        continue;
      }
      bool header = !p.extension().string().starts_with(".c");
      std::size_t max = header ? kHeader : kSource;
      if (text.size() > max || text.size() > budget) {
        out += fmt::format("=== {} ({} bytes, {} lines; read_file with offset/limit, grep)\n", rel, text.size(), lines);
        continue;
      }
      budget -= text.size();
      out += fmt::format("=== {}\n{}{}", rel, text, text.ends_with("\n") ? "" : "\n");
    }
  }

  // Lines [from, to) of a header: the declarations whose names the block uses, each with the comment
  // lines before it. A used type whose body is big shows its head, its access labels and the members
  // the block uses, without its private part (the classes of imported code, ADR 0027).
  static void pick_used(const std::vector<std::string>& lines, std::size_t from, std::size_t to, const std::set<std::string>& used,
                        std::string& kept, std::vector<std::string>& names, bool in_class, bool private_default) {
    static const std::regex type_start(R"(^\s*(?:template\s*<[^>]*>\s*)?(?:struct|class|enum(?:\s+class)?|union)\s+([A-Za-z_]\w*))");
    static const std::regex class_start(R"(^\s*(?:template\s*<[^>]*>\s*)?(struct|class|union)\s+[A-Za-z_]\w*[^;]*$)");
    static const std::regex label(R"(^\s*(public|protected|private)\s*:(?!:))");
    static const std::regex constant(R"(([A-Za-z_]\w*)\s*(?:=|\{))");
    static const std::regex function(R"(([A-Za-z_]\w*)\s*\()");
    static const std::regex alias(R"(^\s*using\s+([A-Za-z_]\w*)\s*=)");
    constexpr std::size_t kWholeType = 1500;
    auto depth_of = [](const std::string& l) {
      int d = 0;
      for (char c : l) d += c == '{' ? 1 : c == '}' ? -1 : 0;
      return d;
    };
    bool hidden = in_class && private_default;
    bool block_comment = false;  // inside /* ... */ (license headers of imported code)
    std::string comment;
    for (std::size_t i = from; i < to; ++i) {
      const auto& l = lines[i];
      auto t = std::string(core::trim(l));
      if (block_comment) {
        block_comment = !core::contains(t, "*/");
        continue;
      }
      if (t.starts_with("/*")) {
        auto close = t.find("*/", 2);
        block_comment = close == std::string::npos;
        if (block_comment || core::trim(std::string_view(t).substr(close + 2)).empty()) {
          comment.clear();
          continue;
        }
      }
      std::smatch m;
      if (in_class && std::regex_search(l, m, label)) {
        hidden = m[1] == "private";
        if (!hidden) kept += l + "\n";
        comment.clear();
        continue;
      }
      if (t.empty() || t.starts_with("#")) {
        comment.clear();
        continue;
      }
      if (t.starts_with("//")) {
        comment += l + "\n";
        continue;
      }
      if (!in_class && (t.starts_with("namespace ") || t.starts_with("}  // namespace") || t == "}")) {
        kept += l + "\n";
        comment.clear();
        continue;
      }
      std::size_t end = i;
      std::string decl;
      bool type = std::regex_search(l, m, type_start);
      if (type) {
        decl = m[1];
        int depth = 0;
        bool opened = false;
        for (end = i; end < to; ++end) {
          for (char c : lines[end]) {
            if (c == '{') {
              ++depth;
              opened = true;
            } else if (c == '}') {
              --depth;
            }
          }
          if ((opened && depth <= 0) || (!opened && lines[end].find(';') != std::string::npos)) break;
        }
      } else {
        while (end < to && lines[end].find(';') == std::string::npos && lines[end].find('{') == std::string::npos) ++end;
        // An inline body (or a braced initializer) over several lines belongs to its declaration;
        // an extern "C" block does not (its declarations are read one by one).
        if (end < to && lines[end].find('{') != std::string::npos && !core::contains(lines[end], "extern \"C\"")) {
          const std::size_t open_line = end;
          int depth = 0;
          for (std::size_t k = i; k < to; ++k) {
            depth += depth_of(lines[k]);
            end = k;
            if (k >= open_line && depth <= 0) break;
          }
        }
        std::string joined;
        for (std::size_t k = i; k <= end && k < to; ++k) joined += lines[k] + " ";
        if (std::regex_search(joined, m, alias)) decl = m[1];
        else if (joined.find('(') != std::string::npos && std::regex_search(joined, m, function)) decl = m[1];
        else if (std::regex_search(joined, m, constant)) decl = m[1];
      }
      end = std::min(end, to - 1);
      if (hidden) {
        comment.clear();
        i = end;
        continue;
      }
      if (!decl.empty()) names.push_back(decl);
      if (!decl.empty() && used.contains(decl)) {
        std::size_t bytes = 0;
        for (std::size_t k = i; k <= end; ++k) bytes += lines[k].size() + 1;
        std::smatch cm;
        if (type && bytes > kWholeType && !in_class && end > i && std::regex_search(l, cm, class_start)) {
          // Head (up to its '{'), the used public members, the closing line.
          std::size_t body = i;
          while (body < end && lines[body].find('{') == std::string::npos) ++body;
          kept += comment;
          for (std::size_t k = i; k <= body; ++k) kept += lines[k] + "\n";
          std::vector<std::string> members;
          pick_used(lines, body + 1, end, used, kept, members, true, cm[1] == "class");
          kept += lines[end] + "\n";
        } else {
          kept += comment;
          for (std::size_t k = i; k <= end; ++k) kept += lines[k] + "\n";
        }
      }
      comment.clear();
      i = end;
    }
  }

  // A header of an imported neighbor: only the declarations the block uses (a big class as its head,
  // labels and used public members). False, and nothing written, when it has none or they do not fit.
  static bool add_used_declarations(std::string& out, const fs::path& f, const std::string& name, const std::set<std::string>& used,
                                    std::size_t& budget) {
    auto text = core::read_file(f).value_or("");
    std::vector<std::string> lines;
    for (std::size_t start = 0; start < text.size();) {
      auto end = text.find('\n', start);
      if (end == std::string::npos) end = text.size();
      lines.push_back(text.substr(start, end - start));
      start = end + 1;
    }
    if (lines.empty()) return false;
    std::string kept;
    std::vector<std::string> names;
    pick_used(lines, 0, lines.size(), used, kept, names, false, false);
    bool any = std::any_of(names.begin(), names.end(), [&](const std::string& n) { return used.contains(n); });
    if (!any || kept.size() > budget) return false;
    budget -= kept.size();
    out += fmt::format("=== {} ({} bytes; the declarations this block uses)\n{}", name, text.size(), kept);
    return true;
  }

  // One header of a neighbor: whole when small, else the declarations the block uses.
  static void add_neighbor_file(std::string& out, const fs::path& f, const std::string& name, const std::set<std::string>& used,
                                std::size_t& budget) {
    constexpr std::size_t kWhole = 2000;
    auto text = core::read_file(f).value_or("");
    if (text.size() <= kWhole && text.size() <= budget) {
      budget -= text.size();
      out += fmt::format("=== {}\n{}{}", name, text, text.ends_with("\n") ? "" : "\n");
      return;
    }
    // Chunks: a type with its body, or one declaration, each with the comment lines before it.
    std::vector<std::string> lines;  // raw: core::split trims, and a struct body keeps its indentation
    for (std::size_t start = 0; start < text.size();) {
      auto end = text.find('\n', start);
      if (end == std::string::npos) end = text.size();
      lines.push_back(text.substr(start, end - start));
      start = end + 1;
    }
    if (lines.empty()) return;
    std::string kept;
    std::vector<std::string> names;
    pick_used(lines, 0, lines.size(), used, kept, names, false, false);
    bool any = std::any_of(names.begin(), names.end(), [&](const std::string& n) { return used.contains(n); });
    if (any && kept.size() <= budget) {
      budget -= kept.size();
      out += fmt::format("=== {} ({} bytes; the declarations this block uses, read_file for the rest)\n{}", name, text.size(), kept);
    } else {
      if (names.size() > 40) names.resize(40);
      out += fmt::format("=== {} ({} bytes, use read_file; declares {})\n", name, text.size(), core::join(names, ", "));
    }
  }

  // Tests of the block as a list: names say what is covered; one read_file shows a file.
  static void list_tests(std::string& out, const fs::path& dir, const std::string& label) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
      if (e.is_regular_file(ec) && core::is_code_file(e.path())) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    static const std::regex test_case(R"re(TEST_CASE\s*\(\s*"([^"]*)")re");
    for (const auto& f : files) {
      auto text = core::read_file(f).value_or("");
      std::vector<std::string> cases;
      for (auto it = std::sregex_iterator(text.begin(), text.end(), test_case); it != std::sregex_iterator(); ++it) {
        cases.push_back("\"" + (*it)[1].str() + "\"");
      }
      std::size_t more = cases.size() > 8 ? cases.size() - 8 : 0;
      if (more > 0) cases.resize(8);
      out += fmt::format("=== {}{} ({} bytes, use read_file){}{}\n", label, core::rel_str(dir, f), text.size(),
                         cases.empty() ? "" : ": " + core::join(cases, ", "), more > 0 ? fmt::format(" and {} more", more) : "");
    }
  }

  // task: what the agent is asked (a worker of an imported block gets the headers its functions use).
  std::string context_for(Role role, const std::string& block, const std::string& task = {}) {
    std::lock_guard lk(mu);
    auto s = schema();
    if (!s) return s.error().str();
    const auto& dirs = s->project.dirs;
    std::string out;
    switch (role) {
      case Role::Orchestrator: {
        out += "idea " + s->project.idea.value + "\n";
        out += idx->overview();
        auto all = tickets.list();
        if (all && !all->empty()) {
          out += "--- recent tickets\n";
          std::size_t start = all->size() > 6 ? all->size() - 6 : 0;
          for (std::size_t i = start; i < all->size(); ++i) {
            const auto& t = (*all)[i];
            out += fmt::format("{} {} {} {}\n", t.id, t.kind.value, t.status.value,
                               shorten(t.symptom.set ? t.symptom.value : t.task.value, 80));
          }
        }
        return out;
      }
      case Role::Navigator: {
        const auto* b = s->find(block);
        std::string parent = b != nullptr ? b->parent : "";
        const auto* p = parent.empty() ? nullptr : s->find(parent);
        out += "parent purpose " + (p != nullptr ? p->purpose.value : s->project.idea.value) + "\n";
        auto v = idx->view(block, 1);
        out += v ? *v : v.error().message + "\n";
        return out;
      }
      case Role::Architect: {
        out += "idea " + s->project.idea.value + "\n";
        for (const auto& r : s->project.rules) out += "rule " + r.text + "\n";
        if (!s->blocks.empty()) {
          std::string parent = block.empty() ? "" : core::parent_id(block);
          if (parent.empty()) {
            out += idx->overview({true});
          } else {
            auto v = idx->view(parent, 1, {true});
            if (v) out += *v;
          }
          if (!block.empty()) {
            if (const auto* b = s->find(block)) {
              out += "--- card\n" + index::card_text_without_comments(*b);
              // The architect never rewrites a header blind (ADR 0020): an existing api/ is
              // shown to be updated in place; for a new leaf, a header of the project shows
              // the style to follow (free functions or interfaces, namespaces).
              std::size_t budget = 32 * 1024;
              auto own = s->block_dir(block) / dirs.api;
              std::error_code ec;
              if (b->has_code() && fs::is_directory(own, ec) && !fs::is_empty(own, ec)) {
                out += "--- current api/ of " + block + " (update it in place: keep its style, namespace and every "
                       "declaration the card still lists)\n";
                add_files(out, own, "api/", 16384, budget);
              } else if (b->kind != core::BlockKind::Group) {
                std::string own_parent = core::parent_id(block);
                const core::Block* sample = nullptr;
                for (const auto& x : s->blocks) {
                  if (x.id == block || !x.has_code() || x.kind == core::BlockKind::Shared) continue;
                  auto d = s->block_dir(x.id) / dirs.api;
                  if (!fs::is_directory(d, ec) || fs::is_empty(d, ec)) continue;
                  if (sample == nullptr || core::parent_id(x.id) == own_parent) sample = &x;
                  if (core::parent_id(x.id) == own_parent) break;
                }
                if (sample != nullptr) {
                  out += "--- api/ of " + sample->id + " (the style of this project's headers: follow it)\n";
                  add_files(out, s->block_dir(sample->id) / dirs.api, sample->id + ":api/", 4096, budget);
                }
              }
            }
          }
        }
        return out;
      }
      case Role::Worker:
      case Role::Tester: {
        const auto* b = s->find(block);
        if (b == nullptr) return "unknown block " + block;
        if (b->kind == core::BlockKind::Group) {
          auto v = idx->view(block, 1);
          out += v ? *v : "";
          std::size_t budget = 64 * 1024;
          for (const auto* l : s->leaves_under(block)) add_files(out, s->block_dir(l->id) / dirs.api, l->id + ":api/", 16384, budget);
          return out;
        }
        if (b->external()) {
          // An imported block (ADR 0027): the files of its card where they are, the declarations of
          // the linked blocks it uses (the most called first), the project's own build as its check.
          auto v = idx->view(block, 2);
          out += v ? *v : v.error().message + "\n";
          std::size_t budget = 48 * 1024;
          Sandbox sb(*s, block, role);
          out += "--- files of this block (paths from the project root, as in the card; change them in place)\n";
          add_listed(out, s->root, sb.own_files(), budget);
          std::vector<std::string> focus;
          auto used = role == Role::Worker ? focus_identifiers(*s, block, task, &focus) : std::set<std::string>{};
          if (used.empty()) used = used_identifiers(*s, block, role, v ? *v : std::string());
          out += focus.empty() ? std::string("--- headers of linked blocks (the declarations this block uses; read_file <path> for the rest)\n")
                               : fmt::format("--- headers of linked blocks (the declarations that {} of this task use; read_file <path> "
                                             "or grep for the rest)\n",
                                             core::join(focus, ", "));
          std::map<std::string, int> calls;
          for (const auto& c : b->calls) ++calls[c.target.block];
          auto neighbors = sb.neighbor_blocks();
          std::stable_sort(neighbors.begin(), neighbors.end(), [&](const std::string& x, const std::string& y) { return calls[x] > calls[y]; });
          // Their own budget, and only headers with declarations the block uses: a turn over the MCP
          // output limit is saved to a file the model reads in pieces (ADR 0028).
          std::size_t neighbor_budget = 20 * 1024;
          std::vector<std::string> more;
          for (const auto& n : neighbors) {
            const auto* nb = s->find(n);
            if (nb == nullptr || n == "shared.probe") continue;
            if (!nb->external()) {
              add_neighbor_api(out, s->block_dir(n) / dirs.api, n + ":api/", used, neighbor_budget);
              continue;
            }
            for (const auto& f : nb->file_list()) {
              fs::path p = s->root / f;
              if (!core::is_contract_file(p)) continue;  // headers, or the files of a language without them (ADR 0051)
              if (!add_used_declarations(out, p, f, used, neighbor_budget)) more.push_back(f);
            }
          }
          if (!more.empty()) {
            std::vector<std::string> first(more.begin(), more.begin() + static_cast<long>(std::min<std::size_t>(more.size(), 10)));
            out += fmt::format("(+{} more headers of linked blocks: {}{}; grep them or read_file)\n", more.size(), core::join(first, " "),
                               more.size() > first.size() ? " ..." : "");
          }
          out += "--- checks\nthe project's own build and tests (build(), test()); Karkas keeps no test files for an imported block\n";
          return out;
        }
        auto v = idx->view(block, 2);
        out += v ? *v : v.error().message + "\n";
        std::size_t budget = 48 * 1024;
        out += "--- api (frozen contract)\n";
        add_files(out, s->block_dir(block) / dirs.api, "api/", 16384, budget);
        Sandbox sb(*s, block, role);
        out += "--- neighbor api\n";
        auto used = used_identifiers(*s, block, role, v ? *v : std::string());
        for (const auto& n : sb.neighbor_blocks()) {
          if (n == "shared.probe") continue;
          add_neighbor_api(out, s->block_dir(n) / dirs.api, n + ":api/", used, budget);
        }
        if (b->kind == core::BlockKind::Entry) {
          out += "--- links of the schema (wire exactly these)\n";
          for (const auto& l : s->links()) {
            if (l.kind == core::LinkKind::Use) continue;
            out += fmt::format("{} -{}-> {}\n", l.src_port.empty() ? l.src_block : l.src_block + ":" + l.src_port,
                               core::to_string(l.kind), l.dst_port.empty() ? l.dst_block : l.dst_block + ":" + l.dst_port);
          }
        }
        if (role == Role::Worker) {
          out += "--- impl\n";
          add_files(out, s->block_dir(block) / dirs.impl, "impl/", 8192, budget);
          out += "--- tests/unit\n";
          list_tests(out, s->block_dir(block) / dirs.tests / "unit", "tests/unit/");
          out += "--- tests/contract (written by the tester)\n";
          add_files(out, s->block_dir(block) / dirs.tests / "contract", "tests/contract/", 0, budget, true);
        } else {
          out += "--- mocks\n";
          add_files(out, s->block_dir(block) / dirs.mocks, "mocks/", 12000, budget);
          out += "--- tests/contract (existing)\n";
          add_files(out, s->block_dir(block) / dirs.tests / "contract", "tests/contract/", 4096, budget);
        }
        return out;
      }
    }
    return out;
  }

  std::string last_tests(const std::string& block) {
    std::lock_guard lk(mu);
    auto st = idx->status(block);
    return st ? *st : "";
  }

  // --- leaves ---------------------------------------------------------------------------------

  // approve_now: Auto approves the worker's card diff in its run, which goes on (ADR 0026).
  LeafOutcome run_worker(const std::string& block, const std::string& tid, const std::string& task,
                         const std::string& difficulty, const std::string& prompt_name = "worker",
                         const std::string& fixed_tier = "", const LeafToolHost::ApproveNow& approve_now = {}) {
    LeafOutcome out;
    auto s = schema();
    if (!s) {
      out.status = "failed";
      out.cause = s.error().message;
      return out;
    }
    const auto* b = s->find(block);
    if (b == nullptr || !b->has_code()) {
      out.status = "failed";
      out.cause = "not a leaf: " + block;
      return out;
    }
    // An imported block: its files where they are, its headers' public part as the contract and the
    // project's own build as the check (ADR 0027).
    const bool imported = b->external();
    std::string tier = fixed_tier;
    std::string reason;
    if (auto p = cfg.pin(block, "worker"); tier.empty() && !p.tier.empty()) {
      tier = p.tier;
      reason = "pinned by " + p.by;
    }
    if (tier.empty()) {
      std::lock_guard lk(mu);
      auto snap = idx->snapshot();
      auto runs = idx->runs();
      // Self-tuning statistics: first-attempt cheap worker runs of the same bucket.
      auto rt = route_block(*s, snap, runs, block, difficulty, cfg);
      reason = rt.reason;
      tier = tier_for("worker", rt.tier);
    }
    say(fmt::format("worker {}: tier {} ({})", block, tier, reason.empty() ? "fixed" : reason));
    auto system = prompt(imported && prompt_name == "worker" ? "worker_imported" : prompt_name, {{"block", block}});
    // The focus of the context is what the plan asks of this block (its why names the functions in the
    // orchestrator's words); the ticket's prose would match plain words like "ships" (ADR 0029).
    const std::string& focus_task = task;
    auto context = context_for(Role::Worker, block, focus_task);
    int level = core::trail_level(block);
    // Access to the own api/ in a ticket (ADR 0020): a frozen api lets constant values change
    // (tuning a number is not a contract change); an api unfrozen by an approved @change is
    // brought in line with the card by the worker itself. Outside tickets api/ is read-only.
    ApiAccess api_access = ApiAccess::None;
    std::string api_note;
    std::string users_note;
    if (auto users = Sandbox(*s, block, Role::Worker).user_blocks(); !users.empty()) {
      users_note = imported ? "--- blocks that use this block (their headers are readable under their paths)\n" + core::join(users, ", ") + "\n"
                            : "--- blocks that use this block (their api/ is readable as <block>:api/<file>)\n" + core::join(users, ", ") + "\n";
    }
    const std::string imported_unfrozen_note =
        "--- headers of this block\nunfrozen by a change approved in this ticket: bring them in line with the card (add or "
        "change exactly what the card names, keep every other declaration), then the sources. Report api=changed.\n";
    if (imported) {
      // The headers' public part is what other blocks build on; the rest is free (ADR 0027). A change
      // of this ticket already approved for the block unfreezes them, as it does an api/.
      if (ticket_changed_block(changes, tid, block)) {
        api_access = ApiAccess::Full;
        api_note = imported_unfrozen_note;
      } else if (b->kind == core::BlockKind::Entry) {
        // Nothing builds on the entry block and its card lists no fn (E08): its public names are free,
        // or a new global function of a script could never be added (bugs.txt 28).
        api_access = ApiAccess::Imported;
        api_note = "--- files of this block\nthe entry block: no other block builds on it, so its public names are free (a new "
                   "function, type or subcommand needs no card change).\n";
      } else {
        api_access = ApiAccess::Imported;
        api_note = "--- headers of this block (in a language without headers: its files)\ntheir public part is the contract: "
                   "keep every public or protected declaration as it is. Free: private members (fields, helper methods, "
                   "types), bodies, initial values, comments, new #include or import lines and new public constants (C/C++). "
                   "A new or changed public function, type, field or shell subcommand is a card change: report "
                   "needs_contract_change with the card line in proposal.\n";
      }
    } else if (!tid.empty() && prompt_name == "worker") {
      auto st = index::StateStore(root).load();
      bool frozen = st && st->frozen_api.contains(block);
      bool any_frozen = st && !st->frozen_api.empty();
      bool has_api = !index::block_api_hash(index::collect_code_files(*s), block).empty();
      if (frozen) {
        api_access = ApiAccess::ValuesOnly;
        api_note = "--- api/ of this block\nfrozen: its declarations, types and signatures are the contract. Values of "
                   "constants in api/ may be changed and new constexpr constants added when the ticket asks for it "
                   "(that is not a contract change).\n";
      } else if (any_frozen && has_api) {
        api_access = ApiAccess::Full;
        bool behind = false;
        std::vector<std::string> differ;
        {
          std::lock_guard lk(mu);
          auto snap = idx->snapshot();
          for (const auto& d : snap.drift) {
            if (d.block != block || d.code != "D04") continue;
            if (d.severity == "E") behind = true;
            else differ.push_back(d.message);
          }
        }
        // Behind its card (D04 E): the worker catches up. Other parameter counts (D04 W): either
        // side may be right. Already in line: an old unfreeze, not a request; telling it "the
        // card changed" made a worker look for work that was not there.
        if (behind) {
          api_note = "--- api/ of this block\nunfrozen and behind its card: an approved @change added declarations to the "
                     "card that api/ lacks (see the drift in the last build/test). Bring api/ in line with the card: add "
                     "or change exactly those declarations, in the style and namespace of the existing header, keep "
                     "every other declaration (new constexpr constants the ticket needs are allowed besides). Then the "
                     "impl. Report api=changed.\n";
        } else if (!differ.empty()) {
          api_note = fmt::format("--- api/ of this block\nunfrozen; api/ and the card differ: {}. When the card is right, change "
                                 "api/ to match it (report api=changed); when api/ is right (say, a parameter with a default that "
                                 "keeps the old calls), keep it and report needs_contract_change with the card line in proposal.\n",
                                 core::join(differ, "; "));
        } else {
          // Constants are never on a card: "only what the card names" made the worker of config
          // refuse new constants a frozen api/ would take (flappy T-0014).
          api_note = "--- api/ of this block\nunfrozen but already in line with its card: as in a frozen api/, add new "
                     "constexpr constants and change values of constants when the ticket needs them (a card never names "
                     "constants); any other declaration changes only when the ticket needs one the card names and api/ "
                     "lacks.\n";
        }
      }
    }
    int at_tier = 0;
    std::string history;
    bool stale_test_asked = false;  // the tester looked at failing project tests of no block (bugs.txt 26)
    for (int attempt = 1; attempt <= 12; ++attempt) {
      // The context shows the block's files: a retry sees them as the last attempt left them, not as they were
      // before it (bugs.txt 27: the retry of workspaces.profiles was shown the old notify_profile).
      if (attempt > 1) context = context_for(Role::Worker, block, focus_task);
      std::string task_text;
      if (!tid.empty()) {
        auto t = tickets.load(tid);
        if (t) task_text += core::path_view(*t, block) + "\n";
      }
      task_text += api_note + users_note + "--- task\n" + task + "\n--- last build/test\n" + last_tests(block);
      if (!history.empty()) task_text += "--- previous attempts (do not repeat them)\n" + history;
      AgentSpec spec;
      spec.role = Role::Worker;
      spec.role_name = prompt_name == "worker" ? "worker" : prompt_name;
      spec.block = block;
      spec.ticket = tid;
      spec.tier = tier;
      spec.attempt = attempt;
      spec.system_text = system;
      spec.context_text = context;
      spec.task_text = task_text;
      spec.max_steps = cfg.max_steps_worker;
      spec.max_tokens = cfg.max_tokens_per_run;
      spec.timeout = std::chrono::seconds(cfg.run_timeout_s);
      // The card as it is now: an approval in an earlier attempt may have given the block new files.
      auto now = schema();
      Sandbox sandbox(now ? *now : *s, block, Role::Worker);
      sandbox.set_api_access(api_access);
      LeafToolHost host(std::move(sandbox), Role::Worker, *tests, {block});
      if (approve_now) host.set_approve_now(approve_now);
      auto res = runner().run(spec, host);
      if (host.approved_in_run()) {
        // A later attempt starts from the card and the access the approval gave.
        context = context_for(Role::Worker, block, focus_task);
        api_access = ApiAccess::Full;
        api_note = imported ? imported_unfrozen_note
                            : "--- api/ of this block\nunfrozen by a change approved in this ticket: bring it in line with the card "
                              "(add or change exactly what the card names, keep every other declaration), then the impl. Report "
                              "api=changed.\n";
      }
      if (host.api_written()) {
        if (api_access == ApiAccess::ValuesOnly) {
          // Only values changed (the tool refused anything else): the contract holds, so the
          // frozen hash follows the file and check reports no D05.
          if (auto s2 = schema()) {
            auto h = index::block_api_hash(index::collect_code_files(*s2), block);
            if (!h.empty()) (void)index::StateStore(root).freeze_api(block, h);
          }
          out.api_note = "constants tuned or added, declarations unchanged (re-frozen)";
        } else if (api_access == ApiAccess::Imported) {
          // The public part held (the tool refused anything else); a frozen block keeps being frozen
          // with the new text, a block never frozen stays so.
          bool refrozen = false;
          auto st = index::StateStore(root).load();
          if (auto s2 = schema(); s2 && st && st->frozen_api.contains(block)) {
            auto h = index::block_api_hash(index::collect_code_files(*s2), block);
            refrozen = !h.empty() && index::StateStore(root).freeze_api(block, h).has_value();
          }
          out.api_note = b->kind == core::BlockKind::Entry
                             ? std::string("files of the entry block changed (no block builds on its public names)")
                             : std::string("headers changed in their private part, constants or includes; public declarations unchanged") +
                                   (refrozen ? " (re-frozen)" : "");
        } else if (cfg.auto_approve) {
          // auto_approve: the human left approvals to the program, so it also freezes the api it
          // approved, now in line with the card; no command is left for a client to run (ADR 0029).
          bool frozen = false;
          if (auto s2 = schema()) {
            auto h = index::block_api_hash(index::collect_code_files(*s2), block);
            frozen = !h.empty() && index::StateStore(root).freeze_api(block, h).has_value();
          }
          out.api_note = frozen ? "changed to the card and frozen (auto_approve)" : "changed to the card";
        } else {
          out.api_note = "changed to the card; a human freezes it with `karkas approve api " + block + "` (not a step of an agent or client)";
        }
      }
      out.tokens += res.tokens;
      out.cost += res.cost;
      out.attempts = attempt;
      out.tier = tier;
      out.reads.insert(out.reads.end(), res.reads.begin(), res.reads.end());
      if (res.outcome == "cancelled") {
        // Stopped by the user (ADR 0019): no retry, no escalation.
        out.status = "cancelled";
        out.cause = "stopped by the user";
        trail(tid, level_entry(level, "worker",
                               {{"at", block}, {"model", tier}, {"attempt", std::to_string(attempt)}, {"outcome", "cancelled"}}));
        return out;
      }
      std::string status = "failed";
      json rep;
      if (res.terminal_tool == "report") {
        rep = json::parse(res.terminal_input, nullptr, false);
        if (rep.is_object()) {
          status = rep.value("status", "failed");
          out.cause = sget(rep, "cause");
          out.fix = sget(rep, "fix");
          out.files = sget(rep, "files");
          out.tests = sget(rep, "tests");
          out.api = sget(rep, "api");
          auto prop = sget(rep, "proposal");
          if (!prop.empty()) {
            auto d = parse_diff(prop);
            if (d) {
              out.proposal.clear();
              for (const auto& l : *d) out.proposal.push_back(std::string(1, l.op) + " " + l.text);
            } else {
              // Keep the reason the proposal was rejected so the caller can tell the human
              // (a worker often sends a unified diff of api/ instead of a KSL card diff).
              out.proposal_error = d.error().message;
            }
          }
        }
      } else {
        out.cause = res.outcome + (res.error.empty() ? "" : ": " + res.error);
      }
      // Tests matter more than words (11.3): "fixed" with failing tests is not fixed. An imported block's
      // files are compiled here; the project's full build and tests run in the ticket (ADR 0028).
      if (status == "fixed" || status == "done") {
        auto sources = imported ? own_sources(*b) : std::vector<std::string>{};
        auto tr = !sources.empty() ? tests->compile_check(sources) : tests->build_and_test({block});
        out.tests_ok = tr && tr->ok;
        out.test_text = tr ? tr->text : tr.error().message;
        // A project test of no block fails: it may check the behaviour this ticket changes on purpose. No worker
        // may change it, so the behaviour tester looks at it once instead of the worker going up the tiers
        // (bugs.txt 26: T-0003 of ~/.config/hypr, a test that expected the old workspace number).
        if (!out.tests_ok && tr && tr->build_ok && imported && !tid.empty() && !stale_test_asked) {
          if (auto stale = tests_of_no_block(*s, cfg.test_dir, out.test_text); !stale.empty()) {
            stale_test_asked = true;
            say(fmt::format("worker {}: the project's tests {} fail after its change; they belong to no block: the tester looks at them",
                            block, core::join(stale, " ")));
            std::string what = fmt::format(
                "The worker of {} made this change for the ticket:\n{}\nThe task of the block: {}\nAfter it the project's "
                "tests {} fail. They belong to no block, so no worker may change them:\n{}\nRead the tests and the changed "
                "code. If a test checks the old behaviour that this task changes on purpose, update it to the new behaviour "
                "and keep everything else it checks; check behaviour where you can, not the exact text of a source line. "
                "If the change broke what a test checks, leave the test as it is and report failed with the cause. Run "
                "test() and report done when it passes.",
                block, shorten(out.fix.empty() ? out.cause : out.fix, 600), shorten(task, 600), core::join(stale, " "),
                shorten(out.test_text, 1500));
            auto bt = run_behavior_tester(block, tid, what);
            out.tokens += bt.tokens;
            out.cost += bt.cost;
            trail(tid, level_entry(level, "tester", {{"at", block}, {"model", bt.tier}, {"attempt", "1"}, {"outcome", bt.status}}));
            if (bt.status == "cancelled") {
              out.status = "cancelled";
              out.cause = "stopped by the user";
              return out;
            }
            auto tr2 = !sources.empty() ? tests->compile_check(sources) : tests->build_and_test({block});
            out.tests_ok = tr2 && tr2->ok;
            out.test_text = tr2 ? tr2->text : tr2.error().message;
            if (out.tests_ok) {
              out.tests = fmt::format("the project's tests {} checked the old behaviour; the tester updated them ({})",
                                      core::join(stale, " "), shorten(bt.fix.empty() ? bt.cause : bt.fix, 200));
            } else if (!bt.cause.empty()) {
              history += fmt::format("tester of the failing project tests ({}): {}\n", bt.status, shorten(bt.cause, 300));
            }
          }
        }
        if (!out.tests_ok) {
          status = "failed";
          out.cause = "tests fail after the report: " + shorten(out.test_text, 160);
        }
      }
      trail(tid, level_entry(level, "worker",
                             {{"at", block}, {"model", tier}, {"attempt", std::to_string(attempt)}, {"outcome", status}}));
      out.status = status;
      if (status == "fixed" || status == "done" || status == "not_here" || status == "needs_contract_change" ||
          status == "needs_info" || status == "needs_tester") {
        return out;
      }
      ++at_tier;
      history += fmt::format("attempt {} ({}): cause={} fix={} -> {}\n", attempt, tier, shorten(out.cause, 120),
                             shorten(out.fix, 120), shorten(out.test_text.empty() ? res.outcome : out.test_text, 160));
      bool exhausted = res.outcome == "budget" || res.outcome == "max_steps" || res.outcome == "timeout";
      if (at_tier >= cfg.attempts_before_escalation || exhausted) {
        auto nt = next_tier(cfg, tier);
        if (nt.empty() || cfg.tier(nt) == nullptr) {
          out.status = "escalated";
          return out;
        }
        say(fmt::format("worker {}: escalating {} -> {}", block, tier, nt));
        tier = nt;
        at_tier = 0;
      }
    }
    out.status = "escalated";
    return out;
  }

  // The behaviour test of a finished feature of an imported project (ADR 0029): a tester writes one test
  // in the project's own framework ([test] dir, as [test] guide says) and runs it with the project's test
  // command, which gets the ticket's test files in KK_TEST_FILES. `what`: the ticket and what was done.
  LeafOutcome run_behavior_tester(const std::string& block, const std::string& tid, const std::string& what) {
    LeafOutcome out;
    auto s = schema();
    if (!s) {
      out.status = "failed";
      out.cause = s.error().message;
      return out;
    }
    std::error_code ec;
    const fs::path dir = root / cfg.test_dir;
    fs::create_directories(dir, ec);
    std::string ctx = context_for(Role::Tester, block);
    ctx += fmt::format("--- the project's test folder: {} (write your test there, as a new file)\n", cfg.test_dir);
    std::vector<fs::path> existing;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (e.is_regular_file(ec)) existing.push_back(e.path());
    }
    std::sort(existing.begin(), existing.end());
    for (const auto& f : existing) ctx += "  " + core::rel_str(root, f) + "\n";
    if (!existing.empty()) {
      // The shortest existing test shows the format best.
      auto shortest = *std::min_element(existing.begin(), existing.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code e1;
        return fs::file_size(a, e1) < fs::file_size(b, e1);
      });
      ctx += "--- an existing test: " + core::rel_str(root, shortest) + "\n" + shorten(core::read_file(shortest).value_or(""), 6000) + "\n";
    }
    if (!cfg.test_guide.empty()) {
      ctx += "--- how a test of this project is written (" + cfg.test_guide + ")\n" +
             shorten(core::read_file(root / cfg.test_guide).value_or("(the guide file is missing)"), 12000) + "\n";
    }
    AgentSpec spec;
    spec.role = Role::Tester;
    spec.role_name = "tester";
    spec.block = block;
    spec.ticket = tid;
    spec.tier = tier_for("tester", "cheap", block);
    spec.system_text = prompt("tester_imported", {{"block", block}});
    spec.context_text = ctx;
    spec.task_text = "--- task\n" + what + "\n--- last build/test\n" + last_tests(block);
    spec.max_steps = std::max(cfg.max_steps_tester, cfg.max_steps_worker);  // it reads the code it tests
    spec.max_tokens = cfg.max_tokens_per_run;
    spec.timeout = std::chrono::seconds(cfg.run_timeout_s);
    Sandbox sb(*s, block, Role::Tester);
    sb.allow_write_dir(dir);
    LeafToolHost host(std::move(sb), Role::Tester, *tests, {block});
    auto res = runner().run(spec, host);
    out.tokens = res.tokens;
    out.cost = res.cost;
    out.tier = spec.tier;
    out.attempts = 1;
    if (res.terminal_tool == "report") {
      auto rep = json::parse(res.terminal_input, nullptr, false);
      out.status = rep.is_object() ? rep.value("status", "done") : "done";
      if (rep.is_object()) {
        out.cause = sget(rep, "cause");
        out.fix = sget(rep, "fix");
        out.files = sget(rep, "files");
      }
    } else {
      out.status = res.outcome == "cancelled" ? "cancelled" : "failed";
      out.cause = res.outcome;
    }
    out.test_text = host.last_test_text();
    return out;
  }

  LeafOutcome run_tester(const std::string& block, const std::string& tid, bool integration = false,
                         const std::string& fix_task = "") {
    LeafOutcome out;
    auto s = schema();
    if (!s) {
      out.status = "failed";
      out.cause = s.error().message;
      return out;
    }
    const auto* b = s->find(block);
    if (b == nullptr) {
      out.status = "failed";
      out.cause = "unknown block";
      return out;
    }
    if (b->external()) {
      out.status = "failed";
      out.cause = kImportedNoTester;
      return out;
    }
    if (!integration) (void)code::write_mocks(*s, block);
    // A model pinned for the block, then the role of [roles]; the defaults only when neither
    // is configured (a project on other models never falls back to a hard-coded tier).
    std::string tier = integration ? tier_for("integration_tester", "medium", block)
                                   : tier_for("tester", b->est.value == "L" ? "medium" : "cheap", block);
    AgentSpec spec;
    spec.role = Role::Tester;
    spec.role_name = integration ? "integration_tester" : "tester";
    spec.block = block;
    spec.ticket = tid;
    spec.tier = tier;
    spec.system_text = prompt(integration ? "integration_tester" : "tester", {{"block", block}});
    spec.context_text = context_for(Role::Tester, block);
    spec.task_text = integration ? fmt::format("Write the integration tests of group {} now.", block)
                                 : "Write the contract tests of this leaf now: one TEST_CASE per inv of the card.";
    if (!fix_task.empty()) {
      spec.task_text.clear();
      if (!tid.empty()) {
        if (auto t = tickets.load(tid)) spec.task_text = core::path_view(*t, block) + "\n";
      }
      spec.task_text += "--- task\n" + fix_task + "\n--- last build/test\n" + last_tests(block);
    }
    spec.max_steps = cfg.max_steps_tester;
    spec.max_tokens = cfg.max_tokens_per_run;
    spec.timeout = std::chrono::seconds(cfg.run_timeout_s);
    // A tester with a task of a ticket (correct a test, cover new invariants) starts at its tier and
    // escalates on failure like a worker (ADR 0026): leaf work starts cheap, a hard test goes up.
    const bool may_escalate = !tid.empty() && !fix_task.empty();
    const std::string base_task = spec.task_text;
    std::string history;
    for (int attempt = 1; attempt <= 3; ++attempt) {
      spec.tier = tier;
      spec.attempt = attempt;
      spec.task_text = history.empty() ? base_task : base_task + "\n--- previous attempts (do not repeat them)\n" + history;
      LeafToolHost host(Sandbox(*s, block, Role::Tester), Role::Tester, *tests, {block});
      auto res = runner().run(spec, host);
      out.tokens += res.tokens;
      out.cost += res.cost;
      out.tier = tier;
      out.attempts = attempt;
      out.reads.insert(out.reads.end(), res.reads.begin(), res.reads.end());
      if (res.terminal_tool == "report") {
        auto rep = json::parse(res.terminal_input, nullptr, false);
        out.status = rep.is_object() ? rep.value("status", "done") : "done";
        if (rep.is_object()) {
          out.tests = sget(rep, "tests");
          out.cause = sget(rep, "cause");
          out.fix = sget(rep, "fix");
          out.files = sget(rep, "files");
        }
      } else {
        out.status = res.outcome == "cancelled" ? "cancelled" : "failed";
        out.cause = res.outcome;
      }
      out.test_text = host.last_test_text();
      if (out.status != "failed" || !may_escalate) return out;
      auto nt = next_tier(cfg, tier);
      if (nt.empty() || cfg.tier(nt) == nullptr) return out;
      history += fmt::format("attempt {} ({}): {} -> {}\n", attempt, tier, shorten(out.cause, 200), shorten(out.test_text, 200));
      say(fmt::format("tester {}: escalating {} -> {}", block, tier, nt));
      trail(tid, level_entry(core::trail_level(block), spec.role_name,
                             {{"at", block}, {"model", tier}, {"attempt", std::to_string(attempt)}, {"outcome", "failed"}}));
      tier = nt;
    }
    return out;
  }

  // --- ask (10.5) -------------------------------------------------------------------------

  std::string ask(const std::string& block, const std::string& question, const std::string& tid) {
    auto s = schema();
    if (!s) return s.error().message;
    const auto* b = s->find(block);
    if (b == nullptr) return "unknown block " + block;
    AgentSpec spec;
    spec.role = b->has_code() ? Role::Worker : Role::Navigator;
    spec.role_name = "ask";
    spec.block = block;
    spec.ticket = tid;
    spec.tier = tier_for("ask", "cheap", block);
    spec.system_text = prompt("asked", {{"block", block}});
    spec.context_text = context_for(b->has_code() ? Role::Worker : Role::Navigator, block);
    spec.task_text = "Question: " + question;
    spec.max_steps = 6;  // read a file or two, then answer; 4 ran out before the answer (T-0004)
    spec.max_tokens = cfg.max_tokens_per_run;
    spec.timeout = std::chrono::seconds(300);
    AskToolHost host(*idx, *s, block);
    auto res = runner().run(spec, host);
    std::string answer;
    if (res.terminal_tool == "answer") {
      auto j = json::parse(res.terminal_input, nullptr, false);
      if (j.is_object()) answer = sget(j, "text");
    }
    if (answer.empty()) answer = res.final_text.empty() ? "no answer (" + res.outcome + ")" : res.final_text;
    // At most ~80 tokens (10.5), cut on a code point and a word boundary, never mid-word.
    if (core::estimate_tokens(answer) > 80) {
      std::size_t keep = answer.size();
      while (keep > 20 && core::estimate_tokens(std::string_view(answer).substr(0, keep)) > 78) keep = keep * 9 / 10;
      auto cut = core::truncate_utf8(answer, keep);
      if (auto sp = cut.find_last_of(" \n"); sp != std::string::npos && sp > cut.size() / 2) cut.resize(sp);
      answer = cut + "...";
    }
    core::TrailEntry e;
    e.type = core::TrailEntry::Type::Answer;
    e.attrs = {{"at", block}};
    e.text = shorten(answer, 400);
    trail(tid, e);
    return answer;
  }

  // The assistant of a notes project (docs/adr/0039): one run, read-only tools and proposed edits.
  kk::Result<std::string> notes(const std::string& request, const std::string& scope) {
    auto s = schema();
    if (!s) return std::unexpected(s.error());
    if (!core::is_notes_project(s->project)) return kk::fail("notes", "not a notes project (project.ksl: lang notes)");
    if (core::trim(request).empty()) return kk::fail("notes", "an empty request");
    if (!scope.empty() && s->find(scope) == nullptr) return kk::fail("notes", "no note or topic " + scope);
    AgentSpec spec;
    spec.role = Role::Worker;
    spec.role_name = "notes";
    spec.block = scope;
    spec.tier = tier_for("notes", "medium", scope);
    spec.system_text = prompt("notes", {});
    spec.context_text = notes_overview(*s, "");
    std::string task = "Request of the human: " + std::string(core::trim(request)) + "\n";
    if (const auto* b = scope.empty() ? nullptr : s->find(scope)) {
      task += fmt::format("The request is about the {} {} \"{}\"{}.\n", b->kind == core::BlockKind::Group ? "topic" : "note", b->id,
                          b->purpose.value, b->kind == core::BlockKind::Group ? "" : " (read_note shows its text)");
    }
    spec.task_text = task;
    spec.max_steps = std::max(cfg.max_steps_worker, 12);
    spec.max_tokens = cfg.max_tokens_per_run;
    spec.timeout = std::chrono::seconds(cfg.run_timeout_s);
    say(fmt::format("notes assistant ({}): {}", spec.tier, shorten(std::string(core::trim(request)), 80)));
    NotesToolHost host(*s, scope);
    auto res = runner().run(spec, host);
    if (res.outcome == "cancelled") return kk::fail("cancelled", "stopped: the proposal was not made");
    if (res.outcome == "error" && host.edits().empty()) return kk::fail("notes", "the assistant failed: " + res.error);
    std::string out;
    std::string answer = host.answer().empty() ? res.final_text : host.answer();
    if (!answer.empty()) out += std::string(core::trim(answer)) + "\n";
    if (!res.terminal()) out += fmt::format("(the assistant stopped: {}{})\n", res.outcome, res.error.empty() ? "" : ", " + res.error);
    if (!host.edits().empty()) {
      NotesProposal p;
      p.request = std::string(core::trim(request));
      p.scope = scope;
      p.summary = host.summary().empty() ? fmt::format("{} edits", host.edits().size()) : host.summary();
      p.edits = host.edits();
      auto made = NotesProposalStore(root).create(std::move(p));
      if (!made) return std::unexpected(made.error());
      std::vector<std::string> what;
      for (const auto& e : made->edits) what.push_back((e.kind == "write" ? "~ " : "+ ") + e.id);
      out += fmt::format("proposal {}: {} ({})\n", made->id, made->summary, core::join(what, ", "));
      if (cfg.auto_approve) {
        auto r = approve_notes_proposal(root, made->id);
        out += r ? "auto: " + *r : "auto: not applied: " + r.error().message + "\n";
        reindex();
      } else {
        out += fmt::format("next: karkas proposal show {0}   karkas proposal approve {0}   karkas proposal reject {0}\n", made->id);
      }
    }
    out += fmt::format("cost ${:.4f} · {} steps · {}\n", res.cost, res.steps, res.model);
    return out;
  }

  SchemaCallbacks callbacks(const std::string& tid, const std::map<std::string, std::string>* answered = nullptr) {
    SchemaCallbacks cb;
    cb.ask = [this, tid](const std::string& b, const std::string& q) { return ask(b, q, tid); };
    cb.run_tests = [this](const std::string& b) {
      auto r = tests->build_and_test({b});
      return r ? r->text : r.error().message;
    };
    cb.check_change = [this](const std::string& diff) -> std::string {
      auto ok = check_proposal(root, diff);
      return ok ? std::string() : ok.error().message;
    };
    if (answered != nullptr) {
      cb.excluded = [answered](const std::string& b) -> std::string {
        for (const auto& [blk, why] : *answered) {
          if (core::Schema::is_under(blk, b)) return blk == b ? why : blk + " " + why;
        }
        return {};
      };
    }
    return cb;
  }
};

// =========================================================================================

kk::Result<std::unique_ptr<Engine>> Engine::create(fs::path root, core::Config cfg, EngineOptions opts) {
  std::unique_ptr<Engine> e(new Engine(std::move(root), std::move(cfg), std::move(opts)));
  auto idx = index::Index::open(e->root_);
  if (!idx) return std::unexpected(idx.error());
  e->impl_->idx = std::move(*idx);
  e->impl_->reindex();
  return e;
}

Engine::Engine(fs::path root, core::Config cfg, EngineOptions opts) : root_(core::normalize(root)), cfg_(std::move(cfg)) {
  impl_ = std::make_unique<Impl>(root_, cfg_, std::move(opts));
  auto& m = *impl_;
  if (m.opts.analyzer != nullptr) {
    m.analyzer = m.opts.analyzer;
  } else {
    m.own_analyzer = std::make_unique<code::CppAnalyzer>();
    m.analyzer = m.own_analyzer.get();
  }
  if (m.opts.tests != nullptr) {
    m.tests = m.opts.tests;
  } else {
    m.own_tests = std::make_unique<TestService>(root_, cfg_);
    m.tests = m.own_tests.get();
  }
  if (m.opts.providers) {
    m.providers = m.opts.providers;
  } else {
    m.http = llm::make_transport_from_env();
    m.providers = [this](const std::string& tier) -> kk::Result<std::unique_ptr<llm::IProvider>> {
      const auto* t = cfg_.tier(tier);
      if (t == nullptr) return kk::fail("config", fmt::format("no [models.{}] in karkas.toml", tier));
      // Mode "mcp" (ADR 0024): every turn goes to the MCP client; the engine stays the same.
      if (cfg_.mcp()) return std::unique_ptr<llm::IProvider>(std::make_unique<llm::McpBridgeProvider>(root_ / ".karkas" / "mcp", cfg_.mcp_wait_s));
      return llm::make_provider(*t, cfg_, *impl_->http);
    };
  }
  // Karkas serves its agents' turns itself, without a client session (ADR 0029).
  if (cfg_.mcp() && cfg_.mcp_launch == "claude" && !m.opts.providers) {
    m.launcher = std::make_unique<RunLauncher>(root_, cfg_);
    m.launcher->start();
  }
}

Engine::~Engine() = default;

index::Index& Engine::index() { return *impl_->idx; }

std::string Engine::context_for(Role role, const std::string& block, const std::string& task) {
  return impl_->context_for(role, block, task);
}

LeafOutcome Engine::run_tester(const std::string& block, const std::string& ticket) { return impl_->run_tester(block, ticket); }

LeafOutcome Engine::run_worker(const std::string& block, const std::string& ticket, const std::string& task,
                               const std::string& difficulty) {
  auto r = impl_->run_worker(block, ticket, task, difficulty);
  impl_->reindex();
  return r;
}

kk::Result<TestRun> Engine::test(const std::string& block) {
  std::vector<std::string> labels;
  if (!block.empty() && block != "all") labels.push_back(block);
  auto r = impl_->tests->build_and_test(labels);
  impl_->reindex();
  return r;
}

kk::Result<std::string> Engine::ask(const std::string& block, const std::string& question, const std::string& ticket) {
  return impl_->ask(block, question, ticket);
}

// --- 13.1 -----------------------------------------------------------------------------------

namespace {
std::set<std::string> frozen_set(const fs::path& root) {
  std::set<std::string> out;
  auto st = index::StateStore(root).load();
  if (st) {
    for (const auto& [k, _] : st->frozen_api) out.insert(k);
  }
  return out;
}

std::string issues_text(const std::vector<core::Issue>& issues) {
  std::string out;
  for (const auto& i : issues) out += i.str() + "\n";
  return out;
}
}  // namespace

namespace {

// What the architect is told about the human's prototype (docs/adr/0035).
std::string prototype_rules(const Sketch& s) {
  return "\n\nThe human drew a PROTOTYPE of this level (below). Build the schema from the idea AND the prototype. Keep its "
         "blocks (the same ids; a block may become a group, get a better purpose, kind or ports) and its dependencies (each "
         "'dep a -> b' stays a link between a and b, or between blocks inside them; a dependency of a group expanded later is "
         "kept by its expansion). Develop the rest freely: shared types, the entry, ports, blocks the idea needs. A renamed "
         "block goes into report.map ('prototype_id -> block_id'); a block or dependency of the prototype that is wrong for the "
         "idea is left out only with its reason in report.deviations, naming it. If the prototype is really bad in a critical "
         "place (a cycle of dependencies, a block with two jobs, a piece the idea cannot work without), write the cards as "
         "drawn and, after all cards are written, offer your variant of only those places with propose_alternative (title, "
         "reason, a KSL diff over your cards): the human chooses. Not a new design.\n" +
         sketch_text(s);
}

// The links the prototype asks of a group about to be expanded: kept by its children.
std::string prototype_hints(const fs::path& root, const core::Schema& schema, const std::string& group) {
  auto sk = load_sketch(root);
  if (!sk || sk->empty() || sk->sent.empty()) return {};
  auto check = check_sketch(*sk, shape_of(schema));
  std::vector<std::string> deps;
  for (std::size_t i = 0; i < sk->links.size() && i < check.links.size(); ++i) {
    if (check.links[i] == 1) continue;
    auto end = [&](const std::string& e) {
      auto it = check.found.find(e);
      return it != check.found.end() ? it->second : e;
    };
    auto a = end(sk->links[i].from);
    auto b = end(sk->links[i].to);
    if (a == group || b == group) deps.push_back(fmt::format("{} -> {}", a, b) + (sk->links[i].note.empty() ? "" : " (" + sk->links[i].note + ")"));
  }
  if (deps.empty()) return {};
  return "\nThe human's prototype also asks for these dependencies of " + group +
         "; keep each as a link between a child of it and the other block (or a block inside that): " + core::join(deps, "; ") + ".";
}

// The prototype after the architect: what is kept, renamed, left out, and the variants offered.
std::string prototype_summary(const fs::path& root, Sketch s, const core::Schema& schema, const ArchitectToolHost& host,
                              const std::string& sent) {
  // The file as it is now (the user may have moved the ghosts in karkas-gui while the architect worked).
  if (auto now = load_sketch(root); now && !now->empty() && now->scope == s.scope) s = *now;
  for (const auto& [k, v] : host.sketch_map()) s.map[k] = v;
  s.sent = sent;
  s.sent_at = core::unix_now();
  (void)save_sketch(root, s);
  auto c = check_sketch(s, shape_of(schema));
  auto pending = std::count(c.links.begin(), c.links.end(), -1);
  std::string out = fmt::format("prototype: {}/{} blocks kept, {}/{} dependencies kept{}\n", c.kept_blocks(), s.blocks.size(), c.kept_links(),
                                s.links.size(), pending > 0 ? fmt::format(", {} come with the expansion of their groups", pending) : "");
  for (const auto& [k, v] : s.map) out += fmt::format("renamed: {} -> {}\n", k, v);
  if (!host.deviations().empty()) out += "deviations: " + host.deviations() + "\n";
  for (const auto& g : c.gaps) out += "not kept: " + g + "\n";
  for (const auto& a : host.alternatives()) {
    auto id = a.substr(0, a.find(' '));
    out += fmt::format("alternative {} — approve it (karkas approve {}) or reject it (karkas change reject {})\n", a, id, id);
  }
  return out;
}

}  // namespace

kk::Result<std::string> Engine::design(const std::string& idea_in, const Sketch* sketch) {
  auto& m = *impl_;
  auto s = m.schema();
  if (!s) return std::unexpected(s.error());
  std::string idea = idea_in.empty() && sketch != nullptr ? sketch->idea : idea_in;
  if (sketch != nullptr && !sketch->scope.empty()) return kk::fail("design", "the prototype is drawn inside " + sketch->scope + ": karkas expand " + sketch->scope + " --sketch");
  if (!idea.empty() && s->project.idea.value != idea) {
    s->project.idea = {idea, {}, 0, 0, true};
    if (auto w = core::write_file(root_ / "project.ksl", core::write_project(s->project)); !w) return std::unexpected(w.error());
    m.reindex();
  }
  std::string the_idea = idea.empty() ? s->project.idea.value : idea;
  if (the_idea.empty()) return kk::fail("design", "no idea: karkas design \"<idea>\"");
  AgentSpec spec;
  spec.role = Role::Architect;
  spec.role_name = "architect";
  spec.block = "root";
  spec.tier = m.tier_for("architect", "strong", "");
  spec.system_text = m.prompt("architect", {{"job", "design the top level of the project"}, {"target", "the project"}});
  spec.context_text = m.context_for(Role::Architect, "");
  spec.task_text = fmt::format(
      "Idea: {}\nWrite the cards of 3-9 top-level blocks with write_card (ids without dots, plus shared.* under a "
      "'shared' group for common types), with purposes, kinds, threads, ports and links between leaves. Exactly one "
      "entry block. Groups get only purpose and kind now; they are expanded after approval.",
      the_idea);
  if (sketch != nullptr) spec.task_text += prototype_rules(*sketch);
  spec.max_steps = cfg_.max_steps_architect;
  spec.max_tokens = cfg_.max_tokens_per_run * 2;
  spec.timeout = std::chrono::seconds(cfg_.run_timeout_s);
  ArchitectToolHost host(root_, *m.idx, "", frozen_set(root_));
  if (sketch != nullptr) host.set_sketch(*sketch);
  auto res = m.runner().run(spec, host);
  m.reindex();
  if (res.outcome == "cancelled") return kk::fail("cancelled", "design stopped by the user");
  if (res.outcome == "error") return kk::fail("design", res.error);
  if (res.terminal_tool == "propose_change") {
    auto j = json::parse(res.terminal_input, nullptr, false);
    auto d = parse_diff(sget(j, "diff"));
    if (d) (void)m.changes.create("architect design", sget(j, "reason"), *d);
  }
  auto s2 = m.schema();
  std::string out = m.idx->overview();
  if (s2) out += issues_text(core::check_schema(*s2));
  out += fmt::format("architect: {} files written, {} steps, ${:.4f}\n", host.written().size(), res.steps, res.cost);
  if (sketch != nullptr && s2) out += prototype_summary(root_, *sketch, *s2, host, "design");
  out += "STOP: review the top level (karkas view, karkas gui), edit the cards if needed, then: karkas approve L0\n";
  return out;
}

kk::Result<std::string> Engine::expand(const std::string& block, bool all, const Sketch* sketch) {
  auto& m = *impl_;
  if (sketch != nullptr && (all || sketch->scope != block)) return kk::fail("expand", "the prototype is drawn on " + (sketch->scope.empty() ? std::string("the top level") : sketch->scope) + ": expand that group alone");
  auto expand_one = [&](const std::string& id) -> std::string {
    auto s = m.schema();
    if (!s) return s.error().message;
    const auto* b = s->find(id);
    if (b == nullptr) return "unknown block " + id + "\n";
    bool group = b->kind == core::BlockKind::Group;
    AgentSpec spec;
    spec.role = Role::Architect;
    spec.role_name = "architect";
    spec.block = id;
    spec.tier = m.tier_for("architect", "strong", id);
    std::error_code ec;
    auto api_dir = s->block_dir(id) / s->project.dirs.api;
    bool has_api = !group && fs::is_directory(api_dir, ec) && !fs::is_empty(api_dir, ec);
    std::string job = group ? fmt::format("decompose group {} into 2-9 children (ids {}.<child>) and write api/ headers "
                                          "for every child that is a leaf",
                                          id, id)
                      : has_api ? fmt::format("update the api/ headers of leaf {} to its card: keep the existing style, "
                                              "namespace and declarations, add or change only what the card requires",
                                              id)
                                : fmt::format("write the api/ headers of leaf {} from its card", id);
    spec.system_text = m.prompt("architect", {{"job", job}, {"target", id}});
    spec.context_text = m.context_for(Role::Architect, id);
    spec.task_text = "Do it now: " + job + ". Keep links consistent with the neighbors shown above.";
    if (group) spec.task_text += sketch != nullptr ? prototype_rules(*sketch) : prototype_hints(root_, *s, id);
    spec.max_steps = cfg_.max_steps_architect;
    spec.max_tokens = cfg_.max_tokens_per_run * 2;
    spec.timeout = std::chrono::seconds(cfg_.run_timeout_s);
    ArchitectToolHost host(root_, *m.idx, id, frozen_set(root_));
    if (sketch != nullptr && group) host.set_sketch(*sketch);
    auto res = m.runner().run(spec, host);
    m.reindex();
    m.say(fmt::format("expand {}: {} files, outcome {}", id, host.written().size(), res.outcome));
    std::string text = fmt::format("expand {}: {} files written ({}), ${:.4f}\n", id, host.written().size(), res.outcome, res.cost);
    if (sketch != nullptr && group) {
      if (auto after = m.schema()) text += prototype_summary(root_, *sketch, *after, host, "expand");
    }
    return text;
  };
  // L0 must be approved first (13.1 step 3).
  auto st = index::StateStore(root_).load();
  auto s = m.schema();
  if (!s) return std::unexpected(s.error());
  std::string out;
  if (st && st->l0_hash.empty()) out += "warning: L0 is not approved yet (karkas approve L0)\n";
  if (st && !st->l0_hash.empty() && st->l0_hash != index::l0_hash(*s)) out += "warning: L0 changed since it was approved\n";
  if (!all) return out + expand_one(block);
  std::set<std::string> done;
  for (int round = 0; round < 64; ++round) {
    if (core::cancel_requested()) return kk::fail("cancelled", out + "expand stopped by the user");
    auto cur = m.schema();
    if (!cur) return std::unexpected(cur.error());
    auto files = index::collect_code_files(*cur);
    std::string next;
    for (const auto& b : cur->blocks) {
      if (done.contains(b.id)) continue;
      if (!block.empty() && block != "all" && !core::Schema::is_under(block, b.id)) continue;
      bool group_empty = b.kind == core::BlockKind::Group && b.children.empty();
      bool leaf_no_api = (b.kind == core::BlockKind::Leaf || (b.kind == core::BlockKind::Shared && b.id != "shared.probe")) &&
                         index::block_api_hash(files, b.id).empty();
      if (group_empty || leaf_no_api) {
        next = b.id;
        break;
      }
    }
    if (next.empty()) break;
    done.insert(next);
    out += expand_one(next);
  }
  auto fin = m.schema();
  if (fin) out += issues_text(core::check_schema(*fin));
  out += "STOP: review the schema and the api/ headers, then: karkas approve api\n";
  return out;
}

kk::Result<std::string> Engine::approve(const std::string& what, const std::string& block) {
  auto& m = *impl_;
  auto s = m.schema();
  if (!s) return std::unexpected(s.error());
  index::StateStore store(root_);
  if (what == "L0") {
    auto issues = core::check_schema(*s);
    if (core::has_errors(issues)) {
      bool only_empty_groups = std::all_of(issues.begin(), issues.end(), [](const core::Issue& i) {
        return i.severity != 'E' || (i.code == "E06" && core::contains(i.message, "no children"));
      });
      if (!only_empty_groups) return kk::fail("approve", "fix the schema errors first:\n" + issues_text(issues));
    }
    if (auto r = store.approve_l0(index::l0_hash(*s)); !r) return std::unexpected(r.error());
    return std::string("L0 approved. next: karkas expand --all\n");
  }
  if (what == "api") {
    auto files = index::collect_code_files(*s);
    auto issues = core::check_schema(*s);
    if (core::has_errors(issues)) return kk::fail("approve", "fix the schema errors first:\n" + issues_text(issues));
    std::string out;
    int n = 0;
    for (const auto& b : s->blocks) {
      if (!b.has_code()) continue;
      if (!block.empty() && !core::Schema::is_under(block, b.id)) continue;
      // Approving the contracts also confirms blocks marked needs_review, including blocks
      // without an api/ of their own (the entry), which have nothing to freeze.
      (void)store.clear_review(b.id);
      auto h = index::block_api_hash(files, b.id);
      if (h.empty()) continue;
      if (auto r = store.freeze_api(b.id, h); !r) return std::unexpected(r.error());
      ++n;
    }
    out += fmt::format("api frozen for {} blocks\n", n);
    if (code::has_own_cmake(root_)) {
      // An imported project keeps its own build: skeletons and mocks of native leaves only, no
      // probe and no CMakeLists.txt (ADR 0016).
      auto s2 = m.schema();
      if (!s2) return std::unexpected(s2.error());
      (void)code::write_skel(*s2, "all");
      (void)code::write_mocks(*s2, "all");
      m.reindex();
      out += "the project has its own CMakeLists.txt: Karkas does not generate it (add new source files to it yourself)\n"
             "next: contract tests in the project's test suite, then the code of one leaf at a time\n";
      return out;
    }
    // 13.1 step 7: deterministic generation.
    auto s2 = m.schema();
    if (!s2) return std::unexpected(s2.error());
    (void)code::write_probe(*s2);
    auto s3 = m.schema();
    if (!s3) return std::unexpected(s3.error());
    (void)code::write_skel(*s3, "all");
    (void)code::write_mocks(*s3, "all");
    auto g = code::write_cmake(*s3);
    if (!g) return std::unexpected(g.error());
    m.reindex();
    out += cfg_.mcp() ? "generated: probe, skeletons, mocks, CMakeLists.txt\nnext: kk_implement (MCP; or `karkas implement all` in the background)\n"
                      : "generated: probe, skeletons, mocks, CMakeLists.txt\nnext: karkas implement all\n";
    return out;
  }
  if (what.starts_with("C-")) {
    auto c = m.changes.load(what);
    if (!c) return std::unexpected(c.error());
    if (c->status.value != "pending") return kk::fail("approve", fmt::format("{} is {}", what, c->status.value));
    auto r = apply_diff(root_, c->diff);
    if (!r) return std::unexpected(r.error());
    if (!r->issues.empty()) return kk::fail("approve", "the change breaks the schema:\n" + issues_text(r->issues));
    if (auto w = m.changes.set_status(what, "approved"); !w) return std::unexpected(w.error());
    std::string out = fmt::format("{} approved: {} card(s) written\n", what, r->files.size());
    // Contract changes of frozen blocks: unfreeze and mark dependents (12.4). Only card lines
    // touch a contract; a block that gains constants only keeps its frozen api/ (ADR 0026).
    auto st = store.load();
    std::set<std::string> touched;
    std::vector<std::string> fresh;
    std::string current;
    for (const auto& d : c->diff) {
      if (d.text.starts_with("@block ")) {
        current = std::string(core::trim(std::string_view(d.text).substr(7)));
        if (d.op == '+') fresh.push_back(current);
        continue;
      }
      if (!current.empty() && !is_const_line(d.text) &&
          std::find(fresh.begin(), fresh.end(), current) == fresh.end()) {
        touched.insert(current);
      }
    }
    // The mechanical part of the change, written by the program (ADR 0026): its constants, and the
    // declarations of new fn whose types the header can take. The workers find them done.
    std::vector<std::string> mech;
    std::set<std::string> const_blocks;
    if (auto consts = change_constants(c->diff); consts && !consts->empty()) {
      if (auto sc = core::load_schema(root_)) {
        if (auto w = write_constants(*sc, *consts)) {
          mech.insert(mech.end(), w->begin(), w->end());
        } else {
          out += "constants not written (the worker adds them): " + w.error().message + "\n";
        }
      }
      for (const auto& k : *consts) const_blocks.insert(k.block);
    }
    if (auto sd = core::load_schema(root_)) {
      auto decl = write_declarations(*sd, c->diff);
      mech.insert(mech.end(), decl.begin(), decl.end());
    }
    for (const auto& line : mech) out += line + "\n";
    // New constants keep a frozen contract: the frozen hash follows the header (no D05).
    if (auto sf = core::load_schema(root_)) {
      auto files = index::collect_code_files(*sf);
      for (const auto& id : const_blocks) {
        if (touched.contains(id) || !st || !st->frozen_api.contains(id)) continue;
        if (auto h = index::block_api_hash(files, id); !h.empty()) (void)store.freeze_api(id, h);
      }
    }
    m.reindex();
    for (const auto& id : touched) {
      if (st && st->frozen_api.contains(id)) {
        (void)store.unfreeze_api(id);
        for (const auto& dep : m.idx->dependent_blocks(id)) (void)store.mark_review(dep, "contract of " + id + " changed by " + what);
        out += fmt::format("{}: api unfrozen, dependents marked needs_review\n", id);
      }
    }
    // Note on the originating ticket ("orchestrator T-0051"), naming the blocks it opened: the
    // orchestrator reads it and goes there, not to a block unfrozen long before.
    std::vector<std::string> opened;
    for (const auto& id : touched) {
      if (st && st->frozen_api.contains(id)) opened.push_back(id);
    }
    for (const auto& part : core::split_ws(c->from.value)) {
      if (core::is_record_id(part) && part[0] == 'T') {
        m.note(part, what + " approved" +
                         (opened.empty() ? std::string() : ": api of " + core::join(opened, ", ") + " unfrozen, its worker updates it") +
                         (mech.empty() ? std::string() : "; written by the program: " + core::join(mech, "; ")));
      }
    }
    // New blocks need an api/ from the architect, except imported code (files), which
    // already has it; groups made only of imported blocks need nothing either.
    auto fresh_schema = core::load_schema(root_);
    auto needs_expand = [&](const std::string& id) {
      if (!fresh_schema) return true;
      const auto* b = fresh_schema->find(id);
      if (b == nullptr) return false;
      if (b->external()) return false;
      if (b->kind == core::BlockKind::Group) {
        auto leaves = fresh_schema->leaves_under(id);
        return leaves.empty() || std::any_of(leaves.begin(), leaves.end(), [](const core::Block* l) { return !l->external(); });
      }
      return true;
    };
    std::size_t imported = 0;
    for (const auto& id : fresh) {
      if (needs_expand(id)) {
        out += fmt::format("next: karkas expand {}  (architect writes the api/)\n", id);
      } else {
        ++imported;
      }
    }
    if (imported > 0 && imported == fresh.size()) {
      out += "the blocks describe existing code (files): see them with karkas gui or karkas view\n";
      m.reindex();
      return out;
    }
    // A change that came from a ticket continues there: the worker of each unfrozen block
    // brings its api/ in line with the card and implements it (ADR 0020).
    std::vector<std::string> from_tickets;
    for (const auto& part : core::split_ws(c->from.value)) {
      if (core::is_record_id(part) && part[0] == 'T') from_tickets.push_back(part);
    }
    if (!from_tickets.empty() && !touched.empty()) {
      for (const auto& tid : from_tickets) {
        out += fmt::format("next: karkas ticket run {}  (the workers update the api/ of {} to the cards)\n", tid,
                           core::join(std::vector<std::string>(touched.begin(), touched.end()), ", "));
      }
      out += "then: karkas approve api (freezes the updated contracts)\n";
      m.reindex();
      return out;
    }
    for (const auto& id : touched) out += fmt::format("next: karkas expand {}  (the architect updates the api/ to the new card)\n", id);
    out += "then: karkas approve api, karkas implement <block>\n";
    m.reindex();
    return out;
  }
  return kk::fail("approve", "approve L0 | api [block] | C-NNNN");
}

kk::Result<std::string> Engine::reject(const std::string& change_id) {
  if (auto r = impl_->changes.set_status(change_id, "rejected"); !r) return std::unexpected(r.error());
  impl_->reindex();
  return change_id + " rejected\n";
}

kk::Result<std::string> Engine::implement(const std::string& target) {
  auto& m = *impl_;
  auto s = m.schema();
  if (!s) return std::unexpected(s.error());
  std::string scope = target == "all" ? "" : target;
  auto issues = core::check_schema(*s);
  if (core::has_errors(issues)) return kk::fail("implement", "schema errors block the agents:\n" + issues_text(issues));
  auto st = index::StateStore(root_).load();
  auto files = index::collect_code_files(*s);
  std::vector<std::string> leaves;
  std::vector<std::string> not_frozen;
  for (const auto& b : s->blocks) {
    if (!core::Schema::is_under(scope, b.id)) continue;
    bool impl_needed = b.kind == core::BlockKind::Leaf ||
                       (b.kind == core::BlockKind::Shared && std::any_of(b.ports.begin(), b.ports.end(), [](const core::Port& p) {
                          return p.dir == core::PortDir::Fn;
                        }));
    if (!impl_needed) continue;
    leaves.push_back(b.id);
    if (!st || !st->frozen_api.contains(b.id)) not_frozen.push_back(b.id);
  }
  if (!not_frozen.empty()) {
    return kk::fail("implement", "api/ is not frozen for: " + core::join(not_frozen, ", ") + " (karkas approve api)");
  }
  (void)code::write_probe(*s);
  (void)code::write_skel(*s, scope.empty() ? "all" : scope);
  (void)code::write_mocks(*s, scope.empty() ? "all" : scope);
  (void)code::write_cmake(*s);
  m.reindex();
  std::string out;
  std::mutex out_mu;
  auto parallel = [&](const std::vector<std::string>& items, const std::function<std::string(const std::string&)>& fn) {
    std::counting_semaphore<64> sem(std::clamp(cfg_.max_parallel_agents, 1, 64));
    std::vector<std::future<void>> futs;
    for (const auto& id : items) {
      futs.push_back(std::async(std::launch::async, [&, id] {
        sem.acquire();
        auto line = core::cancel_requested() ? fmt::format("{}: skipped (stopped by the user)\n", id) : fn(id);
        sem.release();
        std::lock_guard lk(out_mu);
        out += line;
      }));
    }
    for (auto& f : futs) f.get();
  };
  auto has_contract = [&](const std::string& id) {
    auto dir = s->block_dir(id) / s->project.dirs.tests / "contract";
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (core::is_code_file(e.path())) return true;
    }
    return false;
  };
  // Phase 1: contract tests before the implementation (12.1).
  std::vector<std::string> need_tests;
  for (const auto& id : leaves) {
    const auto* b = s->find(id);
    if (b->kind == core::BlockKind::Leaf && !has_contract(id)) need_tests.push_back(id);
  }
  m.say(fmt::format("implement: {} leaves, {} need contract tests", leaves.size(), need_tests.size()));
  parallel(need_tests, [&](const std::string& id) {
    auto r = m.run_tester(id, "");
    return fmt::format("tester {}: {} tier={} ${:.4f}\n", id, r.status, r.tier, r.cost);
  });
  if (auto s2 = m.schema()) {
    (void)code::write_mocks(*s2, scope.empty() ? "all" : scope);
    (void)code::write_cmake(*s2);
  }
  m.reindex();
  // Phase 2: workers, in parallel: interfaces are frozen, so every leaf only needs
  // the api/ of its neighbors (13.1 step 9).
  std::vector<std::string> todo;
  {
    std::lock_guard lk(m.mu);
    auto snap = m.idx->snapshot();
    for (const auto& id : leaves) {
      const auto* row = snap.block(id);
      if (row == nullptr || row->status != "passing") todo.push_back(id);
    }
  }
  if (core::cancel_requested()) return kk::fail("cancelled", "implement stopped by the user:\n" + out);
  parallel(todo, [&](const std::string& id) {
    auto r = m.run_worker(id, "", "Implement the api/ contract of this leaf in impl/ so that every contract test passes.", "");
    return fmt::format("worker {}: {} tier={} attempts={} ${:.4f}\n", id, r.status, r.tier, r.attempts, r.cost);
  });
  m.reindex();
  if (core::cancel_requested()) return kk::fail("cancelled", "implement stopped by the user:\n" + out);
  // Phase 3: composition root by a medium worker (13.1 step 10).
  for (const auto& b : s->blocks) {
    if (b.kind != core::BlockKind::Entry || !core::Schema::is_under(scope, b.id)) continue;
    auto impl_files = index::collect_code_files(*s);
    bool skeleton = std::none_of(impl_files.begin(), impl_files.end(), [&](const index::CodeFile& f) {
      return f.block == b.id && f.role == "impl" && !f.skeleton;
    });
    if (!skeleton) continue;
    auto r = m.run_worker(b.id, "", "Write the composition root: create all leaves and wire them along the links.", "",
                          "composer", m.tier_for("composer", "medium", b.id));
    out += fmt::format("composer {}: {} ${:.4f}\n", b.id, r.status, r.cost);
  }
  // Phase 4: integration tests of groups (12.3).
  std::vector<std::string> groups;
  for (const auto& b : s->blocks) {
    if (b.kind != core::BlockKind::Group || !core::Schema::is_under(scope, b.id) || b.id == "shared") continue;
    auto dir = root_ / "tests" / "integration" / b.id;
    std::error_code ec;
    if (!fs::exists(dir, ec)) groups.push_back(b.id);
  }
  if (core::cancel_requested()) return kk::fail("cancelled", "implement stopped by the user:\n" + out);
  parallel(groups, [&](const std::string& id) {
    auto r = m.run_tester(id, "", true);
    return fmt::format("integration tester {}: {} ${:.4f}\n", id, r.status, r.cost);
  });
  auto fin = m.tests->build_and_test(scope.empty() ? std::vector<std::string>{} : std::vector<std::string>{scope});
  m.reindex();
  out += fin ? fin->text : "final test run failed: " + fin.error().message + "\n";
  out += m.idx->status().value_or("");
  return out;
}

// --- tickets (13.2, 13.3) --------------------------------------------------------------------

namespace {
// The scope of a new ticket: a known block; "" / "root" = the whole project.
kk::Result<std::string> checked_scope(index::Index& idx, const std::string& scope) {
  if (scope.empty() || scope == "root") return std::string();
  if (idx.snapshot().block(scope) == nullptr) return kk::fail("ticket", "unknown block " + scope + " (the scope of the request)");
  return scope;
}
}  // namespace

kk::Result<std::string> Engine::notes(const std::string& request, const std::string& scope) { return impl_->notes(request, scope); }

kk::Result<std::string> Engine::task(const std::string& description, const std::string& scope) {
  // A notes project has no tickets of code: its requests go to the notes assistant (docs/adr/0039).
  if (auto s = impl_->schema(); s && core::is_notes_project(s->project)) return impl_->notes(description, scope);
  auto sc = checked_scope(*impl_->idx, scope);
  if (!sc) return std::unexpected(sc.error());
  auto t = impl_->tickets.create(core::TicketKind::Feature, "human", description, {}, *sc);
  if (!t) return std::unexpected(t.error());
  impl_->say("created " + t->id);
  return run_ticket(t->id);
}

kk::Result<std::string> Engine::debug(const std::string& symptom, const std::vector<std::string>& facts,
                                      const std::string& origin, const std::string& scope) {
  if (auto s = impl_->schema(); s && core::is_notes_project(s->project)) {
    return kk::fail("notes", "notes have no code to debug: write a request to the notes assistant (karkas notes \"...\")");
  }
  auto sc = checked_scope(*impl_->idx, scope);
  if (!sc) return std::unexpected(sc.error());
  auto t = impl_->tickets.create(core::TicketKind::Bug, origin, symptom, facts, *sc);
  if (!t) return std::unexpected(t.error());
  impl_->say("created " + t->id);
  return run_ticket(t->id);
}

kk::Result<std::string> Engine::run_ticket(const std::string& id, bool force, const std::string& note) {
  auto& m = *impl_;
  auto lock = m.tickets.lock(id, "karkas run_ticket");
  if (!lock) return std::unexpected(lock.error());
  // The human's message about the ticket (the chat in ticket mode): a fact every agent sees, and
  // a reason to run again whatever the last run ended with.
  if (!core::trim(note).empty()) {
    if (auto r = m.tickets.add_human_fact(id, note); !r) return std::unexpected(r.error());
    force = true;
    m.say(id + ": the human's message added as a fact");
  }
  auto t = m.tickets.load(id);
  if (!t) return std::unexpected(t.error());
  // A closed ticket runs again only on purpose (--force): its result did not convince the human, or
  // it closed too early (flappy T-0014 before its fix of the engine closed with its HUD part undone).
  if (t->status.value == "closed" && !force) return id + " is closed (karkas ticket run " + id + " --force reopens it)\n";
  // Statuses, drift and test freshness as the project is now (a human may have edited cards or
  // code since the last command): the agents' tools and notes read the index.
  m.reindex();
  auto s0 = m.schema();
  if (!s0) return std::unexpected(s0.error());
  if (core::has_errors(core::check_schema(*s0))) {
    return kk::fail("ticket", "schema errors (E) block the agents; run karkas check");
  }
  // The same run twice gives the same answer: when the last run stopped on something only a
  // human resolves and nothing changed since, no model is started (ADR 0020).
  if (!force) {
    if (auto why = m.blocked_reason(*t); !why.empty()) return kk::fail("ticket", why);
  }
  bool bug = t->kind_enum() == core::TicketKind::Bug;
  // The block the human limited the ticket to (docs/adr/0034): the descent stays inside it.
  const std::string limit = t->scope.set ? t->scope.value : std::string();
  if (!limit.empty() && s0->find(limit) == nullptr) {
    return kk::fail("ticket", fmt::format("{}: its scope {} is not a block any more (edit the ticket's scope line)", id, limit));
  }
  m.set_status(id, core::TicketStatus::Descending);
  std::string current = "root";
  std::string difficulty;
  std::map<std::string, int> not_here;         // scope → count of children that said not_here
  std::map<std::string, std::string> answered;  // block → what it answered in this run (not sent again)
  std::vector<core::ResultLine> done_lines;     // leaves finished in this run (a feature over blocks)
  std::vector<std::string> done_blocks;
  std::set<std::string> inv_tested;             // leaves whose new invariants went to the tester in this run
  std::map<std::string, std::string> retask;    // leaf → what its next worker run must do first
  // The plan of a feature (ADR 0026): the blocks the orchestrator listed in `next`, sent one after
  // another without asking it again; it hears of the ticket only when a block does not end done.
  struct PlanStep {
    std::string block, difficulty, why;
  };
  std::deque<PlanStep> plan;
  bool planned = false;
  int plan_no = 0;
  int plan_total = 0;
  bool reviewed = false;     // one review of the orchestrator's change by its leaves per run
  std::string review_gaps;   // what the leaves found, for the orchestrator's next decision
  bool linted = false;          // one check of the orchestrator's change for fns named only in notes (ADR 0029)
  bool behavior_tested = false;  // the behaviour test of an imported feature ran (ADR 0029)
  std::string behavior_failure;  // its failure, for the orchestrator's next decision
  std::string behavior_retask;   // and for the worker it sends to fix it
  int behavior_fixes = 0;        // sends left to fix a failing behaviour test, each a tier higher (ADR 0029)
  bool behavior_fix_now = false; // the worker of this step fixes it
  std::string behavior_passed;   // the fix made the test pass: said to the orchestrator once
  bool escalate_orch = false;   // a decision of the orchestrator came back: the next ones are a tier higher (ADR 0029)
  bool escalation_said = false;
  int root_visits = 0;
  int build_repairs = 0;  // full builds of an imported project sent back to a block (ADR 0028)
  std::string build_failure;  // a full build no error of which names a block's file: for the orchestrator (ADR 0030)
  std::string build_retask;   // and for the worker it sends next
  constexpr int kMaxRootVisits = 8;  // orchestrator decisions in one run (leaves of a feature + bounces)
  constexpr int kMaxLeavesDone = 10;  // leaves of one feature in one run (a plan has up to 9)
  constexpr int kLikelyBlocks = 8;    // candidates the code index gives the orchestrator (ADR 0028)
  metrics::Tokens total;
  double cost = 0;
  const std::int64_t started = core::unix_now();  // the runs of this ticket from now on (its cost line)
  // The behaviour tests of this ticket for the project's test command (ADR 0029): a list of another
  // ticket is gone, this ticket's goes when it ends.
  const fs::path ticket_tests = root_ / ".karkas" / "cache" / "ticket_tests.txt";
  if (auto old_list = core::read_file(ticket_tests);
      old_list && !old_list->starts_with("# " + id + "\n") && !old_list->starts_with("# " + id + " ")) {
    std::error_code ec;
    fs::remove(ticket_tests, ec);
  }
  // The ticket's cost line. In mode mcp with launched sessions (ADR 0029) the sessions' own cost, read when they
  // have ended (docs/adr/0031): the estimate from the turns was 3-5x below it on Endless Sky.
  auto cost_line = [&]() -> std::string {
    if (cfg_.mcp() && m.launcher) {
      (void)m.launcher->settle(30);
      auto tc = ticket_cost(m.root, id, started);
      if (tc.sessions > 0) {
        return fmt::format("in={} out={} cache_write={} cache_read={} usd={:.4f} ({} of {} run(s) at the cost their Claude Code "
                           "session reported, at list prices{})",
                           tc.input, tc.output, tc.cache_write, tc.cache_read, tc.usd, tc.sessions, tc.runs,
                           tc.sessions < tc.runs ? "; the others estimated from their turns" : "");
      }
    }
    return fmt::format("in={} out={} cache_write={} cache_read={} usd={:.4f}{}", total.input, total.output, total.cache_write(),
                       total.cache_read, cost, cfg_.mcp() ? " (MCP: an estimate of the client's tokens from the turns)" : "");
  };
  auto finish = [&](core::TicketStatus st, std::vector<core::ResultLine> lines) -> std::string {
    {
      std::error_code ec;
      fs::remove(ticket_tests, ec);
    }
    // The status line stays first: it starts a result block (the path view finds the latest).
    if (!done_lines.empty()) lines.insert(lines.begin() + (lines.empty() ? 0 : 1), done_lines.begin(), done_lines.end());
    lines.push_back({"state", m.state_hash(), {}, 0});
    lines.push_back({"cost", cost_line(), {}, 0});
    (void)m.tickets.add_result(id, lines);
    m.set_status(id, st);
    m.reindex();
    auto fin = m.tickets.load(id);
    return fin ? core::write_ticket(*fin) : id + "\n";
  };
  // Stopped by the user (ADR 0019): a note on the trail, the ticket open again so that
  // `karkas ticket run` continues it; no escalation.
  auto stopped = [&](const std::string& role, const std::string& at) -> kk::Result<std::string> {
    m.note(id, fmt::format("stopped by the user at {}{}", at, role.empty() ? "" : " (" + role + ")"));
    // Blocks finished before the stop keep their result lines (what changed, which files).
    if (!done_lines.empty()) {
      std::vector<core::ResultLine> lines = {{"status", "stopped", {}, 0}};
      lines.insert(lines.end(), done_lines.begin(), done_lines.end());
      // The fingerprint like any result: the next run tells "the project changed since" from
      // "nothing changed" (without it, the orchestrator was told "an older run").
      lines.push_back({"state", m.state_hash(), {}, 0});
      lines.push_back({"cost", cost_line(), {}, 0});
      (void)m.tickets.add_result(id, lines);
    }
    m.set_status(id, core::TicketStatus::Open);
    m.reindex();
    m.say(id + " stopped by the user");
    return kk::fail("cancelled", fmt::format("{} stopped by the user at {}; continue with: karkas ticket run {}", id, at, id));
  };
  // What a leaf answered goes on the trail as a fact right after its outcome: the agents
  // above see it in their path view (earlier descents), not only the human in the result.
  auto answer_fact = [&](const std::string& block, const std::string& status, const std::string& text) {
    if (text.empty()) return;
    core::TrailEntry f;
    f.type = core::TrailEntry::Type::Fact;
    f.text = shorten(block + " " + status + ": " + text, 240);
    m.trail(id, f);
  };
  // "Auto" ([agents] auto_approve): a proposed change is approved at once and the descent goes
  // back to the orchestrator, which now finds the unfrozen api/ in its scope. At most 3 per run.
  int auto_approved = 0;
  auto auto_approve = [&](const std::string& change) -> bool {
    if (!cfg_.auto_approve || auto_approved >= 3) return false;
    auto a = approve(change);
    if (!a) {
      m.note(id, fmt::format("auto-approval of {} failed: {}", change, shorten(a.error().message, 200)));
      return false;
    }
    ++auto_approved;
    m.note(id, change + " approved automatically (auto_approve)");
    m.say(fmt::format("{}: {} approved automatically", id, change));
    current = "root";
    return true;
  };
  // Auto approves a worker's card diff in its own run, which goes on with the new card (ADR 0026).
  auto approve_now = [&](const std::string& block) -> LeafToolHost::ApproveNow {
    if (!cfg_.auto_approve) return {};
    return [&, block](const std::string& reason, const std::string& diff) -> kk::Result<std::string> {
      if (auto_approved >= 3) return kk::fail("auto", "three changes were approved automatically in this run already");
      auto c = propose_change(root_, "worker " + id, shorten(reason.empty() ? "contract change requested by the worker" : reason, 200), diff);
      if (!c) return std::unexpected(c.error());
      auto a = approve(c->id);
      if (!a) {
        m.note(id, fmt::format("auto-approval of {} failed: {}", c->id, shorten(a.error().message, 200)));
        return std::unexpected(a.error());
      }
      ++auto_approved;
      m.note(id, fmt::format("{} approved automatically (auto_approve); the worker of {} goes on in the same run", c->id, block));
      m.say(fmt::format("{}: {} approved automatically", id, c->id));
      m.reindex();
      std::string card;
      {
        std::lock_guard lk(m.mu);
        auto v = m.idx->view(block, 2);
        card = v ? *v : "";
      }
      std::string written;
      for (const auto& line : core::split(*a, '\n')) {
        if (core::contains(line, "written to api/") || core::contains(line, ": declared `")) written += line + "\n";
      }
      // An imported block has headers, not api/, and a change of it may only give it files (ADR 0027).
      auto now = m.schema();
      const auto* nb = now ? now->find(block) : nullptr;
      if (nb != nullptr && nb->external()) {
        return fmt::format("{} approved automatically (Auto). The card of {} now reads:\n{}\n{}The headers of this block are "
                           "unfrozen for this change and every file of its card (a new one too) is writable: add the "
                           "declarations the card names, if any, do your part and report done (api=changed only when you "
                           "changed a declaration of a header). Other blocks of the change are sent after yours.",
                           c->id, block, card, written.empty() ? "" : "Written by the program:\n" + written);
      }
      return fmt::format("{} approved automatically (Auto). The card of {} now reads:\n{}\n{}api/ is unfrozen for this change: "
                         "bring it in line with the card, then the impl, and report done with api=changed when the tests "
                         "pass. Other blocks of the change are sent after yours.",
                         c->id, block, card, written.empty() ? "" : "Written by the program:\n" + written);
    };
  };
  auto read_plan = [](const json& in) {
    std::deque<PlanStep> out;
    if (in.contains("next") && in["next"].is_array()) {
      for (const auto& st : in["next"]) {
        if (st.is_object()) out.push_back({sget(st, "block"), sget(st, "difficulty"), sget(st, "why")});
      }
    }
    return out;
  };
  auto plan_text = [](const std::deque<PlanStep>& steps) {
    std::vector<std::string> parts;
    for (const auto& st : steps) parts.push_back(st.block + " (" + st.difficulty + ")");
    return core::join(parts, ", ");
  };
  // Sends the next block of the plan, as if the orchestrator dispatched it.
  auto start_step = [&]() {
    auto st = plan.front();
    plan.pop_front();
    ++plan_no;
    m.trail(id, level_entry(0, "orchestrator",
                            {{"at", "root"}, {"pick", st.block}, {"difficulty", st.difficulty}, {"why", shorten(st.why, 160)},
                             {"plan", fmt::format("{}/{}", plan_no, plan_total)}}));
    m.say(fmt::format("plan {}/{}: {}", plan_no, plan_total, st.block));
    difficulty = st.difficulty;
    current = st.block;
  };
  // Blocks the approved changes of this ticket change in their cards that no run of the ticket
  // finished: a plan that skipped one goes back to the orchestrator instead of closing.
  auto scope_not_done = [&]() {
    std::set<std::string> done(done_blocks.begin(), done_blocks.end());
    if (auto tk = m.tickets.load(id)) {
      for (const auto& r : tk->result) {
        if (r.key != "fix" && r.key != "files") continue;
        auto colon = r.text.find(':');
        if (colon != std::string::npos) done.insert(r.text.substr(0, colon));
      }
    }
    std::vector<std::string> out;
    if (auto cs = m.changes.list()) {
      for (const auto& c : *cs) {
        if (c.status.value != "approved") continue;
        auto from = core::split_ws(c.from.value);
        if (std::find(from.begin(), from.end(), id) == from.end()) continue;
        std::string at;
        for (const auto& d : c.diff) {
          if (d.text.starts_with("@block ")) {
            at = std::string(core::trim(std::string_view(d.text).substr(7)));
            continue;
          }
          if (at.empty() || is_const_line(d.text) || done.contains(at)) continue;
          if (std::find(out.begin(), out.end(), at) == out.end()) out.push_back(at);
        }
      }
    }
    return out;
  };
  // After a finished leaf of a feature: the next block of the plan, the end of the plan, or the
  // orchestrator. Returns the finished ticket when the plan closes it.
  auto after_done = [&]() -> std::optional<std::string> {
    if (!planned) {
      current = "root";
      return std::nullopt;
    }
    if (!plan.empty()) {
      start_step();
      return std::nullopt;
    }
    planned = false;
    if (auto missing = scope_not_done(); !missing.empty()) {
      m.note(id, "the plan is done, but the approved changes of this ticket also change " + core::join(missing, ", ") +
                     ": back to the orchestrator");
      current = "root";
      return std::nullopt;
    }
    m.trail(id, level_entry(0, "orchestrator", {{"at", "root"}, {"outcome", "closed"}, {"why", "the planned blocks are done"}}));
    return finish(core::TicketStatus::Closed,
                  {{"status", "done", {}, 0}, {"note", "the planned blocks are done: " + core::join(done_blocks, ", "), {}, 0}});
  };
  // A plan that waited for a human's approval goes on after it (the orchestrator is not asked again).
  auto plan_file = m.root / ".karkas" / "tickets" / (id + ".plan.json");
  if (std::error_code ec; fs::exists(plan_file, ec)) {
    auto j = json::parse(core::read_file(plan_file).value_or("{}"), nullptr, false);
    auto change = j.is_object() ? sget(j, "change") : std::string();
    auto c = change.empty() ? kk::Result<core::Change>(kk::fail("plan", "none")) : m.changes.load(change);
    if (c && c->status.value == "approved" && !bug) {
      plan = read_plan(j);
      planned = !plan.empty();
      plan_total = static_cast<int>(plan.size());
      if (planned) {
        m.note(id, change + " approved: the plan goes on with " + plan_text(plan));
        start_step();
      }
    }
    if (!c || c->status.value != "pending") fs::remove(plan_file, ec);
  }
  // A scoped ticket's first run starts at its block: no orchestrator decision is needed to pick it
  // (deterministic first, 0.2); later runs and anything that comes back up go to the orchestrator,
  // which may pick only inside the scope.
  if (!limit.empty() && !planned &&
      std::none_of(t->trail.begin(), t->trail.end(), [](const core::TrailEntry& e) { return e.type == core::TrailEntry::Type::Level; })) {
    m.trail(id, level_entry(0, "human", {{"at", "root"}, {"pick", limit}, {"why", "the human limited the request to this block"}}));
    m.say(fmt::format("{}: scoped to {}", id, limit));
    current = limit;
  }
  for (int guard = 0; guard < 30; ++guard) {
    if (core::cancel_requested()) return stopped("", current);
    auto s = m.schema();
    if (!s) return std::unexpected(s.error());
    auto ticket = m.tickets.load(id);
    if (!ticket) return std::unexpected(ticket.error());
    if (!limit.empty() && current != "root" && !core::Schema::is_under(limit, current)) {
      // Going up past the scope (not_here twice on its level): the orchestrator decides, inside the scope.
      m.note(id, fmt::format("{} is outside the scope {} the human gave: back to the orchestrator", current, limit));
      current = "root";
    }
    const core::Block* b = current == "root" ? nullptr : s->find(current);
    if (current != "root" && b == nullptr) return kk::fail("ticket", "unknown block " + current);

    if (current == "root" || b->kind == core::BlockKind::Group) {
      bool orch = current == "root";
      if (orch && ++root_visits > kMaxRootVisits) {
        m.note(id, fmt::format("the orchestrator decided {} times in one run without an end", kMaxRootVisits));
        return finish(core::TicketStatus::Escalated,
                      {{"status", "escalated", {}, 0},
                       {"note", "the descent did not converge in one run; the earlier descents are in the path view (karkas ticket show " + id + " --path --at root)", {}, 0}});
      }
      std::string role = orch ? "orchestrator" : "navigator";
      std::string tier = orch ? m.tier_for("orchestrator", "strong", "")
                              : (m.navigator_tier.contains(current) ? m.navigator_tier[current]
                                                                    : m.tier_for("navigator", "medium", current));
      // A cheaper orchestrator whose change or block came back decides again a tier higher (ADR 0029).
      if (orch && escalate_orch) {
        if (auto up = next_tier(cfg_, tier); !up.empty()) {
          if (!escalation_said) {
            m.note(id, fmt::format("the orchestrator decides on tier {} from here (was {}): a decision of it came back", up, tier));
            escalation_said = true;
          }
          tier = up;
        }
      }
      // A decision below the strongest tier gets its change reviewed by the leaves it touches.
      const bool weaker_orchestrator = orch && !next_tier(cfg_, tier).empty();
      AgentSpec spec;
      spec.role = orch ? Role::Orchestrator : Role::Navigator;
      spec.role_name = role;
      spec.block = current;
      spec.ticket = id;
      spec.tier = tier;
      spec.system_text = m.prompt(role, {{"block", current}});
      spec.context_text = m.context_for(spec.role, current);
      std::string decide;
      if (!orch) {
        decide = "\nDecide: dispatch to a child or report not_here.";
      } else if (!done_blocks.empty()) {
        decide = fmt::format("\nFinished in this run: {}. Decide: dispatch the next block that still has to change for "
                             "this ticket, or close with verdict done when the whole request is in place (escalate if "
                             "only a human can go on).",
                             core::join(done_blocks, ", "));
      } else {
        decide = "\nDecide: dispatch, propose_change, escalate or close.";
      }
      // Is the last result still about the project as it is? Its advice ("a human adds X")
      // may be done already: the state fingerprint tells.
      if (orch && root_visits == 1 && !ticket->result.empty()) {
        std::string last_state;
        for (const auto& r : ticket->result) {
          if (r.key == "status") last_state.clear();
          if (r.key == "state") last_state = r.text;
        }
        if (last_state.empty()) {
          decide += "\nThe last result comes from an older run without a state fingerprint: check the current status of "
                    "the blocks it names before you act on its advice.";
        } else if (last_state != m.state_hash()) {
          decide += "\nThe cards or api/ changed since the last result: its advice may be done or outdated; check the "
                    "current status of the blocks it names before you act on it.";
        } else {
          decide += "\nNothing changed in the cards or api/ since the last result.";
        }
        if (force) {
          decide += "\nThe human ran this ticket again on purpose (--force): the last result's conclusion did not convince "
                    "them. Check the current status yourself before you repeat it.";
        }
      }
      // The agreed scope of a feature: the approved changes of this ticket, their reasons and
      // blocks. A worker's "other blocks need X" is checked against it, not taken as an order.
      if (orch) {
        if (auto cs = m.changes.list()) {
          std::string scope;
          for (const auto& c : *cs) {
            if (c.status.value != "approved") continue;
            auto from = core::split_ws(c.from.value);
            if (std::find(from.begin(), from.end(), id) == from.end()) continue;
            std::vector<std::string> blocks;
            for (const auto& d : c.diff) {
              if (d.text.starts_with("@block ")) blocks.push_back(std::string(core::trim(std::string_view(d.text).substr(7))));
            }
            scope += fmt::format("\n- {} ({}): {}", c.id, core::join(blocks, ", "), shorten(c.reason.value, 200));
          }
          if (!scope.empty()) {
            decide += "\nApproved changes of this ticket (the agreed scope; a block outside it needs a reason in the ticket, "
                      "not only a worker's guess):" + scope;
          }
        }
      }
      if (orch && !plan.empty()) {
        decide += "\nPlanned and not sent yet: " + plan_text(plan) + " (a new dispatch replaces the plan; list them in next to keep them).";
      }
      if (orch && !behavior_failure.empty() && behavior_failure.starts_with("The behaviour test is in doubt")) {
        decide += "\n" + behavior_failure +
                  "\nClose with verdict done when the facts show the request is in place, or escalate for a human to check "
                  "the test.";
        behavior_failure.clear();
      }
      if (orch && !behavior_failure.empty()) {
        decide += "\n" + behavior_failure +
                  "\nDispatch the block whose code must change — a block done in this run may get it once more, with the test's "
                  "output as its task (the test stays as it is; its worker can run it with test()) — or escalate when the test is "
                  "what is wrong.";
        behavior_failure.clear();
      }
      if (orch && !behavior_passed.empty()) {
        decide += "\n" + behavior_passed + " Close the ticket when the whole request is in place.";
        behavior_passed.clear();
      }
      if (orch && !build_failure.empty()) {
        decide += "\n" + build_failure +
                  "\nDispatch the block whose files must change (a block done in this run may get it once more; its worker "
                  "gets these errors as its task), or escalate when no block's code can fix them.";
        build_failure.clear();
      }
      if (orch && behavior_fixes > 0 && behavior_failure.empty() && !behavior_retask.empty()) {
        decide += fmt::format("\nThe behaviour test still fails: {} more send(s) to fix it, each with a stronger model and the "
                              "test's output. Put the cause you see into why (the file, the condition, what to change): the worker "
                              "reads only its block.",
                              behavior_fixes);
      }
      if (orch && !review_gaps.empty()) {
        decide += "\nThe blocks of your change found gaps; it was set aside. Propose it again with them closed:\n- " + review_gaps;
        review_gaps.clear();
      }
      if (!answered.empty()) {
        decide += "\nAlready answered in this run (do not send them the ticket again):";
        for (const auto& [blk, why] : answered) decide += "\n- " + blk + ": " + why;
      }
      // The blocks the code index finds for the words of the ticket (ADR 0028): on a big imported project
      // the orchestrator otherwise viewed whole groups to find the place (Endless Sky: 157K characters).
      std::string likely;
      if (orch && root_visits == 1 && done_blocks.empty() && limit.empty()) {  // a scoped ticket names its place
        std::lock_guard lk(m.mu);
        auto found = m.idx->locate(ticket->symptom.set ? ticket->symptom.value : ticket->task.value, kLikelyBlocks);
        if (found.starts_with("@locate")) {
          likely = "\n--- blocks the code index finds for the words of the ticket (a hint, not a decision)\n" +
                   found.substr(found.find('\n') + 1);
        }
      }
      if (orch && !limit.empty()) {
        decide += fmt::format("\nThe human limited this ticket to {} and the blocks inside it: dispatch only those (a change may add "
                              "blocks inside {}); escalate when the request needs a block outside it.",
                              limit, limit);
      }
      spec.task_text = core::path_view(*ticket, current) + likely + decide;
      spec.max_steps = orch ? cfg_.max_steps_orchestrator : cfg_.max_steps_navigator;
      spec.max_tokens = cfg_.max_tokens_per_run;
      spec.timeout = std::chrono::seconds(cfg_.run_timeout_s);
      auto cbs = m.callbacks(id, &answered);
      cbs.plans = orch && !bug;
      if (orch) cbs.limit = limit;
      SchemaToolHost host(*m.idx, spec.role, current, std::move(cbs));
      auto res = m.runner().run(spec, host);
      total += res.tokens;
      cost += res.cost;
      if (res.outcome == "cancelled") return stopped(role, current);
      auto in = json::parse(res.terminal_input.empty() ? "{}" : res.terminal_input, nullptr, false);
      int level = core::trail_level(current);
      if (res.terminal_tool == "dispatch") {
        std::string pick = sget(in, "block");
        std::vector<core::Attr> attrs = {{"at", current},
                                         {"suspect", core::join(sarr(in, "suspect"), ",")},
                                         {"ruled_out", core::join(sarr(in, "ruled_out"), ",")},
                                         {"pick", pick},
                                         {"difficulty", sget(in, "difficulty")},
                                         {"why", shorten(sget(in, "why"), 160)}};
        if (orch) {
          // A new decision of the orchestrator replaces any plan; `next` makes the new one.
          plan = bug ? std::deque<PlanStep>{} : read_plan(in);
          planned = !plan.empty();
          plan_no = 1;
          plan_total = 1 + static_cast<int>(plan.size());
          if (planned) {
            attrs.push_back({"plan", fmt::format("1/{}", plan_total)});
            attrs.push_back({"then", core::join([&] {
                               std::vector<std::string> names;
                               for (const auto& st : plan) names.push_back(st.block);
                               return names;
                             }(), ",")});
          }
        }
        m.trail(id, level_entry(level, role, attrs), sarr(in, "new_facts"));
        m.say(fmt::format("{} {} -> {}", role, current, pick));
        difficulty = sget(in, "difficulty");
        current = pick;
        continue;
      }
      if (orch && res.terminal_tool == "propose_change") {
        // The same checks as a worker's proposal (the tool already refused a bad diff).
        auto c = propose_change(root_, "orchestrator " + id, shorten(sget(in, "reason"), 200), sget(in, "diff"));
        if (!c) {
          m.note(id, "orchestrator proposal refused: " + c.error().message);
          return finish(core::TicketStatus::Escalated,
                        {{"status", "escalated", {}, 0}, {"note", "the orchestrator's change is not applicable: " + c.error().message, {}, 0}});
        }
        m.trail(id, level_entry(0, role, {{"at", "root"}, {"outcome", "change"}, {"why", shorten("proposed " + c->id, 160)}}));
        // A note that names a fn the change does not declare: the Sonnet orchestrator of Endless Sky's C2
        // wrote "fn ships_destroyed() -> int64_t" into a note only, and the panel's worker had to ask for
        // the fn and call lines. Found here without a model; the change goes back once (ADR 0029).
        if (!linted) {
          linted = true;
          if (auto gaps = fns_only_in_notes(*c, *s); !gaps.empty()) {
            (void)m.changes.set_status(c->id, "rejected");
            m.note(id, c->id + " set aside: " + shorten(core::join(gaps, "; "), 400));
            review_gaps = core::join(gaps, "\n- ");
            escalate_orch = true;
            continue;  // back to the orchestrator, which amends the change
          }
        }
        // review_changes: the leaves the change touches say whether their card is enough, before
        // anyone approves it — cheaper than a second change after a worker finds the gap. On by itself
        // when a cheaper tier than the strongest decided (ADR 0029).
        if ((cfg_.review_changes || weaker_orchestrator) && !reviewed) {
          reviewed = true;
          std::map<std::string, std::vector<std::string>> lines_of;
          std::string at;
          for (const auto& d : c->diff) {
            if (d.text.starts_with("@block ")) {
              at = d.op == '~' ? std::string(core::trim(std::string_view(d.text).substr(7))) : std::string();
              continue;
            }
            if (!at.empty()) lines_of[at].push_back(std::string(1, d.op) + " " + d.text);
          }
          std::vector<std::string> gaps;
          for (const auto& [blk, lines] : lines_of) {
            const auto* lb = s->find(blk);
            if (lb == nullptr || lb->kind != core::BlockKind::Leaf || lb->external()) continue;
            auto q = fmt::format("The ticket: {}\nThe change of your card:\n{}\nIs this card enough for your block's part of the "
                                 "ticket (the declarations your code and tests will need)? Answer \"yes\", or name the card line "
                                 "that is missing.",
                                 shorten(ticket->task.set ? ticket->task.value : ticket->symptom.value, 400), core::join(lines, "\n"));
            auto a = m.ask(blk, q, id);
            if (!core::to_lower(std::string(core::trim(a))).starts_with("yes")) gaps.push_back(blk + ": " + shorten(a, 300));
          }
          if (!gaps.empty()) {
            (void)m.changes.set_status(c->id, "rejected");
            m.note(id, c->id + " set aside after its blocks reviewed it: " + shorten(core::join(gaps, "; "), 400));
            review_gaps = core::join(gaps, "\n- ");
            escalate_orch = true;
            continue;  // back to the orchestrator, which amends the change
          }
        }
        auto steps = bug ? std::deque<PlanStep>{} : read_plan(in);
        if (auto_approve(c->id)) {
          if (!steps.empty()) {
            plan = std::move(steps);
            planned = true;
            plan_no = 0;
            plan_total = static_cast<int>(plan.size());
            start_step();
          }
          continue;
        }
        if (!steps.empty()) {
          // Kept for the run after the approval, which goes on with it.
          json pj = {{"change", c->id}, {"next", json::array()}};
          for (const auto& st : steps) pj["next"].push_back({{"block", st.block}, {"difficulty", st.difficulty}, {"why", st.why}});
          (void)core::write_file(plan_file, pj.dump(2) + "\n");
          m.note(id, "planned after the approval of " + c->id + ": " + plan_text(steps));
        }
        return finish(core::TicketStatus::Escalated,
                      {{"status", "waiting", {}, 0}, {"note", c->id + " needs human approval (karkas approve " + c->id + ")", {}, 0}});
      }
      if (orch && res.terminal_tool == "close") {
        m.trail(id, level_entry(0, role, {{"at", "root"}, {"outcome", "closed"}, {"why", shorten(sget(in, "note"), 160)}}));
        return finish(core::TicketStatus::Closed,
                      {{"status", sget(in, "verdict"), {}, 0}, {"note", sget(in, "note").empty() ? "closed by the orchestrator" : sget(in, "note"), {}, 0}});
      }
      if (orch && res.terminal_tool == "escalate") {
        auto note = sget(in, "note");
        m.trail(id, level_entry(0, role, {{"at", "root"}, {"outcome", "escalated"}, {"why", shorten(note, 160)}}));
        return finish(core::TicketStatus::Escalated, {{"status", "escalated", {}, 0}, {"note", note, {}, 0}});
      }
      if (!orch && res.terminal_tool == "report") {
        m.trail(id, level_entry(level, role, {{"at", current}, {"outcome", "not_here"}, {"why", shorten(sget(in, "cause"), 160)}}));
        answered[current] = "not_here: " + shorten(sget(in, "cause"), 160);
        std::string up = b->parent.empty() ? "root" : b->parent;
        if (++not_here[up] >= 2) {
          // Twice not_here on one level: go one more level up with a stronger navigator (11.3).
          auto nt = next_tier(cfg_, m.navigator_tier.contains(up) ? m.navigator_tier[up] : m.tier_for("navigator", "medium", up));
          if (!nt.empty()) m.navigator_tier[up] = nt;
          not_here[up] = 0;
          up = up == "root" ? "root" : (s->find(up) != nullptr && !s->find(up)->parent.empty() ? s->find(up)->parent : "root");
        }
        current = up;
        continue;
      }
      // No decision: stronger model for this level, then the human (11.3).
      if (!orch) {
        auto nt = next_tier(cfg_, tier);
        if (!nt.empty() && cfg_.tier(nt) != nullptr) {
          m.navigator_tier[current] = nt;
          continue;
        }
      }
      m.note(id, fmt::format("{} at {} gave no decision ({})", role, current, res.outcome));
      return finish(core::TicketStatus::Escalated, {{"status", "escalated", {}, 0}, {"note", role + " failed: " + res.outcome + " " + res.error, {}, 0}});
    }

    // Leaf: worker with routing and escalation.
    m.set_status(id, core::TicketStatus::Fixing);
    std::string task = bug ? "Find and fix the cause of the bug in the ticket. Add a unit test that reproduces it first."
                           : "Implement the part of the ticket's change that belongs to this block. If other blocks must "
                             "change too, do not wait for them: finish this block and name them in fix.";
    if (auto rt = retask.find(current); rt != retask.end()) {
      task = rt->second + "\n" + task;
      retask.erase(rt);
    } else if (!build_retask.empty()) {
      task = build_retask + "\n" + task;  // the block the orchestrator chose for a failing full build
    }
    build_retask.clear();
    // A send after a failing behaviour test: the test's output with the orchestrator's why, and a stronger model
    // (Endless Sky C3: two cheap fixes missed that Actor() is empty for kills the orchestrator had explained).
    std::string fix_tier;
    behavior_fix_now = behavior_fixes > 0 && !behavior_retask.empty();
    if (behavior_fix_now) {
      task = behavior_retask + "\n" + task;
      fix_tier = behavior_fixes == 2 ? "medium" : "strong";
      if (cfg_.tier(fix_tier) == nullptr) fix_tier.clear();
      --behavior_fixes;
      if (behavior_fixes == 0) behavior_retask.clear();
    }
    const auto files_before = cfg_.done_needs_change ? block_files_state(root_, *s, *b) : std::map<std::string, std::string>{};
    auto lo = m.run_worker(current, id, task, difficulty, "worker", fix_tier, approve_now(current));
    total += lo.tokens;
    cost += lo.cost;
    if (lo.status == "cancelled") return stopped("worker", current);
    // A done or fixed with no file of the block changed (and no change of its card) did nothing: the orchestrator
    // hears it instead of the plan going on as if the part were in place (docs/adr/0031, bugs.txt 14).
    if (cfg_.done_needs_change && (lo.status == "done" || lo.status == "fixed") && lo.api_note.empty()) {
      auto card = core::load_block_file(s->block_dir(current) / "block.ksl", current);
      if (block_files_state(root_, *s, card ? *card : *b) == files_before) {
        const std::string said = lo.fix.empty() ? lo.cause : lo.fix;
        answer_fact(current, "not_here", "reported " + lo.status + " but changed no file of the block: " + said);
        answered[current] = "reported " + lo.status + " without changing a file: " + shorten(said, 160);
        m.note(id, fmt::format("{} reported {} but changed no file of its block: back to the orchestrator", current, lo.status));
        current = "root";
        continue;
      }
    }
    // A fix worker that blames the behaviour test (it names the test's file) in a not_here found a flaw in the test,
    // not a block to go to: the behaviour tester corrects it (Endless Sky C3 run 5: the target of the test was
    // disabled from the start, the worker said so in a not_here, and the ticket went up to the orchestrator).
    if (behavior_fix_now && behavior_tested && (lo.status == "not_here" || lo.status == "needs_info")) {
      const std::string said = lo.cause + "\n" + lo.fix;
      for (const auto& l : core::split(core::read_file(ticket_tests).value_or(""), '\n')) {
        auto entry = std::string(core::trim(l));
        if (entry.empty() || entry.starts_with("#")) continue;
        if (core::contains(said, entry) || core::contains(said, fs::path(entry).filename().string())) {
          m.note(id, fmt::format("{} blames the behaviour test {} ({}): it goes to the behaviour tester", current, entry,
                                 lo.status));
          lo.status = "needs_tester";
          break;
        }
      }
    }
    if (lo.status == "fixed" || lo.status == "done") {
      m.set_status(id, core::TicketStatus::Verifying);
      // A new source file of an imported block reaches the project's own build: Karkas adds it to the
      // CMakeLists.txt that names the files of its folder (no worker may edit it; docs/adr/0030).
      if (b->external()) {
        std::vector<std::string> files;
        if (auto card = core::load_block_file(s->block_dir(current) / "block.ksl", current)) files = card->file_list();
        for (const auto& f : core::split(lo.files, ',')) files.emplace_back(core::trim(f));
        if (auto added = add_sources_to_build(root_, files); !added.empty()) {
          m.note(id, "Karkas added the new file(s) of " + current + " to the project's build: " + core::join(added, "; "));
          lo.fix += (lo.fix.empty() ? "" : " ") + std::string("(Karkas added to the build: ") + core::join(added, "; ") + ")";
        }
      }
      // An imported block of a feature with blocks still to come was checked by compiling its files in its
      // run; the project's full build and tests run once, after the last block (ADR 0028): Endless Sky's
      // release build with LTO took 3-5 min after every block.
      const bool defer_full = b->external() && !bug && planned && !plan.empty();
      // An edited api/ (constant values or the card's declarations) is re-tested with every
      // dependent (12.4): a tuned number is checked where it is used.
      auto regression = regression_plan(*s, m.idx->dependent_blocks(current), current, !lo.api_note.empty());
      auto tr = defer_full ? kk::Result<TestRun>(TestRun{true, true, "", {}, "compile check ok; the full build and tests run after the last block\n"})
                           : m.tests->build_and_test(regression.labels);
      std::string verify = tr ? shorten(tr->text, 300) : tr.error().message;
      bool green = tr && tr->ok;
      // The full build of an imported project fails: the block whose file an error names gets the errors
      // as its task (twice at most) instead of the human at once (ADR 0028).
      if (!green && tr && !tr->build_ok && b->external() && build_repairs < 2) {
        auto fresh = m.schema();  // the worker's card may have gained files in its run
        if (auto owner = block_of_build_error(fresh ? *fresh : *s, tr->build_summary); !owner.empty()) {
          ++build_repairs;
          if (owner != current) {
            // This block's part is in place; the error is in another block's file.
            std::string what = lo.fix.empty() ? lo.cause : lo.fix;
            done_lines.push_back({"fix", current + ": " + what, {}, 0});
            if (!lo.files.empty()) done_lines.push_back({"files", current + ": " + lo.files, {}, 0});
            done_blocks.push_back(current);
            answered[current] = "done: " + shorten(what, 160);
          }
          m.note(id, fmt::format("the full build fails in a file of {}: its worker gets the errors", owner));
          retask[owner] = "The project's full build fails after the changes of this ticket:\n" + shorten(tr->build_summary, 1500) +
                          "\nFix these errors in your files (the rest of the change is in place).";
          current = owner;
          continue;
        }
        // No error names a block's file: the orchestrator gets the build's errors once, not the human at once
        // (Endless Sky's round 3 escalated on a linker error right after its last block; docs/adr/0030).
        ++build_repairs;
        std::string what = lo.fix.empty() ? lo.cause : lo.fix;
        done_lines.push_back({"fix", current + ": " + what, {}, 0});
        if (!lo.files.empty()) done_lines.push_back({"files", current + ": " + lo.files, {}, 0});
        done_blocks.push_back(current);
        for (const auto& blk : done_blocks) answered.erase(blk);
        build_failure = "The project's full build fails after the blocks of this ticket, and no error names a file of a block:\n" +
                        shorten(tr->build_summary, 1500);
        build_retask = "The project's full build fails after the changes of this ticket:\n" + shorten(tr->build_summary, 1500) +
                       "\nFix in your files what causes these errors (the rest of the change is in place).";
        m.note(id, "the full build fails and no error names a block's file: back to the orchestrator");
        current = "root";
        continue;
      }
      // History of the leaf, written by the program (3.10); only kinds whose card takes hist
      // lines (leaf, entry): a hist line in a shared card is a schema error (E08) that stops
      // every agent afterwards.
      if (b->kind == core::BlockKind::Leaf || b->kind == core::BlockKind::Entry) {
        if (auto blk = core::load_block_file(s->block_dir(current) / "block.ksl", current)) {
          blk->add_hist(id, shorten(lo.cause.empty() ? lo.fix : lo.cause + " -> " + lo.fix, 120));
          (void)core::write_file(s->block_dir(current) / "block.ksl", core::write_block(*blk));
        }
      }
      // New invariants of this leaf, agreed in this ticket: the worker writes no contract tests
      // (12.1), so the tester adds theirs now — or the new promises stay unchecked (flappy
      // T-0013: rules.columns gained four inv lines, its contract tests stayed 12/12).
      std::string inv_line;
      if (!bug && green && b->kind == core::BlockKind::Leaf && b->external() && !inv_tested.contains(current)) {
        // No Karkas contract tests in imported code (ADR 0027): its new promises are the project's own
        // tests' business, and the ticket says so.
        if (auto invs = ticket_invariants(m.changes, id, current); !invs.empty()) {
          inv_tested.insert(current);
          inv_line = fmt::format("{}: {} new invariant(s), checked only by the project's own tests (imported block)", current,
                                 invs.size());
        }
      } else if (!bug && green && b->kind == core::BlockKind::Leaf && !inv_tested.contains(current)) {
        auto invs = ticket_invariants(m.changes, id, current);
        if (!invs.empty()) {
          inv_tested.insert(current);
          std::string tt = "The card of this leaf gained these invariants in this ticket (approved changes):\n- " +
                           core::join(invs, "\n- ") +
                           "\nThe worker has implemented them. Add a contract TEST_CASE for each one to tests/contract/, "
                           "through the api/ only; keep the existing tests (change one only when a changed or removed inv "
                           "of the card makes it wrong). Run test() and report done when the tests of the block pass; if "
                           "a new test fails because the code breaks the invariant, report done with that in cause.";
          auto tf = m.run_tester(current, id, false, tt);
          total += tf.tokens;
          cost += tf.cost;
          if (tf.status == "cancelled") return stopped("tester", current);
          m.trail(id, level_entry(core::trail_level(current), "tester",
                                  {{"at", current}, {"model", tf.tier}, {"attempt", "1"}, {"outcome", tf.status}}));
          auto tr2 = m.tests->build_and_test(regression.labels);
          verify = tr2 ? shorten(tr2->text, 300) : tr2.error().message;
          green = tr2 && tr2->ok;
          if (green) {
            inv_line = tf.status == "done"
                           ? fmt::format("{}: contract tests for {} new invariant(s) by the tester", current, invs.size())
                           : fmt::format("{}: no contract tests for its {} new invariant(s) yet (tester {}: {})", current,
                                         invs.size(), tf.status, shorten(tf.cause, 160));
          } else if (tf.status == "done") {
            // The new tests fail against the code: once more to the worker, with the failures.
            answer_fact(current, "tester", "contract tests for the new invariants added; they fail: " + verify);
            retask[current] = "The tester added contract tests for the new invariants of this block and they fail:\n" + verify +
                              "\nMake them pass without touching tests/contract; if a test checks what the card does not "
                              "say, report needs_tester with the flaw.";
            continue;
          }
        }
      }
      // The behaviour of the finished feature of an imported project (ADR 0029): after its last block and the
      // full build, a test in the project's own framework. Compiling is not enough (Endless Sky, method B: a
      // counter that compiled, saved and showed, and never counted). A failing test goes to the orchestrator.
      if (!bug && green && b->external() && plan.empty() && !behavior_tested && !cfg_.test_dir.empty() &&
          !cfg_.test_command.empty()) {
        behavior_tested = true;
        std::string what = fmt::format("Ticket {}: {}\nWhat the workers did:\n", id,
                                       ticket ? (ticket->task.set ? ticket->task.value : ticket->symptom.value) : std::string());
        for (const auto& l : done_lines) {
          if (l.key == "fix") what += "- " + l.text + "\n";
        }
        what += "- " + current + ": " + (lo.fix.empty() ? lo.cause : lo.fix) + "\n";
        // From here the test command gets the files the tester writes into the test folder: its own test()
        // runs its test (KK_TEST_FILES; "since" = files written from now on).
        const std::string list_head = fmt::format("# {} since={}\n", id, core::unix_now());
        (void)core::write_file(ticket_tests, list_head);
        auto bt = m.run_behavior_tester(current, id, what);
        total += bt.tokens;
        cost += bt.cost;
        if (bt.status == "cancelled") return stopped("tester", current);
        m.trail(id, level_entry(core::trail_level(current), "tester",
                                {{"at", current}, {"model", bt.tier}, {"attempt", "1"}, {"outcome", bt.status}}));
        std::vector<std::string> files;
        for (const auto& f : core::split(bt.files, ',')) {
          auto path = std::string(core::trim(f));
          std::error_code ec;
          if (!path.empty() && core::is_within(root_ / cfg_.test_dir, root_ / path) && fs::is_regular_file(root_ / path, ec)) {
            files.push_back(path);
          }
        }
        if (!files.empty()) (void)core::write_file(ticket_tests, list_head + core::join(files, "\n") + "\n");
        auto tr3 = files.empty() ? kk::Result<TestRun>(kk::fail("tester", "no test file in the test folder"))
                                 : m.tests->build_and_test({current});
        if (tr3 && tr3->ok) {
          inv_line = fmt::format("behaviour test {} passes: {}", core::join(files, " "), shorten(bt.fix, 200));
        } else if (tr3 && tr3->build_ok) {
          // The project builds, the feature does not do what the test checks.
          behavior_failure = fmt::format("The behaviour test {} written for this ticket fails after the last block:\n{}{}",
                                         core::join(files, " "), shorten(tr3->text, 1500),
                                         bt.cause.empty() ? "" : "\nThe tester: " + shorten(bt.cause, 400));
          behavior_fixes = 2;
          behavior_retask = fmt::format("The behaviour test {} of this ticket fails after the blocks were done:\n{}\nMake your block's part "
                                        "do what the test checks; run test() (it runs the test too) and do not change the test. "
                                        "If the test itself is wrong (what it waits for cannot happen, or it checks what the "
                                        "request does not ask), report status=needs_tester with the flaw in cause.",
                                        core::join(files, " "), shorten(tr3->text, 1500));
          std::string done_what = lo.fix.empty() ? lo.cause : lo.fix;
          done_lines.push_back({"fix", current + ": " + done_what, {}, 0});
          if (!lo.files.empty()) done_lines.push_back({"files", current + ": " + lo.files, {}, 0});
          done_lines.push_back({"tests", "behaviour test " + core::join(files, " ") + " fails", {}, 0});
          done_blocks.push_back(current);
          // The failing test is news: every block done in this run may be sent once more to fix it (the no-circles
          // rule kept the orchestrator of Endless Sky's C3 from sending the block it had found the cause in).
          for (const auto& blk : done_blocks) answered.erase(blk);
          m.note(id, "the behaviour test " + core::join(files, " ") + " fails: back to the orchestrator");
          current = "root";
          continue;
        } else {
          inv_line = fmt::format("no behaviour test (tester {}: {})", bt.status,
                                 shorten(!bt.cause.empty() ? bt.cause : (tr3 ? tr3->text : tr3.error().message), 200));
        }
      }
      if (!bug && green && behavior_fix_now) {
        // The fix passed the build and the test command, which runs the behaviour test (KK_TEST_FILES): the test
        // passes now; no more sends for it, and the orchestrator hears it (C3 run 3 was told "still fails").
        behavior_fixes = 0;
        behavior_retask.clear();
        behavior_passed = fmt::format("The behaviour test passes now, after the fix of {}.", current);
        inv_line = "behaviour test passes after the fix of " + current;
        m.note(id, behavior_passed);
      }
      if (!bug && green) {
        // A feature often spans blocks: the orchestrator sees what is done and sends the
        // next block, or closes the ticket (11.3 for features; ADR 0020).
        std::string what = lo.fix.empty() ? lo.cause : lo.fix;
        done_lines.push_back({"fix", current + ": " + what, {}, 0});
        if (!lo.files.empty()) done_lines.push_back({"files", current + ": " + lo.files, {}, 0});
        if (!lo.api_note.empty()) done_lines.push_back({"api", current + ": " + lo.api_note, {}, 0});
        if (!inv_line.empty()) done_lines.push_back({"tests", inv_line, {}, 0});
        done_lines.push_back({"verify", current + ": " + verify, {}, 0});
        done_blocks.push_back(current);
        answered[current] = "done: " + shorten(what, 160);
        answer_fact(current, lo.status, what);
        if (static_cast<int>(done_blocks.size()) >= kMaxLeavesDone) {
          return finish(core::TicketStatus::Closed,
                        {{"status", "done", {}, 0},
                         {"note", fmt::format("{} blocks done in one run; run the ticket again if more must change", kMaxLeavesDone), {}, 0}});
        }
        if (auto fin = after_done()) return *fin;
        continue;
      }
      std::vector<core::ResultLine> lines = {{"status", lo.status, {}, 0}};
      if (!lo.cause.empty()) lines.push_back({"cause", lo.cause, {}, 0});
      if (!lo.fix.empty()) lines.push_back({"fix", lo.fix, {}, 0});
      if (!lo.files.empty()) lines.push_back({"files", lo.files, {}, 0});
      lines.push_back({"api", lo.api_note.empty() ? "unchanged" : lo.api_note, {}, 0});
      lines.push_back({"verify", (green ? "" : "REGRESSION ") + verify, {}, 0});
      return finish(green ? core::TicketStatus::Closed : core::TicketStatus::Escalated, lines);
    }
    if (lo.status == "needs_tester" && behavior_tested) {
      // The worker found the flaw in the behaviour test of this ticket, not in its block. An imported project has
      // no contract tests: the contract tester failed at once there (Endless Sky's C3 run 4, a test that waited for
      // a kill that seldom came). The behaviour tester corrects its test with the finding (ADR 0029).
      answer_fact(current, lo.status, lo.cause);
      std::string list = core::read_file(ticket_tests).value_or("");
      std::vector<std::string> test_files;
      for (const auto& l : core::split(list, '\n')) {
        auto entry = std::string(core::trim(l));
        if (!entry.empty() && !entry.starts_with("#")) test_files.push_back(entry);
      }
      std::string what = fmt::format(
          "The worker of {} found that the behaviour test of this ticket is wrong, not the code:\n{}{}\nThe test: {}\n"
          "Correct the test so that it checks the request reliably: what it waits for must happen for certain, and its "
          "check must hold only when the feature works. Never drop or weaken the check of the request. Run test() and "
          "report done when it passes, failed with the cause otherwise.",
          current, shorten(lo.cause, 1200), lo.fix.empty() ? "" : "\nSuggested: " + shorten(lo.fix, 600),
          test_files.empty() ? "(see the test folder)" : core::join(test_files, " "));
      auto bt = m.run_behavior_tester(current, id, what);
      total += bt.tokens;
      cost += bt.cost;
      if (bt.status == "cancelled") return stopped("tester", current);
      m.trail(id, level_entry(core::trail_level(current), "tester",
                              {{"at", current}, {"model", bt.tier}, {"attempt", "2"}, {"outcome", bt.status}}));
      for (const auto& f : core::split(bt.files, ',')) {
        auto path = std::string(core::trim(f));
        std::error_code ec;
        if (!path.empty() && core::is_within(root_ / cfg_.test_dir, root_ / path) && fs::is_regular_file(root_ / path, ec) &&
            std::find(test_files.begin(), test_files.end(), path) == test_files.end()) {
          test_files.push_back(path);
          list += path + "\n";
        }
      }
      (void)core::write_file(ticket_tests, list);
      auto tr4 = m.tests->build_and_test({current});
      behavior_fixes = 0;  // the test was in doubt: no more sends of code for it
      behavior_retask.clear();
      if (tr4 && tr4->ok) {
        behavior_passed = fmt::format("The behaviour test passes now: {} found a flaw in it and the tester corrected it ({}).",
                                      current, shorten(bt.fix.empty() ? bt.cause : bt.fix, 300));
        done_lines.push_back({"tests", fmt::format("behaviour test {} passes after the tester corrected it: {}",
                                                   core::join(test_files, " "), shorten(bt.fix, 200)), {}, 0});
        m.note(id, fmt::format("the tester corrected the behaviour test ({} found the flaw); it passes", current));
      } else {
        behavior_failure = fmt::format(
            "The behaviour test is in doubt: {} found a flaw in it ({}) and the tester could not correct it:\n{}",
            current, shorten(lo.cause, 300), shorten(tr4 ? tr4->text : tr4.error().message, 800));
        m.note(id, "the tester could not correct the behaviour test: back to the orchestrator");
      }
      current = "root";
      continue;
    }
    if (lo.status == "needs_tester") {
      // The cause is in a contract test of this block: the tester owns tests/contract and
      // fixes it without reading impl/ (12.1); the worker's finding is its task.
      answer_fact(current, lo.status, lo.cause);
      std::string fix_task = "The worker of this block found the cause of the ticket in a contract test (tests/contract): " +
                             lo.cause + (lo.fix.empty() ? "" : "\nSuggested fix: " + lo.fix) +
                             "\nCorrect the contract test so that it checks its invariant without this flaw. Never weaken or "
                             "drop an invariant. Run test() and report done when the tests of the block pass, failed otherwise.";
      // New invariants of the leaf in this feature go to the same tester run, not to a second one.
      std::vector<std::string> invs;
      if (!bug && b->kind == core::BlockKind::Leaf && !b->external() && !inv_tested.contains(current)) {
        invs = ticket_invariants(m.changes, id, current);
        if (!invs.empty()) {
          inv_tested.insert(current);
          fix_task += "\nThe card of this leaf also gained these invariants in this ticket: add a contract TEST_CASE for each one, "
                      "through the api/ only:\n- " +
                      core::join(invs, "\n- ");
        }
      }
      auto tf = m.run_tester(current, id, false, fix_task);
      total += tf.tokens;
      cost += tf.cost;
      if (tf.status == "cancelled") return stopped("tester", current);
      m.trail(id, level_entry(core::trail_level(current), "tester",
                              {{"at", current}, {"model", tf.tier}, {"attempt", "1"}, {"outcome", tf.status}}));
      auto regression = regression_plan(*s, m.idx->dependent_blocks(current), current, false);
      auto tr = m.tests->build_and_test(regression.labels);
      std::string verify = tr ? shorten(tr->text, 300) : tr.error().message;
      bool green = tf.status == "done" && tr && tr->ok;
      // What both changed: the worker's files and api/ (it may have done its part before it found
      // the flawed test), then the tester's.
      std::string files = lo.files;
      std::string tester_files = tf.files.empty() ? "tests/contract (tester)" : tf.files;
      files += (files.empty() ? "" : ", ") + tester_files;
      if (!bug && green) {
        // A feature goes on (flappy T-0014 closed here with its HUD part undone): this leaf is done,
        // the orchestrator sends the next block or closes.
        std::string what = shorten(tf.fix.empty() ? lo.fix : tf.fix, 300);
        done_lines.push_back({"fix", current + ": " + what, {}, 0});
        done_lines.push_back({"files", current + ": " + files, {}, 0});
        if (!lo.api_note.empty()) done_lines.push_back({"api", current + ": " + lo.api_note, {}, 0});
        if (!invs.empty()) {
          done_lines.push_back({"tests", fmt::format("{}: contract tests for {} new invariant(s) by the tester", current, invs.size()), {}, 0});
        }
        done_lines.push_back({"verify", current + ": " + verify, {}, 0});
        done_blocks.push_back(current);
        answered[current] = "done (after the tester fixed a contract test): " + shorten(what, 140);
        answer_fact(current, "done", what);
        if (static_cast<int>(done_blocks.size()) >= kMaxLeavesDone) {
          return finish(core::TicketStatus::Closed,
                        {{"status", "done", {}, 0},
                         {"note", fmt::format("{} blocks done in one run; run the ticket again if more must change", kMaxLeavesDone), {}, 0}});
        }
        if (auto fin = after_done()) return *fin;
        continue;
      }
      std::vector<core::ResultLine> lines = {{"status", green ? "fixed" : "escalated", {}, 0}, {"cause", lo.cause, {}, 0}};
      if (!tf.fix.empty() || !lo.fix.empty()) lines.push_back({"fix", tf.fix.empty() ? lo.fix : tf.fix, {}, 0});
      lines.push_back({"files", files, {}, 0});
      lines.push_back({"api", lo.api_note.empty() ? "unchanged" : lo.api_note, {}, 0});
      lines.push_back({"verify", (green ? "" : "REGRESSION ") + verify, {}, 0});
      if (!green) lines.push_back({"note", "the tester could not make the contract tests pass; a human checks the test", {}, 0});
      return finish(green ? core::TicketStatus::Closed : core::TicketStatus::Escalated, lines);
    }
    if ((lo.status == "not_here" || lo.status == "needs_info") && behavior_fix_now) {
      // The orchestrator picked this block for the failing test: its answer goes back to it, not up the tree,
      // and the block may be sent again with a stronger model (ADR 0029).
      answer_fact(current, lo.status, lo.cause);
      m.note(id, fmt::format("{} could not fix the behaviour test ({}): back to the orchestrator", current, shorten(lo.cause, 200)));
      current = "root";
      continue;
    }
    if ((lo.status == "not_here" || lo.status == "needs_info") && (planned || !plan.empty())) {
      // A block of the orchestrator's plan: its answer goes back to the orchestrator, who planned the feature with
      // the whole ticket in view, not up the tree to a navigator that sees one group (Endless Sky round 3: the
      // needs_info of flight.engine went up to flight, down to flight.combat.ship_event, whose worker did nothing;
      // bugs.txt 13, docs/adr/0031). The plan's remaining blocks are in its next decision.
      answer_fact(current, lo.status, lo.cause);
      answered[current] = lo.status + ": " + shorten(lo.cause, 160);
      m.note(id, fmt::format("{} of the plan answered {}: back to the orchestrator", current, lo.status));
      current = "root";
      continue;
    }
    if (lo.status == "not_here" || lo.status == "needs_info") {
      answer_fact(current, lo.status, lo.cause);
      answered[current] = lo.status + ": " + shorten(lo.cause, 160);
      std::string up = b->parent.empty() ? "root" : b->parent;
      ++not_here[up];
      if (up == "root") escalate_orch = true;  // the orchestrator sent it to the wrong block
      current = up;
      continue;
    }
    if (lo.status == "needs_contract_change") {
      // Only a KSL card diff that passes the same checks as propose_change becomes a @change.
      std::string rejected = lo.proposal_error;
      if (!lo.proposal.empty()) {
        auto reason = shorten(lo.cause.empty() ? "contract change requested by the worker" : lo.cause, 200);
        auto c = propose_change(root_, "worker " + id, reason, core::join(lo.proposal, "\n"));
        if (c && auto_approve(c->id)) {
          answer_fact(current, lo.status, lo.cause);
          continue;
        }
        if (c) {
          return finish(core::TicketStatus::Escalated,
                        {{"status", "needs_contract_change", {}, 0}, {"cause", lo.cause, {}, 0},
                         {"note", c->id + " needs human approval (karkas approve " + c->id + ")", {}, 0}});
        }
        rejected = c.error().message;
      }
      // No applicable card diff: up to the orchestrator (11.3), which sees the worker's cause
      // and proposes the change itself, sends another block or gives the ticket to the human.
      // The block is not sent the same ticket again in this run.
      std::string cause = lo.cause;
      auto add = [&cause](const std::string& part) {
        if (part.empty()) return;
        if (!cause.empty()) cause += "; ";
        cause += part;
      };
      add(lo.fix);
      if (!rejected.empty()) add("proposal rejected: " + rejected);
      answer_fact(current, lo.status, cause);
      answered[current] = "needs_contract_change: " + shorten(cause, 200);
      escalate_orch = true;  // the orchestrator's change was not enough for this block
      current = "root";
      continue;
    }
    return finish(core::TicketStatus::Escalated,
                  {{"status", "escalated", {}, 0},
                   {"cause", lo.cause, {}, 0},
                   {"note",
                    fmt::format("the worker of {} ended with {}{}; a human gets the ticket with its full history", current,
                                lo.status.empty() ? "no result" : lo.status,
                                lo.tokens.input + lo.tokens.output + lo.tokens.cache_write() + lo.tokens.cache_read == 0
                                    ? " before any model call"
                                    : ""),
                    {}, 0}});
  }
  return finish(core::TicketStatus::Escalated, {{"status", "escalated", {}, 0}, {"note", "descent did not converge", {}, 0}});
}

}  // namespace kk::agents
