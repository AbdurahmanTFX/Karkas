#include "kk/agents/bench.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <nlohmann/json.hpp>
#include <random>
#include <regex>
#include <set>

#include "kk/agents/changes.hpp"
#include "kk/agents/engine.hpp"
#include "kk/code/init.hpp"
#include "kk/index/state.hpp"
#include "kk/agents/testing.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/runner/runner.hpp"

namespace kk::agents {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string today() {
  auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

std::string random_hex() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  return fmt::format("{:016x}", rng());
}

void copy_project(const fs::path& from, const fs::path& to) {
  fs::create_directories(to);
  for (const auto& e : fs::directory_iterator(from)) {
    auto name = e.path().filename().string();
    if (name.starts_with("build") || name == ".karkas") continue;
    fs::copy(e.path(), to / name, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
  }
}

// Method A works on plain code: the schema files are removed (17.4).
void strip_karkas(const fs::path& root) {
  std::error_code ec;
  fs::remove_all(root / ".karkas", ec);
  fs::remove(root / "project.ksl", ec);
  fs::remove(root / "karkas.toml", ec);
  std::vector<fs::path> cards;
  for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
    if (e.path().extension() == ".ksl") cards.push_back(e.path());
  }
  for (const auto& c : cards) fs::remove(c, ec);
}

std::vector<std::string> lines_of(const fs::path& p) {
  std::vector<std::string> out;
  for (auto& l : core::split(core::read_file(p).value_or(""), '\n')) {
    if (!core::trim(l).empty()) out.push_back(std::string(core::trim(l)));
  }
  return out;
}

struct Bug {
  std::string id, file, old_text, new_text, symptom, fact;
};

kk::Result<Bug> load_bug(const fs::path& p) {
  auto text = core::read_file(p);
  if (!text) return std::unexpected(text.error());
  try {
    auto j = json::parse(*text);
    return Bug{j.at("id"), j.at("file"), j.at("old"), j.at("new"), j.at("symptom"), j.at("fact")};
  } catch (const json::exception& e) {
    return kk::fail("bench", fmt::format("{}: {}", p.string(), e.what()));
  }
}

std::vector<fs::path> bug_files(const fs::path& bench_dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(bench_dir / "bugs", ec)) {
    if (e.path().extension() == ".json") out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  auto n = v.size();
  return n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// Approvals in a project of method B: approved changes, the approved top level and frozen api/.
struct ApprovalMark {
  int changes = 0;
  std::string l0;
  std::map<std::string, std::string> api;
};

ApprovalMark approval_mark(const fs::path& dir) {
  ApprovalMark m;
  if (auto all = ChangeStore(dir).list()) {
    for (const auto& c : *all) m.changes += c.status.value == "approved" ? 1 : 0;
  }
  if (auto st = index::StateStore(dir).load()) {
    m.l0 = st->l0_hash;
    m.api = st->frozen_api;
  }
  return m;
}

// The agent approving its own proposals stands for a person doing it (17.6): every approved
// change, a newly approved top level and a newly frozen set of api/ headers count once.
int approvals_between(const ApprovalMark& a, const ApprovalMark& b) {
  int n = std::max(0, b.changes - a.changes);
  if (b.l0 != a.l0 && !b.l0.empty()) ++n;
  if (b.api != a.api && !b.api.empty()) ++n;
  return n;
}

// Method B: the Karkas frame, the guide, the subagents of the tiers and the MCP config.
kk::VoidResult prepare_method_b(const fs::path& dir, const BenchOptions& o) {
  if (o.scenario == "s1") {
    auto init = code::init_project(dir, "CLI audio pipeline wavkit exactly as specified in spec.md");
    if (!init) return std::unexpected(init.error());
  } else if (auto g = code::write_agent_guide(dir); !g) {
    return std::unexpected(g.error());
  }
  auto pcfg = core::load_config(dir);
  if (!pcfg) return std::unexpected(pcfg.error());
  auto agents = code::claude_agents_json(*pcfg);
  if (!agents) return std::unexpected(agents.error());
  if (auto w = core::write_file(dir / ".bench-agents.json", *agents); !w) return w;
  json mcp = {{"mcpServers", {{"karkas", {{"command", o.karkas.string()}, {"args", {"mcp", "--project", dir.string()}}}}}}};
  if (auto w = core::write_file(dir / ".bench-mcp.json", mcp.dump(2)); !w) return w;
  // The schema must be indexed before the agent asks for it.
  (void)runner::run({{o.karkas.string(), "index", "--project", dir.string()}, dir, std::chrono::minutes(5), {}, false});
  return {};
}

}  // namespace

// --- JSON ------------------------------------------------------------------------------------

std::string bench_result_json(const BenchResult& r) {
  json j = {{"scenario", r.scenario},
            {"method", r.method},
            {"model", r.model},
            {"auth", r.auth},
            {"rep", r.rep},
            {"date", r.date},
            {"cost_usd", r.cost_usd},
            {"input_tokens", r.input_tokens},
            {"output_tokens", r.output_tokens},
            {"cache_write_tokens", r.cache_write_tokens},
            {"cache_read_tokens", r.cache_read_tokens},
            {"model_calls", r.model_calls},
            {"seconds", r.seconds},
            {"interventions", r.interventions},
            {"build_ok", r.build_ok},
            {"hidden_pass", r.hidden_pass},
            {"hidden_total", r.hidden_total},
            {"cost_by_model", r.cost_by_model},
            {"cost_by_role", r.cost_by_role},
            {"notes", r.notes},
            {"workdir", r.workdir}};
  return j.dump(2) + "\n";
}

kk::Result<BenchResult> parse_bench_result(std::string_view text) {
  try {
    auto j = json::parse(text);
    BenchResult r;
    r.scenario = j.value("scenario", "");
    r.method = j.value("method", "");
    r.model = j.value("model", "");
    r.auth = j.value("auth", "");
    r.rep = j.value("rep", 0);
    r.date = j.value("date", "");
    r.cost_usd = j.value("cost_usd", 0.0);
    r.input_tokens = j.value("input_tokens", std::int64_t{0});
    r.output_tokens = j.value("output_tokens", std::int64_t{0});
    r.cache_write_tokens = j.value("cache_write_tokens", std::int64_t{0});
    r.cache_read_tokens = j.value("cache_read_tokens", std::int64_t{0});
    r.model_calls = j.value("model_calls", 0);
    r.seconds = j.value("seconds", 0.0);
    r.interventions = j.value("interventions", 0);
    r.build_ok = j.value("build_ok", false);
    r.hidden_pass = j.value("hidden_pass", 0);
    r.hidden_total = j.value("hidden_total", 0);
    if (j.contains("cost_by_model")) r.cost_by_model = j["cost_by_model"].get<std::map<std::string, double>>();
    if (j.contains("cost_by_role")) r.cost_by_role = j["cost_by_role"].get<std::map<std::string, double>>();
    if (j.contains("notes")) r.notes = j["notes"].get<std::vector<std::string>>();
    r.workdir = j.value("workdir", "");
    return r;
  } catch (const json::exception& e) {
    return kk::fail("bench", e.what());
  }
}

void parse_claude_json(std::string_view text, BenchResult& r) {
  // `claude -p --output-format json` prints one JSON object (the last line when more).
  auto lines = core::split(text, '\n');
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    auto j = json::parse(*it, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("total_cost_usd")) continue;
    r.cost_usd += j.value("total_cost_usd", 0.0);
    r.model_calls += j.value("num_turns", 0);
    if (j.contains("usage")) {
      const auto& u = j["usage"];
      r.input_tokens += u.value("input_tokens", std::int64_t{0});
      r.output_tokens += u.value("output_tokens", std::int64_t{0});
      r.cache_write_tokens += u.value("cache_creation_input_tokens", std::int64_t{0});
      r.cache_read_tokens += u.value("cache_read_input_tokens", std::int64_t{0});
    }
    // modelUsage covers helper models that `usage` omits (17.5).
    if (j.contains("modelUsage") && j["modelUsage"].is_object()) {
      for (const auto& [model, mu] : j["modelUsage"].items()) r.cost_by_model[model] += mu.value("costUSD", 0.0);
    }
    if (j.value("is_error", false)) r.notes.push_back("claude reported an error: " + j.value("subtype", std::string("?")));
    return;
  }
  r.notes.push_back("no JSON result from claude");
}

kk::VoidResult apply_bug(const fs::path& project, const fs::path& bug_json) {
  auto b = load_bug(bug_json);
  if (!b) return std::unexpected(b.error());
  auto p = project / b->file;
  auto text = core::read_file(p);
  if (!text) return std::unexpected(text.error());
  auto pos = text->find(b->old_text);
  if (pos == std::string::npos) return kk::fail("bench", fmt::format("bug {}: text not found in {}", b->id, b->file));
  text->replace(pos, b->old_text.size(), b->new_text);
  return core::write_file(p, *text);
}

std::vector<std::string> bench_claude_argv(const BenchOptions& o, const core::Config& cfg, const fs::path& dir,
                                           const std::string& prompt) {
  const bool api = o.auth != "subscription";
  std::string text = prompt;
  if (o.method == "B") {
    text += fmt::format(
        "\n\nThis is an unattended run: nobody will review or approve anything. Where the Karkas method says to stop "
        "for the user's approval, approve it yourself in the shell (`{} approve C-NNNN`, `{} approve L0`, `{} approve api`) "
        "and go on.",
        o.karkas.string(), o.karkas.string(), o.karkas.string());
  }
  std::vector<std::string> argv = {cfg.claude_command, "-p", text, "--output-format", "json", "--permission-mode",
                                   "bypassPermissions", "--max-budget-usd", fmt::format("{:.2f}", cfg.bench_max_budget_usd),
                                   "--no-session-persistence"};
  if (api) {
    argv.push_back("--bare");  // ANTHROPIC_API_KEY only; no user settings, hooks, CLAUDE.md discovery
  } else {
    // The claude.ai login, without the user's own settings, hooks, plugins and skills.
    for (const char* a : {"--setting-sources", "project,local", "--disable-slash-commands"}) argv.emplace_back(a);
  }
  if (!o.model.empty()) {
    argv.push_back("--model");
    argv.push_back(o.model);
  }
  if (o.method == "B") {
    for (const auto& a : {std::string("--mcp-config"), (dir / ".bench-mcp.json").string(), std::string("--agents"),
                          (dir / ".bench-agents.json").string()}) {
      argv.push_back(a);
    }
    if (api) {
      argv.push_back("--append-system-prompt-file");
      argv.push_back((dir / "CLAUDE.md").string());
    }
  }
  argv.push_back("--strict-mcp-config");  // A: no MCP servers at all; B: only Karkas
  return argv;
}

// --- run ---------------------------------------------------------------------------------------

kk::Result<std::vector<BenchResult>> bench_run(const BenchOptions& o, const core::Config& cfg,
                                               const std::function<void(const std::string&)>& progress) {
  auto say = [&](const std::string& m) {
    if (progress) progress(m);
  };
  if (o.scenario != "s1" && o.scenario != "s2" && o.scenario != "s3") return kk::fail("bench", "--scenario s1|s2|s3");
  if (o.method != "A" && o.method != "B" && o.method != "C") return kk::fail("bench", "--method A|B|C");
  if (!fs::exists(o.bench_dir / "spec.md")) return kk::fail("bench", "no spec.md in " + o.bench_dir.string());
  if (o.method == "C") {
    for (const auto& [tier, t] : cfg.models) {
      if (auto p = metrics::check_price(cfg, t.provider, t.model); !p) return std::unexpected(p.error());
    }
  }
  if ((o.method == "A" || o.method == "B") && o.auth != "subscription" && std::getenv("ANTHROPIC_API_KEY") == nullptr) {
    return kk::fail("bench", "auth api needs ANTHROPIC_API_KEY (or use --auth subscription)");
  }
  if (o.auth != "api" && o.auth != "subscription") return kk::fail("bench", "--auth api|subscription");
  std::vector<BenchResult> all;
  auto work = o.work.empty() ? fs::temp_directory_path() : o.work;
  for (int rep = 1; rep <= o.reps; ++rep) {
    BenchResult r;
    r.scenario = o.scenario;
    r.method = o.method;
    r.model = o.model;
    r.rep = rep;
    r.date = today();
    auto marker = random_hex();
    auto dir = work / fmt::format("karkas-bench-{}-{}-{}-{}", o.scenario, o.method, rep, marker.substr(0, 8));
    r.workdir = dir.string();
    say(fmt::format("{} {} rep {}: {}", o.scenario, o.method, rep, dir.string()));
    // Fresh copy (17.6).
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (o.scenario == "s1") {
      fs::create_directories(dir);
      fs::copy_file(o.bench_dir / "spec.md", dir / "spec.md");
    } else {
      copy_project(o.reference, dir);
      if (o.scenario == "s3") {
        for (const auto& b : bug_files(o.bench_dir)) {
          if (auto a = apply_bug(dir, b); !a) return std::unexpected(a.error());
        }
      }
      fs::copy_file(o.bench_dir / "spec.md", dir / "spec.md", fs::copy_options::overwrite_existing);
    }
    if (o.method == "A") strip_karkas(dir);
    // The requests of the scenario.
    std::vector<std::string> prompts;
    std::vector<Bug> bugs;
    if (o.scenario == "s1") {
      prompts.push_back("Implement the program described in spec.md in this directory as a C++20 CMake project. "
                        "`cmake -S . -B build -G Ninja && cmake --build build` must produce build/wavkit.");
    } else if (o.scenario == "s2") {
      for (const auto& t : lines_of(o.bench_dir / "s2_tasks.txt")) {
        prompts.push_back(t + " Keep everything of spec.md working; the build must still produce build/wavkit.");
      }
    } else {
      for (const auto& b : bug_files(o.bench_dir)) {
        auto bug = load_bug(b);
        if (!bug) return std::unexpected(bug.error());
        bugs.push_back(*bug);
        prompts.push_back(fmt::format("Bug report from a user: {}. Observed: {}. Find the cause in this codebase and fix it; "
                                      "keep the build working.",
                                      bug->symptom, bug->fact));
      }
    }
    auto t0 = std::chrono::steady_clock::now();
    if (o.method == "A" || o.method == "B") {
      r.auth = o.auth == "subscription" ? "subscription" : "api";
      ApprovalMark before;
      if (o.method == "B") {
        if (auto prep = prepare_method_b(dir, o); !prep) return std::unexpected(prep.error());
        before = approval_mark(dir);
      }
      for (const auto& p : prompts) {
        // A unique marker at the start defeats prompt-cache reuse across runs (17.6).
        runner::RunOptions ro;
        ro.argv = bench_claude_argv(o, cfg, dir, fmt::format("[bench {}] {}", marker, p));
        ro.cwd = dir;
        ro.timeout = std::chrono::hours(2);
        ro.clean_env = false;  // Claude Code needs its own environment (login, API key)
        if (r.auth == "subscription") ro.unset_env = {"ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN"};
        auto run = runner::run(ro);
        if (!run) {
          r.notes.push_back(run.error().message);
          continue;
        }
        parse_claude_json(run->output, r);
        if (run->timed_out) r.notes.push_back("claude timed out");
      }
      if (o.method == "B") r.interventions = approvals_between(before, approval_mark(dir));
      if (r.auth == "subscription") r.notes.push_back("subscription: costs are Claude Code's estimates");
    } else {
      EngineOptions eo;
      eo.providers = o.providers;
      eo.progress = say;
      if (o.scenario == "s1") {
        // The task description becomes the project's idea and rules (13.1).
        std::string project = "@project wavkit\nidea    CLI audio pipeline wavkit exactly as specified by the rules below\n"
                              "lang    cpp\nbuild   cmake-ninja\nstd     c++20\ntest    doctest\n";
        for (const auto& l : lines_of(o.bench_dir / "spec.md")) {
          if (l.starts_with("#") || l.starts_with("```")) continue;
          auto t = core::replace_all(core::replace_all(l, "`", ""), "#", "No.");
          if (t.starts_with("- ")) t = t.substr(2);
          project += core::format_line("rule", core::format_free_text(t)) + "\n";
        }
        (void)core::write_file(dir / "project.ksl", project);
      }
      auto engine = Engine::create(dir, cfg, eo);
      if (!engine) return std::unexpected(engine.error());
      auto& e = **engine;
      auto note = [&](const kk::Result<std::string>& res) {
        if (!res) r.notes.push_back(res.error().message);
      };
      if (o.scenario == "s1") {
        note(e.design(""));
        ++r.interventions;  // approval of L0 (17.6)
        note(e.approve("L0"));
        note(e.expand("all", true));
        ++r.interventions;  // approval of the interfaces
        note(e.approve("api"));
        note(e.implement("all"));
      } else if (o.scenario == "s2") {
        for (const auto& p : prompts) note(e.task(fmt::format("[bench {}] {}", marker, p)));
      } else {
        for (const auto& b : bugs) note(e.debug(fmt::format("[bench {}] {}", marker, b.symptom), {b.fact}));
      }
      (void)e.index().update();
      for (const auto& run : e.index().runs()) {
        r.cost_usd += run.cost_usd;
        r.input_tokens += run.input_tokens;
        r.output_tokens += run.output_tokens;
        r.cache_write_tokens += run.cache_write_tokens;
        r.cache_read_tokens += run.cache_read_tokens;
        r.model_calls += run.steps;
        r.cost_by_model[run.model] += run.cost_usd;
        r.cost_by_role[run.role] += run.cost_usd;
      }
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // Build + hidden acceptance tests, run by the harness only (17.6).
    runner::BuildOptions bo;
    bo.source = dir;
    bo.build_dir = dir / "build";
    bo.doctest_include = TestService::doctest_include();
    bo.timeout = std::chrono::seconds(1800);
    auto b = runner::build(bo);
    r.build_ok = b && b->ok;
    if (b && !b->ok) r.notes.push_back("build failed: " + core::split(b->summary, '\n').front());
    if (cfg.bench_hidden_tests.empty()) {
      r.notes.push_back("no [bench] hidden_tests configured");
    } else {
      runner::RunOptions ro;
      ro.argv = {"python3", "-I", (fs::path(cfg.bench_hidden_tests) / "run.py").string(), "--wavkit", (dir / "build" / "wavkit").string(),
                 "--scenario", o.scenario};
      ro.cwd = dir;
      ro.timeout = std::chrono::minutes(20);
      auto h = runner::run(ro);
      if (h) {
        std::smatch m;
        std::string out = h->output;
        if (std::regex_search(out, m, std::regex(R"(hidden (\d+)/(\d+))"))) {
          r.hidden_pass = std::stoi(m[1]);
          r.hidden_total = std::stoi(m[2]);
        } else {
          r.notes.push_back("hidden tests printed no summary");
        }
      }
    }
    auto out_dir = o.results / r.date;
    auto file = out_dir / fmt::format("{}-{}-{}.json", r.scenario, r.method, rep);
    for (int k = 2; fs::exists(file); ++k) file = out_dir / fmt::format("{}-{}-{}-{}.json", r.scenario, r.method, rep, k);
    if (auto w = core::write_file(file, bench_result_json(r)); !w) return std::unexpected(w.error());
    say(fmt::format("  cost ${:.4f}, hidden {}/{}, build {}, {:.0f} s -> {}", r.cost_usd, r.hidden_pass, r.hidden_total,
                    r.build_ok ? "ok" : "failed", r.seconds, file.string()));
    all.push_back(std::move(r));
  }
  return all;
}

// --- report -------------------------------------------------------------------------------------

kk::Result<std::string> bench_report(const fs::path& results_dir) {
  std::map<std::tuple<std::string, std::string, std::string>, std::vector<BenchResult>> groups;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(results_dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->path().extension() != ".json") continue;
    auto r = parse_bench_result(core::read_file(it->path()).value_or(""));
    if (!r) continue;
    // Subscription runs are reported apart: their costs are estimates.
    auto label = r->auth == "subscription" ? r->model + " (sub)" : r->model;
    groups[{r->scenario, r->method, label}].push_back(*r);
  }
  if (groups.empty()) return kk::fail("bench", "no results in " + results_dir.string());
  std::string csv =
      "scenario,method,model,runs,cost_median,cost_min,cost_max,input_median,output_median,cache_read_median,"
      "seconds_median,hidden_rate_median,hidden_rate_min,interventions_median,build_ok\n";
  std::string text = fmt::format("{:<4} {:<2} {:<20} {:>4} {:>24} {:>12} {:>12} {:>10} {:>16} {:>5}\n", "scn", "m", "model", "runs",
                                 "usd median [min..max]", "in median", "out median", "seconds", "hidden median", "human");
  std::string html =
      "<!doctype html><meta charset=utf-8><title>Karkas benchmark</title><style>body{font-family:sans-serif}"
      "table{border-collapse:collapse}td,th{border:1px solid #999;padding:4px 8px;text-align:right}</style>"
      "<h1>Karkas benchmark</h1><table><tr><th>scenario<th>method<th>model<th>runs<th>usd median<th>min<th>max"
      "<th>input<th>output<th>cache read<th>seconds<th>hidden pass rate<th>interventions</tr>";
  std::map<std::string, double> roles;
  for (const auto& [key, rs] : groups) {
    std::vector<double> cost, in, out, cr, sec, rate, hum;
    int builds = 0;
    for (const auto& r : rs) {
      cost.push_back(r.cost_usd);
      in.push_back(static_cast<double>(r.input_tokens));
      out.push_back(static_cast<double>(r.output_tokens));
      cr.push_back(static_cast<double>(r.cache_read_tokens));
      sec.push_back(r.seconds);
      rate.push_back(r.hidden_total > 0 ? static_cast<double>(r.hidden_pass) / r.hidden_total : 0.0);
      hum.push_back(r.interventions);
      builds += r.build_ok ? 1 : 0;
      for (const auto& [role, c] : r.cost_by_role) roles[std::get<1>(key) + "/" + role] += c;
    }
    auto [mn, mx] = std::minmax_element(cost.begin(), cost.end());
    auto rmin = *std::min_element(rate.begin(), rate.end());
    const auto& [s, m, model] = key;
    csv += fmt::format("{},{},{},{},{:.4f},{:.4f},{:.4f},{:.0f},{:.0f},{:.0f},{:.1f},{:.3f},{:.3f},{:.1f},{}/{}\n", s, m, model,
                       rs.size(), median(cost), *mn, *mx, median(in), median(out), median(cr), median(sec), median(rate), rmin,
                       median(hum), builds, rs.size());
    text += fmt::format("{:<4} {:<2} {:<20} {:>4} {:>24} {:>12.0f} {:>12.0f} {:>10.0f} {:>15.1f}% {:>5.0f}\n", s, m, model, rs.size(),
                        fmt::format("{:.3f} [{:.3f}..{:.3f}]", median(cost), *mn, *mx), median(in), median(out), median(sec),
                        100.0 * median(rate), median(hum));
    html += fmt::format("<tr><td>{}<td>{}<td>{}<td>{}<td>{:.4f}<td>{:.4f}<td>{:.4f}<td>{:.0f}<td>{:.0f}<td>{:.0f}<td>{:.0f}<td>{:.1f}%<td>{:.0f}</tr>",
                        s, m, model, rs.size(), median(cost), *mn, *mx, median(in), median(out), median(cr), median(sec),
                        100.0 * median(rate), median(hum));
  }
  html += "</table>";
  if (!roles.empty()) {
    // Where the tokens are spent in method C (17.8).
    text += "\ncost by role (sum over runs):\n";
    html += "<h2>Cost by role</h2><table><tr><th>method/role<th>usd</tr>";
    for (const auto& [k, v] : roles) {
      text += fmt::format("  {:<28} {:.4f}\n", k, v);
      html += fmt::format("<tr><td style=text-align:left>{}<td>{:.4f}</tr>", k, v);
    }
    html += "</table>";
  }
  (void)core::write_file(results_dir / "report.csv", csv);
  (void)core::write_file(results_dir / "report.txt", text);
  (void)core::write_file(results_dir / "report.html", html + "\n");
  return text;
}

}  // namespace kk::agents
