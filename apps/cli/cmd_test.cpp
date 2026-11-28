// karkas test [<block>] — build and run tests without a model (8, 12); also behind kk_run_tests.
#include <fmt/format.h>

#include <expected>
#include <iostream>

#include "app.hpp"
#include "kk/agents/testing.hpp"
#include "kk/core/config.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/tickets.hpp"
#include "kk/index/index.hpp"

namespace kk::cli {

bool have_test_pipeline() { return true; }

namespace {

// Builds and runs the tests of `block` ("" / "all" / "root": every test); the text of an error on failure.
std::expected<agents::TestRun, std::string> run_block_tests(const fs::path& root, const std::string& block) {
  auto cfg = core::load_config(root);
  if (!cfg) return std::unexpected(cfg.error().str() + "\n");
  agents::TestService svc(root, *cfg);
  std::vector<std::string> labels;
  if (!block.empty() && block != "all" && block != "root") labels.push_back(block);
  auto r = svc.build_and_test(labels);
  if (!r) return std::unexpected(r.error().message + "\n");
  return std::move(*r);
}

}  // namespace

std::string run_tests_text(const fs::path& root, const std::string& block) {
  auto r = run_block_tests(root, block);
  if (!r) return r.error();
  return r->build_ok ? r->text : "build failed\n" + r->build_summary + "\n";
}

// kk_run_tests: the summary, then the failing blocks that already have an open bug ticket and those
// without one (docs/adr/0050).
std::string mcp_run_tests_text(const fs::path& root, const std::string& block) {
  auto r = run_block_tests(root, block);
  if (!r) return r.error();
  if (!r->build_ok) return "build failed\n" + r->build_summary + "\n";
  std::vector<std::string> failing;
  for (const auto& b : r->blocks) {
    auto short_of = [](const runner::Counts& c) { return c.pass < c.total; };
    if (b.build_failed || !b.failures.empty() || short_of(b.contract) || short_of(b.unit) || short_of(b.integration)) {
      failing.push_back(b.block);
    }
  }
  if (failing.empty()) return r->text;
  auto schema = core::load_schema(root);
  if (!schema) return r->text;
  auto bugs = core::open_bugs(core::TicketStore(root), *schema);
  if (!bugs) return r->text;
  return r->text + core::known_failures_text(failing, *bugs, *schema);
}

namespace {

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("test", "build and run the tests of the project or of a block subtree (no model)");
  static std::string block;
  cmd->add_option("block", block, "block id (ctest label); default: all tests");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto text = run_tests_text(root, block);
    std::cout << text;
    auto idx = open_index(root);
    auto st = idx->status(block);
    if (st) std::cout << *st;
    if (text.starts_with("build failed") || text.find("\nfail ") != std::string::npos || text.starts_with("fail ")) std::exit(1);
  });
});

}  // namespace
}  // namespace kk::cli
