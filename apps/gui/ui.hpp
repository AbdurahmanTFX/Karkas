#pragma once
// karkas-gui window (14.4), laid out like an IDE: activity bar, side bar (explorer, search,
// schema changes, settings), editor with tabs (the schema of the current level and opened
// code files), inspector, bottom panel (tickets, agents, build & tests, costs, drift) and a
// status bar; plus the edit dialogs (14.6). ui_shell.cpp holds the shell, ui.cpp the schema.
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chat.hpp"
#include "feed.hpp"
#include "cleared.hpp"
#include "kk/agents/prototype.hpp"
#include "kk/core/config.hpp"
#include "kk/llm/mcp_bridge.hpp"
#include "picker.hpp"
#include "selectable.hpp"
#include "settings.hpp"
#include "ui_common.hpp"
#include "worker.hpp"

namespace ax::NodeEditor {
struct EditorContext;
}  // namespace ax::NodeEditor

namespace kk::gui {
// Fits the blocks of `editor` into the screen part [free_min, free_max] of its canvas
// [view_min, view_max] (panels cover the rest), zooming in at most to `max_zoom` (canvas_fit.cpp).
void fit_canvas(ax::NodeEditor::EditorContext* editor, ImVec2 view_min, ImVec2 view_max, ImVec2 free_min, ImVec2 free_max, float max_zoom);
// Forgets what the schema canvas keeps between frames (another project is opened).
void reset_canvas_caches();
}  // namespace kk::gui

class TextEditor;

namespace kk::gui {
// What the prototype check needs of the index snapshot (ui_prototype.cpp, ADR 0035).
agents::SchemaShape snapshot_shape(const index::Snapshot& snap);
}  // namespace kk::gui

namespace kk::gui {

// One link of the schema, by value (the snapshot may change while a menu is open).
struct LinkDesc {
  std::string kind, src_block, src_port, dst_block, dst_port;
};

class Ui {
 public:
  // `persist`: save settings changes to ~/.config/karkas/gui.json; `wake` redraws the window
  // from another thread (the orchestrator chat).
  Ui(Worker& w, GuiSettings& settings, bool persist, std::function<void()> wake = {});
  ~Ui();
  Ui(const Ui&) = delete;
  Ui& operator=(const Ui&) = delete;

  // Draws one frame from the latest model; returns true while animations need frames.
  bool draw(const std::shared_ptr<const Model>& m, bool first_frame);
  Request& request() { return req_; }
  [[nodiscard]] bool quit_requested() const { return quit_; }
  void open_settings() { show_settings_ = true; }
  void open_prototype() { set_prototype(true); }  // prototype mode (headless checks: --prototype)
  void open_chat() {
    show_chat_ = true;
    chat_focus_ = true;
  }
  // A project chosen in "Open project" (main switches to it); empty when none.
  std::optional<std::filesystem::path> take_open_request() { return std::exchange(open_request_, std::nullopt); }
  void set_project_root(std::filesystem::path root);

 private:
  enum class SideView { Explorer, Search, Changes, Settings, Chat };

  // shell (ui_shell.cpp)
  void draw_menu();
  void draw_activity_bar(const Model* m);
  void draw_status_bar(const Model* m);
  void draw_side_bar(const Model* m);
  void side_explorer(const Model& m);
  void side_search(const Model& m);
  void side_changes(const Model& m);
  void draw_settings_window(const Model* m);
  void draw_windows_menu();
  // Tiles over the schema (ADR 0021, 0032): the schema keeps the whole work area behind a
  // dockspace whose central part is empty and lets the mouse through; the side bar, the
  // inspector, the bottom tabs and the chat are tiles — docked in the dockspace (dragged by
  // their tab to any edge, tabbed together), floating inside the main window, or windows of
  // the desktop. Showing, hiding or resizing a tile never changes the schema's size.
  struct Area {
    ImVec2 min{0, 0}, max{0, 0};
    [[nodiscard]] float w() const { return max.x - min.x; }
    [[nodiscard]] float h() const { return max.y - min.y; }
  };
 public:
  enum class Tile { Side, Inspector, Tickets, Agents, Build, Costs, Drift, Chat, Count };
  enum class TileMode { Docked, Floating, Window };
  // Where a tile is now (a hidden tile: where it was last).
  [[nodiscard]] TileMode tile_mode(Tile t) const;
  // Moves a tile: into the layout (its last place, or its default edge), floating inside the
  // main window, or out into a window of the desktop. Applied when the tile is drawn next.
  void set_tile_mode(Tile t, TileMode mode);
  void reset_layout() { reset_layout_ = true; }
 private:
  void dock_space();  // the dockspace host over the work area; free_area_ = its central part
  void build_default_layout();
  void apply_dock_requests();
  // Begins the window of a tile (End() always follows); false when it is collapsed or hidden.
  bool begin_tile(Tile t, const std::string& title, bool* open, ImU32 bg, ImVec2 padding, ImGuiWindowFlags extra = 0);
  void tile_menu_items(Tile t);  // dock / float / own window (context menu of the tab and the header button)
  void tile_button(Tile t);      // a header button with that menu
  bool begin_editor_window();
  void order_windows();  // the schema at the back, the dockspace over it, floating windows on top
  void fit_view();       // fit the blocks into the part of the schema that no panel covers
  [[nodiscard]] bool bottom_visible() const;
  [[nodiscard]] bool tab_visible(const char* key) const;
  void set_tab_visible(const char* key, bool on);
  Area work_, free_area_;
  std::uint32_t dock_id_ = 0;
  bool reset_layout_ = false;
  std::array<std::uint32_t, static_cast<std::size_t>(Tile::Count)> last_dock_{};  // node a tile was docked in
  std::array<int, static_cast<std::size_t>(Tile::Count)> tile_request_{};         // 0 none, 1 dock, 2 float, 3 window
  float chrome_h_ = 0.0f;  // the editor's tab bar and toolbar over the schema (last frame)
  ImVec2 canvas_min_{0, 0}, canvas_max_{0, 0};
  std::string editor_tab_ = "#schema";  // "#schema" or the open file shown
  // models and APIs per level / block (ui_models.cpp, ADR 0011)
  void settings_models(const Model* m);
  void reload_models();
  void draw_model_chip(const Model& m);
  void inspector_model(const Model& m, const std::string& block);
  void start_api_edit(const std::string& tier);  // "+" = a new API
  void draw_api_form();
  bool tier_combo(const char* id, const std::string& current, const char* none_label, std::string* out);
  void set_pin(const std::string& section, const std::string& key, const std::string& tier);  // "" = unpin
  [[nodiscard]] std::string tier_label(const std::string& tier) const;
  [[nodiscard]] std::string effective_model(const Model& m, const std::string& block) const;
  void draw_editor(const Model& m);
  void draw_schema_toolbar(const Model& m);
  void draw_file_header(const Model& m, const std::string& file);  // path and "open in the editor"
  void draw_file_body(const Model& m, const std::string& file);    // the code
  void open_file_tab(const std::string& file);
  void draw_inspector(const Model& m);
  // Editing in place (ADR 0033): the card as KSL text, the guarantees one by one, a ticket.
  // Cards go through the KSL diff preview (checked, undoable); tickets are checked and written.
  void inspector_card(const Model& m, const index::BlockRow& b);
  void inspector_guarantees(const Model& m, const index::BlockRow& b);
  void ticket_editor(const Model& m, const std::string& id);
  std::string card_edit_block_, card_edit_buf_, card_edit_base_;
  bool card_saved_ = false;  // sent to the preview; the editor closes when the card on disk changes
  std::string inv_edit_block_;
  std::vector<std::string> inv_edit_, inv_edit_base_;
  bool inv_saved_ = false;
  std::string ticket_edit_id_, ticket_edit_buf_;
  std::uint64_t ticket_edit_seen_ = 0;
  bool ticket_edit_wait_ = false;
  // bottom panel and stopping agents (ui_panels.cpp, ADR 0019)
  void draw_bottom(const Model& m);
  void bottom_tickets(const Model& m);
  void bottom_agents(const Model& m);
  void bottom_tests(const Model& m);
  void bottom_costs(const Model& m);
  void bottom_drift(const Model& m);
  // "Clear" (with a confirmation) and "Restore (N)" at the right of a panel's first line.
  void panel_actions(const char* key, int shown, int hidden, const std::function<void()>& clear_all,
                     const std::function<void()>& restore);
  void cleared_changed();
  void interrupt_process(int pid);  // SIGINT, like Ctrl+C in the terminal of the command
  // the orchestrator chat (ui_chat.cpp, ADR 0019) and the live feed of agent runs (ADR 0021)
  void draw_chat_window(const Model* m);
  void chat_entry(const Model* m, const ChatEntry& e, std::size_t i);
  void chat_dialog(const Model* m);
  void chat_live(const Model* m);
  void live_step(const FeedRun& run, const FeedStep& s, std::size_t i);
  void change_buttons(const Model* m, const std::string& change);  // show on schema, edit, approve, reject
  void set_auto(bool on);  // [agents] auto_approve in karkas.toml
  // [agents] mode (ADR 0024): "api" or "mcp"; mode mcp writes the Claude Code subagents if missing.
  void set_mode(const std::string& mode);
  void fit_mcp_models();  // [mcp.*] maps fitted to the API ones (Claude tiers of the same strength)
  // Turns of agents waiting for their MCP client (read at most once a second).
  const std::vector<llm::McpTurn>& mcp_waiting();
  std::vector<llm::McpTurn> mcp_waiting_;
  double mcp_polled_ = -10.0;
  void mode_switch(const char* id);  // "API | MCP" toggles
 public:
  // Opens the chat window on the live feed of one run (journal file name; "" = follow the active run).
  void open_live(const std::string& journal);
 private:
  void show_picker(bool create = false);
  void go_to(const Model& m, const std::string& id);  // open the level of a block and select it
  ImVec4 color_of(const Model& m, const std::string& id) { return colors_.get(*m.snap, m.serial, id); }
  // prototype mode (ui_prototype.cpp, ADR 0035): ghost blocks and dependencies the human
  // sketches on a level, given to the architect (or the orchestrator) when approved.
  bool proto_on_ = false;
  agents::Sketch sketch_;
  std::string sketch_msg_;                  // the last problem or what was done
  std::filesystem::file_time_type sketch_mtime_{};  // of prototype.json as we last read or wrote it
  double sketch_polled_ = 0;                // the engine writes it too (after a design): re-read when it changes
  std::map<std::uintptr_t, std::string> sketch_nodes_;  // node id → sketch block id (this frame)
  std::map<std::string, std::pair<ImVec2, ImVec2>> sketch_socks_;  // sketch block id → in / out socket (canvas)
  std::set<std::string> sketch_placed_;     // sketch nodes positioned in the node editor
  std::set<std::string> sketch_positioned_;  // realized blocks given the sketch's place once
  std::map<std::string, ImVec2> sketch_apply_;  // ... to be moved there in the node editor now
  std::string sketch_target_;               // sketch block of the open menu / dialog ("" = a new one)
  bool sketch_dialog_ = false;
  bool sketch_add_center_ = false;          // "+ block" pressed: the dialog opens at the view's center
  ImVec2 sketch_new_pos_{0, 0};
  std::string sk_name_, sk_purpose_;
  int sk_kind_ = 0;
  void load_prototype();
  void save_prototype();
  void set_prototype(bool on);
  void draw_prototype_bar(const Model& m);
  void draw_sketch(const Model& m, int underlay_channel);  // inside the node editor (ui.cpp)
  void sketch_new_link(std::uintptr_t a, std::uintptr_t b, const std::map<std::uintptr_t, std::string>& pin_of);
  void sketch_popups(const Model& m);  // the menu of a sketch block and the block dialog
  void send_prototype(const Model& m);
  void place_realized(const Model& m);  // realized blocks take the places the human drew them at
  std::map<std::uintptr_t, std::string> sketch_pin_of_;  // pin id → sketch block id (this frame)
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
  // Ports of a block from a per-model map (the snapshot keeps them in one flat list).
  const std::vector<const index::PortRow*>& ports_of(const Model& m, const std::string& id) const;
  mutable std::uint64_t ports_serial_ = ~0ULL;
  mutable std::unordered_map<std::string, std::vector<const index::PortRow*>> ports_cache_;
  mutable std::uint64_t node_serial_ = ~0ULL;
  mutable std::unordered_map<std::string, std::size_t> node_index_;
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
  std::function<void()> wake_;
  std::filesystem::path root_;  // the open project
  std::unique_ptr<ProjectPicker> picker_;
  bool picker_open_ = false;
  std::optional<std::filesystem::path> open_request_;
  // shell
  SideView side_ = SideView::Explorer;
  bool show_settings_ = false;  // the settings window
  int settings_page_ = 0;
  core::Config cfg_;  // karkas.toml of the project (models, levels, blocks, roles)
  std::string cfg_error_;
  std::string models_msg_;
  std::string api_edit_;  // API being edited ("" none, "+" new)
  char api_name_[32] = {};
  char api_model_[96] = {};
  char api_url_[160] = {};
  char api_env_[64] = {};
  int api_provider_ = 0;
  bool side_visible_ = true;
  char filter_[128] = {};
  char search_[128] = {};
  bool focus_search_ = false;
  std::vector<std::string> open_files_;                         // code tabs of the editor
  std::map<std::string, std::unique_ptr<TextEditor>> editors_;  // per open file
  std::map<std::string, std::string> editor_loaded_;            // text shown per file
  std::string select_tab_;     // tab to bring forward next frame ("#schema" or a file)
  std::string bottom_select_;  // bottom panel tab to bring forward next frame
  Cleared cleared_;            // rows removed from the bottom panels (display only)
  std::map<int, double> interrupted_;  // pid → time SIGINT was sent (Kill appears after a while)
  // orchestrator chat
  std::unique_ptr<Chat> chat_;
  bool show_chat_ = false;
  bool chat_focus_ = false;
  int chat_mode_ = 0;  // 0 task, 1 bug
  // What a message is about (ADR 0034): follows the last block or ticket selected; "Whole
  // project" until the next selection.
  ChatScope chat_scope_;
  std::string scope_seen_block_, scope_seen_ticket_;
  void update_chat_scope(const Model& m);
  void chat_scope_bar(const Model* m);
  char chat_input_[4096] = {};
  std::uint64_t chat_serial_ = ~0ULL;
  std::vector<ChatEntry> chat_cache_;
  bool chat_scroll_ = false;
  int chat_tab_ = 0;               // 0 dialog, 1 live feed of the agents
  int chat_tab_select_ = -1;       // tab to bring forward next frame
  bool chat_window_focus_ = false;
  std::unique_ptr<JournalFeed> feed_;
  ui::TextSelect chat_sel_;        // mouse selection of the chat's texts
  std::string live_journal_;       // run shown in the live feed
  bool live_follow_ = true;        // follow the newest active run
  std::size_t live_seen_ = 0;      // steps shown last frame (scroll down on new ones)
  std::set<std::string> live_open_;  // expanded steps ("journal#index")
  float live_list_w_ = 0.0f;
  // schema changes (side bar) and their ghost on the schema (ADR 0021)
  bool ghost_on_ = true;          // draw the selected change on the schema
  std::string ghost_shown_;       // change whose level was opened (navigate once per selection)
  bool change_editing_ = false;
  std::string change_edit_id_;
  std::string change_edit_buf_;
  std::string edit_change_next_;  // "Edit" pressed in the chat: open this change in edit mode
  // address bar
  bool editing_path_ = false;
  bool focus_path_ = false;
  char path_buf_[256] = {};
  std::string path_error_;
  // highlighting (hover, Shift+click)
  std::set<std::string> pinned_;  // blocks whose links stay lit
  std::vector<LinkDesc> menu_links_;  // links of the line whose menu is open
  std::string menu_line_key_;          // its key (for "Straighten the line")
  int suppress_bg_menu_ = 0;           // frames in which the editor's background menu is ignored
  // Lines bent by the user (ADR 0022): the points a line passes through, as (u, v) in the frame of
  // its ends, by the line's key; kept in .karkas/bends.json.
  std::map<std::string, std::vector<ImVec2>> bends_;
  std::uint64_t bends_version_ = 0;  // bumped on every change: the plan of lines is made again
  std::string bend_drag_key_;    // the line whose bend is dragged
  int bend_drag_index_ = -1;     // that bend
  std::string bend_drag_label_;  // the handle pressed (the same id keeps it pressed)
  void load_bends();
  void save_bends();
  std::string hover_unit_;        // block under the mouse on the schema (last frame)
  std::string tree_hover_;        // block under the mouse in the tree (this frame)
  std::string hover_frame_;       // opened group whose path bar is under the mouse
  bool fit_on_apply_ = false;     // fit the view when the next layout arrives (open/close)
  bool canvas_hovered_ = false;   // the mouse is over the schema canvas (it sets its own cursor)
  bool drag_from_node_ = false;   // the left drag started on a block (move cursor)
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
