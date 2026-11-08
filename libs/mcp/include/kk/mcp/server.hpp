#pragma once
// MCP server over stdio (15): JSON-RPC 2.0, one message per line. stdout carries
// protocol messages only; logs go to stderr / .karkas/karkas.log (6.6).
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/error.hpp"

namespace kk::index {
class Index;
class ICodeAnalyzer;
}  // namespace kk::index

namespace kk::mcp {

struct ServerOptions {
  std::string version = "0.1.0";
  // kk_run_tests(block) → compact result text. Not set → the tool is not offered.
  std::function<std::string(const std::string& block)> run_tests;
  index::ICodeAnalyzer* analyzer = nullptr;  // used for index updates before each call
  // Write side, wired by `karkas mcp` (this library does not depend on kk_agents). A tool is
  // offered only when its function is set.
  std::function<std::string()> check;  // kk_check: KSL checks + drift, as `karkas check`
  // kk_propose_change: checks a KSL diff and stores it as C-NNNN for the user to approve.
  std::function<kk::Result<std::string>(const std::string& reason, const std::string& diff)> propose_change;
  std::function<kk::Result<std::string>(const std::string& id)> changes;  // kk_changes: "" = list
  // kk_model_for: the model Karkas assigns to work on a block (karkas.toml, ADR 0011/0013) and
  // the Claude Code subagent that runs it. role "" = by the kind of the block.
  std::function<kk::Result<std::string>(const std::string& block, const std::string& role)> model_for;
};

class Server {
 public:
  Server(index::Index& idx, ServerOptions opts);

  // Handles one JSON-RPC message; returns the response line, or nullopt for notifications.
  std::optional<std::string> handle(std::string_view line);
  // Reads messages from `in` until EOF; returns the process exit code.
  int serve(std::istream& in, std::ostream& out);

  static const std::vector<std::string>& supported_versions();  // newest first
  // tools/list payload (JSON text) — also used for documentation.
  [[nodiscard]] std::string tools_json() const;

 private:
  index::Index& idx_;
  ServerOptions opts_;
  bool initialized_ = false;
};

}  // namespace kk::mcp
