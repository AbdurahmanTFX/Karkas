// karkas check — KSL checks (4.6) and, when kk_code is available, drift checks (8.2).
#include <fmt/format.h>

#include <algorithm>

#include <iostream>

#include "app.hpp"
#include "kk/core/validate.hpp"
#include "kk/index/index.hpp"
#ifdef KK_HAVE_CODE
#include "kk/code/drift.hpp"
#endif

namespace kk::cli {

namespace {

// KSL checks and, with drift, the code checks. These come from the index, updated first: only files
// that changed are analyzed, and nothing is when nothing changed (ADR 0029; `karkas check` on
// Endless Sky 4.9 s -> 0.3 s). Without an index the code is analyzed here as before.
std::vector<core::Issue> current_issues(const fs::path& root, const core::Schema& schema, bool drift) {
#ifdef KK_HAVE_CODE
  if (drift) {
    if (auto idx = index::Index::open(root)) {
      if (auto st = (*idx)->update({false, analyzer()}); st) return (*idx)->issues();
    }
    auto issues = core::check_schema(schema);
    auto d = code::check_drift(schema);
    issues.insert(issues.end(), d.begin(), d.end());
    return issues;
  }
#else
  (void)root;
  (void)drift;
#endif
  return core::check_schema(schema);
}

}  // namespace

std::string check_text(const fs::path& root, bool drift, int* errors_out, bool quiet) {
  auto schema = core::load_schema(root);
  if (!schema) {
    if (errors_out != nullptr) *errors_out = 1;
    return schema.error().str() + "\n";
  }
  auto issues = current_issues(root, *schema, drift);
  int errors = 0;
  int warnings = 0;
  std::string out;
  for (const auto& i : issues) {
    (i.severity == 'E' ? errors : warnings)++;
    if (quiet && i.severity != 'E') continue;
    out += i.str() + "\n";
  }
  out += fmt::format("check: {} blocks, {} errors, {} warnings\n", schema->blocks.size(), errors, warnings);
  if (errors_out != nullptr) *errors_out = errors;
  return out;
}

std::string check_text(const fs::path& root, bool drift, int* errors) { return check_text(root, drift, errors, false); }

// kk_check: every error in full, warnings counted per code with two examples (a model rarely
// needs 140 heuristic warnings of an imported project; `karkas check` prints them all).
std::string check_summary(const fs::path& root) {
  auto schema = core::load_schema(root);
  if (!schema) return schema.error().str() + "\n";
  auto issues = current_issues(root, *schema, true);
  int errors = static_cast<int>(std::count_if(issues.begin(), issues.end(), [](const core::Issue& i) { return i.severity == 'E'; }));
  return core::issues_summary(issues) + fmt::format("check: {} blocks, {} errors, {} warnings{}\n", schema->blocks.size(), errors,
                                                     issues.size() - static_cast<std::size_t>(errors),
                                                     issues.size() > static_cast<std::size_t>(errors) ? " (all of them: karkas check)" : "");
}

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
    int errors = 0;
    std::cout << check_text(root, !no_drift, &errors, quiet);
    if (errors > 0) std::exit(1);
  });
});

}  // namespace
}  // namespace kk::cli
