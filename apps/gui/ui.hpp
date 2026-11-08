#pragma once
// Panels of karkas-gui (14.4): block tree, schema of the current level, inspector,
// bottom tabs, code view, the edit dialogs (14.6) and the settings window.
#include <map>
#include <memory>
#include <set>
#include <string>

#include "settings.hpp"
#include "worker.hpp"

namespace ax::NodeEditor {
struct EditorContext;
}
class TextEditor;

namespace kk::gui {

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

 private:
  void draw_menu();
  void draw_settings(const Model* m);
  void draw_toolbar(const Model& m);
  void draw_address(const Model& m);
  void navigate_path(const Model& m, const std::string& text);
  void draw_tree(const Model& m);
  void draw_graph(const Model& m);
  void draw_empty(const Model& m);  // explains an empty level instead of a blank canvas
  void draw_all_levels(const Model& m);
  void draw_inspector(const Model& m);
  void draw_bottom(const Model& m);
  void draw_code(const Model& m);
  void draw_dialogs(const Model& m);
  void enter(const std::string& scope);
  void select(const std::string& id);
  void settings_changed();
  // Default size of a block at scale 1 (the text fits at font scale 1) and its size now.
  std::pair<float, float> base_size(const Model& m, const std::string& id) const;
  std::pair<float, float> node_size(const Model& m, const std::string& id) const;
  void save_view(const Model& m, bool reset_sizes = false, const std::string& reset_one = "");

  Worker& w_;
  GuiSettings& settings_;
  bool persist_ = true;
  Request req_;
  ax::NodeEditor::EditorContext* ed_ = nullptr;
  std::unique_ptr<TextEditor> code_;
  std::string code_loaded_;
  std::string applied_layout_;
  std::string navigated_view_;
  std::map<std::string, std::pair<float, float>> placed_;  // node positions we set
  std::set<std::string> active_blocks_;
  double pulse_ = 0;
  bool quit_ = false;
  bool show_settings_ = false;
  // address bar
  bool editing_path_ = false;
  bool focus_path_ = false;
  char path_buf_[256] = {};
  std::string path_error_;
  // highlighting (hover, Shift+click)
  std::set<std::string> pinned_;  // blocks whose links stay lit
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
  int navigate_ = 0;    // frames until "fit to content" (node sizes are known after drawing)
  float zoom_ = 0.55f;  // all-levels view
};

}  // namespace kk::gui
