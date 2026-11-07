// karkas test [<block>] — build and run tests without a model (8, 12); also behind kk_run_tests.
#include <fmt/format.h>

#include <iostream>

#include "app.hpp"
#include "kk/agents/testing.hpp"
#include "kk/core/config.hpp"
#include "kk/index/index.hpp"

namespace kk::cli {

bool have_test_pipeline() { return true; }

std::string run_tests_text(const fs::path& root, const std::string& block) {
  auto cfg = core::load_config(root);
  if (!cfg) return cfg.error().str() + "\n";
  agents::TestService svc(root, *cfg);
  std::vector<std::string> labels;
  if (!block.empty() && block != "all" && block != "root") labels.push_back(block);
  auto r = svc.build_and_test(labels);
  if (!r) return r.error().message + "\n";
  return r->build_ok ? r->text : "build failed\n" + r->build_summary + "\n";
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
