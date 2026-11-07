// karkas gui — starts karkas-gui for the project (appendix G).
#include <unistd.h>

#include <filesystem>
#include <iostream>

#include "app.hpp"

namespace kk::cli {
namespace {

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("gui", "graphical interface (karkas-gui)");
  cmd->callback([&app] {
    auto root = app.root();
    std::error_code ec;
    auto self = fs::read_symlink("/proc/self/exe", ec);
    std::string exe = "karkas-gui";
    for (const auto& cand : {self.parent_path() / "karkas-gui", self.parent_path().parent_path() / "gui" / "karkas-gui"}) {
      if (!ec && fs::exists(cand)) {
        exe = cand.string();
        break;
      }
    }
    std::string proj = root.string();
    const char* argv[] = {exe.c_str(), "--project", proj.c_str(), nullptr};
    execvp(exe.c_str(), const_cast<char* const*>(argv));
    die("cannot start " + exe + " (build with -DKARKAS_GUI=ON)");
  });
});

}  // namespace
}  // namespace kk::cli
