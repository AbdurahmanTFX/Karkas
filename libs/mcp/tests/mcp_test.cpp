#include <doctest/doctest.h>

#include <nlohmann/json.hpp>
#include <sstream>

#include "kk/core/text.hpp"
#include "kk/core/tickets.hpp"
#include "kk/index/index.hpp"
#include "kk/mcp/server.hpp"
#include "kk/test/support.hpp"

using namespace kk;
using json = nlohmann::json;

namespace {

struct McpFixture {
  test::FixtureCopy p{"wavkit"};
  std::unique_ptr<index::Index> idx;
  McpFixture() {
    auto i = index::Index::open(p.path());
    REQUIRE(i);
    idx = std::move(*i);
    core::TicketStore store(p.path());
    auto t = store.create(core::TicketKind::Bug, "human", "crackle after 10 s", {"contract stream.ring/no_drop FAIL"});
    REQUIRE(t);
    core::TrailEntry e;
    e.type = core::TrailEntry::Type::Level;
    e.level = 0;
    e.role = "orchestrator";
    e.attrs = {{"at", "root"}, {"suspect", "stream,dsp"}, {"pick", "stream"}, {"difficulty", "M"}, {"why", "frames dropped"}};
    REQUIRE(store.append(t->id, e));
  }
};

}  // namespace

TEST_CASE("mcp: golden JSON-RPC session") {
  McpFixture f;
  mcp::Server server(*f.idx, {});
  std::istringstream in(test::read(test::golden("mcp/session.in.jsonl")));
  std::ostringstream out;
  CHECK(server.serve(in, out) == 0);
  CHECK_MESSAGE(test::matches_golden("mcp/session.out.jsonl", out.str()), out.str());
}

TEST_CASE("mcp: protocol details") {
  McpFixture f;
  mcp::Server server(*f.idx, {});
  auto init = server.handle(R"({"jsonrpc":"2.0","id":"a","method":"initialize","params":{"protocolVersion":"2024-11-05"}})");
  REQUIRE(init);
  auto j = json::parse(*init);
  CHECK(j["id"] == "a");
  CHECK(j["result"]["protocolVersion"] == "2024-11-05");
  CHECK(j["result"]["capabilities"].contains("tools"));
  CHECK(j["result"]["serverInfo"]["name"] == "karkas");
  // Notifications get no answer.
  CHECK_FALSE(server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
  CHECK_FALSE(server.handle(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}})"));
  // tools/list uses inputSchema (MCP), not input_schema (Claude API).
  auto list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
  CHECK(list["result"]["tools"].size() == 15);
  // The instructions tell a fresh agent the method and where to read it.
  CHECK(j["result"]["instructions"].get<std::string>().find("kk_guide") != std::string::npos);
  for (const auto& t : list["result"]["tools"]) {
    CHECK(t.contains("inputSchema"));
    CHECK_FALSE(t.contains("input_schema"));
    CHECK(t["inputSchema"]["type"] == "object");
  }
  // Invalid request.
  auto bad = json::parse(*server.handle(R"({"id":3,"method":"ping"})"));
  CHECK(bad["error"]["code"] == -32600);
}

TEST_CASE("mcp: kk_run_tests is offered only with a runner") {
  McpFixture f;
  mcp::ServerOptions o;
  o.run_tests = [](const std::string& b) { return "tests " + b + " contract 3/3\n"; };
  mcp::Server server(*f.idx, o);
  auto list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
  CHECK(list["result"]["tools"].size() == 16);
  auto r = json::parse(*server.handle(
      R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"kk_run_tests","arguments":{"block":"dsp"}}})"));
  CHECK(r["result"]["content"][0]["text"] == "tests dsp contract 3/3\n");
  CHECK(r["result"]["isError"] == false);
}

TEST_CASE("mcp: the guide, the KSL reference and the write side") {
  McpFixture f;
  mcp::ServerOptions o;
  std::string got_reason, got_diff;
  o.check = [] { return std::string("check: 9 blocks, 0 errors, 0 warnings\n"); };
  o.propose_change = [&](const std::string& reason, const std::string& diff) -> kk::Result<std::string> {
    got_reason = reason;
    got_diff = diff;
    if (diff.find("nowhere") != std::string::npos) return kk::fail("change", "the change breaks the schema checks");
    return std::string("proposed C-0001");
  };
  o.changes = [](const std::string& id) -> kk::Result<std::string> { return id.empty() ? "C-0001  pending" : "@change " + id; };
  mcp::Server server(*f.idx, o);
  auto call = [&](const std::string& name, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
  };
  auto list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
  CHECK(list["result"]["tools"].size() == 18);
  auto guide = call("kk_guide", json::object());
  CHECK(guide["content"][0]["text"].get<std::string>().find("Schema first") != std::string::npos);
  auto ref = call("kk_ksl_reference", json::object());
  auto ref_text = ref["content"][0]["text"].get<std::string>();
  CHECK(ref_text.find("Change diff lines") != std::string::npos);
  CHECK(ref_text.find("+ @block world.pickups") != std::string::npos);
  CHECK(call("kk_check", json::object())["content"][0]["text"] == "check: 9 blocks, 0 errors, 0 warnings\n");
  auto ok = call("kk_propose_change", {{"reason", "reverb"}, {"diff", "+ @block dsp.reverb"}});
  CHECK(ok["isError"] == false);
  CHECK(got_reason == "reverb");
  CHECK(got_diff == "+ @block dsp.reverb");
  auto bad = call("kk_propose_change", {{"reason", "x"}, {"diff", "+ call nowhere:x"}});
  CHECK(bad["isError"] == true);
  CHECK(call("kk_changes", json::object())["content"][0]["text"] == "C-0001  pending");
  CHECK(call("kk_changes", {{"id", "C-0001"}})["content"][0]["text"] == "@change C-0001");
}
