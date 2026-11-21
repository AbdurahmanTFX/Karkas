// karkas gui — starts karkas-gui for the project (appendix G); outside a project the GUI
// opens with "Open project".
#ifdef _WIN32
#include "kk/core/win_process.hpp"
#else
#include <unistd.h>
#endif

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
    auto self = core::self_executable();
#ifdef _WIN32
    const char* name = "karkas-gui.exe";
#else
    const char* name = "karkas-gui";
#endif
    std::string exe = "karkas-gui";
    for (const auto& cand : {self.parent_path() / name, self.parent_path().parent_path() / "gui" / name}) {
      if (!self.empty() && fs::exists(cand, ec)) {
        exe = cand.string();
        break;
      }
    }
#ifdef _WIN32
    // No exec on Windows: the window starts on its own and this command ends (docs/adr/0046).
    core::win::SpawnSpec sp;
    sp.argv = {exe};
    if (!proj.empty()) sp.argv.insert(sp.argv.end(), {"--project", proj});
    sp.out = sp.err = core::win::SpawnSpec::Io::Null;
    sp.own_job = false;
    auto child = core::win::Child::spawn(sp);
    if (!child) die("cannot start " + exe + ": " + child.error().message);
    child->detach();
#else
    const char* argv[] = {exe.c_str(), proj.empty() ? nullptr : "--project", proj.c_str(), nullptr};
    execvp(exe.c_str(), const_cast<char* const*>(argv));
    die("cannot start " + exe + " (build with -DKARKAS_GUI=ON)");
#endif
  });
});

}  // namespace
}  // namespace kk::cli
