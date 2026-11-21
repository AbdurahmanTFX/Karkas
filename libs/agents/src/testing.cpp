#include "kk/agents/testing.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <set>

#include "kk/code/gen.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/platform.hpp"
#include "kk/core/text.hpp"
#include "kk/index/index.hpp"
#include "kk/runner/runner.hpp"

namespace kk::agents {

namespace {

// The behaviour tests a tester wrote in this ticket (ADR 0029), for the project's test command: it gets
// them in KK_TEST_FILES and may run only those, not its whole suite.
std::string with_ticket_tests(const std::filesystem::path& root, const std::string& test_dir, const std::string& command) {
  auto list = core::read_file(root / ".karkas" / "cache" / "ticket_tests.txt");
  if (!list) return command;
  std::vector<std::string> files;
  std::int64_t since = -1;
  for (const auto& l : core::split(*list, '\n')) {
    auto f = std::string(core::trim(l));
    if (f.starts_with('#')) {  // "# T-0001 since=<unix s>": whose list, and from when the test folder's new files count
      if (auto at = f.find("since="); at != std::string::npos) since = std::atoll(f.c_str() + at + 6);
      continue;
    }
    if (!f.empty() && f.find('\'') == std::string::npos) files.push_back(f);
  }
  if (since >= 0 && !test_dir.empty()) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root / test_dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      auto mt = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(e.path(), ec)).time_since_epoch())
                    .count();
      auto rel = core::rel_str(root, e.path());
      if (mt >= since && std::find(files.begin(), files.end(), rel) == files.end() && rel.find('\'') == std::string::npos) {
        files.push_back(rel);
      }
    }
  }
  if (files.empty()) return command;
  return "export KK_TEST_FILES='" + core::join(files, " ") + "'; " + command;
}

}  // namespace


namespace fs = std::filesystem;

namespace {

// One build at a time per process (mutex) and per project (flock), 6.4.
std::mutex g_build_mu;

// flock (Windows: LockFileEx), close-on-exec: neither the build's processes nor a program started after the
// build keep it.
using FileLock = core::FileLock;

}  // namespace

TestService::TestService(fs::path root, const core::Config& cfg) : root_(std::move(root)), cfg_(cfg) {}

std::string TestService::doctest_include() {
#ifdef KK_DOCTEST_INCLUDE_DIR
  return KK_DOCTEST_INCLUDE_DIR;
#else
  return {};
#endif
}

namespace {

// Output of a project's own test command for a model: failure and summary lines, colours removed.
std::string compress_test_output(const std::string& output) {
  static const std::regex ansi("\x1b\\[[0-9;]*m");
  static const std::regex interesting(R"((fail|error|assert|passed|abort|expected|test cases?|tests? ok))", std::regex::icase);
  std::vector<std::string> keep;
  std::vector<std::string> all;
  for (const auto& raw : core::split(std::regex_replace(output, ansi, ""), '\n')) {
    auto t = std::string(core::trim(raw));
    if (t.empty()) continue;
    all.push_back(t);
    if (std::regex_search(t, interesting)) keep.push_back(t);
  }
  std::string out;
  const std::size_t max = 30;
  for (std::size_t i = 0; i < keep.size() && i < max; ++i) out += keep[i] + "\n";
  if (keep.size() > max) out += fmt::format("... {} more lines\n", keep.size() - max);
  // The last lines usually hold the verdict.
  for (std::size_t i = all.size() > 3 ? all.size() - 3 : 0; i < all.size(); ++i) {
    if (std::find(keep.begin(), keep.end(), all[i]) == keep.end()) out += all[i] + "\n";
  }
  return out;
}

// compile_commands.json of the project's own build: [project] compile_commands, the project root, the
// build folder, or a folder right under it (a preset's binary dir, build/linux).
std::optional<fs::path> find_compile_commands(const fs::path& root, const core::Config& cfg) {
  std::vector<fs::path> cands;
  if (!cfg.compile_commands.empty()) cands.push_back(root / cfg.compile_commands);
  cands.push_back(root / "compile_commands.json");
  cands.push_back(root / cfg.build_dir / "compile_commands.json");
  std::error_code ec;
  std::vector<fs::path> under;
  if (fs::is_directory(root / cfg.build_dir, ec)) {
    for (const auto& e : fs::directory_iterator(root / cfg.build_dir, ec)) {
      if (e.is_directory(ec) && fs::is_regular_file(e.path() / "compile_commands.json", ec)) under.push_back(e.path() / "compile_commands.json");
    }
  }
  std::sort(under.begin(), under.end());
  cands.insert(cands.end(), under.begin(), under.end());
  for (const auto& c : cands) {
    if (fs::is_regular_file(c, ec)) return c;
  }
  return std::nullopt;
}

// A shell-like split of a "command" entry: blanks separate, quotes group, a backslash escapes.
std::vector<std::string> split_command(const std::string& cmd) {
  std::vector<std::string> out;
  std::string cur;
  bool any = false;
  char quote = 0;
  for (std::size_t i = 0; i < cmd.size(); ++i) {
    char c = cmd[i];
    if (c == '\\' && i + 1 < cmd.size() && quote != '\'') {
      cur += cmd[++i];
      any = true;
    } else if (quote != 0) {
      if (c == quote) quote = 0;
      else cur += c;
    } else if (c == '"' || c == '\'') {
      quote = c;
      any = true;
    } else if (c == ' ' || c == '\t' || c == '\n') {
      if (any) out.push_back(cur);
      cur.clear();
      any = false;
    } else {
      cur += c;
      any = true;
    }
  }
  if (any) out.push_back(cur);
  return out;
}

// The compile command of a file without its outputs (object, dependency file): -fsyntax-only checks
// the file, its warnings included, and writes nothing.
std::vector<std::string> syntax_only(const std::vector<std::string>& argv) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    const auto& a = argv[i];
    if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ") {
      ++i;
      continue;
    }
    if (a == "-c" || a == "-MD" || a == "-MMD" || (a.starts_with("-o") && a.size() > 2)) continue;
    out.push_back(a);
  }
  out.emplace_back("-fsyntax-only");
  return out;
}

}  // namespace

kk::Result<TestRun> TestService::compile_check(const std::vector<std::string>& files) {
  if (!code::has_own_cmake(root_)) return build_and_test({}, true);
  auto cc = find_compile_commands(root_, cfg_);
  if (!cc) return build_and_test({}, true);
  auto text = core::read_file(*cc);
  if (!text) return build_and_test({}, true);
  auto j = nlohmann::json::parse(*text, nullptr, false);
  if (!j.is_array()) return build_and_test({}, true);
  struct Entry {
    fs::path dir;
    std::vector<std::string> argv;
  };
  // One entry per file; a multi-config build lists one per configuration: the release one is preferred
  // (the configuration a task usually asks to build without warnings).
  std::map<fs::path, Entry> entries;
  std::set<fs::path> release;
  for (const auto& e : j) {
    if (!e.is_object() || !e.contains("file") || !e.contains("directory")) continue;
    fs::path dir = e["directory"].get<std::string>();
    fs::path file = e["file"].get<std::string>();
    if (file.is_relative()) file = dir / file;
    Entry en{dir, {}};
    if (e.contains("arguments") && e["arguments"].is_array()) {
      for (const auto& a : e["arguments"]) en.argv.push_back(a.get<std::string>());
    } else if (e.contains("command") && e["command"].is_string()) {
      en.argv = split_command(e["command"].get<std::string>());
    }
    if (en.argv.empty()) continue;
    auto key = core::normalize(file);
    bool is_release = e.contains("output") && e["output"].is_string() && core::contains(e["output"].get<std::string>(), "Release");
    if (!entries.contains(key) || (is_release && !release.contains(key))) {
      entries.insert_or_assign(key, std::move(en));
      if (is_release) release.insert(key);
    }
  }
  TestRun out;
  std::string log;
  std::vector<std::string> checked, missing;
  bool ok = true;
  for (const auto& rel : files) {
    auto it = entries.find(core::normalize(root_ / rel));
    if (it == entries.end()) {
      missing.push_back(rel);
      continue;
    }
    runner::RunOptions ro;
    ro.argv = syntax_only(it->second.argv);
    ro.cwd = it->second.dir;
    ro.timeout = std::chrono::seconds(cfg_.build_timeout_s);
    auto r = runner::run(ro);
    if (!r) return std::unexpected(r.error());
    log += r->output;
    if (!r->ok()) ok = false;
    checked.push_back(rel);
  }
  if (checked.empty()) return build_and_test({}, true);  // none of them is compiled by the build: build it
  out.build_ok = ok;
  out.ok = ok;
  out.build_warnings = runner::compress_build_warnings(log, root_);
  if (!ok) {
    out.build_summary = runner::compress_build_log(log, root_);
    if (out.build_summary.empty()) out.build_summary = "compile check failed";
    out.text = "build failed (compile check)\n" + out.build_summary + "\n";
  } else {
    out.text = fmt::format("compile check ok: {} (the project's full build and tests run after the last block of the ticket)\n",
                           core::join(checked, " "));
  }
  if (!missing.empty()) out.text += fmt::format("(not compiled by the project's build, not checked: {})\n", core::join(missing, " "));
  return out;
}

kk::Result<runner::BuildResult> TestService::build_program(const runner::OutputSink& on_output, const std::function<void()>& on_wait) {
  std::unique_lock mu(g_build_mu, std::try_to_lock);
  if (!mu.owns_lock()) {
    if (on_wait) on_wait();
    mu.lock();
  }
  FileLock lock(root_ / ".karkas" / "build.lock", on_wait);
  // A human waits for this build and can stop it: the agents' build timeout is no limit for it.
  auto timeout = std::chrono::seconds(std::max(cfg_.build_timeout_s, 3600));
  if (!cfg_.build_command.empty()) return runner::build_command(cfg_.build_command, root_, timeout, on_output);
  if (!code::has_own_cmake(root_)) {
    auto schema = core::load_schema(root_);
    if (!schema) return std::unexpected(schema.error());
    if (auto g = code::write_cmake(*schema); !g) return std::unexpected(g.error());
  }
  runner::BuildOptions bo;
  bo.source = root_;
  bo.build_dir = root_ / cfg_.build_dir;
  bo.doctest_include = doctest_include();
  bo.timeout = timeout;
  for (const auto& a : core::split(cfg_.cmake_args, ' ')) {
    if (!core::trim(a).empty()) bo.cmake_args.emplace_back(core::trim(a));
  }
  bo.on_output = on_output;
  return runner::build(bo);
}

kk::Result<TestRun> TestService::build_and_test(const std::vector<std::string>& blocks, bool build_only) {
  std::lock_guard lk(g_build_mu);
  FileLock lock(root_ / ".karkas" / "build.lock");
  TestRun out;
  auto schema = core::load_schema(root_);
  if (!schema) return std::unexpected(schema.error());
  // An imported project keeps its own CMakeLists.txt: build it as it is (ADR 0016).
  const bool own = code::has_own_cmake(root_);
  if (!own) {
    if (auto g = code::write_cmake(*schema); !g) return std::unexpected(g.error());
  }
  auto build_dir = root_ / cfg_.build_dir;
  runner::BuildOptions bo;
  bo.source = root_;
  bo.build_dir = build_dir;
  bo.doctest_include = doctest_include();
  bo.timeout = std::chrono::seconds(cfg_.build_timeout_s);
  for (const auto& a : core::split(cfg_.cmake_args, ' ')) {
    if (!core::trim(a).empty()) bo.cmake_args.emplace_back(core::trim(a));
  }
  // With its own build command the project builds itself (its presets, its build tree): a configure
  // by Karkas would start a second, full build next to it (ADR 0027). The quicker verify_command, when
  // there is one, checks the agents' work instead of the release build (ADR 0029).
  const std::string& own_command = cfg_.verify_command.empty() ? cfg_.build_command : cfg_.verify_command;
  auto b = own && !own_command.empty() ? runner::build_command(own_command, root_, bo.timeout) : runner::build(bo);
  if (!b) return std::unexpected(b.error());
  out.build_ok = b->ok;
  out.build_summary = b->summary;
  out.build_warnings = b->warnings;
  index::StateStore store(root_);
  auto files = index::collect_code_files(*schema);
  if (!b->ok) {
    out.text = "build failed\n" + b->summary + "\n";
    std::vector<index::TestResult> rec;
    for (const auto& id : blocks) {
      const auto* blk = schema->find(id);
      if (blk == nullptr || !blk->has_code()) continue;
      index::TestResult r;
      r.block = id;
      r.code_hash = index::block_test_hash(*schema, files, id);
      r.build_failed = true;
      r.failures = core::split(b->summary, '\n');
      if (r.failures.size() > 10) r.failures.resize(10);
      r.ts = core::unix_now();
      rec.push_back(std::move(r));
    }
    if (!rec.empty()) (void)store.record_tests(rec);
    return out;
  }
  if (build_only) {
    out.ok = true;
    out.text = "build ok\n";
    return out;
  }
  if (own) {
    // The project's own tests run as a whole: [test] command, or ctest.
    runner::RunOptions ro;
    const std::string what = cfg_.test_command.empty() ? "ctest" : cfg_.test_command;
    ro.argv = cfg_.test_command.empty()
                  ? std::vector<std::string>{"ctest", "--test-dir", build_dir.string(), "--output-on-failure"}
                  : runner::shell_argv(with_ticket_tests(root_, cfg_.test_dir, cfg_.test_command));
    ro.cwd = root_;
    ro.timeout = std::chrono::seconds(cfg_.test_timeout_s);
    auto r = runner::run(ro);
    if (!r) return std::unexpected(r.error());
    if (cfg_.test_command.empty() && core::contains(r->output, "No tests were found")) {
      out.ok = true;
      out.text = "build ok; ctest found no tests: set [test] command in karkas.toml to the command that runs this "
                 "project's tests\n";
      return out;
    }
    out.ok = r->ok();
    std::string verdict = out.ok ? "ok" : r->timed_out ? "timed out" : fmt::format("FAILED (exit {})", r->exit_code);
    out.text = fmt::format("build ok; tests ({}): {}\n", what, verdict) + compress_test_output(r->output);
    if (!blocks.empty()) out.text += "(the project's own tests run as a whole, not per block)\n";
    return out;
  }
  runner::TestOptions to;
  to.source = root_;
  to.build_dir = build_dir;
  to.blocks = blocks;
  to.timeout = std::chrono::seconds(cfg_.test_timeout_s);
  to.probe_dir = root_ / ".karkas" / "probes";
  to.probe_run = fmt::format("test-{}", core::unix_now());
  auto t = runner::test(to);
  if (!t) return std::unexpected(t.error());
  out.blocks = t->blocks;
  out.ok = t->ok;
  out.text = t->text;
  std::vector<index::TestResult> rec;
  for (const auto& bt : t->blocks) {
    index::TestResult r;
    r.block = bt.block;
    r.code_hash = index::block_test_hash(*schema, files, bt.block);
    r.contract = {bt.contract.pass, bt.contract.total};
    r.unit = {bt.unit.pass, bt.unit.total};
    r.integration = {bt.integration.pass, bt.integration.total};
    r.build_failed = bt.build_failed;
    r.failures = bt.failures;
    r.ts = core::unix_now();
    rec.push_back(std::move(r));
  }
  if (!rec.empty()) {
    if (auto w = store.record_tests(rec); !w) return std::unexpected(w.error());
  }
  return out;
}

RegressionPlan regression_plan(const core::Schema& schema, const std::vector<std::string>& dependents,
                               const std::string& block, bool api_changed) {
  RegressionPlan p;
  std::set<std::string> labels = {block};
  for (const auto& a : schema.ancestors(block)) labels.insert(a);
  if (api_changed) {
    for (const auto& d : dependents) {
      labels.insert(d);
      for (const auto& a : schema.ancestors(d)) labels.insert(a);
      p.needs_review.push_back(d);
    }
  }
  p.labels.assign(labels.begin(), labels.end());
  return p;
}

}  // namespace kk::agents
