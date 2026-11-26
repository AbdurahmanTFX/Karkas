// karkas hsgm [measurement] (ADR 0049): experimental read-only measurements of a code project
// from its index — what the tests can tell apart, how true the views are, how far a change spreads.
#include <iostream>

#include "app.hpp"
#include "kk/hsgm/consistency.hpp"
#include "kk/hsgm/diag.hpp"
#include "kk/hsgm/propagation.hpp"
#include "kk/hsgm/views.hpp"
#include "kk/index/index.hpp"

namespace kk::cli {
namespace {

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("hsgm", "experimental HSGM measurements (ADR 0049): diag views propagation consistency width");
  static std::string what;
  cmd->add_option("measurement", what, "diag | views | propagation | consistency | width (none: a summary)");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto idx = open_index(root);
    auto in = hsgm::gather(*idx);
    if (!in) die(in.error().str());
    if (!hsgm::is_code_project(*in)) {
      std::cout << "hsgm: not a code project\n";
      return;
    }
    if (what == "diag") {
      std::cout << hsgm::format(hsgm::diag(*in));
      return;
    }
    if (what == "views") {
      std::cout << hsgm::format(hsgm::views(*in));
      return;
    }
    if (what == "propagation") {
      std::cout << hsgm::format(hsgm::propagation(*in));
      return;
    }
    if (what == "consistency") {
      std::cout << hsgm::format(hsgm::consistency(*in));
      return;
    }
    die("unknown measurement '" + what + "': diag views propagation consistency width");
  });
});

}  // namespace
}  // namespace kk::cli
