#pragma once
// The scenarios of section 13 on top of the agent loop: design top-down with stops for
// approval, implement leaves (testers, workers, composition root, integration tests),
// tasks and debugging by descending the schema with append-only tickets (10, 11, 12).
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kk/agents/loop.hpp"
#include "kk/agents/router.hpp"
#include "kk/agents/testing.hpp"
#include "kk/agents/types.hpp"
#include "kk/core/config.hpp"
#include "kk/core/tickets.hpp"
#include "kk/index/index.hpp"
#include "kk/llm/http.hpp"

namespace kk::agents {

struct EngineOptions {
  ProviderFactory providers;      // default: make_provider for the tier
  ITestService* tests = nullptr;  // default: TestService (gen cmake + cmake + ctest)
  index::ICodeAnalyzer* analyzer = nullptr;
  std::function<void(const std::string&)> progress;  // one-line status messages
};

struct LeafOutcome {
  std::string status;  // fixed | done | failed | not_here | needs_contract_change | needs_info | needs_tester | escalated
  std::string cause, fix, files, tests, api;
  std::vector<std::string> proposal;
  std::string proposal_error;  // why the worker's proposal was not usable as a KSL diff (if any)
  std::string api_note;        // what happened to the own api/ (values re-frozen, changed to the card)
  std::string tier;
  int attempts = 0;
  metrics::Tokens tokens;
  double cost = 0;
  bool tests_ok = false;
  std::string test_text;
  std::vector<std::string> reads;  // paths read (journal checks)
};

class Engine {
 public:
  // Opens the project index; fails without throwing (6.6).
  static kk::Result<std::unique_ptr<Engine>> create(fs::path root, core::Config cfg, EngineOptions opts = {});
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  index::Index& index();
  [[nodiscard]] const core::Config& config() const { return cfg_; }

  // --- 13.1 new project -------------------------------------------------------------------
  kk::Result<std::string> design(const std::string& idea);
  kk::Result<std::string> expand(const std::string& block, bool all);
  // what: "L0", "api" (optionally one block), or a change id C-NNNN.
  kk::Result<std::string> approve(const std::string& what, const std::string& block = "");
  kk::Result<std::string> reject(const std::string& change_id);
  kk::Result<std::string> implement(const std::string& target);

  // --- 13.2 / 13.3 tickets -------------------------------------------------------------------
  kk::Result<std::string> task(const std::string& description);
  kk::Result<std::string> debug(const std::string& symptom, const std::vector<std::string>& facts,
                                const std::string& origin = "human");
  // force: run even when the last run stopped on something only a human resolves and
  // nothing changed since (otherwise refused without starting a model, ADR 0020).
  kk::Result<std::string> run_ticket(const std::string& id, bool force = false);
  // ask(block, question): short cheap agent; the answer is appended to the trail (10.5).
  kk::Result<std::string> ask(const std::string& block, const std::string& question, const std::string& ticket);

  // --- leaves (stage 6) ----------------------------------------------------------------------
  LeafOutcome run_tester(const std::string& block, const std::string& ticket = "");
  LeafOutcome run_worker(const std::string& block, const std::string& ticket, const std::string& task,
                         const std::string& difficulty = "");
  kk::Result<TestRun> test(const std::string& block);

  // Context texts (exposed for tests: the orchestrator never sees code).
  std::string context_for(Role role, const std::string& block);

 private:
  Engine(fs::path root, core::Config cfg, EngineOptions opts);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  fs::path root_;
  core::Config cfg_;
};

}  // namespace kk::agents
