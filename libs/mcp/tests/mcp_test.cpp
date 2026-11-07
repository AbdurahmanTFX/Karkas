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
  CHECK(list["result"]["tools"].size() == 13);
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
  CHECK(list["result"]["tools"].size() == 14);
  auto r = json::parse(*server.handle(
      R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"kk_run_tests","arguments":{"block":"dsp"}}})"));
  CHECK(r["result"]["content"][0]["text"] == "tests dsp contract 3/3\n");
  CHECK(r["result"]["isError"] == false);
}
