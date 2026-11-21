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

#include "cleared.hpp"
#include "kk/agents/edit.hpp"
#include "kk/agents/notes_agent.hpp"
#include "kk/code/notes.hpp"
#include "kk/core/config.hpp"
#include "kk/core/markdown.hpp"
#include "kk/core/model.hpp"
#include "kk/index/index.hpp"
#include "kk/layout/layout.hpp"
#include "kk/metrics/metrics.hpp"

namespace kk::gui {

namespace fs = std::filesystem;

// A selected schema change drawn on the schema before it is applied (ADR 0021): new blocks,
// ports added or removed, links added or removed, the blocks it edits, and the level (with
// the groups to open in place) that shows all of it.
struct GhostPort {
  std::string block, name, dir;  // dir: in | out | fn
  bool added = true;
};
struct GhostLink {
  std::string kind, src_block, src_port, dst_block, dst_port;
  bool added = true;
};
struct GhostBlock {
  std::string id, kind, purpose;
  std::vector<GhostPort> ports;
};
struct ChangeGhost {
  std::string id;
  std::vector<GhostBlock> blocks;
  std::vector<GhostPort> ports;
  std::vector<GhostLink> links;
  std::vector<std::string> touched;  // existing blocks whose cards change
  std::string scope;                 // level to open ("" = root)
  std::vector<std::string> expand;   // groups of that level to open in place
  std::string error;
  [[nodiscard]] bool empty() const { return blocks.empty() && ports.empty() && links.empty() && touched.empty(); }
};

// What "build and run" starts (ADR 0037), worked out in the worker when the GUI asks.
struct RunInfo {
  bool known = false;
  std::string shown;        // the program (from the project root) or the command
  std::string run_command;  // [project] run_command as written ("" = the program Karkas builds)
  std::string generated;    // the program Karkas's generated CMake builds ("" = none)
  std::string error;        // nothing to run: why
  std::vector<std::string> candidates;  // programs in the build folder, newest first
};

// A note of a notes project (ADR 0038): its file (from the project root) and text; the first lines shown
// on the schema, and a Markdown note's text parsed (ADR 0040), worked out off the UI thread.
struct NoteText {
  std::string file, text;
  bool markdown = false;
  std::vector<std::string> preview;
  std::shared_ptr<const std::vector<core::md::Block>> doc;
};

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
  std::vector<std::string> expanded;  // groups of this level opened in place (sorted)
  bool operator==(const Request&) const = default;
};

struct Model {
  std::shared_ptr<const index::Snapshot> snap;
  Request req;
  layout::Graph graph;
  layout::Layout level;
  std::map<std::string, layout::Pos> manual;  // saved positions and sizes of this view
  std::vector<layout::Box> nested;            // groups opened in place: frames and their contents
  std::string layout_key;  // changes when positions must be re-applied
  std::vector<index::RunRow> runs;
  std::vector<core::Change> changes;
  std::vector<metrics::ActiveRun> active;
  std::string card_text, view_text, code_text, ticket_text, change_preview;
  ChangeGhost ghost;         // of the selected change
  std::string change_cards;  // its cards before → after (unified diff)
  std::vector<std::string> files;  // code files of the selected block (relative to root)
  std::string pending_title, pending_preview, pending_error;
  bool has_pending = false;
  bool can_undo = false;
  // The last edit of a ticket by hand (ADR 0033): its error, and a counter of the saved ones.
  std::string ticket_edit_error;
  std::uint64_t ticket_edit_saved = 0;
  RunInfo run;
  bool notes = false;                           // a notes project (project.ksl: lang notes)
  std::shared_ptr<const std::map<std::string, NoteText>> note_texts;  // notes projects: block id → its note
  // The notes assistant (ADR 0039): its proposals (newest first), the selected one's preview (req.change
  // = N-NNNN), and the links to suggest for the selected note (its plain mentions, and of it in others).
  std::vector<agents::NotesProposal> proposals;
  std::string proposal_preview;
  std::vector<code::Mention> mentions;
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
  // Replaces the diff of a pending change (checked like an agent's proposal first).
  void edit_change(const std::string& id, const std::string& diff_text);
  // Writes a ticket edited by hand (parsed, refused while an agent holds it; ADR 0033).
  void save_ticket(const std::string& id, const std::string& text);
  void save_positions(const std::string& view, std::map<std::string, layout::Pos> pos);
  void open_in_editor(const std::string& rel_file, int line);
  // Opens a folder of the project in the system file manager (xdg-open): "" = the root.
  void open_folder(const std::string& rel_dir);
  // The folder of a block: its card folder, or the folder of its code for imported blocks.
  void open_block_folder(const std::string& block);
  void reindex();
  void reload_config();
  // Works out RunInfo again (karkas.toml, the schema, the build folder) for the next model.
  void refresh_run();
  // Notes projects (ADR 0038): an edit built and applied at once (no KSL preview; undo keeps it), a
  // note's text saved, links added or removed in a note's text, a new title (links by it follow).
  void apply_now(MakeEdit make);
  void save_note(const std::string& rel_file, std::string text);
  // A link of a note that leads out of the notes (ADR 0040): a web or mail address in the browser, a file
  // (relative to the note `from_file`) in its program; xdg-open, without a shell.
  void open_link(const std::string& from_file, const std::string& href);
  void link_notes(const std::string& src, const std::string& dst);
  void unlink_notes(const std::string& src, const std::string& dst);
  void retitle_note(const std::string& id, const std::string& title);
  void approve_proposal(const std::string& id);
  void reject_proposal(const std::string& id);
  void link_mention(const std::string& src, const std::string& dst);
  // Asks one agent run to stop (its stop file; ADR 0019).
  void stop_run(const fs::path& journal);
  // Saves the rows removed from the bottom panels (.karkas/cleared.json).
  void save_cleared(Cleared c);  // karkas.toml changed (models chosen in the GUI)
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
  std::string ticket_edit_error_;
  std::uint64_t ticket_edit_saved_ = 0;
  RunInfo run_info_;
  bool notes_ = false;
  std::shared_ptr<const std::map<std::string, NoteText>> note_texts_;
  void load_notes();  // notes_ and note_texts_ from the project as it is on disk
  std::vector<code::Mention> mentions_;
  std::string mentions_for_;  // the note they are of
  std::uint64_t serial_ = 0;
};

}  // namespace kk::gui
