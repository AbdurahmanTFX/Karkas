#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>

#include "kk/agents/bench.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;

TEST_CASE("bench: Claude Code JSON result is parsed with modelUsage (17.5)") {
  agents::BenchResult r;
  agents::parse_claude_json(R"({"type":"result","subtype":"success","is_error":false,"num_turns":7,"total_cost_usd":0.4321,)"
                            R"("usage":{"input_tokens":1200,"output_tokens":300,"cache_creation_input_tokens":5000,"cache_read_input_tokens":20000},)"
                            R"("modelUsage":{"claude-opus-5-5":{"costUSD":0.40},"claude-haiku-5-5":{"costUSD":0.0321}}})",
                            r);
  CHECK(r.cost_usd == doctest::Approx(0.4321));
  CHECK(r.model_calls == 7);
  CHECK(r.cache_read_tokens == 20000);
  CHECK(r.cost_by_model["claude-haiku-5-5"] == doctest::Approx(0.0321));
  agents::BenchResult bad;
  agents::parse_claude_json("not json", bad);
  CHECK(bad.notes.size() == 1);
}

TEST_CASE("bench: every S3 bug patch applies to the reference project") {
  test::FixtureCopy p("wavkit");
  int n = 0;
  for (const auto& e : std::filesystem::directory_iterator(test::source_dir() / "bench/wavkit/bugs")) {
    REQUIRE_MESSAGE(agents::apply_bug(p.path(), e.path()), e.path().string());
    ++n;
  }
  CHECK(n == 5);
  CHECK(core::contains(test::read(p / "blocks/stream/ring/impl/ring.cpp"), "% (buf_.size() - 1)"));
}

TEST_CASE("bench: report with medians and spread") {
  test::TempDir d{"bench"};
  for (int i = 1; i <= 3; ++i) {
    agents::BenchResult r;
    r.scenario = "s3";
    r.method = "C";
    r.model = "tiers";
    r.rep = i;
    r.cost_usd = 0.1 * i;
    r.hidden_pass = 10 + i % 2;
    r.hidden_total = 11;
    r.build_ok = true;
    r.cost_by_role = {{"worker", 0.05 * i}, {"orchestrator", 0.01}};
    test::write(d / "2026-11-07" / ("s3-C-" + std::to_string(i) + ".json"), agents::bench_result_json(r));
  }
  auto rep = agents::bench_report(d.path());
  REQUIRE_MESSAGE(rep, (rep ? "" : rep.error().message));
  CHECK(core::contains(*rep, "0.200 [0.100..0.300]"));
  CHECK(core::contains(*rep, "C/worker"));
  CHECK(core::contains(test::read(d / "report.csv"), "s3,C,tiers,3,0.2000,0.1000,0.3000"));
  CHECK(std::filesystem::exists(d / "report.html"));
}

TEST_CASE("bench: method C on S3 runs the pipeline, builds and runs the hidden tests (scripted model)") {
  test::TempDir work{"benchrun"};
  test::ScriptedModel model;
  for (int i = 0; i < 5; ++i) model.tool("close", {{"verdict", "wont_fix"}, {"note", "benchmark smoke run"}});
  auto cfg = test::test_config();
  cfg.bench_hidden_tests = (test::source_dir() / "bench/wavkit/hidden_tests.example").string();
  agents::BenchOptions o;
  o.scenario = "s3";
  o.method = "C";
  o.model = "tiers";
  o.reps = 1;
  o.bench_dir = test::source_dir() / "bench/wavkit";
  o.reference = test::fixture("wavkit");
  o.results = work / "results";
  o.work = work.path();
  o.providers = model.factory();
  auto r = agents::bench_run(o, cfg, nullptr);
  REQUIRE_MESSAGE(r, (r ? "" : r.error().message));
  REQUIRE(r->size() == 1);
  const auto& res = (*r)[0];
  CHECK(res.build_ok);
  CHECK(res.model_calls == 5);
  CHECK(res.cost_usd > 0.0);
  CHECK(res.cost_by_role.contains("orchestrator"));
  CHECK(res.hidden_total == 11);
  CHECK(res.hidden_pass < 11);  // the injected bugs were not fixed
  CHECK(res.interventions == 0);
}

TEST_CASE("bench: Claude Code command lines of methods A and B, API key or subscription (ADR 0014)") {
  core::Config cfg;
  agents::BenchOptions o;
  o.scenario = "s3";
  o.model = "sonnet";
  o.karkas = "/opt/karkas";
  std::filesystem::path dir = "/w/run";
  auto has = [](const std::vector<std::string>& v, const std::string& a) { return std::ranges::find(v, a) != v.end(); };
  o.method = "A";
  auto a = agents::bench_claude_argv(o, cfg, dir, "fix it");
  CHECK(a[2] == "fix it");
  CHECK(has(a, "--bare"));
  CHECK(has(a, "--strict-mcp-config"));  // no MCP servers at all
  CHECK_FALSE(has(a, "--mcp-config"));
  CHECK_FALSE(has(a, "--agents"));
  o.method = "B";
  auto b = agents::bench_claude_argv(o, cfg, dir, "fix it");
  CHECK(has(b, "/w/run/.bench-mcp.json"));
  CHECK(has(b, "/w/run/.bench-agents.json"));
  CHECK(has(b, "--append-system-prompt-file"));  // --bare reads no CLAUDE.md
  CHECK(has(b, "/w/run/CLAUDE.md"));
  CHECK(core::contains(b[2], "/opt/karkas approve C-NNNN"));
  o.auth = "subscription";
  auto s = agents::bench_claude_argv(o, cfg, dir, "fix it");
  CHECK_FALSE(has(s, "--bare"));
  CHECK(has(s, "--setting-sources"));
  CHECK(has(s, "project,local"));
  CHECK(has(s, "--disable-slash-commands"));
  CHECK_FALSE(has(s, "--append-system-prompt-file"));  // CLAUDE.md is discovered
  CHECK(has(s, "--agents"));
}

TEST_CASE("bench: method B on S3 with a subscription prepares Karkas and hides the API key (fake claude)") {
  test::TempDir work{"benchb"};
  auto fake = work / "claude";
  test::write(fake,
              "#!/bin/sh\n"
              "printf '%s\\n' \"$@\" >> \"$PWD/.fake-args\"\n"
              "env | grep -c '^ANTHROPIC_API_KEY=' >> \"$PWD/.fake-key\"\n"
              "echo '{\"type\":\"result\",\"is_error\":false,\"num_turns\":3,\"total_cost_usd\":0.02,"
              "\"usage\":{\"input_tokens\":100,\"output_tokens\":10}}'\n");
  std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
  ::setenv("ANTHROPIC_API_KEY", "sk-test-not-for-subscription", 1);
  auto cfg = test::test_config();
  cfg.claude_command = fake.string();
  cfg.bench_hidden_tests = (test::source_dir() / "bench/wavkit/hidden_tests.example").string();
  agents::BenchOptions o;
  o.scenario = "s3";
  o.method = "B";
  o.model = "sonnet";
  o.auth = "subscription";
  o.reps = 1;
  o.bench_dir = test::source_dir() / "bench/wavkit";
  o.reference = test::fixture("wavkit");
  o.results = work / "results";
  o.work = work.path();
  auto r = agents::bench_run(o, cfg, nullptr);
  ::unsetenv("ANTHROPIC_API_KEY");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().message));
  REQUIRE(r->size() == 1);
  const auto& res = (*r)[0];
  std::filesystem::path dir = res.workdir;
  CHECK(res.auth == "subscription");
  CHECK(res.model_calls == 15);  // five bug reports, three turns each
  CHECK(res.cost_usd == doctest::Approx(0.10));
  CHECK(res.interventions == 0);
  CHECK(res.build_ok);
  CHECK(res.hidden_total == 11);
  // The Karkas frame of method B: guide, subagents, MCP config.
  CHECK(core::contains(test::read(dir / "CLAUDE.md"), "kk_locate"));
  auto agents = nlohmann::json::parse(test::read(dir / ".bench-agents.json"));
  CHECK(agents.contains("karkas-cheap"));
  CHECK(std::filesystem::exists(dir / ".bench-mcp.json"));
  auto args = test::read(dir / ".fake-args");
  CHECK(core::contains(args, ".bench-agents.json"));
  CHECK_FALSE(core::contains(args, "--bare"));
  // The API key never reached Claude Code: five runs, zero matches each.
  CHECK(test::read(dir / ".fake-key") == "0\n0\n0\n0\n0\n");
  CHECK(core::contains(test::read(work / "results" / res.date / "s3-B-1.json"), "\"auth\": \"subscription\""));
}

TEST_CASE("bench: stream-json — tool calls, tokens per model (subagents included), report of what was used") {
  agents::BenchResult r;
  std::string stream =
      R"({"type":"system","subtype":"init","session_id":"s"})" "\n"
      R"({"type":"assistant","message":{"id":"m1","model":"claude-sonnet-5-5","content":[{"type":"tool_use","name":"mcp__karkas__kk_view","input":{"block":"dsp"}},{"type":"tool_use","name":"Agent","input":{"subagent_type":"karkas-cheap"}}]}})" "\n"
      R"({"type":"user","message":{"content":[{"type":"tool_result","content":"a tool_use mentioned in text"}]}})" "\n"
      R"({"type":"assistant","parent_tool_use_id":"t1","message":{"id":"m2","model":"claude-haiku-5-5","content":[{"type":"tool_use","name":"Edit","input":{}}]}})" "\n"
      R"({"type":"result","is_error":false,"num_turns":9,"total_cost_usd":0.5,"usage":{"input_tokens":10,"output_tokens":700},)"
      R"("modelUsage":{"claude-sonnet-5-5":{"inputTokens":8,"outputTokens":500,"cacheReadInputTokens":9000,"cacheCreationInputTokens":300,"costUSD":0.45},)"
      R"("claude-haiku-5-5":{"inputTokens":2,"outputTokens":200,"cacheReadInputTokens":1000,"cacheCreationInputTokens":50,"costUSD":0.05}}})" "\n";
  agents::parse_claude_json(stream, r);
  CHECK(r.cost_usd == doctest::Approx(0.5));
  CHECK(r.model_calls == 9);
  CHECK(r.tools["mcp__karkas__kk_view"] == 1);
  CHECK(r.tools["Agent"] == 1);
  CHECK(r.tools["Edit"] == 1);  // the subagent's own calls count too
  CHECK(r.tools.size() == 3);
  CHECK(r.tokens_by_model["claude-haiku-5-5"].output == 200);
  CHECK(r.tokens_by_model["claude-sonnet-5-5"].cache_read == 9000);
  // Round trip and report.
  r.scenario = "s3";
  r.method = "B";
  r.model = "sonnet";
  r.logs = {"2026-11-09/s3-B-1.p1.jsonl"};
  auto back = agents::parse_bench_result(agents::bench_result_json(r));
  REQUIRE(back);
  CHECK(back->tools == r.tools);
  CHECK(back->logs == r.logs);
  CHECK(back->tokens_by_model["claude-haiku-5-5"].cache_write == 50);
  test::TempDir d{"benchrep"};
  test::write(d / "2026-11-09" / "s3-B-1.json", agents::bench_result_json(r));
  auto rep = agents::bench_report(d.path());
  REQUIRE(rep);
  CHECK(core::contains(*rep, "s3 B sonnet: subagent calls 1, karkas tool calls 1; claude-haiku-5-5 200 out, claude-sonnet-5-5 500 out"));
}
