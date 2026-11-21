// Mode mcp without a client session (docs/adr/0029): see launcher.hpp.
#include "kk/agents/launcher.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <nlohmann/json.hpp>

#include "kk/core/paths.hpp"
#include "kk/core/platform.hpp"
#include "kk/core/text.hpp"

#ifdef _WIN32
#include "kk/core/win_process.hpp"
#else
extern char** environ;
#endif

namespace kk::agents {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::int64_t now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The karkas whose `mcp --only-agent` the sessions talk to: this executable when it is the CLI, else
// the one on PATH (an engine may run inside karkas-gui).
std::string karkas_cli() {
  auto self = core::self_executable();
  if (self.stem() == "karkas") return self.string();
#ifdef _WIN32
  return core::find_in_path("karkas");
#else
  if (const char* path = std::getenv("PATH")) {
    for (const auto& d : core::split(path, ':')) {
      auto p = fs::path(d) / "karkas";
      std::error_code ec;
      if (fs::exists(p, ec) && ::access(p.c_str(), X_OK) == 0) return p.string();
    }
  }
  return "karkas";
#endif
}

std::string system_prompt() {
  return "You are the model of one agent of Karkas, a program that develops this project with its own agents. Each turn "
         "shows the agent's system prompt, its tools and its messages. Answer every turn by calling kk_agent_act with the "
         "agent's tool calls, as its instructions ask, until the run ends; do nothing else.";
}

}  // namespace

fs::path RunLauncher::marker(const fs::path& root, const std::string& run) {
  return root / ".karkas" / "mcp" / "launch" / (run + ".served");
}

RunLauncher::RunLauncher(fs::path root, core::Config cfg)
    : root_(std::move(root)), cfg_(std::move(cfg)), queue_(root_ / ".karkas" / "mcp"), dir_(root_ / ".karkas" / "mcp" / "launch") {
  std::error_code ec;
  // A folder outside the project: Claude Code finds no CLAUDE.md and no memory of other sessions there.
  auto tag = fmt::format("{}-{:x}", root_.filename().string(), std::hash<std::string>{}(root_.string()) & 0xffffffU);
  work_ = fs::temp_directory_path(ec) / "karkas-launch" / tag;
  fs::create_directories(dir_, ec);
  fs::create_directories(work_, ec);
  json servers = {{"mcpServers",
                   {{"karkas", {{"type", "stdio"}, {"command", karkas_cli()}, {"args", {"--project", root_.string(), "mcp", "--only-agent"}}}}}}};
  if (auto r = core::write_file(dir_ / "mcp.json", servers.dump(2) + "\n"); !r) spdlog::warn("launch: {}", r.error().message);
}

RunLauncher::~RunLauncher() { stop(); }

std::vector<std::string> RunLauncher::command_for(const llm::McpTurn& t, const std::string& model) const {
  std::vector<std::string> a = {
      cfg_.mcp_launch_command,
      "-p",
      fmt::format("Serve Karkas run {} to its end: call kk_agent_next(run=\"{}\"), then answer each turn with kk_agent_act until an "
                  "answer says the run ended; then stop.",
                  t.run, t.run),
      "--model",
      model.empty() ? t.model : model,
      "--output-format",
      "stream-json",
      "--verbose",
      "--mcp-config",
      (dir_ / "mcp.json").string(),
      "--strict-mcp-config",
      "--setting-sources",
      "local",
      "--tools",
      "",
      "--allowedTools",
      "mcp__karkas__kk_agent_next",
      "mcp__karkas__kk_agent_act",
      "--system-prompt",
      system_prompt(),
      "--disable-slash-commands"};
  if (cfg_.mcp_launch_budget_usd > 0) {
    a.emplace_back("--max-budget-usd");
    a.push_back(fmt::format("{}", cfg_.mcp_launch_budget_usd));
  }
  return a;
}

std::vector<std::string> RunLauncher::session_environment() {
  std::vector<std::string> env;
  for (const auto& kv : core::environment()) {
    std::string_view v(kv);
    if (v.starts_with("ANTHROPIC_API_KEY=") || v.starts_with("ANTHROPIC_AUTH_TOKEN=") || v.starts_with("MCP_TIMEOUT=") ||
        v.starts_with("MCP_TOOL_TIMEOUT=")) {
      continue;
    }
    env.emplace_back(v);
  }
  env.emplace_back("MCP_TIMEOUT=300000");
  env.emplace_back("MCP_TOOL_TIMEOUT=900000");
  return env;
}

std::vector<std::string> RunLauncher::launched() const {
  std::lock_guard lk(mu_);
  std::vector<std::string> out;
  for (const auto& [run, s] : sessions_) out.push_back(run);
  return out;
}

void RunLauncher::start() {
  if (thread_.joinable()) return;
  stop_ = false;
  thread_ = std::thread([this] { watch(); });
}

bool RunLauncher::settle(int timeout_s) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(0, timeout_s));
  while (true) {
    {
      std::lock_guard lk(mu_);
      reap();
      bool any = false;
      for (const auto& [run, s] : sessions_) any = any || s.running;
      if (!any) return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

void RunLauncher::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  // The engine has ended its runs: their sessions end by themselves; a moment for that, then they are ended.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard lk(mu_);
      reap();
      bool any = false;
      for (const auto& [run, s] : sessions_) any = any || s.running;
      if (!any) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  std::lock_guard lk(mu_);
  for (auto& [run, s] : sessions_) {
    if (!s.running) continue;
#ifdef _WIN32
    if (auto* c = static_cast<core::win::Child*>(s.process.get())) c->kill_tree();  // with the MCP server it started
    s.running = false;
    spdlog::warn("launch: the session of run {} was ended with its ticket", run);
#else
    ::kill(-s.pid, SIGTERM);  // its own process group: the session and the MCP server it started
    for (int i = 0; i < 20 && ::waitpid(s.pid, nullptr, WNOHANG) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (::waitpid(s.pid, nullptr, WNOHANG) == 0) {
      ::kill(-s.pid, SIGKILL);
      ::waitpid(s.pid, nullptr, 0);
    }
    s.running = false;
    spdlog::warn("launch: the session of run {} was ended with its ticket", run);
#endif
  }
}

void RunLauncher::watch() {
  while (!stop_) {
    {
      std::lock_guard lk(mu_);
      reap();
      auto now = now_s();
      auto paused = queue_.paused_until();
      if (paused != 0 && paused <= now) {
        queue_.pause_until(0);
        paused = 0;
      }
      for (const auto& t : paused != 0 ? std::vector<llm::McpTurn>{} : queue_.waiting()) {
        if (t.taken || now - t.ts < kGraceS) continue;
        if (auto it = sessions_.find(t.run); it != sessions_.end()) {
          if (it->second.running || it->second.launches >= 2) continue;  // a session that died early starts once more
        } else {
          // Claimed atomically: another launcher (the GUI's engine next to a terminal's) may serve the run.
          auto m = marker(root_, t.run);
          if (!core::create_exclusive(m)) continue;
        }
        launch(t);
      }
    }
    for (int i = 0; i < 5 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void RunLauncher::launch(const llm::McpTurn& t) {
  const std::string model = [&] {
    auto it = sessions_.find(t.run);
    return it != sessions_.end() && !it->second.model_override.empty() ? it->second.model_override : t.model;
  }();
  auto args = command_for(t, model);
  auto env = session_environment();
#ifdef _WIN32
  {
    auto log = dir_ / t.run;
    auto err = dir_ / (t.run + ".err");
    std::error_code size_ec;
    const auto log_size = fs::file_size(log, size_ec);
    core::win::SpawnSpec sp;
    sp.argv = args;
    sp.cwd = work_;
    sp.env = env;
    sp.out = core::win::SpawnSpec::Io::File;
    sp.out_file = log;
    sp.err = core::win::SpawnSpec::Io::File;
    sp.err_file = err;
    auto child = core::win::Child::spawn(sp);  // in a job of its own: stop() ends it with its MCP server
    auto& s = sessions_[t.run];
    ++s.launches;
    s.log_from = size_ec ? 0 : log_size;
    if (!child) {
      spdlog::warn("launch: cannot start {} for run {}: {}", args[0], t.run, child.error().message);
      s.running = false;
      return;
    }
    s.pid = child->pid();
    s.process = std::make_shared<core::win::Child>(std::move(*child));
    s.model = model;
    s.running = true;
    spdlog::info("launch: {} serves run {} ({} {}, {}, pid {})", args[0], t.run, t.role, t.block, model, s.pid);
    return;
  }
#else
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  std::vector<char*> envp;
  for (auto& e : env) envp.push_back(e.data());
  envp.push_back(nullptr);
  auto log = dir_ / t.run;  // the run's journal name: <...>.jsonl, here the session's stream-json
  auto err = dir_ / (t.run + ".err");
  std::error_code size_ec;
  const auto log_size = fs::file_size(log, size_ec);
  const std::uintmax_t log_from = size_ec ? 0 : log_size;  // before the session writes to it
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  posix_spawn_file_actions_addopen(&fa, 2, err.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  posix_spawn_file_actions_addchdir_np(&fa, work_.c_str());
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);  // its own group: stop() ends it with its MCP server
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, argv[0], &fa, &at, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  auto& s = sessions_[t.run];
  ++s.launches;
  s.log_from = log_from;
  if (rc != 0) {
    spdlog::warn("launch: cannot start {} for run {}: {}", args[0], t.run, std::strerror(rc));
    s.running = false;
    return;
  }
  s.pid = pid;
  s.model = model;
  s.running = true;
  spdlog::info("launch: {} serves run {} ({} {}, {}, pid {})", args[0], t.run, t.role, t.block, model, pid);
#endif
}

void RunLauncher::reap() {
  for (auto& [run, s] : sessions_) {
    if (!s.running) continue;
#ifdef _WIN32
    auto* child = static_cast<core::win::Child*>(s.process.get());
    auto code = child != nullptr ? child->try_wait() : std::optional<int>(-1);
    if (!code) continue;
    const int exit_code = *code;
#else
    int status = 0;
    if (::waitpid(s.pid, &status, WNOHANG) != s.pid) continue;
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    s.running = false;
    auto line = result_line(run, s.log_from);
    if (auto limit = usage_limit(line); !limit.empty()) {
      if (s.launches > 0) --s.launches;  // the try does not count
      if (queue_.paused_until() == 0) {
        spdlog::warn("launch: the claude.ai usage limit is reached (\"{}\"): no session starts for {} s, then one tries "
                     "again; the turns wait",
                     limit, limit_retry_s);
      }
      queue_.pause_until(now_s() + limit_retry_s);
    } else if (auto refused = refusal(line); !refused.empty() && !stronger_model(s.model).empty()) {
      auto next = stronger_model(s.model);
      spdlog::warn("launch: {} refused run {} (\"{}\"): it goes on with {}", s.model, run,
                   refused.substr(0, std::min<std::size_t>(refused.size(), 160)), next);
      record_cost(run, s, line);
      s.model_override = next;
      if (s.launches > 0) --s.launches;  // the try does not count
    } else {
      spdlog::info("launch: the session of run {} ended (exit {})", run, exit_code);
      record_cost(run, s, line);
    }
    // A turn the session took and never answered goes back to waiting: the next session takes it.
    for (const auto& t : queue_.waiting()) {
      if (t.run == run && t.taken) queue_.release(t.id);
    }
  }
}

std::string RunLauncher::result_line(const std::string& run, std::uintmax_t from) const {
  auto text = core::read_file(dir_ / run);
  if (!text) return {};
  if (from > 0) *text = from < text->size() ? text->substr(from) : std::string();
  auto lines = core::split(*text, '\n');
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    if (it->find("\"type\":\"result\"") != std::string::npos) return *it;
  }
  return {};
}

std::string RunLauncher::refusal(const std::string& result_line) {
  auto j = json::parse(result_line, nullptr, false);
  if (!j.is_object()) return {};
  auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
  auto text = str("result");
  if (str("stop_reason") == "refusal" || text.find("safeguards flagged") != std::string::npos) {
    return text.empty() ? "refusal" : text;
  }
  return {};
}

std::string RunLauncher::stronger_model(const std::string& model) const {
  static const std::vector<std::string> order = {"cheap", "medium", "strong"};
  for (std::size_t i = 0; i + 1 < order.size(); ++i) {
    auto it = cfg_.models.find(order[i]);
    if (it == cfg_.models.end() || it->second.model != model) continue;
    for (std::size_t k = i + 1; k < order.size(); ++k) {
      auto up = cfg_.models.find(order[k]);
      if (up != cfg_.models.end() && !up->second.model.empty() && up->second.model != model) return up->second.model;
    }
  }
  return {};
}

std::string RunLauncher::usage_limit(const std::string& result_line) {
  auto j = json::parse(result_line, nullptr, false);
  if (!j.is_object()) return {};
  auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
  bool error = j.contains("is_error") && j["is_error"].is_boolean() && j["is_error"].get<bool>();
  int status = j.contains("api_error_status") && j["api_error_status"].is_number_integer() ? j["api_error_status"].get<int>() : 0;
  auto text = str("result");
  if (!error) return {};
  if (str("api_error") == "usage_limit_reached" || text.starts_with("You've hit your") ||
      (status == 429 && text.find("limit") != std::string::npos)) {
    return text.empty() ? str("api_error") : text;
  }
  return {};
}

// The session's own result line (Claude Code's cost at list prices, its time and turns) into costs.jsonl.
void RunLauncher::record_cost(const std::string& run, const Session& s, const std::string& result_line) const {
  auto j = json::parse(result_line, nullptr, false);
  if (!j.is_object()) return;
  json row = {{"run", run},
              {"model", s.model},
              {"cost_usd", j.value("total_cost_usd", 0.0)},
              {"duration_ms", j.value("duration_ms", 0)},
              {"num_turns", j.value("num_turns", 0)},
              {"is_error", j.value("is_error", false)},
              {"ts", now_s()}};
  if (j.contains("usage")) row["usage"] = j["usage"];
  (void)core::append_file(dir_ / "costs.jsonl", row.dump() + "\n");
}

}  // namespace kk::agents
