#include "kk/agents/engine.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <future>
#include <map>
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
  return s.size() <= n ? s : s.substr(0, n - 3) + "...";
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

  std::string tier_for(const std::string& role, const std::string& fallback) {
    auto it = cfg.roles.find(role);
    std::string t = it == cfg.roles.end() ? fallback : it->second;
    return t == "auto" ? fallback : t;
  }
  // A model pinned for the block ([blocks], [levels]; ADR 0011) beats the role's tier.
  std::string tier_for(const std::string& role, const std::string& fallback, std::string_view block) {
    if (auto p = cfg.pin(block); !p.tier.empty()) return p.tier;
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
            if (const auto* b = s->find(block)) out += "--- card\n" + index::card_text_without_comments(*b);
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
        for (const auto& n : sb.neighbor_blocks()) {
          if (n == "shared.probe") continue;
          add_files(out, s->block_dir(n) / dirs.api, n + ":api/", 8192, budget);
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
          add_files(out, s->block_dir(block) / dirs.tests / "unit", "tests/unit/", 8192, budget);
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

  LeafOutcome run_worker(const std::string& block, const std::string& tid, const std::string& task,
                         const std::string& difficulty, const std::string& prompt_name = "worker",
                         const std::string& fixed_tier = "") {
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
    if (auto p = cfg.pin(block); tier.empty() && !p.tier.empty()) {
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
    int at_tier = 0;
    std::string history;
    for (int attempt = 1; attempt <= 12; ++attempt) {
      std::string task_text;
      if (!tid.empty()) {
        auto t = tickets.load(tid);
        if (t) task_text += core::path_view(*t, block) + "\n";
      }
      task_text += "--- task\n" + task + "\n--- last build/test\n" + last_tests(block);
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
      LeafToolHost host(Sandbox(*s, block, Role::Worker), Role::Worker, *tests, {block});
      auto res = runner().run(spec, host);
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
          status == "needs_info") {
        return out;
      }
      ++at_tier;
      history += fmt::format("attempt {} ({}): cause={} fix={} -> {}\n", attempt, tier, shorten(out.cause, 120),
                             shorten(out.fix, 120), shorten(out.test_text.empty() ? res.outcome : out.test_text, 160));
      bool exhausted = res.outcome == "budget" || res.outcome == "max_steps" || res.outcome == "timeout";
      if (at_tier >= cfg.attempts_before_escalation || exhausted) {
        auto nt = next_tier(tier);
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

  LeafOutcome run_tester(const std::string& block, const std::string& tid, bool integration = false) {
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
    std::string tier = cfg.pin(block).tier;
    if (tier.empty()) {
      tier = integration ? tier_for("integration_tester", "medium") : (b->est.value == "L" ? "medium" : tier_for("tester", "cheap"));
    }
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
    spec.max_steps = cfg.max_steps_tester;
    spec.max_tokens = cfg.max_tokens_per_run;
    spec.timeout = std::chrono::seconds(cfg.run_timeout_s);
    LeafToolHost host(Sandbox(*s, block, Role::Tester), Role::Tester, *tests, {block});
    auto res = runner().run(spec, host);
    out.tokens = res.tokens;
    out.cost = res.cost;
    out.tier = tier;
    out.attempts = 1;
    out.reads = res.reads;
    if (res.terminal_tool == "report") {
      auto rep = json::parse(res.terminal_input, nullptr, false);
      out.status = rep.is_object() ? rep.value("status", "done") : "done";
      if (rep.is_object()) out.tests = sget(rep, "tests");
    } else {
      out.status = res.outcome == "cancelled" ? "cancelled" : "failed";
      out.cause = res.outcome;
    }
    out.test_text = host.last_test_text();
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
    spec.max_steps = 4;
    spec.max_tokens = 40000;
    spec.timeout = std::chrono::seconds(300);
    AskToolHost host(*idx, *s, block);
    auto res = runner().run(spec, host);
    std::string answer;
    if (res.terminal_tool == "answer") {
      auto j = json::parse(res.terminal_input, nullptr, false);
      if (j.is_object()) answer = sget(j, "text");
    }
    if (answer.empty()) answer = res.final_text.empty() ? "no answer (" + res.outcome + ")" : res.final_text;
    // At most ~80 tokens (10.5).
    while (core::estimate_tokens(answer) > 80 && answer.size() > 20) answer = answer.substr(0, answer.size() * 9 / 10);
    core::TrailEntry e;
    e.type = core::TrailEntry::Type::Answer;
    e.attrs = {{"at", block}};
    e.text = shorten(answer, 400);
    trail(tid, e);
    return answer;
  }

  SchemaCallbacks callbacks(const std::string& tid) {
    SchemaCallbacks cb;
    cb.ask = [this, tid](const std::string& b, const std::string& q) { return ask(b, q, tid); };
    cb.run_tests = [this](const std::string& b) {
      auto r = tests->build_and_test({b});
      return r ? r->text : r.error().message;
    };
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
    std::string job = group ? fmt::format("decompose group {} into 2-9 children (ids {}.<child>) and write api/ headers "
                                          "for every child that is a leaf",
                                          id, id)
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
    out += "generated: probe, skeletons, mocks, CMakeLists.txt\nnext: karkas implement all\n";
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
    // Contract changes of frozen blocks: unfreeze and mark dependents (12.4).
    auto st = store.load();
    std::set<std::string> touched;
    std::vector<std::string> fresh;
    std::string current;
    for (const auto& d : c->diff) {
      if (d.text.starts_with("@block ")) {
        current = std::string(core::trim(std::string_view(d.text).substr(7)));
        if (d.op == '~') touched.insert(current);
        if (d.op == '+') fresh.push_back(current);
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
    // Note on the originating ticket ("orchestrator T-0051").
    for (const auto& part : core::split_ws(c->from.value)) {
      if (core::is_record_id(part) && part[0] == 'T') m.note(part, what + " approved");
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
    for (const auto& id : touched) out += fmt::format("next: karkas expand {}  (update the api/ to the new card)\n", id);
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

kk::Result<std::string> Engine::run_ticket(const std::string& id) {
  auto& m = *impl_;
  auto lock = m.tickets.lock(id, "karkas run_ticket");
  if (!lock) return std::unexpected(lock.error());
  auto t = m.tickets.load(id);
  if (!t) return std::unexpected(t.error());
  if (t->status.value == "closed") return id + " is closed\n";
  auto s0 = m.schema();
  if (!s0) return std::unexpected(s0.error());
  if (core::has_errors(core::check_schema(*s0))) {
    return kk::fail("ticket", "schema errors (E) block the agents; run karkas check");
  }
  bool bug = t->kind_enum() == core::TicketKind::Bug;
  m.set_status(id, core::TicketStatus::Descending);
  std::string current = "root";
  std::string difficulty;
  std::map<std::string, int> not_here;  // scope → count of children that said not_here
  metrics::Tokens total;
  double cost = 0;
  auto finish = [&](core::TicketStatus st, std::vector<core::ResultLine> lines) -> std::string {
    lines.push_back({"cost", fmt::format("in={} out={} cache_read={} usd={:.4f}", total.input, total.output, total.cache_read, cost), {}, 0});
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
    m.set_status(id, core::TicketStatus::Open);
    m.reindex();
    m.say(id + " stopped by the user");
    return kk::fail("cancelled", fmt::format("{} stopped by the user at {}; continue with: karkas ticket run {}", id, at, id));
  };
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
      spec.task_text = core::path_view(*ticket, current) + (orch ? "\nDecide: dispatch, propose_change or close." : "\nDecide: dispatch to a child or report not_here.");
      spec.max_steps = orch ? cfg_.max_steps_orchestrator : cfg_.max_steps_navigator;
      spec.max_tokens = cfg_.max_tokens_per_run;
      spec.timeout = std::chrono::seconds(cfg_.run_timeout_s);
      SchemaToolHost host(*m.idx, spec.role, current, m.callbacks(id));
      auto res = m.runner().run(spec, host);
      total += res.tokens;
      cost += res.cost;
      if (res.outcome == "cancelled") return stopped(role, current);
      auto in = json::parse(res.terminal_input.empty() ? "{}" : res.terminal_input, nullptr, false);
      int level = core::trail_level(current);
      if (res.terminal_tool == "dispatch") {
        std::string pick = sget(in, "block");
        m.trail(id,
                level_entry(level, role,
                            {{"at", current},
                             {"suspect", core::join(sarr(in, "suspect"), ",")},
                             {"ruled_out", core::join(sarr(in, "ruled_out"), ",")},
                             {"pick", pick},
                             {"difficulty", sget(in, "difficulty")},
                             {"why", shorten(sget(in, "why"), 160)}}),
                sarr(in, "new_facts"));
        m.say(fmt::format("{} {} -> {}", role, current, pick));
        difficulty = sget(in, "difficulty");
        current = pick;
        continue;
      }
      if (orch && res.terminal_tool == "propose_change") {
        auto d = parse_diff(sget(in, "diff"));
        if (!d) return std::unexpected(d.error());
        auto c = m.changes.create("orchestrator " + id, sget(in, "reason"), *d);
        if (!c) return std::unexpected(c.error());
        m.trail(id, level_entry(0, role, {{"at", "root"}, {"outcome", "change"}, {"why", shorten("proposed " + c->id, 160)}}));
        return finish(core::TicketStatus::Escalated,
                      {{"status", "waiting", {}, 0}, {"note", c->id + " needs human approval (karkas approve " + c->id + ")", {}, 0}});
      }
      if (orch && res.terminal_tool == "close") {
        m.trail(id, level_entry(0, role, {{"at", "root"}, {"outcome", "closed"}, {"why", shorten(sget(in, "note"), 160)}}));
        return finish(core::TicketStatus::Closed,
                      {{"status", sget(in, "verdict"), {}, 0}, {"note", sget(in, "note").empty() ? "closed by the orchestrator" : sget(in, "note"), {}, 0}});
      }
      if (!orch && res.terminal_tool == "report") {
        m.trail(id, level_entry(level, role, {{"at", current}, {"outcome", "not_here"}, {"why", shorten(sget(in, "cause"), 160)}}));
        std::string up = b->parent.empty() ? "root" : b->parent;
        if (++not_here[up] >= 2) {
          // Twice not_here on one level: go one more level up with a stronger navigator (11.3).
          auto nt = next_tier(m.navigator_tier.contains(up) ? m.navigator_tier[up] : m.tier_for("navigator", "medium", up));
          if (!nt.empty()) m.navigator_tier[up] = nt;
          not_here[up] = 0;
          up = up == "root" ? "root" : (s->find(up) != nullptr && !s->find(up)->parent.empty() ? s->find(up)->parent : "root");
        }
        current = up;
        continue;
      }
      // No decision: stronger model for this level, then the human (11.3).
      if (!orch) {
        auto nt = next_tier(tier);
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
                           : "Implement the change requested in the ticket inside this block.";
    auto lo = m.run_worker(current, id, task, difficulty);
    total += lo.tokens;
    cost += lo.cost;
    if (lo.status == "cancelled") return stopped("worker", current);
    if (lo.status == "fixed" || lo.status == "done") {
      m.set_status(id, core::TicketStatus::Verifying);
      auto plan = regression_plan(*s, m.idx->dependent_blocks(current), current, false);
      auto tr = m.tests->build_and_test(plan.labels);
      std::string verify = tr ? shorten(tr->text, 300) : tr.error().message;
      bool green = tr && tr->ok;
      // History of the leaf, written by the program (3.10).
      if (auto blk = core::load_block_file(s->block_dir(current) / "block.ksl", current)) {
        blk->add_hist(id, shorten(lo.cause.empty() ? lo.fix : lo.cause + " -> " + lo.fix, 120));
        (void)core::write_file(s->block_dir(current) / "block.ksl", core::write_block(*blk));
      }
      std::vector<core::ResultLine> lines = {{"status", lo.status, {}, 0}};
      if (!lo.cause.empty()) lines.push_back({"cause", lo.cause, {}, 0});
      if (!lo.fix.empty()) lines.push_back({"fix", lo.fix, {}, 0});
      if (!lo.files.empty()) lines.push_back({"files", lo.files, {}, 0});
      lines.push_back({"api", "unchanged", {}, 0});
      lines.push_back({"verify", (green ? "" : "REGRESSION ") + verify, {}, 0});
      return finish(green ? core::TicketStatus::Closed : core::TicketStatus::Escalated, lines);
    }
    if (lo.status == "not_here" || lo.status == "needs_info") {
      if (!lo.cause.empty()) {
        core::TrailEntry f;
        f.type = core::TrailEntry::Type::Fact;
        f.text = shorten(current + " " + lo.status + ": " + lo.cause, 200);
        m.trail(id, f);
      }
      std::string up = b->parent.empty() ? "root" : b->parent;
      ++not_here[up];
      current = up;
      continue;
    }
    if (lo.status == "needs_contract_change") {
      std::vector<core::DiffLine> diff;
      if (auto d = parse_diff(core::join(lo.proposal, "\n"))) diff = *d;
      auto c = m.changes.create("worker " + id, lo.cause.empty() ? "contract change requested by the worker" : lo.cause, diff);
      std::string cid = c ? c->id : "?";
      return finish(core::TicketStatus::Escalated,
                    {{"status", "needs_contract_change", {}, 0}, {"cause", lo.cause, {}, 0},
                     {"note", cid + " needs human approval", {}, 0}});
    }
    return finish(core::TicketStatus::Escalated,
                  {{"status", "escalated", {}, 0}, {"cause", lo.cause, {}, 0},
                   {"note", "the strongest tier failed; a human gets the ticket with its full history", {}, 0}});
  }
  return finish(core::TicketStatus::Escalated, {{"status", "escalated", {}, 0}, {"note", "descent did not converge", {}, 0}});
}

}  // namespace kk::agents
