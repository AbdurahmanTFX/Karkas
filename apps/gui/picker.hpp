#pragma once
// "Open project" window of karkas-gui: a small folder browser. Folders that hold a
// project.ksl are marked; recent projects are listed on top.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kk::gui {

enum class PickerMode { Open, Create };

class ProjectPicker {
 public:
  ProjectPicker(const std::filesystem::path& start, std::vector<std::string> recent, PickerMode mode = PickerMode::Open);
  // Draws the window; returns the chosen project root once. `open` is cleared on Cancel.
  // `welcome`: no project is open yet (karkas-gui started outside a project).
  std::optional<std::filesystem::path> draw(bool* open, bool welcome);

 private:
  struct Entry {
    std::string name;
    bool project = false;
  };
  void go(const std::filesystem::path& dir);
  void draw_browser(float footer, std::optional<std::filesystem::path>* chosen);
  // "Create": a new folder here, or Karkas in an existing folder (optionally importing its code).
  std::optional<std::filesystem::path> draw_create(bool* open, bool welcome);
  std::filesystem::path create_target() const;

  std::filesystem::path dir_;
  std::vector<Entry> entries_;
  int selected_ = -1;
  bool show_hidden_ = false;
  bool here_is_project_ = false;
  char path_buf_[1024] = {};
  std::vector<std::string> recent_;
  std::string error_;
  PickerMode mode_ = PickerMode::Open;
  bool switch_tab_ = true;     // select the tab of mode_ on the next frame
  bool in_new_folder_ = true;  // create: a new folder vs an existing one
  bool import_code_ = true;    // create in a folder with code: propose `karkas import`
  bool target_has_code_ = false;
  std::filesystem::path code_checked_;  // folder target_has_code_ is about
  char name_buf_[128] = {};
  char idea_buf_[256] = {};
  std::string create_error_;
};

}  // namespace kk::gui
