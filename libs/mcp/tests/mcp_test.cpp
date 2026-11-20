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
  CHECK(list["result"]["tools"].size() == 17);
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
  CHECK(list["result"]["tools"].size() == 18);
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
  CHECK(list["result"]["tools"].size() == 20);
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

TEST_CASE("mcp: kk_model_for is offered when wired and passes block and role") {
  McpFixture f;
  mcp::ServerOptions o;
  std::string got;
  o.model_for = [&](const std::string& block, const std::string& role) -> kk::Result<std::string> {
    got = block + "/" + role;
    if (block == "nowhere") return kk::fail("model", "no block nowhere");
    return std::string("tier cheap\n");
  };
  mcp::Server server(*f.idx, o);
  auto list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
  CHECK(list["result"]["tools"].size() == 18);
  auto call = [&](const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"}, {"params", {{"name", "kk_model_for"}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
  };
  auto r = call({{"block", "stream.ring"}, {"role", "tester"}});
  CHECK(r["isError"] == false);
  CHECK(r["content"][0]["text"] == "tier cheap\n");
  CHECK(got == "stream.ring/tester");
  CHECK(call(json::object())["isError"] == false);
  CHECK(got == "/");
  CHECK(call({{"block", "nowhere"}})["isError"] == true);
}

TEST_CASE("mcp: kk_view carries the model line of the block when wired") {
  McpFixture f;
  mcp::ServerOptions o;
  o.model_hint = [](const std::string& block) { return "model: tier cheap -> subagent karkas-cheap for " + block + "\n"; };
  mcp::Server server(*f.idx, o);
  auto r = json::parse(*server.handle(
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"kk_view","arguments":{"block":"stream.ring"}}})"));
  auto text = r["result"]["content"][0]["text"].get<std::string>();
  CHECK(text.ends_with("model: tier cheap -> subagent karkas-cheap for stream.ring\n"));
  CHECK(core::contains(text, "stream.ring"));
}

TEST_CASE("mcp: kk_locate answers with ranked blocks") {
  McpFixture f;
  mcp::Server server(*f.idx, {});
  auto r = json::parse(*server.handle(
      R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"kk_locate","arguments":{"text":"ring buffer drops frames"}}})"));
  CHECK(r["result"]["isError"] == false);
  CHECK(core::contains(r["result"]["content"][0]["text"].get<std::string>(), "1. stream.ring"));
}

TEST_CASE("mcp: kk_note_bug records a bug found on the way as an open ticket") {
  McpFixture f;
  mcp::Server server(*f.idx, {});
  auto call = [&](const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "kk_note_bug"}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
  };
  auto r = call({{"summary", "echo feedback is not clamped to 0.99"},
                 {"where", "blocks/dsp/echo/impl/echo.cpp:24"},
                 {"evidence", "feedback_ = feedback; spec says values above are clamped"}});
  CHECK(r["isError"] == false);
  auto text = r["content"][0]["text"].get<std::string>();
  CHECK(text.starts_with("noted T-0002"));  // the fixture already holds T-0001
  core::TicketStore store(f.p.path());
  auto t = store.load("T-0002");
  REQUIRE(t);
  CHECK(t->kind.value == "bug");
  CHECK(t->origin.value == "agent");
  CHECK(t->status.value == "open");
  CHECK(t->symptom.value == "echo feedback is not clamped to 0.99");
  REQUIRE(t->facts.size() == 2);
  CHECK(t->facts[0].text == "found at blocks/dsp/echo/impl/echo.cpp:24");
  CHECK(call({{"summary", "  "}})["isError"] == true);
}

TEST_CASE("mcp: a request limited to a block, a message about a ticket, the prototype (ADR 0034, 0035)") {
  McpFixture f;
  mcp::ServerOptions o;
  std::string got;
  o.start_ticket = [&](const std::string& kind, const std::string& text, const std::vector<std::string>& facts,
                       const std::string& block) -> kk::Result<std::string> {
    got = kind + "|" + text + "|" + std::to_string(facts.size()) + "|" + block;
    return std::string("created T-0002");
  };
  o.run_ticket = [&](const std::string& id, bool force, const std::string& note) -> kk::Result<std::string> {
    got = id + "|" + (force ? "force" : "") + "|" + note;
    return std::string("runs again");
  };
  o.prototype = [&](const std::string& action, const std::string& sketch) -> kk::Result<std::string> {
    got = action + "|" + sketch;
    if (action == "send") return kk::fail("prototype", "no prototype");
    return std::string("ok");
  };
  mcp::Server server(*f.idx, o);
  auto list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
  json schema;
  for (const auto& t : list["result"]["tools"]) {
    if (t["name"] == "kk_task") schema = t["inputSchema"];
  }
  CHECK(schema["properties"].contains("block"));
  auto call = [&](const std::string& name, const json& args) {
    json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", args}}}};
    return json::parse(*server.handle(req.dump()))["result"];
  };
  CHECK(call("kk_task", {{"text", "add reset"}, {"block", "stream.ring"}})["isError"] == false);
  CHECK(got == "feature|add reset|0|stream.ring");
  CHECK(call("kk_debug", {{"text", "drops"}, {"facts", {"fill=0"}}})["isError"] == false);
  CHECK(got == "bug|drops|1|");
  CHECK(call("kk_ticket_run", {{"id", "T-0001"}, {"note", "also on seek"}})["isError"] == false);
  CHECK(got == "T-0001||also on seek");
  auto set = call("kk_prototype", {{"action", "set"}, {"idea", "player"}, {"blocks", {{{"name", "io"}}}}});
  CHECK(set["isError"] == false);
  auto sent = json::parse(got.substr(got.find('|') + 1));
  CHECK(got.starts_with("set|"));
  CHECK(sent["idea"] == "player");
  CHECK(sent["blocks"][0]["name"] == "io");
  CHECK_FALSE(sent.contains("action"));
  CHECK(call("kk_prototype", {{"action", "send"}})["isError"] == true);
}
