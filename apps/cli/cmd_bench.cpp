// karkas bench run | report (17.7).
#include <fmt/format.h>

#include <iostream>

#include "app.hpp"
#include "kk/agents/bench.hpp"
#include "kk/core/config.hpp"
#include "kk/core/paths.hpp"

namespace kk::cli {
namespace {

fs::path default_bench_dir() {
  if (fs::exists("bench/wavkit/spec.md")) return core::normalize("bench/wavkit");
  return fs::path(KK_SOURCE_DIR) / "bench" / "wavkit";
}

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* bench = cli.add_subcommand("bench", "benchmarks: run | report (section 17)");
  bench->require_subcommand(1);
  static agents::BenchOptions o;
  static std::string bench_dir, reference, results, work;
  auto* run = bench->add_subcommand("run", "run a scenario with a method in fresh copies");
  run->add_option("--scenario", o.scenario, "s1 | s2 | s3")->required()->check(CLI::IsMember({"s1", "s2", "s3"}));
  run->add_option("--method", o.method, "A | B | C")->required()->check(CLI::IsMember({"A", "B", "C"}));
  run->add_option("--model", o.model, "model for A/B (C uses the tiers of the config)");
  run->add_option("--auth", o.auth, "A/B: api (claude --bare, ANTHROPIC_API_KEY) | subscription (claude.ai login)")
      ->check(CLI::IsMember({"api", "subscription"}));
  run->add_option("--reps", o.reps, "repetitions (>= 3 for the report, 17.6)");
  run->add_option("--bench-dir", bench_dir, "bench/wavkit");
  run->add_option("--reference", reference, "reference project for S2/S3 (default tests/fixtures/wavkit)");
  run->add_option("--results", results, "results folder (default bench/results)");
  run->add_option("--work", work, "where the fresh copies go (default: temp)");
  run->callback([&app] {
    setup_logging(app, {}, false);
    auto cfg = core::load_config(fs::current_path());
    if (!cfg) die(cfg.error().str());
    o.bench_dir = bench_dir.empty() ? default_bench_dir() : core::normalize(bench_dir);
    o.reference = reference.empty() ? fs::path(KK_SOURCE_DIR) / "tests" / "fixtures" / "wavkit" : core::normalize(reference);
    o.results = results.empty() ? o.bench_dir.parent_path() / "results" : core::normalize(results);
    o.work = work;
    std::error_code ec;
    o.karkas = fs::read_symlink("/proc/self/exe", ec);
    auto r = agents::bench_run(o, *cfg, [](const std::string& m) { std::cerr << "· " << m << "\n"; });
    if (!r) die(r.error().str());
    for (const auto& x : *r) {
      std::cout << fmt::format("{} {} rep {}: ${:.4f} hidden {}/{} build {} {:.0f}s\n", x.scenario, x.method, x.rep, x.cost_usd,
                               x.hidden_pass, x.hidden_total, x.build_ok ? "ok" : "failed", x.seconds);
    }
  });
  auto* report = bench->add_subcommand("report", "medians and spreads over all results (CSV, text, HTML)");
  report->add_option("--results", results, "results folder (default bench/results)");
  report->callback([] {
    auto dir = results.empty() ? default_bench_dir().parent_path() / "results" : core::normalize(results);
    auto r = agents::bench_report(dir);
    if (!r) die(r.error().message);
    std::cout << *r << "written: " << (dir / "report.csv").string() << ", report.txt, report.html\n";
  });
});

}  // namespace
}  // namespace kk::cli
