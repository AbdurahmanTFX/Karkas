#pragma once
// Mode mcp without a client session (docs/adr/0029). Karkas serves the turns of its agents itself: for
// every run whose turn has waited untaken for a few seconds, it starts a headless Claude Code
// (`claude -p` on the claude.ai login: no API key reaches it) that serves that run alone — the model of
// the run's tier, a short system prompt, only kk_agent_next / kk_agent_act, a working folder outside the
// project (no CLAUDE.md, no memory of other sessions) — and ends when the run ends. A nested run (an ask
// of the orchestrator) gets a session of its own tier, not the parent's (a cheap ask at Opus prices).
// Each session's stream goes to .karkas/mcp/launch/<run>.jsonl, its cost to costs.jsonl there.
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kk/core/config.hpp"
#include "kk/llm/mcp_bridge.hpp"

namespace kk::agents {

class RunLauncher {
 public:
  RunLauncher(std::filesystem::path root, core::Config cfg);
  ~RunLauncher();
  RunLauncher(const RunLauncher&) = delete;
  RunLauncher& operator=(const RunLauncher&) = delete;

  void start();  // the watcher thread
  void stop();   // stops watching, gives the sessions a moment to end with their runs, then ends them
  // Waits until no session it started is running (its result and cost are in costs.jsonl), at most `timeout_s`;
  // true when none runs. The ticket's cost is read after it (docs/adr/0031).
  bool settle(int timeout_s);

  // The command line of the session that serves the run of `t` (with `model` in place of the turn's).
  [[nodiscard]] std::vector<std::string> command_for(const llm::McpTurn& t, const std::string& model = {}) const;
  // The environment of a session: this one without ANTHROPIC_API_KEY / ANTHROPIC_AUTH_TOKEN (the
  // claude.ai login, never an API key), with long MCP timeouts (a build can take minutes).
  [[nodiscard]] static std::vector<std::string> session_environment();
  // The runs this launcher started a session for.
  [[nodiscard]] std::vector<std::string> launched() const;
  // A run served by a launched session: an MCP client leaves its turns alone, and a second launcher
  // (the GUI's engine next to a terminal's) does not start another session for it.
  static std::filesystem::path marker(const std::filesystem::path& root, const std::string& run);

  static constexpr int kGraceS = 3;  // a parent run's session takes a nested run's turn within this
  // A session that ends on the claude.ai usage limit ("You've hit your 5-hour limit · resets 11:50pm"):
  // no session starts for this long, then one tries again; the turns wait without timing out
  // (McpQueue::pause_until) and the try does not count. Endless Sky's C3 run 4 ran into the limit: two
  // failed starts, a 30-minute wait, a failed run, and its worker went a tier up.
  static inline int limit_retry_s = 300;
  // The text of a session's result line when it ended on the usage limit, empty otherwise.
  [[nodiscard]] static std::string usage_limit(const std::string& result_line);
  // The text of a session's result line when the model's safeguards refused the turn ("Sonnet 5.5's
  // safeguards flagged this message ... change your model"), empty otherwise. The run then goes on at
  // once with the next tier's model (Endless Sky's C3 run 5: Sonnet refused a fix worker's turn twice,
  // Opus served it); the try does not count.
  [[nodiscard]] static std::string refusal(const std::string& result_line);
  // The model of the tier above the one whose model is `model` ("" when it is the strongest or unknown).
  [[nodiscard]] std::string stronger_model(const std::string& model) const;

 private:
  struct Session {
    int pid = 0;
    std::string model;           // the model of its last start
    std::string model_override;  // after a refusal: the stronger model the run goes on with
    int launches = 0;
    bool running = false;
    std::uintmax_t log_from = 0;  // where its stream starts in the run's log: the tries of a run share one file
    std::shared_ptr<void> process;  // Windows: the core::win::Child of the session (docs/adr/0046)
  };
  void watch();
  void launch(const llm::McpTurn& t);
  void reap();
  void record_cost(const std::string& run, const Session& s, const std::string& result_line) const;
  // The result line of the session that started at `from` in the run's log ("" when it wrote none: an earlier try's
  // line, a usage limit or a refusal, is not taken for its own).
  [[nodiscard]] std::string result_line(const std::string& run, std::uintmax_t from = 0) const;

  std::filesystem::path root_;
  core::Config cfg_;
  llm::McpQueue queue_;
  std::filesystem::path dir_;   // <project>/.karkas/mcp/launch
  std::filesystem::path work_;  // the sessions' working folder, outside the project
  std::atomic<bool> stop_{false};
  std::thread thread_;
  mutable std::mutex mu_;
  std::map<std::string, Session> sessions_;  // by run
};

}  // namespace kk::agents
