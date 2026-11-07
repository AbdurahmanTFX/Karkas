#pragma once
// Scripted model responses for agent tests (no network, no money): each send() takes the
// next scripted answer. Requests are recorded with their tier for assertions.
#include <deque>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "kk/agents/testing.hpp"
#include "kk/agents/types.hpp"
#include "kk/core/config.hpp"

namespace kk::test {

using json = nlohmann::json;

struct ScriptedModel {
  struct Call {
    std::string tier;
    llm::Request req;
  };
  std::mutex mu;
  std::deque<std::function<llm::Response(const llm::Request&)>> script;
  std::vector<Call> calls;

  void then(std::function<llm::Response(const llm::Request&)> f);
  void tool(const std::string& name, const json& input, llm::Usage u = {100, 20, 0, 0, 50});
  void text(const std::string& t);
  agents::ProviderFactory factory();
  // Text of everything sent in a request (system + messages) — for "no code" checks.
  static std::string request_text(const llm::Request& r);
};

llm::Response tool_response(const std::string& name, const json& input, llm::Usage u = {100, 20, 0, 0, 50});

struct FakeTests final : agents::ITestService {
  std::function<agents::TestRun(const std::vector<std::string>&)> fn;
  std::vector<std::vector<std::string>> calls;
  std::mutex mu;
  kk::Result<agents::TestRun> build_and_test(const std::vector<std::string>& blocks, bool build_only) override;
};

core::Config test_config();

}  // namespace kk::test
