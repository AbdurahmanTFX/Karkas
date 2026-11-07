// karkas — command line interface (appendix G).
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <iostream>

#include "app.hpp"
#include "kk/core/paths.hpp"

namespace kk::cli {

std::vector<Register>& registry() {
  static std::vector<Register> r;
  return r;
}

fs::path App::root() const {
  if (!project_opt.empty()) return core::normalize(project_opt);
  auto r = core::find_project_root(fs::current_path());
  if (!r) die(r.error().message + " (use --project <dir> or run karkas init)");
  return *r;
}

void die(const std::string& message, int code) {
  std::cerr << "karkas: " << message << "\n";
  std::exit(code);
}

void setup_logging(const App& app, const fs::path& project_root, bool mcp_mode) {
  // stdout belongs to command output (and to the protocol in MCP mode): logs go to
  // stderr and .karkas/karkas.log only (6.6).
  std::vector<spdlog::sink_ptr> sinks;
  auto err = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
  err->set_level(app.verbose ? spdlog::level::debug : (mcp_mode ? spdlog::level::err : spdlog::level::warn));
  sinks.push_back(err);
  if (!project_root.empty() && fs::exists(project_root / ".karkas")) {
    try {
      auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>((project_root / ".karkas" / "karkas.log").string());
      file->set_level(spdlog::level::debug);
      sinks.push_back(file);
    } catch (const spdlog::spdlog_ex&) {
    }
  }
  auto logger = std::make_shared<spdlog::logger>("karkas", sinks.begin(), sinks.end());
  logger->set_level(spdlog::level::debug);
  logger->set_pattern("%Y-%m-%d %H:%M:%S.%e %^%l%$ %v");
  spdlog::set_default_logger(logger);
}

}  // namespace kk::cli

int main(int argc, char** argv) {
  using namespace kk::cli;
  CLI::App cli{"Karkas — a development environment where the project is a schema for AI agents"};
  cli.require_subcommand(0, 1);
  cli.fallthrough();  // global options also after the subcommand: karkas mcp --project <dir>
  App app;
  cli.add_option("--project,-C", app.project_opt, "project directory (default: nearest with project.ksl)");
  cli.add_flag("-v,--verbose", app.verbose, "verbose logging to stderr");
  cli.set_version_flag("--version", std::string("karkas ") + KARKAS_VERSION);
  for (auto& reg : registry()) reg(cli, app);
  try {
    cli.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return cli.exit(e);
  }
  if (cli.get_subcommands().empty()) {
    std::cout << cli.help();
  }
  return 0;
}
