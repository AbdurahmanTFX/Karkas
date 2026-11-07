// karkas mcp — MCP server over stdio (15). stdout carries protocol messages only.
#include <iostream>

#include "app.hpp"
#include "kk/index/index.hpp"
#include "kk/mcp/server.hpp"

namespace kk::cli {

// Provided by cmd_test.cpp when the test pipeline is available.
std::string run_tests_text(const fs::path& root, const std::string& block);
bool have_test_pipeline();

namespace {

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("mcp", "MCP server over stdio for external agents (Claude Code and others)");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, true);
    auto idx = open_index(root);
    mcp::ServerOptions o;
    o.version = KARKAS_VERSION;
    o.analyzer = analyzer();
    if (have_test_pipeline()) {
      o.run_tests = [root](const std::string& block) { return run_tests_text(root, block); };
    }
    mcp::Server server(*idx, o);
    std::ios::sync_with_stdio(false);
    std::exit(server.serve(std::cin, std::cout));
  });
});

}  // namespace
}  // namespace kk::cli
