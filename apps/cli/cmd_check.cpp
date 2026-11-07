// karkas check — KSL checks (4.6) and, when kk_code is available, drift checks (8.2).
#include <fmt/format.h>

#include <iostream>

#include "app.hpp"
#include "kk/core/validate.hpp"
#ifdef KK_HAVE_CODE
#include "kk/code/drift.hpp"
#endif

namespace kk::cli {
namespace {

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("check", "check KSL cards and code drift");
  static bool no_drift = false;
  static bool quiet = false;
  cmd->add_flag("--no-drift", no_drift, "only KSL checks (E/W), skip drift checks (D)");
  cmd->add_flag("-q,--quiet", quiet, "print errors only");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto schema = core::load_schema(root);
    if (!schema) die(schema.error().str());
    auto issues = core::check_schema(*schema);
#ifdef KK_HAVE_CODE
    if (!no_drift) {
      auto drift = code::check_drift(*schema);
      issues.insert(issues.end(), drift.begin(), drift.end());
    }
#endif
    int errors = 0;
    int warnings = 0;
    for (const auto& i : issues) {
      (i.severity == 'E' ? errors : warnings)++;
      if (quiet && i.severity != 'E') continue;
      std::cout << i.str() << "\n";
    }
    std::cout << fmt::format("check: {} blocks, {} errors, {} warnings\n", schema->blocks.size(), errors, warnings);
    if (errors > 0) std::exit(1);
  });
});

}  // namespace
}  // namespace kk::cli
