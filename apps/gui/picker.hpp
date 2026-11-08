#pragma once
// "Open project" window of karkas-gui: a small folder browser. Folders that hold a
// project.ksl are marked; recent projects are listed on top.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kk::gui {

class ProjectPicker {
 public:
  ProjectPicker(const std::filesystem::path& start, std::vector<std::string> recent);
  // Draws the window; returns the chosen project root once. `open` is cleared on Cancel.
  // `welcome`: no project is open yet (karkas-gui started outside a project).
  std::optional<std::filesystem::path> draw(bool* open, bool welcome);

 private:
  struct Entry {
    std::string name;
    bool project = false;
  };
  void go(const std::filesystem::path& dir);

  std::filesystem::path dir_;
  std::vector<Entry> entries_;
  int selected_ = -1;
  bool show_hidden_ = false;
  bool here_is_project_ = false;
  char path_buf_[1024] = {};
  std::vector<std::string> recent_;
  std::string error_;
};

}  // namespace kk::gui
