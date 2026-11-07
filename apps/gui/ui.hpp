#pragma once
// Panels of karkas-gui (14.4): block tree, schema of the current level, inspector,
// bottom tabs, code view and the edit dialogs (14.6).
#include <map>
#include <memory>
#include <set>
#include <string>

#include "worker.hpp"

namespace ax::NodeEditor {
struct EditorContext;
}
class TextEditor;

namespace kk::gui {

class Ui {
 public:
  explicit Ui(Worker& w);
  ~Ui();
  Ui(const Ui&) = delete;
  Ui& operator=(const Ui&) = delete;

  // Draws one frame from the latest model; returns true while animations need frames.
  bool draw(const std::shared_ptr<const Model>& m, bool first_frame);
  Request& request() { return req_; }

 private:
  void draw_toolbar(const Model& m);
  void draw_tree(const Model& m);
  void draw_graph(const Model& m);
  void draw_all_levels(const Model& m);
  void draw_inspector(const Model& m);
  void draw_bottom(const Model& m);
  void draw_code(const Model& m);
  void draw_dialogs(const Model& m);
  void enter(const std::string& scope);
  void select(const std::string& id);

  Worker& w_;
  Request req_;
  ax::NodeEditor::EditorContext* ed_ = nullptr;
  std::unique_ptr<TextEditor> code_;
  std::string code_loaded_;
  std::string applied_layout_;
  std::map<std::string, std::pair<float, float>> placed_;  // node positions we set
  std::set<std::string> active_blocks_;
  double pulse_ = 0;
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
  int navigate_ = 0;
  float zoom_ = 0.55f;  // all-levels view  // frames until "fit to content" (node sizes are known after drawing)
};

}  // namespace kk::gui
