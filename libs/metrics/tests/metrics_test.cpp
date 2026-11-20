#include <doctest/doctest.h>

#include <unistd.h>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/metrics/metrics.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {
core::Config priced() {
  core::Config c;
  c.prices["claude-opus-5-5"] = {4.0, 20.0, 0.20, -1, -1};
  c.prices["claude-haiku-5-5"] = {0.10, 0.50, -1, -1, -1};
  return c;
}
}  // namespace

TEST_CASE("metrics: cost formula of 16.2") {
  auto cfg = priced();
  metrics::Tokens t{1'000'000, 100'000, 200'000, 0, 500'000};
  // haiku: 1M*0.10 + 0.2M*0.10*1.25 + 0.5M*0.10*0.1 + 0.1M*0.50 = 0.1 + 0.025 + 0.005 + 0.05
  auto h = metrics::cost_usd(cfg, "anthropic", "claude-haiku-5-5", t);
  REQUIRE(h);
  CHECK(*h == doctest::Approx(0.18));
  // opus with an absolute cache-read price and a 1h write: 1M*4 + 0.5M*0.2 + 0.1M*20 + 0.1M*4*2
  metrics::Tokens o{1'000'000, 100'000, 0, 100'000, 500'000};
  auto c = metrics::cost_usd(cfg, "anthropic", "claude-opus-5-5", o);
  REQUIRE(c);
  CHECK(*c == doctest::Approx(4.0 + 0.1 + 2.0 + 0.8));
}

TEST_CASE("metrics: unpriced cloud model is an error, local models are free") {
  auto cfg = priced();
  auto r = metrics::cost_usd(cfg, "anthropic", "claude-sonnet-5-5", {});
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "claude-sonnet-5-5"));
  auto l = metrics::cost_usd(cfg, "openai_compat", "qwen-coder", {100, 100, 0, 0, 0});
  REQUIRE(l);
  CHECK(*l == 0.0);
}

TEST_CASE("metrics: cost report recomputes money from current prices") {
  index::RunRow r;
  r.ticket = "T-1";
  r.role = "worker";
  r.block = "stream.ring";
  r.provider = "openai_compat";
  r.model = "deepseek-v4-pro";
  r.input_tokens = 26937;
  r.output_tokens = 15576;
  r.cache_read_tokens = 164352;
  r.cost_usd = 0.0;  // stored before the model had a price
  core::Config c;
  c.prices["deepseek-v4-pro"] = {0.66, 1.98, 0.022, 0, 0};
  auto rep = metrics::cost_report(c, {r}, "model");
  REQUIRE(rep);
  CHECK(core::contains(*rep, "deepseek-v4-pro"));
  // (26937*0.66 + 164352*0.022 + 15576*1.98) / 1e6 = 0.0522, not the stored 0.
  CHECK(core::contains(*rep, "0.0522"));
}

TEST_CASE("metrics: journal lines, active marker, runs row, report") {
  test::FixtureCopy p("wavkit");
  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  REQUIRE((*idx)->update());
  auto j = metrics::RunJournal::open(p.path(), {"T-0001", "worker", "stream.ring", "anthropic", "claude-haiku-5-5", "cheap", 1});
  REQUIRE(j);
  CHECK(metrics::active_runs(p.path()).size() == 1);
  REQUIRE(j->step("request", R"({"tokens":10})"));
  metrics::Tokens t{1000, 200, 300, 0, 4000};
  auto cost = metrics::cost_usd(priced(), "anthropic", "claude-haiku-5-5", t);
  REQUIRE(cost);
  auto row = j->finish(t, *cost, 1500, 3, "fixed", idx->get());
  REQUIRE(row);
  CHECK(metrics::active_runs(p.path()).empty());
  auto text = test::read(j->path());
  CHECK(core::contains(text, "\"type\":\"run_start\""));
  CHECK(core::contains(text, "\"type\":\"step\""));
  CHECK(core::contains(text, "\"type\":\"run_end\""));
  auto runs = (*idx)->runs();
  REQUIRE(runs.size() == 1);
  CHECK(runs[0].cache_write_tokens == 300);
  // Re-indexing does not duplicate the run (journal is the key).
  REQUIRE((*idx)->update({true, nullptr}));
  CHECK((*idx)->runs().size() == 1);
  auto rep = metrics::cost_report(priced(), runs, "block");
  REQUIRE(rep);
  CHECK(core::contains(*rep, "stream.ring"));
  CHECK(core::contains(*rep, "total"));
  core::Config empty;
  CHECK_FALSE(metrics::cost_report(empty, runs, "role"));
  CHECK_FALSE(metrics::cost_report(priced(), runs, "colour"));
}

TEST_CASE("metrics: a marker whose owner process is gone is dropped") {
  test::FixtureCopy p("wavkit");
  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  REQUIRE((*idx)->update());
  auto j = metrics::RunJournal::open(p.path(), {"T-0001", "worker", "stream.ring", "anthropic", "claude-haiku-5-5", "cheap", 1});
  REQUIRE(j);
  CHECK(metrics::active_runs(p.path()).size() == 1);
  // Simulate a killed run: overwrite the marker's pid with one that does not exist.
  auto marker = j->path().parent_path() / "active" / (j->path().stem().string() + ".json");
  auto text = test::read(marker);
  REQUIRE(core::contains(text, "\"pid\""));
  auto dead = text.substr(0, text.rfind("\"pid\"")) + "\"pid\": 2147483647}";
  test::write(marker, dead);
  CHECK(metrics::active_runs(p.path()).empty());
}

TEST_CASE("metrics: --since parsing") {
  CHECK(metrics::parse_since("2026-11-07").value() == 1794009600);
  CHECK(metrics::parse_since("1730000000").value() == 1730000000);
  CHECK_FALSE(metrics::parse_since("yesterday"));
}

TEST_CASE("metrics: a stop request reaches its run only, and goes away with it (ADR 0019)") {
  test::FixtureCopy p("wavkit");
  auto a = metrics::RunJournal::open(p.path(), {"T-0001", "worker", "stream.ring", "anthropic", "claude-haiku-5-5", "cheap", 1});
  auto b = metrics::RunJournal::open(p.path(), {"T-0001", "tester", "stream.ring", "anthropic", "claude-haiku-5-5", "cheap", 1});
  REQUIRE(a);
  REQUIRE(b);
  CHECK_FALSE(a->stop_requested());
  REQUIRE(metrics::request_stop(p.path(), a->path().filename()));
  CHECK(a->stop_requested());
  CHECK_FALSE(b->stop_requested());
  auto act = metrics::active_runs(p.path());
  REQUIRE(act.size() == 2);
  for (const auto& r : act) {
    CHECK(r.pid == ::getpid());
    CHECK(r.stopping == (r.journal.filename() == a->path().filename()));
  }
  REQUIRE(a->finish({}, 0, 1, 1, "cancelled"));
  auto stop = a->path().parent_path() / "active" / (a->path().stem().string() + ".stop");
  CHECK_FALSE(std::filesystem::exists(stop));
  CHECK(metrics::active_runs(p.path()).size() == 1);
  // Not active any more: nothing to stop.
  CHECK_FALSE(metrics::request_stop(p.path(), a->path()));
  // An orphaned stop file (the run was killed) is cleaned up.
  REQUIRE(core::write_file(stop, "1\n"));
  (void)metrics::active_runs(p.path());
  CHECK_FALSE(std::filesystem::exists(stop));
}

TEST_CASE("metrics: stats per role and tier, and the runs and MCP traffic of a ticket (ADR 0026)") {
  kk::test::TempDir d{"stats"};
  std::vector<kk::index::RunRow> runs(3);
  runs[0].ticket = "T-0001";
  runs[0].role = "orchestrator";
  runs[0].tier = "strong";
  runs[0].steps = 1;
  runs[0].outcome = "dispatch";
  runs[0].journal = "20261113-100000-000-orchestrator-root.jsonl";
  runs[1] = runs[0];
  runs[1].role = "worker";
  runs[1].block = "stream.ring";
  runs[1].tier = "cheap";
  runs[1].steps = 4;
  runs[1].outcome = "max_steps";
  runs[1].journal = "20261113-100010-000-worker-stream.ring.jsonl";
  runs[2] = runs[1];
  runs[2].outcome = "done";
  runs[2].attempt = 2;
  kk::test::write(d.path() / ".karkas/mcp/traffic/20261113-100010-000-worker-stream.ring.jsonl",
                  "{\"turn\":\"x-001\",\"shown\":20000,\"whole\":true,\"seen\":false}\n{\"turn\":\"x-001\",\"answer\":300}\n"
                  "{\"turn\":\"x-002\",\"shown\":1000,\"whole\":false,\"seen\":false}\n");
  auto all = kk::metrics::stats_report(runs, "", d.path());
  CHECK(kk::core::contains(all, "worker               cheap                2       50%"));
  auto one = kk::metrics::stats_report(runs, "T-0001", d.path());
  CHECK(kk::core::contains(one, "total: 3 runs (1 orchestrator, 2 worker), 9 steps"));
  CHECK(kk::core::contains(one, "mcp traffic: 4 turns, 2 of them whole (0 shorter by seen), 42000 chars shown (95% in whole turns)"));
}

TEST_CASE("metrics: cost report prices 1-hour cache writes at 2x and takes a session's own cost (ADR 0031)") {
  core::Config c;
  c.prices["claude-opus-5-5"] = {4.0, 20.0, 0.20, -1, -1};
  index::RunRow est;
  est.ticket = "T-1";
  est.role = "orchestrator";
  est.provider = "mcp";
  est.model = "claude-opus-5-5";
  est.input_tokens = 10;
  est.output_tokens = 2248;
  est.cache_write_tokens = 14883;
  est.cache_write_1h_tokens = 14883;
  est.cache_read_tokens = 37458;
  auto rep = metrics::cost_report(c, {est}, "");
  REQUIRE(rep);
  // 10*4 + 14883*8 + 37458*0.20 + 2248*20 = 171,567 / 1e6 (the session reported $0.1716); at 1.25x it was 0.1270.
  CHECK_MESSAGE(core::contains(*rep, "0.1716"), *rep);
  index::RunRow s = est;
  s.session_cost = true;
  s.cost_usd = 0.2;
  rep = metrics::cost_report(c, {est, s}, "");
  REQUIRE(rep);
  CHECK_MESSAGE(core::contains(*rep, "0.3716"), *rep);
  CHECK(core::contains(*rep, "1 of 2 run(s) at the cost their Claude Code session reported"));
}
