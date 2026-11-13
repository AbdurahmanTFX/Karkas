#pragma once
// Agents through MCP (docs/adr/0024). In mode "mcp" the model of an agent run is an MCP client
// (Claude Code): every turn — the request the engine would send to an API — is written to
// <project>/.karkas/mcp/turns/; the MCP server hands it to the client (kk_agent_next), stores
// the client's answer (kk_agent_act), and this provider returns it to the engine as if an API had
// answered. Prompts, tools, sandbox, journals and the protocol of a ticket stay the engine's.
//
// Files of a turn <id> = <run>-<seq> (run = journal file name without .jsonl):
//   <id>.meta.json  who waits: ticket, role, block, tier, model, subagent, seq, ts
//   <id>.req.json   the request: system, tools, messages
//   <id>.taken      a client took it (kk_agent_next)
//   <id>.resp.json  the client's answer: {"text": "...", "calls": [{"name", "input", "id"?}]}
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "kk/llm/types.hpp"

namespace kk::llm {

struct McpTurn {
  std::string id;   // <run>-<seq>
  std::string run;  // journal file name of the agent run (".jsonl" included)
  std::string ticket, role, block, tier, model, subagent;
  int seq = 0;           // turn of the run, 1 = the first
  std::int64_t ts = 0;   // unix seconds it was written
  bool taken = false;    // a client took it
  std::int64_t taken_ts = 0;
};

class McpQueue {
 public:
  explicit McpQueue(std::filesystem::path dir);  // <project>/.karkas/mcp
  // Turns without an answer, oldest first.
  [[nodiscard]] std::vector<McpTurn> waiting() const;
  [[nodiscard]] std::optional<McpTurn> turn(const std::string& id) const;
  [[nodiscard]] kk::Result<std::string> request(const std::string& id) const;  // request JSON text
  void take(const std::string& id) const;
  [[nodiscard]] kk::VoidResult answer(const std::string& id, const std::string& answer_json) const;
  // Provider side.
  [[nodiscard]] kk::VoidResult put(const McpTurn& meta, const std::string& request_json) const;
  [[nodiscard]] std::optional<std::string> response(const std::string& id) const;
  void remove(const std::string& id) const;
  [[nodiscard]] std::filesystem::path turns_dir() const { return dir_ / "turns"; }

 private:
  std::filesystem::path dir_;
};

// The request of a turn as JSON text: {"id", "system", "tools": [...], "messages": [...]}.
std::string turn_request_json(const Request& req, const std::string& id);
// A client's answer ({"text", "calls"}) as the Response the engine reads.
kk::Result<Response> turn_response(const std::string& answer_json, const std::string& id, const std::string& model);

class McpBridgeProvider final : public IProvider {
 public:
  McpBridgeProvider(std::filesystem::path queue_dir, int wait_s);
  kk::Result<Response> send(const Request& req) override;
  kk::Result<std::int64_t> count_tokens(const Request& req) override;  // local estimate
  [[nodiscard]] std::string name() const override { return "mcp"; }

 private:
  [[nodiscard]] Usage traffic_usage(const McpTurn& t) const;
  std::filesystem::path dir_;
  McpQueue queue_;
  int wait_s_;
  int seq_ = 0;
};

}  // namespace kk::llm
