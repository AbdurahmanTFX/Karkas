#include "kk/agents/engine.hpp"

#include "kk/agents/api_edit.hpp"

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
#include "kk/agents/sandbox.hpp"
#include "kk/agents/tools.hpp"
#include "kk/code/analyzer.hpp"
#include "kk/code/gen.hpp"
#include "kk/core/cancel.hpp"
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

}  // namespace

constexpr const char* kImportedBlock =
    "block is imported code (files): agents do not edit imported blocks yet (docs/adr/0009)";

struct Engine::Impl {
  fs::path root;
  core::Config& cfg;
  EngineOptions opts;
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

  // A neighbor's api/: a small header whole, a big one as the declarations the block uses.
  static void add_neighbor_api(std::string& out, const fs::path& dir, const std::string& label, const std::set<std::string>& used,
                               std::size_t& budget) {
    constexpr std::size_t kWhole = 2000;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
      if (e.is_regular_file(ec) && core::is_code_file(e.path())) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    static const std::regex type_start(R"(^\s*(?:template\s*<[^>]*>\s*)?(?:struct|class|enum(?:\s+class)?|union)\s+([A-Za-z_]\w*))");
    static const std::regex constant(R"(([A-Za-z_]\w*)\s*(?:=|\{))");
    static const std::regex function(R"(([A-Za-z_]\w*)\s*\()");
    static const std::regex alias(R"(^\s*using\s+([A-Za-z_]\w*)\s*=)");
    for (const auto& f : files) {
      auto name = label + core::rel_str(dir, f);
      auto text = core::read_file(f).value_or("");
      if (text.size() <= kWhole && text.size() <= budget) {
        budget -= text.size();
        out += fmt::format("=== {}\n{}{}", name, text, text.ends_with("\n") ? "" : "\n");
        continue;
      }
      // Chunks: a type with its body, or one declaration, each with the comment lines before it.
      std::vector<std::string> lines;  // raw: core::split trims, and a struct body keeps its indentation
      for (std::size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
      }
      if (lines.empty()) continue;
      std::string kept;
      std::vector<std::string> names;
      std::string comment;
      for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];
        auto t = std::string(core::trim(l));
        if (t.empty() || t.starts_with("#")) {
          comment.clear();
          continue;
        }
        if (t.starts_with("//")) {
          comment += l + "\n";
          continue;
        }
        if (t.starts_with("namespace ") || t.starts_with("}  // namespace") || t == "}") {
          kept += l + "\n";
          comment.clear();
          continue;
        }
        std::size_t end = i;
        std::string decl;
        std::smatch m;
        if (std::regex_search(l, m, type_start)) {
          decl = m[1];
          int depth = 0;
          bool opened = false;
          for (end = i; end < lines.size(); ++end) {
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
          while (end < lines.size() && lines[end].find(';') == std::string::npos && lines[end].find('{') == std::string::npos) ++end;
          std::string joined;
          for (std::size_t k = i; k <= end && k < lines.size(); ++k) joined += lines[k] + " ";
          if (std::regex_search(joined, m, alias)) decl = m[1];
          else if (joined.find('(') != std::string::npos && std::regex_search(joined, m, function)) decl = m[1];
          else if (std::regex_search(joined, m, constant)) decl = m[1];
        }
        end = std::min(end, lines.size() - 1);
        if (!decl.empty()) names.push_back(decl);
        if (!decl.empty() && used.contains(decl)) {
          kept += comment;
          for (std::size_t k = i; k <= end; ++k) kept += lines[k] + "\n";
        }
        comment.clear();
        i = end;
      }
      bool any = std::any_of(names.begin(), names.end(), [&](const std::string& n) { return used.contains(n); });
      if (any && kept.size() <= budget) {
        budget -= kept.size();
        out += fmt::format("=== {} ({} bytes; the declarations this block uses, read_file for the rest)\n{}", name, text.size(), kept);
      } else {
        if (names.size() > 40) names.resize(40);
        out += fmt::format("=== {} ({} bytes, use read_file; declares {})\n", name, text.size(), core::join(names, ", "));
      }
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

  std::string context_for(Role role, const std::string& block) {
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
    if (b != nullptr && b->external()) {
      out.status = "failed";
      out.cause = kImportedBlock;
      return out;
    }
    if (b == nullptr || !b->has_code()) {
      out.status = "failed";
      out.cause = "not a leaf: " + block;
      return out;
    }
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
    auto system = prompt(prompt_name, {{"block", block}});
    auto context = context_for(Role::Worker, block);
    int level = core::trail_level(block);
    // Access to the own api/ in a ticket (ADR 0020): a frozen api lets constant values change
    // (tuning a number is not a contract change); an api unfrozen by an approved @change is
    // brought in line with the card by the worker itself. Outside tickets api/ is read-only.
    ApiAccess api_access = ApiAccess::None;
    std::string api_note;
    std::string users_note;
    if (auto users = Sandbox(*s, block, Role::Worker).user_blocks(); !users.empty()) {
      users_note = "--- blocks that use this block (their api/ is readable as <block>:api/<file>)\n" + core::join(users, ", ") + "\n";
    }
    if (!tid.empty() && prompt_name == "worker") {
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
    for (int attempt = 1; attempt <= 12; ++attempt) {
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
      Sandbox sandbox(*s, block, Role::Worker);
      sandbox.set_api_access(api_access);
      LeafToolHost host(std::move(sandbox), Role::Worker, *tests, {block});
      if (approve_now) host.set_approve_now(approve_now);
      auto res = runner().run(spec, host);
      if (host.approved_in_run()) {
        // A later attempt starts from the card and the access the approval gave.
        context = context_for(Role::Worker, block);
        api_access = ApiAccess::Full;
        api_note = "--- api/ of this block\nunfrozen by a change approved in this ticket: bring it in line with the card "
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
        } else {
          out.api_note = "changed to the card (karkas approve api " + block + " freezes it)";
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
      // Tests matter more than words (11.3): "fixed" with failing tests is not fixed.
      if (status == "fixed" || status == "done") {
        auto tr = tests->build_and_test({block});
        out.tests_ok = tr && tr->ok;
        out.test_text = tr ? tr->text : tr.error().message;
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
      out.cause = kImportedBlock;
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
}

Engine::~Engine() = default;

index::Index& Engine::index() { return *impl_->idx; }

std::string Engine::context_for(Role role, const std::string& block) { return impl_->context_for(role, block); }

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

kk::Result<std::string> Engine::design(const std::string& idea) {
  auto& m = *impl_;
  auto s = m.schema();
  if (!s) return std::unexpected(s.error());
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
  spec.max_steps = cfg_.max_steps_architect;
  spec.max_tokens = cfg_.max_tokens_per_run * 2;
  spec.timeout = std::chrono::seconds(cfg_.run_timeout_s);
  ArchitectToolHost host(root_, *m.idx, "", frozen_set(root_));
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
  out += "STOP: review the top level (karkas view, karkas gui), edit the cards if needed, then: karkas approve L0\n";
  return out;
}

kk::Result<std::string> Engine::expand(const std::string& block, bool all) {
  auto& m = *impl_;
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
    spec.max_steps = cfg_.max_steps_architect;
    spec.max_tokens = cfg_.max_tokens_per_run * 2;
    spec.timeout = std::chrono::seconds(cfg_.run_timeout_s);
    ArchitectToolHost host(root_, *m.idx, id, frozen_set(root_));
    auto res = m.runner().run(spec, host);
    m.reindex();
    m.say(fmt::format("expand {}: {} files, outcome {}", id, host.written().size(), res.outcome));
    return fmt::format("expand {}: {} files written ({}), ${:.4f}\n", id, host.written().size(), res.outcome, res.cost);
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

kk::Result<std::string> Engine::task(const std::string& description) {
  auto t = impl_->tickets.create(core::TicketKind::Feature, "human", description, {});
  if (!t) return std::unexpected(t.error());
  impl_->say("created " + t->id);
  return run_ticket(t->id);
}

kk::Result<std::string> Engine::debug(const std::string& symptom, const std::vector<std::string>& facts,
                                      const std::string& origin) {
  auto t = impl_->tickets.create(core::TicketKind::Bug, origin, symptom, facts);
  if (!t) return std::unexpected(t.error());
  impl_->say("created " + t->id);
  return run_ticket(t->id);
}

kk::Result<std::string> Engine::run_ticket(const std::string& id, bool force) {
  auto& m = *impl_;
  auto lock = m.tickets.lock(id, "karkas run_ticket");
  if (!lock) return std::unexpected(lock.error());
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
  int root_visits = 0;
  constexpr int kMaxRootVisits = 8;  // orchestrator decisions in one run (leaves of a feature + bounces)
  constexpr int kMaxLeavesDone = 10;  // leaves of one feature in one run (a plan has up to 9)
  metrics::Tokens total;
  double cost = 0;
  auto finish = [&](core::TicketStatus st, std::vector<core::ResultLine> lines) -> std::string {
    // The status line stays first: it starts a result block (the path view finds the latest).
    if (!done_lines.empty()) lines.insert(lines.begin() + (lines.empty() ? 0 : 1), done_lines.begin(), done_lines.end());
    lines.push_back({"state", m.state_hash(), {}, 0});
    lines.push_back({"cost", fmt::format("in={} out={} cache_read={} usd={:.4f}{}", total.input, total.output, total.cache_read, cost,
                                         cfg_.mcp() ? " (MCP: an estimate of the client's tokens from the turns)" : ""), {}, 0});
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
      lines.push_back({"cost", fmt::format("in={} out={} cache_read={} usd={:.4f}{}", total.input, total.output, total.cache_read, cost,
                                         cfg_.mcp() ? " (MCP: an estimate of the client's tokens from the turns)" : ""), {}, 0});
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
    if (auto t = m.tickets.load(id)) {
      for (const auto& r : t->result) {
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
  for (int guard = 0; guard < 30; ++guard) {
    if (core::cancel_requested()) return stopped("", current);
    auto s = m.schema();
    if (!s) return std::unexpected(s.error());
    auto ticket = m.tickets.load(id);
    if (!ticket) return std::unexpected(ticket.error());
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
      if (orch && !review_gaps.empty()) {
        decide += "\nThe blocks of your change found gaps; it was set aside. Propose it again with them closed:\n- " + review_gaps;
        review_gaps.clear();
      }
      if (!answered.empty()) {
        decide += "\nAlready answered in this run (do not send them the ticket again):";
        for (const auto& [blk, why] : answered) decide += "\n- " + blk + ": " + why;
      }
      spec.task_text = core::path_view(*ticket, current) + decide;
      spec.max_steps = orch ? cfg_.max_steps_orchestrator : cfg_.max_steps_navigator;
      spec.max_tokens = cfg_.max_tokens_per_run;
      spec.timeout = std::chrono::seconds(cfg_.run_timeout_s);
      auto cbs = m.callbacks(id, &answered);
      cbs.plans = orch && !bug;
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
                               std::vector<std::string> b;
                               for (const auto& st : plan) b.push_back(st.block);
                               return b;
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
        // review_changes: the leaves the change touches say whether their card is enough, before
        // anyone approves it — cheaper than a second change after a worker finds the gap.
        if (cfg_.review_changes && !reviewed) {
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
    }
    auto lo = m.run_worker(current, id, task, difficulty, "worker", "", approve_now(current));
    total += lo.tokens;
    cost += lo.cost;
    if (lo.status == "cancelled") return stopped("worker", current);
    if (lo.status == "fixed" || lo.status == "done") {
      m.set_status(id, core::TicketStatus::Verifying);
      // An edited api/ (constant values or the card's declarations) is re-tested with every
      // dependent (12.4): a tuned number is checked where it is used.
      auto plan = regression_plan(*s, m.idx->dependent_blocks(current), current, !lo.api_note.empty());
      auto tr = m.tests->build_and_test(plan.labels);
      std::string verify = tr ? shorten(tr->text, 300) : tr.error().message;
      bool green = tr && tr->ok;
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
      if (!bug && green && b->kind == core::BlockKind::Leaf && !inv_tested.contains(current)) {
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
          auto tr2 = m.tests->build_and_test(plan.labels);
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
      if (!bug && b->kind == core::BlockKind::Leaf && !inv_tested.contains(current)) {
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
      auto plan = regression_plan(*s, m.idx->dependent_blocks(current), current, false);
      auto tr = m.tests->build_and_test(plan.labels);
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
    if (lo.status == "not_here" || lo.status == "needs_info") {
      answer_fact(current, lo.status, lo.cause);
      answered[current] = lo.status + ": " + shorten(lo.cause, 160);
      std::string up = b->parent.empty() ? "root" : b->parent;
      ++not_here[up];
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
      current = "root";
      continue;
    }
    return finish(core::TicketStatus::Escalated,
                  {{"status", "escalated", {}, 0},
                   {"cause", lo.cause, {}, 0},
                   {"note", "the strongest tier failed; a human gets the ticket with its full history", {}, 0}});
  }
  return finish(core::TicketStatus::Escalated, {{"status", "escalated", {}, 0}, {"note", "descent did not converge", {}, 0}});
}

}  // namespace kk::agents
