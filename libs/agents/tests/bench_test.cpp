#include <doctest/doctest.h>

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
