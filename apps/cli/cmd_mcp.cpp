// karkas mcp — MCP server over stdio (15). stdout carries protocol messages only.
#include <fmt/format.h>

#include <iostream>

#include "app.hpp"
#include "kk/agents/changes.hpp"
#include "kk/core/text.hpp"
#include "kk/index/index.hpp"
#include "kk/mcp/server.hpp"

namespace kk::cli {

// Provided by cmd_test.cpp when the test pipeline is available.
std::string run_tests_text(const fs::path& root, const std::string& block);
bool have_test_pipeline();
// Provided by cmd_check.cpp: the text `karkas check` prints.
std::string check_text(const fs::path& root, bool drift, int* errors);

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
    o.check = [root] { return check_text(root, true, nullptr); };
    o.propose_change = [root](const std::string& reason, const std::string& diff) -> kk::Result<std::string> {
      auto c = agents::propose_change(root, "mcp", reason, diff);
      if (!c) return std::unexpected(c.error());
      return fmt::format("proposed {}. Stop here: ask the user to review and approve it (karkas-gui tab Changes, or "
                         "`karkas approve {}`). Cards it would write:\n{}",
                         c->id, c->id, agents::preview_diff(root, c->diff));
    };
    o.changes = [root](const std::string& id) -> kk::Result<std::string> {
      agents::ChangeStore store(root);
      if (id.empty()) {
        auto all = store.list();
        if (!all) return std::unexpected(all.error());
        std::string text;
        for (const auto& c : *all) text += fmt::format("{}  {:<9} {}  ({})\n", c.id, c.status.value, c.reason.value, c.from.value);
        return text.empty() ? std::string("(no changes)\n") : text;
      }
      auto c = store.load(id);
      if (!c) return std::unexpected(c.error());
      return core::write_change(*c) + "\n" + agents::preview_diff(root, c->diff);
    };
    mcp::Server server(*idx, o);
    std::ios::sync_with_stdio(false);
    std::exit(server.serve(std::cin, std::cout));
  });
});

}  // namespace
}  // namespace kk::cli
