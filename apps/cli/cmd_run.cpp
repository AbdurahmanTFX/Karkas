// karkas run [--no-build] [--show] [-- args...] — build the project and run its program (docs/adr/0037).
// For the human (a terminal, the green triangle of karkas-gui): the chat and the agents never run it.
// The build streams to stderr; then the program replaces this process, so its output, exit code and
// signals are its own.
#include <fmt/format.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string_view>

#include "app.hpp"
#include "kk/agents/run.hpp"
#include "kk/agents/testing.hpp"
#include "kk/core/config.hpp"
#include "kk/core/schema.hpp"
#include "kk/runner/runner.hpp"

extern char** environ;

namespace kk::cli {

namespace {

void progress(const std::string& m) { std::cerr << "· " << m << "\n" << std::flush; }

// The program's environment: this one without the variables that look like secrets (API keys stay with
// Karkas, as in the agents' builds); everything else — the display, the desktop session — is kept.
std::vector<std::string> program_environment() {
  std::vector<std::string> env;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    std::string_view kv(*e);
    auto eq = kv.find('=');
    if (eq == std::string_view::npos || runner::is_secret_name(kv.substr(0, eq))) continue;
    env.emplace_back(kv);
  }
  return env;
}

[[noreturn]] void exec_program(const fs::path& root, const agents::RunTarget& t) {
  if (::chdir(root.c_str()) != 0) die(fmt::format("cannot enter {}: {}", root.string(), std::strerror(errno)));
  auto env_s = program_environment();
  std::vector<char*> env;
  for (auto& e : env_s) env.push_back(e.data());
  env.push_back(nullptr);
  auto argv_s = t.argv;
  std::vector<char*> argv;
  for (auto& a : argv_s) argv.push_back(a.data());
  argv.push_back(nullptr);
  std::cout.flush();
  std::cerr.flush();
  ::execve(argv[0], argv.data(), env.data());
  die(fmt::format("cannot start {}: {}", t.shown, std::strerror(errno)));
}

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("run", "build the project and run its program ([project] run_command, or the entry block's program)");
  static bool no_build = false;
  static bool show = false;
  static std::vector<std::string> args;
  cmd->add_flag("--no-build", no_build, "run the program as it was built last");
  cmd->add_flag("--show", show, "print what would run and the programs of the build folder, run nothing");
  cmd->add_option("args", args, "arguments of the program (after --)");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto cfg = core::load_config(root);
    if (!cfg) die(cfg.error().str());
    auto schema = core::load_schema(root);
    if (!schema) die(schema.error().str());
    auto target = agents::run_target(*schema, *cfg, args);
    if (show) {
      if (target.ok()) {
        std::cout << "program: " << target.shown << "\n";
        if (target.command && !target.generated.empty()) std::cout << "(instead of " << target.generated << ")\n";
      } else {
        std::cout << agents::run_target_help(target, agents::program_candidates(root, *cfg));
      }
      return;
    }
    // The build comes first even when Karkas does not know the program yet: the programs it makes are
    // what the human chooses from.
    if (!no_build) {
      progress(cfg->build_command.empty() ? "building" : "building: " + cfg->build_command);
      agents::TestService svc(root, *cfg);
      auto b = svc.build_program([](std::string_view s) { std::cerr.write(s.data(), static_cast<std::streamsize>(s.size())) << std::flush; },
                                 [] { progress("waiting: another build of this project is running"); });
      if (!b) die(b.error().message);
      if (!b->ok) {
        progress("build failed");
        if (!b->summary.empty()) std::cerr << b->summary << "\n";
        die(b->timed_out ? "build timed out" : "build failed");
      }
      progress(fmt::format("built in {:.1f} s", static_cast<double>(b->duration.count()) / 1000.0));
    }
    if (!target.ok()) {
      std::cerr << "karkas: " << agents::run_target_help(target, agents::program_candidates(root, *cfg));
      std::exit(1);
    }
    if (!target.command && ::access(target.argv[0].c_str(), X_OK) != 0) {
      die(fmt::format("there is no program {} (the build made none: is the entry block's main() there?)", target.shown));
    }
    progress("running: " + target.shown);
    exec_program(root, target);
  });
});

}  // namespace
}  // namespace kk::cli
