#pragma once
// Tool sets of the roles (9.3) and their execution.
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "kk/agents/sandbox.hpp"
#include "kk/agents/testing.hpp"
#include "kk/agents/types.hpp"
#include "kk/index/index.hpp"

namespace kk::agents {

// Tool definitions (input_schema JSON per appendix C).
Tool tool_def(std::string_view name);

// Orchestrator and navigator: schema queries only, no files (9.3).
struct SchemaCallbacks {
  std::function<std::string(const std::string& block, const std::string& question)> ask;
  std::function<std::string(const std::string& block)> run_tests;
  // Why a block may not be picked again in this run ("not_here: ..."), empty if it may: a
  // child that already answered is not sent the same ticket twice in one descent.
  std::function<std::string(const std::string& block)> excluded;
  // The block the human limited the ticket to (docs/adr/0034): dispatch and `next` name it or a
  // block inside it; "" = the whole project.
  std::string limit;
  // Full check of a proposed KSL diff (the same as a worker's proposal); empty if it is fine.
  std::function<std::string(const std::string& diff)> check_change;
  // A feature's orchestrator may plan: dispatch / propose_change take `next` (ADR 0026).
  bool plans = false;
};

class SchemaToolHost final : public IToolHost {
 public:
  // `scope` = "root" for the orchestrator, the group id for a navigator.
  SchemaToolHost(index::Index& idx, Role role, std::string scope, SchemaCallbacks cb);
  [[nodiscard]] std::vector<Tool> tools() const override;
  ToolOutcome call(const ToolCall& c) override;

 private:
  index::Index& idx_;
  Role role_;
  std::string scope_;
  SchemaCallbacks cb_;
};

// Worker and tester inside the sandbox of one leaf (or group, for integration tests).
class LeafToolHost final : public IToolHost {
 public:
  LeafToolHost(Sandbox sandbox, Role role, ITestService& tests, std::vector<std::string> test_labels);
  [[nodiscard]] std::vector<Tool> tools() const override;
  ToolOutcome call(const ToolCall& c) override;
  [[nodiscard]] std::vector<std::string> reads() const override { return reads_; }
  [[nodiscard]] const std::set<std::string>& written() const { return written_; }
  [[nodiscard]] const std::string& last_test_text() const { return last_test_; }
  [[nodiscard]] bool api_written() const { return api_written_; }
  // Auto (ADR 0026): a worker's card diff is approved at once and the worker goes on in the same run
  // with the new card; an error (no Auto, the limit of the run) ends the run with the report as before.
  using ApproveNow = std::function<kk::Result<std::string>(const std::string& reason, const std::string& diff)>;
  void set_approve_now(ApproveNow fn) { approve_now_ = std::move(fn); }
  [[nodiscard]] bool approved_in_run() const { return approved_in_run_; }

 private:
  ApproveNow approve_now_;
  bool approved_in_run_ = false;
  Sandbox sb_;
  Role role_;
  ITestService& tests_;
  std::vector<std::string> labels_;
  std::vector<std::string> reads_;
  std::set<std::string> written_;
  std::string last_test_;
  bool api_written_ = false;
};

// Architect: writes cards and api/ headers within its scope (13.1).
class ArchitectToolHost final : public IToolHost {
 public:
  // scope "" = top level (design); otherwise the block being expanded.
  ArchitectToolHost(std::filesystem::path root, index::Index& idx, std::string scope, std::set<std::string> frozen);
  [[nodiscard]] std::vector<Tool> tools() const override;
  ToolOutcome call(const ToolCall& c) override;
  [[nodiscard]] const std::vector<std::string>& written() const { return written_; }

 private:
  std::filesystem::path root_;
  index::Index& idx_;
  std::string scope_;
  std::set<std::string> frozen_;
  std::vector<std::string> written_;
};

// Short question to a block (10.5): read-only tools + answer.
class AskToolHost final : public IToolHost {
 public:
  AskToolHost(index::Index& idx, const core::Schema& schema, std::string block);
  [[nodiscard]] std::vector<Tool> tools() const override;
  ToolOutcome call(const ToolCall& c) override;

 private:
  index::Index& idx_;
  std::string block_;
  bool leaf_ = false;
  std::optional<Sandbox> sb_;
};

}  // namespace kk::agents
