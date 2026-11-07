#pragma once
// Background thread of karkas-gui (6.4, 14.8): owns the index, runs layouts, applies edits
// and watches the project with inotify. The GUI thread only reads finished Models.
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "kk/agents/edit.hpp"
#include "kk/core/config.hpp"
#include "kk/core/model.hpp"
#include "kk/index/index.hpp"
#include "kk/layout/layout.hpp"
#include "kk/metrics/metrics.hpp"

namespace kk::gui {

namespace fs = std::filesystem;

struct Request {
  std::string scope;      // "" = root
  std::string selected;   // block
  std::string open_file;  // relative to the project root
  std::string ticket;     // selected ticket
  std::string change;     // selected change
  bool all_levels = false;
  bool show_use = false;
  float char_w = 7.0f;
  float line_h = 17.0f;
  float block_scale = 1.0f;  // default size of blocks (settings)
  bool operator==(const Request&) const = default;
};

struct Model {
  std::shared_ptr<const index::Snapshot> snap;
  Request req;
  layout::Graph graph;
  layout::Layout level;
  std::map<std::string, layout::Pos> manual;  // saved positions and sizes of this view
  std::string layout_key;  // changes when positions must be re-applied
  std::vector<layout::Box> boxes;
  std::vector<index::RunRow> runs;
  std::vector<core::Change> changes;
  std::vector<metrics::ActiveRun> active;
  std::string card_text, view_text, code_text, ticket_text, change_preview;
  std::vector<std::string> files;  // code files of the selected block (relative to root)
  std::string pending_title, pending_preview, pending_error;
  bool has_pending = false;
  bool can_undo = false;
  std::string message;
  std::string engine;
  double index_ms = 0;
  std::uint64_t serial = 0;
};

using MakeEdit = std::function<kk::Result<agents::EditOp>(const core::Schema&)>;

class Worker {
 public:
  Worker(fs::path root, core::Config cfg, std::function<void()> wake);
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  void set_request(const Request& r);
  [[nodiscard]] std::shared_ptr<const Model> model() const;

  // Edits: build the op (preview shown in the GUI), then confirm or cancel.
  void propose(MakeEdit make);
  void confirm();
  void cancel();
  void undo();
  void approve_change(const std::string& id);
  void reject_change(const std::string& id);
  void save_positions(const std::string& view, std::map<std::string, layout::Pos> pos);
  void open_in_editor(const std::string& rel_file, int line);
  void reindex();
  [[nodiscard]] const fs::path& root() const { return root_; }

 private:
  void loop();
  void rebuild(bool reindex);
  void post(std::function<void()> job);
  fs::path root_;
  core::Config cfg_;
  std::function<void()> wake_;
  std::thread thread_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool dirty_ = true;
  bool files_changed_ = true;
  Request req_;
  std::deque<std::function<void()>> jobs_;
  std::shared_ptr<const Model> model_;
  // worker-thread state
  std::optional<agents::EditOp> pending_;
  std::string pending_error_;
  std::vector<agents::AppliedEdit> undo_;
  std::string message_;
  std::uint64_t serial_ = 0;
};

}  // namespace kk::gui
