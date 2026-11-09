#pragma once
// Agent roles, tools and the contract between the agent loop and the program (9).
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kk/core/error.hpp"
#include "kk/llm/types.hpp"
#include "kk/metrics/metrics.hpp"

namespace kk::agents {

namespace fs = std::filesystem;

enum class Role { Architect, Orchestrator, Navigator, Worker, Tester };
std::string_view to_string(Role r);

struct Tool {
  std::string name;
  std::string description;
  std::string schema_json;  // JSON Schema of the input (Claude API: input_schema)
};

struct ToolCall {
  std::string id, name, input_json;
};

struct ToolOutcome {
  std::string content;  // text returned to the model (compact KSL where possible)
  bool is_error = false;
  bool terminal = false;  // report / dispatch / close / propose_change / answer end the run (9.4)
};

class IToolHost {
 public:
  virtual ~IToolHost() = default;
  [[nodiscard]] virtual std::vector<Tool> tools() const = 0;
  virtual ToolOutcome call(const ToolCall& c) = 0;
  // Paths read through the tools (journal check: the tester never reads impl/).
  [[nodiscard]] virtual std::vector<std::string> reads() const { return {}; }
};

struct AgentSpec {
  Role role = Role::Worker;
  std::string role_name;  // journal name, e.g. "worker", "tester", "navigator", "ask"
  std::string block;      // "" / "root" for the orchestrator
  std::string ticket;
  std::string tier;       // strong | medium | cheap | local
  int attempt = 1;
  std::string system_text;   // instructions + KSL reference (stable, cached)
  std::string context_text;  // stable block context (views, api/) — cached
  std::string task_text;     // ticket path view + task (changes per run)
  int max_steps = 25;
  long long max_tokens = 150000;
  std::chrono::seconds timeout{1800};
};

struct AgentResult {
  std::string outcome;        // terminal | max_steps | budget | timeout | end_turn | refusal | error | cancelled
  std::string terminal_tool;  // name of the terminal tool that ended the run
  std::string terminal_input; // its JSON input
  std::string final_text;
  std::string error;
  std::string model, provider;
  metrics::Tokens tokens;
  double cost = 0;
  int steps = 0;
  std::int64_t ms = 0;
  std::vector<std::string> reads;
  std::string journal;
  [[nodiscard]] bool terminal() const { return outcome == "terminal"; }
};

// Builds the model client for a tier; replaced in tests by scripted providers.
using ProviderFactory = std::function<kk::Result<std::unique_ptr<llm::IProvider>>(const std::string& tier)>;

}  // namespace kk::agents
