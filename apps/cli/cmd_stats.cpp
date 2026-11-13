// karkas stats [ticket] (ADR 0026): how the runs ended per role and tier — the data to route by —
// or the runs of one ticket with the traffic of its turns in mode mcp.
#include <iostream>

#include "app.hpp"
#include "kk/index/index.hpp"
#include "kk/metrics/metrics.hpp"

namespace kk::cli {
namespace {

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("stats", "runs per role and tier (how they ended, steps, cost), or the runs of one ticket");
  static std::string ticket;
  static std::string since;
  cmd->add_option("ticket", ticket, "a ticket id (T-0014): its runs and, in mode mcp, the traffic of its turns");
  cmd->add_option("--since", since, "YYYY-MM-DD or unix time");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto idx = open_index(root);
    auto ts = metrics::parse_since(since);
    if (!ts) die(ts.error().message);
    std::cout << metrics::stats_report(idx->runs(std::to_string(*ts)), ticket, root);
  });
});

}  // namespace
}  // namespace kk::cli
