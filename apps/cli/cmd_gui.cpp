// karkas gui — starts karkas-gui for the project (appendix G); outside a project the GUI
// opens with "Open project".
#include <unistd.h>

#include <filesystem>
#include <iostream>

#include "app.hpp"
#include "kk/core/paths.hpp"

namespace kk::cli {
namespace {

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("gui", "graphical interface (karkas-gui)");
  cmd->callback([&app] {
    std::string proj;
    if (!app.project_opt.empty()) {
      proj = app.root().string();
    } else if (auto r = core::find_project_root(fs::current_path())) {
      proj = r->string();
    }
    std::error_code ec;
    auto self = fs::read_symlink("/proc/self/exe", ec);
    std::string exe = "karkas-gui";
    for (const auto& cand : {self.parent_path() / "karkas-gui", self.parent_path().parent_path() / "gui" / "karkas-gui"}) {
      if (!ec && fs::exists(cand)) {
        exe = cand.string();
        break;
      }
    }
    const char* argv[] = {exe.c_str(), proj.empty() ? nullptr : "--project", proj.c_str(), nullptr};
    execvp(exe.c_str(), const_cast<char* const*>(argv));
    die("cannot start " + exe + " (build with -DKARKAS_GUI=ON)");
  });
});

}  // namespace
}  // namespace kk::cli
