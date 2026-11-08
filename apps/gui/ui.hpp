#pragma once
// karkas-gui window (14.4), laid out like an IDE: activity bar, side bar (explorer, search,
// schema changes, settings), editor with tabs (the schema of the current level and opened
// code files), inspector, bottom panel (tickets, agents, build & tests, costs, drift) and a
// status bar; plus the edit dialogs (14.6). ui_shell.cpp holds the shell, ui.cpp the schema.
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "picker.hpp"
#include "settings.hpp"
#include "ui_common.hpp"
#include "worker.hpp"

namespace ax::NodeEditor {
struct EditorContext;
}
class TextEditor;

namespace kk::gui {

// One link of the schema, by value (the snapshot may change while a menu is open).
struct LinkDesc {
  std::string kind, src_block, src_port, dst_block, dst_port;
};

class Ui {
 public:
  // `persist`: save settings changes to ~/.config/karkas/gui.json.
  Ui(Worker& w, GuiSettings& settings, bool persist);
  ~Ui();
  Ui(const Ui&) = delete;
  Ui& operator=(const Ui&) = delete;

  // Draws one frame from the latest model; returns true while animations need frames.
  bool draw(const std::shared_ptr<const Model>& m, bool first_frame);
  Request& request() { return req_; }
  [[nodiscard]] bool quit_requested() const { return quit_; }
  void open_settings() { show_settings_ = true; }
  // A project chosen in "Open project" (main switches to it); empty when none.
  std::optional<std::filesystem::path> take_open_request() { return std::exchange(open_request_, std::nullopt); }
  void set_project_root(std::filesystem::path root) { root_ = std::move(root); }

 private:
  enum class SideView { Explorer, Search, Changes, Settings };

  // shell (ui_shell.cpp)
  void draw_menu();
  void draw_activity_bar(const Model* m);
  void draw_status_bar(const Model* m);
  void draw_side_bar(const Model* m);
  void side_explorer(const Model& m);
  void side_search(const Model& m);
  void side_changes(const Model& m);
  void draw_settings_window(const Model* m);
  void settings_models(const Model* m);
  void draw_editor(const Model& m);
  void draw_schema_toolbar(const Model& m);
  void draw_file_tab(const Model& m, const std::string& file);
  void open_file_tab(const std::string& file);
  void draw_inspector(const Model& m);
  void draw_bottom(const Model& m);
  void show_picker();
  void go_to(const Model& m, const std::string& id);  // open the level of a block and select it
  ImVec4 color_of(const Model& m, const std::string& id) { return colors_.get(*m.snap, m.serial, id); }
  // schema (ui.cpp)
  void draw_address(const Model& m);
  void navigate_path(const Model& m, const std::string& text);
  void draw_graph(const Model& m);
  void draw_empty(const Model& m);  // explains an empty level instead of a blank canvas
  void draw_dialogs(const Model& m);
  void enter(const std::string& scope);
  void select(const std::string& id);
  void settings_changed();
  // Default size of a block at scale 1 (the text fits at font scale 1) and its size now.
  std::pair<float, float> base_size(const Model& m, const std::string& id) const;
  std::pair<float, float> node_size(const Model& m, const std::string& id) const;
  void save_view(const Model& m, bool reset_sizes = false, const std::string& reset_one = "");
  void set_all_levels(const Model& m, bool on);
  void propose_unlink(std::vector<LinkDesc> links);  // KSL preview, then removal

  Worker& w_;
  GuiSettings& settings_;
  bool persist_ = true;
  Request req_;
  ax::NodeEditor::EditorContext* ed_ = nullptr;
  BlockColors colors_;
  std::string applied_layout_;
  std::string navigated_view_;
  std::map<std::string, std::pair<float, float>> placed_;  // node positions we set
  std::set<std::string> active_blocks_;
  double pulse_ = 0;
  bool quit_ = false;
  std::filesystem::path root_;  // the open project
  std::unique_ptr<ProjectPicker> picker_;
  bool picker_open_ = false;
  std::optional<std::filesystem::path> open_request_;
  // shell
  SideView side_ = SideView::Explorer;
  bool show_settings_ = false;  // the settings window
  int settings_page_ = 0;
  bool side_visible_ = true;
  char filter_[128] = {};
  char search_[128] = {};
  bool focus_search_ = false;
  std::vector<std::string> open_files_;                         // code tabs of the editor
  std::map<std::string, std::unique_ptr<TextEditor>> editors_;  // per open file
  std::map<std::string, std::string> editor_loaded_;            // text shown per file
  std::string select_tab_;     // tab to bring forward next frame ("#schema" or a file)
  std::string bottom_select_;  // bottom panel tab to bring forward next frame
  // address bar
  bool editing_path_ = false;
  bool focus_path_ = false;
  char path_buf_[256] = {};
  std::string path_error_;
  // highlighting (hover, Shift+click)
  std::set<std::string> pinned_;  // blocks whose links stay lit
  std::vector<LinkDesc> menu_links_;  // links of the line whose menu is open
  std::string hover_unit_;        // block under the mouse on the schema (last frame)
  std::string tree_hover_;        // block under the mouse in the tree (this frame)
  std::string hover_frame_;       // opened group whose path bar is under the mouse
  bool fit_on_apply_ = false;     // fit the view when the next layout arrives (open/close)
  // block resizing
  std::string resizing_;
  std::pair<float, float> resize_start_{0, 0};
  std::pair<float, float> resize_mouse_{0, 0};
  std::pair<float, float> live_size_{0, 0};
  std::map<std::string, std::pair<float, float>> sizes_;  // saved, not in the model yet; {0,0} = auto
  std::uint64_t sizes_serial_ = 0;
  std::uintptr_t hover_pin_ = 0;  // connection point under the mouse (last frame)
  bool relayout_ = false;  // "auto layout": re-apply positions from the next model
  std::uint64_t relayout_serial_ = 0;
  // dialogs
  std::string dialog_;  // "add_block", "add_port", "rename", "merge", "split"
  std::string target_;
  char buf_name_[64] = {};
  char buf_purpose_[160] = {};
  char buf_spec_[160] = {};
  char buf_thread_[32] = "main";
  int kind_ = 1;  // leaf
  int dir_ = 0;   // in
  bool preview_open_ = false;
  int navigate_ = 0;  // frames until "fit to content" (node sizes are known after drawing)
};

}  // namespace kk::gui
