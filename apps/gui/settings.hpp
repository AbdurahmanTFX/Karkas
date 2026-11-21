#pragma once
// User settings of karkas-gui: ~/.config/karkas/gui.json (language, sizes).
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "i18n.hpp"

namespace kk::gui {

struct GuiSettings {
  Lang lang = Lang::En;
  float block_scale = 1.0f;  // default size of blocks on the schema
  float ui_scale = 1.0f;     // text size of the interface (io.FontGlobalScale)
  bool links_by_block = true;   // link color: of the block it starts at (else: by link kind, 14.5)
  bool blocks_by_block = true;  // block background: its own color (else: by status, 14.5)
  std::vector<std::string> recent;  // recently opened project roots, newest first (both kinds)
  // The two workspaces (ADR 0038): the last project opened of each kind; the mode switch of the Karkas
  // menu opens it.
  std::string last_code, last_notes;
  int note_view = 0;  // how a Markdown note's tab opens (ADR 0040): 0 its text, 1 text and view side by side, 2 the view
  // Layout of the panels (ADR 0021): sizes of the split areas, windows and tabs shown.
  float tickets_list_w = 0.0f;  // width of the ticket list in the bottom panel (0 = half)
  float changes_list_h = 0.0f;  // height of the change list in the side bar (0 = a third)
  bool show_side = true;        // side bar (explorer, search, changes)
  bool show_inspector = true;
  bool show_bottom = true;      // bottom panel
  std::vector<std::string> hidden_tabs;  // bottom panel tabs hidden: tickets, agents, build, costs, drift
  bool chat_steps = true;       // the chat shows every step of the agents
  bool show_activity = true;    // the activity bar on the left
  bool show_status = true;      // the status bar at the bottom
  // Sizes of the docked panels as parts of the window: the default layout (ADR 0032).
  float side_frac = 0.19f, inspector_frac = 0.26f, bottom_frac = 0.28f;
  // Tiles that are windows of the desktop (side, inspector, tickets, agents, build, costs,
  // drift, chat); where the others are docked or float is kept in gui_layout.ini.
  std::vector<std::string> os_windows;
};

// The dock layout of the tiles (ImGui's ini text), next to gui.json.
std::filesystem::path layout_path();
// The layout text without values that are not numbers (a column "Weight=-nan" saved by ImGui once
// made every later resize of that table abort): such a line goes, ImGui uses its default.
std::string clean_layout_ini(std::string_view text);

// Puts a project root first in the recent list (at most 16) and makes it the last project of its kind.
void add_recent(GuiSettings& s, const std::filesystem::path& root);
// The folder holds a notes project (project.ksl says `lang notes`, ADR 0038).
bool is_notes_project_dir(const std::filesystem::path& root);
// The folder holds a project (project.ksl).
bool is_project_dir(const std::filesystem::path& root);

// Limits of block sizes (canvas units): a block never shrinks to a dot.
inline constexpr float kMinBlockW = 120.0f;
inline constexpr float kMinBlockH = 56.0f;
inline constexpr float kMaxBlockW = 1600.0f;
inline constexpr float kMaxBlockH = 1200.0f;
inline constexpr float kMinBlockScale = 0.6f;
inline constexpr float kMaxBlockScale = 2.5f;

std::filesystem::path settings_path();
GuiSettings load_settings();  // defaults (language from LANG) when the file is missing
bool save_settings(const GuiSettings& s);

}  // namespace kk::gui
