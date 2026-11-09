#include <doctest/doctest.h>

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
