#pragma once
// Agents through MCP, the client's side (docs/adr/0024): what the MCP tools kk_agent_next and
// kk_agent_act show and do. The engine's McpBridgeProvider writes every turn of an agent run to
// the queue (llm::McpQueue); the client — Claude Code or its karkas-<tier> subagent — is the
// model of that run: it reads the turn (the same system prompt, tools and messages an API would
// get) and answers with calls of the agent's own tools, which Karkas runs in the agent's sandbox.
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>
#include <string>

#include "kk/core/error.hpp"
#include "kk/llm/mcp_bridge.hpp"

namespace kk::agents {

class McpTurns {
 public:
  explicit McpTurns(std::filesystem::path project_root);

  // kk_agent_next. list: the turns that wait (who, which subagent), nothing taken. Otherwise the
  // oldest waiting turn — of `run` when given — is taken and shown whole; with `after` (the turn of
  // that run the client answered last) a turn that follows it shows only what is new: the client
  // has the rest already. Waits up to wait_s.
  // seen: the runs this client served already; a new run of the same role leaves out the system prompt
  // and tools it has, and the sections of its first message that did not change (ADR 0026).
  std::string next(const std::string& run, int wait_s, bool list, const std::string& after = {},
                   const std::vector<std::string>& seen = {});
  // kk_agent_act: the answer to `turn` (text and calls [{"name", "input"}]); then the next turn
  // of the same run with only what is new, or how the run ended. Waits up to wait_s.
  kk::Result<std::string> act(const std::string& turn, const std::string& text, const std::string& calls_json, int wait_s);

  // Lines about the tickets this server started (still running or how they ended, with the result
  // of an ended one); set by `karkas mcp`, shown when no turn waits.
  std::function<std::string()> tickets_status;
  // How many tickets or commands this server started still run; -1 when it started none. Set by
  // `karkas mcp`: a list wait ends as soon as they have all ended, not when wait_s runs out (ADR 0029).
  std::function<int()> tickets_running;

 private:
  [[nodiscard]] std::string run_end(const std::string& run) const;  // "" while the run goes on
  // The record (JSON text) of the newest seen run of the same role, preferably of the same block.
  [[nodiscard]] std::optional<std::string> seen_like(const llm::McpTurn& t, const std::vector<std::string>& seen) const;
  std::string show(const llm::McpTurn& t, const std::string& request_json, bool whole, const std::vector<std::string>& seen);
  [[nodiscard]] std::string ticket_of_run(const std::string& run) const;  // from its journal
  std::filesystem::path root_;
  llm::McpQueue queue_;
};

// A turn as the model reads it: whole (system, tools, every message) or only what came after
// the model's last answer (the results of its calls).
std::string format_turn(const llm::McpTurn& t, const std::string& request_json, bool whole);
// The same, leaving out what the client has from `seen_run` (its record of what it was shown).
std::string format_turn(const llm::McpTurn& t, const std::string& request_json, bool whole, const std::string& seen_run_json);

}  // namespace kk::agents
