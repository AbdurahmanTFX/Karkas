// The IDE shell of karkas-gui: menu, activity bar, side bar views, editor tabs, inspector,
// bottom panel and status bar. The schema itself is drawn in ui.cpp.
#include <cstdint>
#include <TextEditor.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <tuple>

#include "i18n.hpp"
#include "kk/agents/edit.hpp"
#include "icons.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "viewports.hpp"
#include "widgets.hpp"

namespace kk::gui {

namespace {

std::string upper_ascii(std::string s) {
  for (auto& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;
}

Icon kind_icon(const std::string& kind, bool open = false) {
  if (kind == "group") return open ? Icon::FolderOpen : Icon::Folder;
  if (kind == "entry") return Icon::Play;
  if (kind == "shared") return Icon::Library;
  return Icon::Cube;
}

const ImVec4 kFolderColor = rgb(220, 182, 122);
const ImVec4 kEntryColor = rgb(137, 209, 133);
const ImVec4 kSharedColor = rgb(197, 134, 192);

// A flat clickable item of the status bar.
bool status_item(const std::string& id, const std::string& text, ImVec4 color, const char* tooltip = nullptr) {
  ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetWindowHeight();
  ImVec2 size(ts.x + 12, h);
  ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetWindowPos().y));
  ImGui::InvisibleButton(id.c_str(), size);
  bool hot = ImGui::IsItemHovered();
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 a = ImGui::GetItemRectMin();
  if (hot) dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), IM_COL32(255, 255, 255, 22));
  dl->AddText(ImVec2(a.x + 6, a.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(color), text.c_str());
  if (hot && tooltip != nullptr) ImGui::SetTooltip("%s", tooltip);
  ImGui::SameLine(0, 2);
  return ImGui::IsItemClicked();
}

float status_item_width(const std::string& text) { return ImGui::CalcTextSize(text.c_str()).x + 14; }

}  // namespace

// --- frame ---------------------------------------------------------------------------------

bool Ui::draw(const std::shared_ptr<const Model>& mp, bool first_frame) {
  pulse_ = ImGui::GetTime();
  const Model* m = mp && mp->snap ? mp.get() : nullptr;
  if (m != nullptr) {
    if (m->serial > sizes_serial_ && resizing_.empty()) sizes_.clear();  // the model has them now
    active_blocks_.clear();
    for (const auto& a : m->active) active_blocks_.insert(a.block);
  }
  (void)first_frame;
  draw_menu(m);
  if (settings_.show_status) draw_status_bar(m);
  if (settings_.show_activity) draw_activity_bar(m);
  dock_space();
  if (m == nullptr) {
    if (begin_editor_window()) {
      ImGui::SetCursorScreenPos(ImVec2(free_area_.min.x + 16, free_area_.min.y + 16));
      ui::muted("%s", mp ? mp->message.c_str() : tr("indexing..."));
    }
    ImGui::End();
    draw_side_bar(m);
    bool insp = settings_.show_inspector;
    if (insp) {
      if (begin_tile(Tile::Inspector, tr("Inspector"), &insp, theme::kSideBg, ImVec2(10, 8))) ui::muted("%s", tr("indexing..."));
      ImGui::End();
      if (!insp) {
        settings_.show_inspector = false;
        settings_changed();
      }
    }
    draw_settings_window(m);
    draw_chat_window(m);
    order_windows();
    return true;
  }
  // A change chosen in the side bar (or the chat): open the level that shows it, its groups in
  // place, and select its first block; once per choice, so the user can look around after.
  if (ghost_on_ && !req_.change.empty() && m->ghost.id == req_.change && ghost_shown_ != req_.change && m->req.change == req_.change) {
    ghost_shown_ = req_.change;
    const auto& g = m->ghost;
    bool known = g.scope.empty() || m->snap->block(g.scope) != nullptr;
    if (known && (!g.touched.empty() || !g.blocks.empty())) {
      enter(g.scope);
      req_.expanded = g.expand;
      if (!g.touched.empty()) select(g.touched.front());
      select_tab_ = "#schema";
      fit_on_apply_ = true;
    }
  }
  if (req_.change.empty()) ghost_shown_.clear();
  // The selected block is gone (removed, renamed): the block of the level shown is selected, at the
  // top the project. Only on a model made for this selection, never on one that lags behind it.
  if (!req_.selected.empty() && m->req.selected == req_.selected && m->snap->block(req_.selected) == nullptr) {
    select(m->snap->block(req_.scope) != nullptr ? req_.scope : "");
  }
  update_chat_scope(*m);
  draw_editor(*m);
  draw_side_bar(m);
  if (settings_.show_inspector) draw_inspector(*m);
  draw_bottom(*m);
  draw_settings_window(m);
  draw_chat_window(m);
  draw_dialogs(*m);
  order_windows();
  ui::update_cursor(canvas_hovered_);
  return !m->active.empty() || (chat_ && chat_->busy());
}

// --- windows ---------------------------------------------------------------------------------

bool Ui::tab_visible(const char* key) const {
  return std::find(settings_.hidden_tabs.begin(), settings_.hidden_tabs.end(), key) == settings_.hidden_tabs.end();
}

void Ui::set_tab_visible(const char* key, bool on) {
  std::erase(settings_.hidden_tabs, std::string(key));
  if (!on) settings_.hidden_tabs.emplace_back(key);
  settings_changed();
}

bool Ui::bottom_visible() const {
  if (!settings_.show_bottom) return false;
  for (const char* k : {"tickets", "agents", "build", "costs", "drift", "run"}) {
    if (tab_visible(k)) return true;
  }
  return false;  // every tab hidden: no empty panel
}

// --- tiles (ADR 0032) ----------------------------------------------------------------------

namespace {

struct TileInfo {
  const char* key;     // in gui.json (os_windows, hidden_tabs)
  const char* window;  // window id ("###...")
  ImGuiDir edge;       // default place in the layout
  float w, h;          // default size when floating (in font sizes)
};

constexpr std::array<TileInfo, static_cast<std::size_t>(Ui::Tile::Count)> kTiles = {{
    {"side", "###side", ImGuiDir_Left, 20, 40},
    {"inspector", "###Inspector", ImGuiDir_Right, 26, 44},
    {"tickets", "###tile_tickets", ImGuiDir_Down, 60, 22},
    {"agents", "###tile_agents", ImGuiDir_Down, 60, 22},
    {"build", "###tile_build", ImGuiDir_Down, 60, 22},
    {"costs", "###tile_costs", ImGuiDir_Down, 60, 22},
    {"drift", "###tile_drift", ImGuiDir_Down, 60, 22},
    {"run", "###tile_run", ImGuiDir_Down, 60, 22},
    {"chat", "###chat_window", ImGuiDir_Right, 52, 48},
}};

const TileInfo& info(Ui::Tile t) { return kTiles[static_cast<std::size_t>(t)]; }

constexpr ImGuiID kDockSpace = 0x6b61726bu;  // "kark": the same id in every run (kept in the layout file)

// The dockspace node holding `node`, or nullptr when it is not part of our dockspace.
bool in_dockspace(ImGuiDockNode* node) {
  while (node != nullptr && node->ParentNode != nullptr) node = node->ParentNode;
  return node != nullptr && node->ID == kDockSpace;
}

}  // namespace

Ui::TileMode Ui::tile_mode(Tile t) const {
  const auto& keys = settings_.os_windows;
  if (std::find(keys.begin(), keys.end(), info(t).key) != keys.end() && viewports::enabled()) return TileMode::Window;
  ImGuiWindow* w = ImGui::FindWindowByName(info(t).window);
  if (w == nullptr) return t == Tile::Chat ? TileMode::Floating : TileMode::Docked;  // not drawn yet: the default layout
  return w->DockId != 0 ? TileMode::Docked : TileMode::Floating;
}

void Ui::set_tile_mode(Tile t, TileMode mode) {
  auto i = static_cast<std::size_t>(t);
  std::erase(settings_.os_windows, std::string(info(t).key));
  if (mode == TileMode::Window && viewports::enabled()) settings_.os_windows.emplace_back(info(t).key);
  tile_request_[i] = mode == TileMode::Docked ? 1 : mode == TileMode::Floating ? 2 : 3;
  settings_changed();
}

void Ui::build_default_layout() {
  // The layout of ADR 0021: the side bar on the left, the inspector on the right, the bottom
  // tabs between them under the schema; the chat floats. The central part stays empty.
  ImGui::DockBuilderRemoveNode(kDockSpace);
  ImGui::DockBuilderAddNode(kDockSpace, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(kDockSpace, ImVec2(std::max(200.0f, work_.w()), std::max(200.0f, work_.h())));
  ImGuiID center = kDockSpace;
  float side = std::clamp(settings_.side_frac, 0.08f, 0.5f);
  float insp = std::clamp(settings_.inspector_frac, 0.08f, 0.5f);
  float bottom = std::clamp(settings_.bottom_frac, 0.08f, 0.8f);
  ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, side, nullptr, &center);
  ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, insp / std::max(0.2f, 1.0f - side), nullptr, &center);
  ImGuiID down = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, bottom, nullptr, &center);
  for (std::size_t i = 0; i < kTiles.size(); ++i) {
    const auto& ti = kTiles[i];
    if (i == static_cast<std::size_t>(Tile::Chat)) continue;
    if (std::find(settings_.os_windows.begin(), settings_.os_windows.end(), ti.key) != settings_.os_windows.end()) continue;
    ImGuiID node = ti.edge == ImGuiDir_Left ? left : ti.edge == ImGuiDir_Right ? right : down;
    ImGui::DockBuilderDockWindow(ti.window, node);
    last_dock_[i] = node;
  }
  ImGui::DockBuilderFinish(kDockSpace);
  bottom_select_ = "tickets";  // the first tab of the bottom tiles in front
}

void Ui::apply_dock_requests() {
  ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(kDockSpace);
  for (std::size_t i = 0; i < kTiles.size(); ++i) {
    if (tile_request_[i] != 1) continue;
    tile_request_[i] = 0;
    const auto& ti = kTiles[i];
    // Its last place; else the place of a tile of the same edge; else a new part split off the
    // schema at its default edge.
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(last_dock_[i]);
    if (node == nullptr || !in_dockspace(node) || node->IsCentralNode() || node->IsSplitNode()) {
      node = nullptr;
      // The chat gets a tile of its own under the inspector (both are read side by side).
      if (i == static_cast<std::size_t>(Tile::Chat)) {
        ImGuiWindow* insp = ImGui::FindWindowByName(info(Tile::Inspector).window);
        if (insp != nullptr && insp->DockNode != nullptr && in_dockspace(insp->DockNode) && !insp->DockNode->IsCentralNode()) {
          ImGuiID below = 0;
          ImGuiID above = 0;
          ImGui::DockBuilderSplitNode(insp->DockNode->ID, ImGuiDir_Down, 0.6f, &below, &above);
          node = ImGui::DockBuilderGetNode(below);
        }
      }
      for (std::size_t k = 0; k < kTiles.size() && node == nullptr; ++k) {
        if (k == static_cast<std::size_t>(Tile::Chat) && i != k) continue;
        if (k == i || kTiles[k].edge != ti.edge) continue;
        ImGuiWindow* w = ImGui::FindWindowByName(kTiles[k].window);
        if (w != nullptr && w->DockNode != nullptr && in_dockspace(w->DockNode) && !w->DockNode->IsCentralNode()) node = w->DockNode;
      }
    }
    ImGuiID target = node != nullptr ? node->ID : 0;
    if (target == 0 && central != nullptr) {
      float ratio = ti.edge == ImGuiDir_Down ? 0.3f : 0.25f;
      ImGuiID rest = 0;
      target = ImGui::DockBuilderSplitNode(central->ID, ti.edge, ratio, nullptr, &rest);
      central = ImGui::DockBuilderGetCentralNode(kDockSpace);
    }
    if (target != 0) {
      ImGui::DockBuilderDockWindow(ti.window, target);
      last_dock_[i] = target;
    }
  }
}

void Ui::dock_space() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  work_ = {vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y)};
  ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::Begin("###dockhost", nullptr, fl);
  ImGui::PopStyleVar(3);
  if (reset_layout_ || ImGui::DockBuilderGetNode(kDockSpace) == nullptr) {
    reset_layout_ = false;
    build_default_layout();
  }
  apply_dock_requests();
  dock_id_ = kDockSpace;
  // The central part is the schema: nothing docks over it, the mouse goes through it.
  ImGui::DockSpace(kDockSpace, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode);
  ImGui::End();
  if (ImGuiDockNode* c = ImGui::DockBuilderGetCentralNode(kDockSpace); c != nullptr && c->Size.x > 1 && c->Size.y > 1) {
    free_area_ = {c->Pos, ImVec2(c->Pos.x + c->Size.x, c->Pos.y + c->Size.y)};
  } else {
    free_area_ = work_;
  }
}

bool Ui::begin_tile(Tile t, const std::string& title, bool* open, ImU32 bg, ImVec2 padding, ImGuiWindowFlags extra) {
  auto i = static_cast<std::size_t>(t);
  const auto& ti = info(t);
  ImGuiWindow* w = ImGui::FindWindowByName(ti.window);
  bool docked = w != nullptr && w->DockIsActive;
  bool out = tile_mode(t) == TileMode::Window;
  // A window of the desktop: never merged back into the main window, decorated by the window
  // manager (its title bar moves and resizes it), with a task bar entry like an application.
  ImGuiWindowClass wc;
  wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoCloseButton;  // each tab has its own close button
  if (out) {
    wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    wc.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoDecoration | ImGuiViewportFlags_NoTaskBarIcon;
  }
  ImGui::SetNextWindowClass(&wc);
  float fs = ImGui::GetFontSize();
  ImVec2 size(std::min(ti.w * fs, std::max(200.0f, free_area_.w() * 0.9f)), std::min(ti.h * fs, std::max(160.0f, free_area_.h() * 0.9f)));
  int& req = tile_request_[i];
  if (req == 2 || req == 3) {
    // Out of the layout (SetNextWindowPos undocks) to the middle of the schema, or to a free
    // spot of the desktop.
    ImVec2 pos = req == 3 ? viewports::spot_for_new_window(size)
                          : ImVec2(free_area_.min.x + (free_area_.w() - size.x) * 0.5f, free_area_.min.y + (free_area_.h() - size.y) * 0.4f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    req = 0;
    docked = false;
  }
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::col(bg));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::col(theme::kBorder));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, docked || out ? 0.0f : 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoCollapse | extra;
  if (out) fl |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking;  // the window manager's title bar
  bool visible = ImGui::Begin((title + ti.window).c_str(), open, fl);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
  if (ImGui::IsWindowDocked()) {
    ImGuiID id = ImGui::GetWindowDockID();
    if (in_dockspace(ImGui::DockBuilderGetNode(id))) last_dock_[i] = id;
  }
  // Right click on the tab or the title bar: where the tile goes.
  if (ImGui::BeginPopupContextItem("##tile_menu")) {
    tile_menu_items(t);
    ImGui::EndPopup();
  }
  return visible;
}

void Ui::tile_menu_items(Tile t) {
  TileMode mode = tile_mode(t);
  if (ImGui::MenuItem(with_icon(Icon::Dock, tr("Dock as a tile")).c_str(), nullptr, mode == TileMode::Docked)) set_tile_mode(t, TileMode::Docked);
  if (ImGui::MenuItem(with_icon(Icon::Float, tr("Float inside the window")).c_str(), nullptr, mode == TileMode::Floating)) {
    set_tile_mode(t, TileMode::Floating);
  }
  if (ImGui::MenuItem(with_icon(Icon::PopOut, tr("Separate window")).c_str(), nullptr, mode == TileMode::Window, viewports::enabled())) {
    set_tile_mode(t, TileMode::Window);
  }
  if (!viewports::enabled() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", tr("Windows of the desktop are off (KARKAS_GUI_WINDOWS=0 or the platform has none)"));
  }
  ImGui::Separator();
  ImGui::TextDisabled("%s", tr("Drag a tab to any edge to dock it there"));
}

void Ui::tile_button(Tile t) {
  TileMode mode = tile_mode(t);
  Icon ic = mode == TileMode::Docked ? Icon::Dock : mode == TileMode::Floating ? Icon::Float : Icon::PopOut;
  std::string id = std::string("tile_btn_") + info(t).key;
  if (ui::icon_button(id.c_str(), ic, tr("Where this panel lives: a tile of the layout, floating, or a window of its own"))) {
    ImGui::OpenPopup((id + "_menu").c_str());
  }
  if (ImGui::BeginPopup((id + "_menu").c_str())) {
    tile_menu_items(t);
    ImGui::EndPopup();
  }
}

bool Ui::begin_editor_window() {
  ImGui::SetNextWindowPos(work_.min, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(work_.w(), work_.h()), ImGuiCond_Always);
  ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::col(theme::kEditorBg));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
  bool open = ImGui::Begin("Editor###editor", nullptr, fl);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
  return open;
}

void Ui::order_windows() {
  // Back to front: the schema, the dockspace with its tiles; floating tiles, the settings and
  // dialogs stay above them whichever was clicked last.
  for (const char* name : {"###dockhost", "###editor"}) {
    if (ImGuiWindow* w = ImGui::FindWindowByName(name)) ImGui::BringWindowToDisplayBack(w);
  }
}

void Ui::draw_windows_menu() {
  auto item = [&](Icon ic, const char* label, bool* v) {
    if (ImGui::MenuItem(with_icon(ic, label).c_str(), nullptr, v)) settings_changed();
  };
  item(Icon::Files, tr("Side bar (explorer, search, changes)"), &settings_.show_side);
  item(Icon::Info, tr("Inspector"), &settings_.show_inspector);
  item(Icon::Ticket, tr("Bottom panel"), &settings_.show_bottom);
  if (ImGui::BeginMenu(with_icon(Icon::Layers, tr("Bottom panel tabs")).c_str(), settings_.show_bottom)) {
    for (const auto& [key, label, ic] : {std::tuple{"tickets", "Tickets", Icon::Ticket}, std::tuple{"agents", "Agents", Icon::Agent},
                                         std::tuple{"build", "Build & tests", Icon::Hammer}, std::tuple{"costs", "Costs", Icon::Coins},
                                         std::tuple{"drift", "Drift", Icon::Wave}, std::tuple{"run", "Run", Icon::Play}}) {
      bool on = tab_visible(key);
      if (ImGui::MenuItem(with_icon(ic, tr(label)).c_str(), nullptr, &on)) set_tab_visible(key, on);
    }
    ImGui::EndMenu();
  }
  item(Icon::Changes, tr("Activity bar"), &settings_.show_activity);
  item(Icon::Dot, tr("Status bar"), &settings_.show_status);
  if (ImGui::MenuItem(with_icon(Icon::Chat, tr("Orchestrator chat")).c_str(), "Ctrl+Shift+O", show_chat_)) {
    show_chat_ = !show_chat_;
    chat_focus_ = show_chat_;
  }
  if (ImGui::MenuItem(with_icon(Icon::Agent, tr("Live: agents step by step")).c_str())) open_live("");
  ImGui::Separator();
  // Where each panel lives (ADR 0032): a tile of the layout, floating, or a window of the desktop.
  if (ImGui::BeginMenu(with_icon(Icon::Dock, tr("Tiles and windows")).c_str())) {
    const std::array<std::pair<Tile, const char*>, 9> tiles = {{{Tile::Side, "Side bar (explorer, search, changes)"},
                                                                {Tile::Inspector, "Inspector"},
                                                                {Tile::Chat, "Orchestrator chat"},
                                                                {Tile::Tickets, "Tickets"},
                                                                {Tile::Agents, "Agents"},
                                                                {Tile::Build, "Build & tests"},
                                                                {Tile::Costs, "Costs"},
                                                                {Tile::Drift, "Drift"},
                                                                {Tile::Run, "Run"}}};
    for (const auto& [t, label] : tiles) {
      if (ImGui::BeginMenu(tr(label))) {
        tile_menu_items(t);
        ImGui::EndMenu();
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(with_icon(Icon::Undo, tr("Reset the layout of the tiles")).c_str())) {
      settings_.os_windows.clear();
      settings_.show_side = settings_.show_inspector = settings_.show_bottom = true;
      settings_.hidden_tabs.clear();
      reset_layout_ = true;
      settings_changed();
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(with_icon(Icon::Schema, tr("Only the schema (hide every panel)")).c_str())) {
    settings_.show_side = settings_.show_inspector = settings_.show_bottom = false;
    settings_.show_activity = false;
    show_chat_ = false;
    settings_changed();
  }
  if (ImGui::MenuItem(with_icon(Icon::Windows, tr("Show every panel")).c_str())) {
    settings_.show_side = settings_.show_inspector = settings_.show_bottom = true;
    settings_.show_activity = settings_.show_status = true;
    settings_.hidden_tabs.clear();
    settings_changed();
  }
}

void Ui::set_project_root(std::filesystem::path root) {
  reset_canvas_caches();
  root_ = std::move(root);
  reload_models();
  cleared_ = load_cleared(root_);
  chat_ = std::make_unique<Chat>(root_, find_karkas_cli(), wake_);
  chat_serial_ = ~0ULL;
  run_ = std::make_unique<RunSession>(root_, find_karkas_cli(), wake_);
  run_serial_ = ~0ULL;
  run_info_asked_ = false;
  feed_ = std::make_unique<JournalFeed>(root_ / ".karkas" / "runs");
  live_journal_.clear();
  load_bends();
  proto_on_ = false;
  load_prototype();
}

void Ui::go_to(const Model& m, const std::string& id) {
  const auto* b = m.snap->block(id);
  if (b == nullptr) return;
  if (b->kind == "group") {
    enter(id);
  } else {
    enter(core::parent_id(id));
  }
  select(id);
  select_tab_ = "#schema";
}

// --- menu ----------------------------------------------------------------------------------

void Ui::show_picker(bool create) {
  picker_ = std::make_unique<ProjectPicker>(root_.empty() ? std::filesystem::path() : root_.parent_path(), settings_.recent,
                                            create ? PickerMode::Create : PickerMode::Open);
  picker_open_ = true;
}

void Ui::draw_menu(const Model* m) {
  run_shortcuts();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) show_picker();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) show_picker(true);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Comma)) show_settings_ = !show_settings_;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O)) {
    show_chat_ = !show_chat_;
    chat_focus_ = show_chat_;
  }
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F)) {
    side_ = SideView::Search;
    focus_search_ = true;
  }
  if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_UpArrow) && !req_.scope.empty()) enter(core::parent_id(req_.scope));
  if (picker_) {
    if (auto chosen = picker_->draw(&picker_open_, false)) {
      open_request_ = *chosen;
      picker_open_ = false;
    }
    if (!picker_open_) picker_.reset();
  }
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
  if (!ImGui::BeginMainMenuBar()) {
    ImGui::PopStyleVar();
    return;
  }
  ImGui::PopStyleVar();
  if (ImGui::BeginMenu("Karkas")) {
    if (ImGui::MenuItem(with_icon(Icon::Plus, tr("New project...")).c_str(), "Ctrl+N")) show_picker(true);
    if (ImGui::MenuItem(with_icon(Icon::FolderOpen, tr("Open project...")).c_str(), "Ctrl+O")) show_picker();
    if (ImGui::BeginMenu(with_icon(Icon::Files, tr("Recent projects")).c_str(), !settings_.recent.empty())) {
      for (const auto& r : settings_.recent) {
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(r) / "project.ksl", ec)) continue;
        if (ImGui::MenuItem(r.c_str(), nullptr, std::filesystem::path(r) == root_)) open_request_ = std::filesystem::path(r);
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(with_icon(Icon::Folder, tr("Show project folder")).c_str())) w_.open_folder("");
    ImGui::Separator();
    if (ImGui::MenuItem(with_icon(Icon::Gear, tr("Settings...")).c_str(), "Ctrl+,")) show_settings_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem(with_icon(Icon::Close, tr("Quit")).c_str())) quit_ = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(tr("View"))) {
    if (ImGui::MenuItem(with_icon(Icon::Files, tr("Explorer")).c_str(), nullptr, side_ == SideView::Explorer)) side_ = SideView::Explorer;
    if (ImGui::MenuItem(with_icon(Icon::Search, tr("Search")).c_str(), "Ctrl+Shift+F", side_ == SideView::Search)) side_ = SideView::Search;
    if (ImGui::MenuItem(with_icon(Icon::Changes, tr("Schema changes")).c_str(), nullptr, side_ == SideView::Changes)) {
      side_ = SideView::Changes;
    }
    if (ImGui::MenuItem(with_icon(Icon::Chat, tr("Orchestrator chat")).c_str(), "Ctrl+Shift+O", show_chat_)) {
      show_chat_ = !show_chat_;
      chat_focus_ = show_chat_;
    }
    ImGui::Separator();
    bool all = req_.all_levels;
    if (ImGui::MenuItem(with_icon(Icon::Layers, tr("All levels")).c_str(), nullptr, &all)) {
      req_.all_levels = all;
      req_.expanded.clear();
      fit_on_apply_ = true;
    }
    ImGui::MenuItem(with_icon(Icon::Link, tr("use links")).c_str(), nullptr, &req_.show_use);
    if (ImGui::MenuItem(with_icon(Icon::Fit, tr("Fit to view")).c_str())) {
      navigate_ = 1;
      select_tab_ = "#schema";
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(tr("Windows"))) {
    draw_windows_menu();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(tr("Go"))) {
    if (ImGui::MenuItem(with_icon(Icon::ArrowUp, tr("Up one level")).c_str(), "Alt+Up", false, !req_.scope.empty())) {
      enter(core::parent_id(req_.scope));
    }
    if (ImGui::MenuItem(with_icon(Icon::Home, tr("Project root")).c_str())) enter("");
    ImGui::EndMenu();
  }
  run_menu(m);
  draw_run_buttons(m);
  ImGui::EndMainMenuBar();
}

// --- activity bar ----------------------------------------------------------------------------

void Ui::draw_activity_bar(const Model* m) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  float w = std::round(ImGui::GetFontSize() * 3.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::col(theme::kActivityBg));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 4));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;
  if (ImGui::BeginViewportSideBar("##activity", vp, ImGuiDir_Left, w, fl)) {
    auto* dl = ImGui::GetWindowDrawList();
    auto button = [&](SideView v, Icon ic, const char* tip, int badge) {
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImVec2 size(w, w * 0.92f);
      ImGui::InvisibleButton(tip, size);
      bool hot = ImGui::IsItemHovered();
      bool on = side_ == v && settings_.show_side;
      if (v == SideView::Settings) on = show_settings_;
      if (v == SideView::Chat) on = show_chat_;
      if (ImGui::IsItemClicked()) {
        if (v == SideView::Search && side_ != v) focus_search_ = true;
        if (v == SideView::Settings) {
          show_settings_ = !show_settings_;  // a separate window, not a side bar view
        } else if (v == SideView::Chat) {
          show_chat_ = !show_chat_;  // a separate window too
          chat_focus_ = show_chat_;
        } else if (side_ == v && settings_.show_side) {
          settings_.show_side = false;  // a second click hides the side bar, like an IDE
          settings_changed();
        } else {
          side_ = v;
          if (!settings_.show_side) {
            settings_.show_side = true;
            settings_changed();
          }
        }
      }
      if (hot) ImGui::SetTooltip("%s", tip);
      if (on) dl->AddRectFilled(p, ImVec2(p.x + 2, p.y + size.y), theme::kAccent);
      ImFont* f = theme::icon_font_big();
      float fs = f->FontSize;
      dl->AddText(f, fs, ImVec2(p.x + (size.x - fs) * 0.5f, p.y + (size.y - fs) * 0.5f),
                  on || hot ? IM_COL32(230, 230, 230, 255) : IM_COL32(133, 133, 133, 255), icon(ic));
      if (badge > 0) {
        std::string n = badge > 99 ? "99+" : std::to_string(badge);
        float r = ImGui::GetFontSize() * 0.48f;
        ImVec2 c(p.x + size.x * 0.70f, p.y + size.y * 0.70f);
        dl->AddCircleFilled(c, r, theme::kAccent);
        ImVec2 ts = ImGui::CalcTextSize(n.c_str());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.8f, ImVec2(c.x - ts.x * 0.4f, c.y - ts.y * 0.4f), IM_COL32_WHITE, n.c_str());
      }
    };
    int pending = 0;
    if (m != nullptr) {
      for (const auto& c : m->changes) pending += c.status.value == "pending" ? 1 : 0;
    }
    button(SideView::Explorer, Icon::Files, tr("Explorer"), 0);
    button(SideView::Search, Icon::Search, tr("Search"), 0);
    button(SideView::Changes, Icon::Changes, tr("Schema changes"), pending);
    button(SideView::Chat, Icon::Chat, tr("Orchestrator chat"), 0);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - w - 4);
    button(SideView::Settings, Icon::Gear, tr("Settings"), 0);
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
}

// --- status bar --------------------------------------------------------------------------------

void Ui::draw_status_bar(const Model* m) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  float h = std::round(ImGui::GetFontSize() * 1.65f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::col(theme::kStatusBg));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;
  if (ImGui::BeginViewportSideBar("##status", vp, ImGuiDir_Down, h, fl)) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y), theme::kBorder);
    ImVec4 text = rgb(204, 204, 204);
    ImVec4 muted = rgb(140, 140, 140);
    std::string project = root_.empty() ? "Karkas" : root_.filename().string();
    if (status_item("st_project", with_icon(Icon::Home, project), text, tr("Open project..."))) show_picker();
    if (m != nullptr) {
      int errors = 0;
      int warnings = 0;
      for (const auto& d : m->snap->drift) (d.severity == "E" ? errors : warnings)++;
      std::string probs = std::string(icon(Icon::Error)) + " " + std::to_string(errors) + "   " + icon(Icon::Warning) + " " +
                          std::to_string(warnings);
      if (status_item("st_problems", probs, errors > 0 ? rgb(241, 120, 120) : text, tr("Drift: errors and warnings"))) {
        bottom_select_ = "drift";
      }
      // The mode of the agents' model (ADR 0024); in mode mcp, the turns waiting for the client.
      {
        std::string mode = cfg_.mcp() ? "MCP" : "API";
        std::size_t waiting = cfg_.mcp() ? mcp_waiting().size() : 0;
        std::string label = waiting > 0 ? fmt::format("{} · {} {}", mode, waiting, tr("waiting")) : mode;
        float a = static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0));
        if (status_item("st_mode", with_icon(cfg_.mcp() ? Icon::Agent : Icon::Coins, label), waiting > 0 ? rgb(240, 200, 30, a) : text,
                        cfg_.mcp() ? tr("Agents' model: an MCP client (Claude Code) answers their turns. Click: the chat")
                                   : tr("Agents' model: the APIs of karkas.toml. Click: the chat"))) {
          show_chat_ = true;
          chat_window_focus_ = true;
        }
      }
      if (!m->active.empty()) {
        float a = static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0));
        if (status_item("st_agents", with_icon(Icon::Agent, fmt::format(fmt::runtime(tr("agents working: {}")), m->active.size())),
                        rgb(240, 200, 30, a), tr("Watch the agents step by step"))) {
          open_live("");  // follow the agent that works now
        }
      }
      {
        double total = 0;
        for (const auto& r : m->runs) total += r.cost_usd;
        if (total > 0 && status_item("st_cost", with_icon(Icon::Coins, fmt::format("${:.2f}", total)), text, tr("Total cost of all agent runs"))) {
          bottom_select_ = "costs";
          settings_.show_bottom = true;
        }
      }
      run_status_item(m);
      if (chat_ && chat_->busy()) {
        float a = static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0));
        std::string t = chat_->current_ticket();
        std::string label = t.empty() ? std::string(tr("orchestrator: working")) : fmt::format(fmt::runtime(tr("orchestrator: {}")), t);
        if (status_item("st_chat", with_icon(Icon::Chat, label), rgb(240, 200, 30, a), tr("Open the orchestrator chat"))) show_chat_ = true;
      }
      status_item("st_engine", m->engine + " · " + fmt::format(fmt::runtime(tr("index {:.0f} ms")), m->index_ms), muted,
                  tr("Layout engine and the time of the last index update"));
      if (!m->message.empty()) status_item("st_msg", m->message, muted);
      // Right side.
      std::string lang_text = lang() == Lang::Ru ? "RU" : "EN";
      std::string pins = pinned_.empty() ? "" : with_icon(Icon::Pin, std::to_string(pinned_.size()));
      std::string opened = m->nested.empty() ? "" : with_icon(Icon::Layers, tr("opened in place"));
      float right = status_item_width(lang_text) + (pins.empty() ? 0 : status_item_width(pins)) +
                    (opened.empty() ? 0 : status_item_width(opened)) + 10;
      ImGui::SameLine(ImGui::GetWindowWidth() - right);
      if (!opened.empty()) status_item("st_open", opened, rgb(120, 180, 255), tr("groups opened in place: positions are not saved"));
      if (!pins.empty() && status_item("st_pins", pins, rgb(255, 230, 120), tr("Highlighted blocks (click to clear)"))) pinned_.clear();
      if (status_item("st_lang", lang_text, text, tr("Interface language (click to switch)"))) {
        settings_.lang = settings_.lang == Lang::Ru ? Lang::En : Lang::Ru;
        settings_changed();
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

// --- side bar ----------------------------------------------------------------------------------

void Ui::draw_side_bar(const Model* m) {
  tree_hover_.clear();
  if (!settings_.show_side) return;
  const std::array<const char*, 4> titles = {"EXPLORER", "SEARCH", "SCHEMA CHANGES", "SETTINGS"};
  const std::array<const char*, 4> tabs = {"Explorer", "Search", "Schema changes", "Settings"};
  auto view = static_cast<std::size_t>(side_);
  bool open = true;
  bool visible = begin_tile(Tile::Side, tr(tabs[view]), &open, theme::kSideBg, ImVec2(8, 8));
  if (!open) {
    settings_.show_side = false;  // closed by its tab (the activity bar or the Windows menu shows it again)
    settings_changed();
  }
  if (!visible) {
    ImGui::End();
    return;
  }
  ui::panel_title(tr(titles[view]));
  float btn_w = ImGui::GetFrameHeight() * 1.1f;
  if (side_ == SideView::Explorer) {
    ImGui::SameLine(ImGui::GetContentRegionMax().x - btn_w * 3);
    if (ui::icon_button("ex_open", Icon::FolderOpen, tr("Open project..."))) show_picker();
    ImGui::SameLine(0, 2);
    if (ui::icon_button("ex_folder", Icon::Folder, tr("Show project folder"))) w_.open_folder("");
    ImGui::SameLine(0, 2);
  } else {
    ImGui::SameLine(ImGui::GetContentRegionMax().x - btn_w);
  }
  tile_button(Tile::Side);  // closed by its tab, its title bar or its window
  ImGui::Spacing();
  if (m == nullptr) {
    ui::muted("%s", tr("indexing..."));
  } else if (side_ == SideView::Explorer) {
    side_explorer(*m);
  } else if (side_ == SideView::Search) {
    side_search(*m);
  } else {
    side_changes(*m);
  }
  ImGui::End();
}

void Ui::side_explorer(const Model& m) {
  const auto& snap = *m.snap;
  ImGui::SetNextItemWidth(-1);
  std::string hint = with_icon(Icon::Search, tr("Filter blocks"));
  ImGui::InputTextWithHint("##filter", hint.c_str(), filter_, sizeof filter_);
  ImGui::Spacing();
  std::string project = upper_ascii(snap.project.empty() ? "root" : snap.project);
  if (!ui::section((project + "###proj").c_str())) return;
  std::string f = core::to_lower(filter_);
  std::map<std::string, bool> memo;
  std::function<bool(const std::string&)> visible = [&](const std::string& id) -> bool {
    if (f.empty()) return true;
    if (auto it = memo.find(id); it != memo.end()) return it->second;
    const auto* b = snap.block(id);
    bool v = b != nullptr && (core::contains(core::to_lower(id), f) || core::contains(core::to_lower(b->purpose), f));
    if (b != nullptr && !v) {
      for (const auto& c : b->children) v = v || visible(c);
    }
    return memo[id] = v;
  };
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
  ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, ImGui::GetFontSize() * 0.9f);
  std::function<void(const std::string&)> node = [&](const std::string& id) {
    if (!visible(id)) return;
    const auto* b = snap.block(id);
    if (b == nullptr) return;
    bool group = b->kind == "group";
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;
    if (!group) fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (req_.selected == id) fl |= ImGuiTreeNodeFlags_Selected;
    if (!f.empty() && group) {
      ImGui::SetNextItemOpen(true);
    } else if (group && (req_.scope == id || req_.scope.starts_with(id + "."))) {
      ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    }
    float x0 = ImGui::GetCursorScreenPos().x;
    bool open = ImGui::TreeNodeEx(("##" + id).c_str(), fl);
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) select(id);
    if (hovered && ImGui::IsMouseDoubleClicked(0)) go_to(m, id);
    if (hovered) {
      tree_hover_ = id;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s\n%s · %s\n%s", id.c_str(), tr_kind(b->kind), tr_status(b->status), b->purpose.c_str());
      }
    }
    if (ImGui::BeginPopupContextItem(("ctx" + id).c_str())) {
      ImGui::TextDisabled("%s", id.c_str());
      ImGui::Separator();
      if (ImGui::MenuItem(with_icon(Icon::Schema, tr("Show on the schema")).c_str())) go_to(m, id);
      if (group && ImGui::MenuItem(with_icon(Icon::Layers, tr("Open")).c_str())) enter(id);
      if (ImGui::MenuItem(with_icon(Icon::Card, tr("Open card in editor")).c_str())) w_.open_in_editor(b->dir + "/block.ksl", 1);
      if (ImGui::MenuItem(with_icon(Icon::Folder, tr("Open folder")).c_str())) w_.open_block_folder(id);
      ImGui::EndPopup();
    }
    auto* dl = ImGui::GetWindowDrawList();
    float label_x = x0 + ImGui::GetTreeNodeToLabelSpacing();
    float ty = mn.y + ImGui::GetStyle().FramePadding.y;
    ImVec4 ic = group ? kFolderColor : b->kind == "entry" ? kEntryColor : b->kind == "shared" ? kSharedColor : color_of(m, id);
    dl->AddText(ImVec2(label_x, ty), ImGui::GetColorU32(ic), icon(kind_icon(b->kind, open && group)));
    float tx = label_x + ImGui::CalcTextSize(icon(Icon::Folder)).x + 6;
    bool here = req_.scope == id;
    dl->AddText(ImVec2(tx, ty), ImGui::GetColorU32(here ? rgb(255, 255, 255) : ImGui::GetStyleColorVec4(ImGuiCol_Text)),
                core::last_segment(id).c_str());
    float r = ImGui::GetFontSize() * 0.22f;
    bool active = active_blocks_.contains(id);
    ImVec4 sc = active ? rgb(240, 200, 30, static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0))) : status_color(b->status);
    dl->AddCircleFilled(ImVec2(mx.x - r * 3, (mn.y + mx.y) * 0.5f), r, ImGui::GetColorU32(sc));
    if (open && group) {
      for (const auto& c : b->children) node(c);
      ImGui::TreePop();
    }
  };
  for (const auto& id : snap.top_level) node(id);
  ImGui::PopStyleVar(2);
  if (snap.blocks.empty()) ui::muted("%s", tr("The schema has no blocks yet."));
}

void Ui::side_search(const Model& m) {
  const auto& snap = *m.snap;
  ImGui::SetNextItemWidth(-1);
  if (focus_search_) {
    ImGui::SetKeyboardFocusHere();
    focus_search_ = false;
  }
  std::string hint = with_icon(Icon::Search, tr("Search blocks, purposes, ports"));
  ImGui::InputTextWithHint("##search", hint.c_str(), search_, sizeof search_);
  std::string q = core::to_lower(core::trim(search_));
  if (q.empty()) {
    ImGui::Spacing();
    ui::muted("%s", tr("Type to search; a click opens the block on the schema."));
    return;
  }
  std::vector<const index::BlockRow*> blocks;
  for (const auto& b : snap.blocks) {
    if (core::contains(core::to_lower(b.id), q) || core::contains(core::to_lower(b.purpose), q)) blocks.push_back(&b);
    if (blocks.size() >= 200) break;
  }
  std::vector<const index::PortRow*> ports;
  for (const auto& p : snap.ports) {
    if (core::contains(core::to_lower(p.name), q) || core::contains(core::to_lower(p.signature), q)) ports.push_back(&p);
    if (ports.size() >= 200) break;
  }
  ImGui::Spacing();
  if (ui::section(fmt::format("{} ({})###sb", tr("BLOCKS"), blocks.size()).c_str())) {
    for (const auto* b : blocks) {
      ImGui::PushID(b->id.c_str());
      if (ImGui::Selectable("##r", req_.selected == b->id, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2.1f))) go_to(m, b->id);
      ImVec2 mn = ImGui::GetItemRectMin();
      auto* dl = ImGui::GetWindowDrawList();
      ImVec4 ic = b->kind == "group" ? kFolderColor : color_of(m, b->id);
      dl->AddText(ImVec2(mn.x + 2, mn.y), ImGui::GetColorU32(ic), icon(kind_icon(b->kind)));
      float tx = mn.x + ImGui::CalcTextSize(icon(Icon::Folder)).x + 8;
      dl->AddText(ImVec2(tx, mn.y), ImGui::GetColorU32(ImGuiCol_Text), b->id.c_str());
      dl->AddText(ImVec2(tx, mn.y + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled), b->purpose.c_str());
      ImGui::PopID();
    }
  }
  if (ui::section(fmt::format("{} ({})###sp", tr("PORTS"), ports.size()).c_str())) {
    for (const auto* p : ports) {
      std::string label = fmt::format("{}  {}:{}", icon(p->dir == "fn" ? Icon::Play : Icon::Dot), p->block, p->name);
      if (ImGui::Selectable((label + "##" + p->block + p->name).c_str())) go_to(m, p->block);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s", p->dir.c_str(), p->signature.c_str());
    }
  }
}

void Ui::side_changes(const Model& m) {
  if (m.changes.empty()) {
    ui::muted("%s", tr("no proposed changes"));
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ui::muted("%s", tr("Agents and karkas import propose schema changes here; nothing is written before you approve."));
    ImGui::PopTextWrapPos();
    return;
  }
  // Pending changes always show; decided ones can be hidden (display only, the files stay).
  std::vector<const core::Change*> rows;
  int pending = 0;
  int hidden = 0;
  for (auto it = m.changes.rbegin(); it != m.changes.rend(); ++it) {  // newest first
    const auto& c = *it;
    bool decided = c.status.value != "pending";
    pending += decided ? 0 : 1;
    if (decided && cleared_.changes.contains(c.id)) {
      ++hidden;
      continue;
    }
    rows.push_back(&c);
  }
  ui::muted("%s", fmt::format(fmt::runtime(tr("{} pending, {} decided")), pending, static_cast<int>(m.changes.size()) - pending).c_str());
  int decided_shown = static_cast<int>(rows.size()) - pending;
  float right = (decided_shown > 0 ? ImGui::GetFrameHeight() * 1.4f : 0) + (hidden > 0 ? ImGui::GetFrameHeight() * 1.4f : 0);
  ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8, ImGui::GetContentRegionMax().x - right));
  if (decided_shown > 0 && ui::icon_button("ch_hide", Icon::Trash, tr("Hide the decided changes from this list (the files stay)"))) {
    for (const auto* c : rows) {
      if (c->status.value != "pending") cleared_.changes.insert(c->id);
    }
    if (const auto* sel = [&]() -> const core::Change* {
          for (const auto& c : m.changes) {
            if (c.id == req_.change) return &c;
          }
          return nullptr;
        }();
        sel != nullptr && sel->status.value != "pending") {
      req_.change.clear();
    }
    cleared_changed();
  }
  if (hidden > 0) {
    if (decided_shown > 0) ImGui::SameLine(0, 2);
    if (ui::icon_button("ch_restore", Icon::Undo, fmt::format(fmt::runtime(tr("Show the {} hidden changes again")), hidden).c_str())) {
      cleared_.changes.clear();
      cleared_changed();
    }
  }
  const core::Change* sel = nullptr;
  for (const auto* c : rows) {
    if (c->id == req_.change) sel = c;
  }
  float avail_h = ImGui::GetContentRegionAvail().y;
  float avail_w = ImGui::GetContentRegionAvail().x;
  float& list_h = settings_.changes_list_h;
  if (list_h <= 0) list_h = avail_h * 0.35f;
  list_h = std::clamp(list_h, 70.0f, std::max(70.0f, avail_h - 140.0f));
  // The list scrolls on its own (wheel), the details below it too.
  ImGui::BeginChild("clist", ImVec2(0, sel != nullptr ? list_h : 0));
  float line = ImGui::GetTextLineHeight();
  for (const auto* cp : rows) {
    const auto& c = *cp;
    ImGui::PushID(c.id.c_str());
    bool is_sel = req_.change == c.id;
    bool decided = c.status.value != "pending";
    // A row never changes with the mouse over it: the hide button of a decided change has its
    // place reserved at the right end, and the row counts as hovered by its rectangle (also
    // while the mouse is on that button), so the button stays and takes the click.
    ImVec2 mn = ImGui::GetCursorScreenPos();
    float row_w = std::max(40.0f, ImGui::GetContentRegionAvail().x);
    float row_h = line * 2.15f;
    ImVec2 mx(mn.x + row_w, mn.y + row_h);
    float btn = ImGui::GetFrameHeight();
    float hide_w = decided ? btn + 4 : 0.0f;
    // (Also while the hide button is held: an active item would make the window "not hovered".)
    bool hot = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(mn, mx);
    bool hidden_now = false;
    bool hide_hot = false;
    ImVec2 bmin(mx.x - btn - 2, mn.y + (row_h - btn) * 0.5f);
    if (decided && hot) {
      // The button is submitted before the row so it gets the click; its X is drawn after the
      // row, over the row's highlight.
      ImGui::SetCursorScreenPos(bmin);
      if (ImGui::InvisibleButton("hide", ImVec2(btn, btn))) {
        cleared_.changes.insert(c.id);
        if (is_sel) req_.change.clear();
        cleared_changed();
        hidden_now = true;
      }
      hide_hot = ImGui::IsItemHovered();
      if (hide_hot) {
        ImGui::SetTooltip("%s", tr("Hide from this list (the file stays)"));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
      ImGui::SetCursorScreenPos(mn);
    }
    if (ImGui::Selectable("##c", is_sel, 0, ImVec2(row_w, row_h)) && !hidden_now) {
      req_.change = is_sel ? std::string() : c.id;  // a second click folds it
      change_editing_ = false;
    }
    ui::hand_on_hover();
    if (!hide_hot && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !c.reason.value.empty()) ImGui::SetTooltip("%s", c.reason.value.c_str());
    auto* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(mn, ImVec2(mx.x - hide_w, mx.y), true);
    ImVec4 sc = c.status.value == "pending" ? rgb(230, 180, 60) : c.status.value == "approved" ? rgb(115, 201, 145) : rgb(140, 140, 140);
    dl->AddText(ImVec2(mn.x + 2, mn.y + 1), ImGui::GetColorU32(sc), icon(Icon::Changes));
    float tx = mn.x + ImGui::CalcTextSize(icon(Icon::Changes)).x + 8;
    dl->AddText(ImVec2(tx, mn.y + 1), ImGui::GetColorU32(ImGuiCol_Text), c.id.c_str());
    std::string st = c.status.value == "pending" ? tr("pending") : c.status.value == "approved" ? tr("approved")
                                                                  : c.status.value == "rejected" ? tr("rejected")
                                                                                                 : c.status.value;
    // The status only where it fits beside the id (the icon has its color anyway).
    float id_w = ImGui::CalcTextSize(c.id.c_str()).x;
    ImVec2 sts = ImGui::CalcTextSize(st.c_str());
    float st_x = mx.x - hide_w - sts.x - 4;
    if (st_x > tx + id_w + 8) dl->AddText(ImVec2(st_x, mn.y + 1), ImGui::GetColorU32(sc), st.c_str());
    std::string reason = ui::ellipsize(c.reason.value, mx.x - hide_w - tx - 4);
    dl->AddText(ImVec2(tx, mn.y + line + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), reason.c_str());
    dl->PopClipRect();
    if (decided && hot) {
      if (hide_hot) dl->AddRectFilled(bmin, ImVec2(bmin.x + btn, bmin.y + btn), ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
      ImVec2 ts = ImGui::CalcTextSize(icon(Icon::Close));
      dl->AddText(ImVec2(bmin.x + (btn - ts.x) * 0.5f, bmin.y + (btn - ts.y) * 0.5f),
                  ImGui::GetColorU32(hide_hot ? ImGuiCol_Text : ImGuiCol_TextDisabled), icon(Icon::Close));
    }
    if (ImGui::BeginPopupContextItem("cctx")) {
      ImGui::TextDisabled("%s", c.id.c_str());
      ImGui::Separator();
      if (ImGui::MenuItem(with_icon(Icon::Ghost, tr("Show on the schema")).c_str())) {
        req_.change = c.id;
        ghost_on_ = true;
        ghost_shown_.clear();
      }
      if (ImGui::MenuItem(with_icon(Icon::Copy, tr("Copy")).c_str())) ImGui::SetClipboardText(core::write_change(c).c_str());
      if (c.status.value == "pending") {
        ImGui::Separator();
        if (ImGui::MenuItem(with_icon(Icon::Check, tr("Approve")).c_str())) w_.approve_change(c.id);
        if (ImGui::MenuItem(with_icon(Icon::Close, tr("Reject")).c_str())) w_.reject_change(c.id);
      } else if (ImGui::MenuItem(with_icon(Icon::Close, tr("Hide from this list (the file stays)")).c_str())) {
        cleared_.changes.insert(c.id);
        if (is_sel) req_.change.clear();
        cleared_changed();
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (sel == nullptr) return;
  ui::hsplitter("csplit", &list_h, 70.0f, 140.0f, avail_h, avail_w);
  if (ImGui::IsItemDeactivated()) settings_changed();
  const auto& c = *sel;
  if (edit_change_next_ == c.id) {
    edit_change_next_.clear();
    if (c.status.value == "pending") {
      change_editing_ = true;
      change_edit_id_ = c.id;
      change_edit_buf_.clear();
      for (const auto& d : c.diff) change_edit_buf_ += std::string(1, d.op) + " " + d.text + "\n";
    }
  }
  ImGui::BeginChild("cdetail", ImVec2(0, 0));
  // Actions.
  if (c.status.value == "pending") {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::accent(1.0f));
    if (ImGui::Button(with_icon(Icon::Check, tr("Approve")).c_str())) w_.approve_change(c.id);
    ui::hand_on_hover();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button(with_icon(Icon::Close, tr("Reject")).c_str())) w_.reject_change(c.id);
    ui::hand_on_hover();
    ImGui::SameLine();
  }
  if (ui::icon_button("ghost", Icon::Ghost, tr("Show what the change does on the schema (ghost blocks and links)"), ghost_on_)) {
    ghost_on_ = !ghost_on_;
    ghost_shown_.clear();
  }
  if (c.status.value == "pending") {
    ImGui::SameLine(0, 2);
    if (ui::icon_button("cedit", Icon::Pencil, tr("Edit the KSL diff of this change"), change_editing_)) {
      change_editing_ = !change_editing_;
      if (change_editing_) {
        change_edit_id_ = c.id;
        change_edit_buf_.clear();
        for (const auto& d : c.diff) change_edit_buf_ += std::string(1, d.op) + " " + d.text + "\n";
      }
    }
  }
  ImGui::SameLine(0, 2);
  ui::copy_button("ccopy", core::write_change(c));
  ImGui::Spacing();
  if (change_editing_ && change_edit_id_ == c.id) {
    ui::muted("%s", tr("'~ @block <id>' selects a card, '+ line' adds, '- line' removes; checked before it is saved."));
    ImGui::PushFont(theme::mono_font());
    float h = std::max(120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.5f);
    ImGui::InputTextMultiline("##cedit", &change_edit_buf_, ImVec2(-1, h));
    ImGui::PopFont();
    if (ImGui::Button(with_icon(Icon::Check, tr("Save")).c_str())) {
      w_.edit_change(c.id, change_edit_buf_);
      change_editing_ = false;
    }
    ui::hand_on_hover();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) change_editing_ = false;
    ui::hand_on_hover();
    ImGui::EndChild();
    return;
  }
  // Why, from where, which blocks; then the diff and the cards after it.
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(c.reason.value.c_str());
  ImGui::PopTextWrapPos();
  auto from = core::split_ws(c.from.value);
  if (!from.empty()) {
    ui::muted("%s", tr("from"));
    for (const auto& part : from) {
      ImGui::SameLine(0, 6);
      if (core::is_record_id(part) && part[0] == 'T') {
        if (ImGui::SmallButton(part.c_str())) {
          req_.ticket = part;
          bottom_select_ = "tickets";
        }
        ui::hand_on_hover();
      } else {
        ImGui::TextUnformatted(part.c_str());
      }
    }
  }
  if (!m.ghost.error.empty() && m.ghost.id == c.id) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(rgb(241, 120, 120), "%s", m.ghost.error.c_str());
    ImGui::PopTextWrapPos();
  }
  if (m.ghost.id == c.id && (!m.ghost.touched.empty() || !m.ghost.blocks.empty())) {
    ui::muted("%s", tr("blocks"));
    auto chip = [&](const std::string& id, bool fresh) {
      ImGui::SameLine(0, 6);
      if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(id.c_str()).x + 20 > ImGui::GetContentRegionMax().x) ImGui::NewLine();
      ImGui::PushStyleColor(ImGuiCol_Button, fresh ? rgb(60, 140, 80, 0.5f) : rgb(255, 255, 255, 0.08f));
      if (ImGui::SmallButton(((fresh ? "+ " : "") + id).c_str()) && m.snap->block(id) != nullptr) go_to(m, id);
      ImGui::PopStyleColor();
      ui::hand_on_hover();
    };
    for (const auto& b : m.ghost.blocks) chip(b.id, true);
    for (const auto& t : m.ghost.touched) chip(t, false);
  }
  ImGui::Spacing();
  if (ui::section(tr("Diff"))) {
    std::string diff;
    for (const auto& d : c.diff) diff += std::string(1, d.op) + " " + d.text + "\n";
    ui::diff_text(diff);
  }
  if (!m.change_cards.empty() && ui::section(tr("Cards after the change"), false)) ui::diff_text(m.change_cards);
  ImGui::EndChild();
}

// --- settings window ----------------------------------------------------------------------

void Ui::draw_settings_window(const Model* m) {
  if (!show_settings_) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(vp->WorkSize.x * 0.9f, ImGui::GetFontSize() * 64), std::min(vp->WorkSize.y * 0.9f, ImGui::GetFontSize() * 44)), ImGuiCond_Appearing);
  // A window of its own: a visible frame and title bar over the IDE.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::col(theme::kSideBg));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(80, 80, 86, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(44, 44, 48, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(44, 44, 48, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
  bool open = ImGui::Begin((with_icon(Icon::Gear, tr("Settings")) + "###settings_window").c_str(), &show_settings_,
                           ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor(4);
  if (open) {
    // Soft shadow around the window.
    auto* fg = ImGui::GetBackgroundDrawList(ImGui::GetWindowViewport());
    ImVec2 a = ImGui::GetWindowPos();
    ImVec2 b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight());
    for (int i = 1; i <= 8; ++i) {
      float d = static_cast<float>(i) * 2.0f;
      fg->AddRect(ImVec2(a.x - d, a.y - d + 4), ImVec2(b.x + d, b.y + d + 4), IM_COL32(0, 0, 0, 40 - i * 4), 6 + d, 0, 2.0f);
    }
  }
  if (!open) {
    ImGui::End();
    return;
  }
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) show_settings_ = false;
  // Categories on the left, the page on the right (as in an IDE).
  struct Page {
    Icon icon;
    const char* name;
  };
  const Page pages[] = {{Icon::Eye, "Appearance"}, {Icon::Schema, "Schema"}, {Icon::Agent, "Models and API"}, {Icon::Info, "Help"}};
  float list_w = ImGui::GetFontSize() * 13;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kActivityBg));
  ImGui::BeginChild("pages", ImVec2(list_w, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::Spacing();
  for (int i = 0; i < static_cast<int>(std::size(pages)); i++) {
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
    if (ImGui::Selectable((with_icon(pages[i].icon, tr(pages[i].name)) + "##page" + std::to_string(i)).c_str(), settings_page_ == i, 0,
                          ImVec2(0, ImGui::GetFrameHeight() * 1.2f))) {
      settings_page_ = i;
    }
    ImGui::PopStyleVar();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 0);
  ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::SetWindowFontScale(1.4f);
  ImGui::TextUnformatted(tr(pages[settings_page_].name));
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Separator();
  ImGui::Spacing();
  bool changed = false;
  ImGui::PushItemWidth(ImGui::GetFontSize() * 18);
  switch (settings_page_) {
    case 0: {  // Appearance
      ui::muted("%s", tr("Language"));
      if (ImGui::BeginCombo("##lang", lang_name(settings_.lang))) {
        for (auto l : {Lang::En, Lang::Ru}) {
          if (ImGui::Selectable(lang_name(l), settings_.lang == l)) {
            settings_.lang = l;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
      ImGui::Spacing();
      ui::muted("%s", tr("Interface text size"));
      changed |= ImGui::SliderFloat("##ui_scale", &settings_.ui_scale, 0.8f, 1.6f, "%.2f");
      break;
    }
    case 1: {  // Schema
      ui::muted("%s", tr("Default block size"));
      changed |= ImGui::SliderFloat("##block_scale", &settings_.block_scale, kMinBlockScale, kMaxBlockScale, "%.2f");
      ImGui::Spacing();
      ui::muted("%s", tr("Block colors"));
      if (ImGui::RadioButton(tr("by block"), settings_.blocks_by_block)) {
        settings_.blocks_by_block = true;
        changed = true;
      }
      ImGui::SameLine();
      if (ImGui::RadioButton(tr("by status"), !settings_.blocks_by_block)) {
        settings_.blocks_by_block = false;
        changed = true;
      }
      ImGui::Spacing();
      ui::muted("%s", tr("Link colors"));
      ImGui::PushID("links");
      if (ImGui::RadioButton(tr("by block"), settings_.links_by_block)) {
        settings_.links_by_block = true;
        changed = true;
      }
      ImGui::SameLine();
      if (ImGui::RadioButton(tr("by link kind"), !settings_.links_by_block)) {
        settings_.links_by_block = false;
        changed = true;
      }
      ImGui::PopID();
      ImGui::Spacing();
      if (m != nullptr && m->snap && ImGui::Button(with_icon(Icon::Fit, tr("Reset block sizes in this view")).c_str())) save_view(*m, true);
      if (m != nullptr) {
        ImGui::Spacing();
        ui::muted(tr("Layout engine: %s"), m->engine.c_str());
      }
      break;
    }
    case 2:  // Models and API
      settings_models(m);
      break;
    default: {  // Help
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(tr("Drag the corner of a block to resize it; the text follows the size."));
      ImGui::Spacing();
      ImGui::TextUnformatted(tr("Hover a block to see only its links. Shift+click keeps a block's links lit (several "
                                "blocks can be lit); Shift+double-click opens a group in place."));
      ImGui::Spacing();
      ImGui::TextUnformatted(tr("Mouse wheel zooms the schema, the right button drags it."));
      ImGui::Spacing();
      ImGui::TextUnformatted(tr("Right click on a line removes its links; Delete removes the selected line."));
      ImGui::PopTextWrapPos();
      break;
    }
  }
  ImGui::PopItemWidth();
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() * 2.5f));
  ImGui::Separator();
  ImGui::PushTextWrapPos(0);
  ui::muted(tr("Settings are saved to %s"), settings_path().string().c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
  if (changed) settings_changed();
  ImGui::End();
}

// --- editor -----------------------------------------------------------------------------------

void Ui::open_file_tab(const std::string& file) {
  if (std::find(open_files_.begin(), open_files_.end(), file) == open_files_.end()) open_files_.push_back(file);
  select_tab_ = file;
  req_.open_file = file;
}

void Ui::draw_editor(const Model& m) {
  if (!begin_editor_window()) {
    ImGui::End();
    return;
  }
  if (!select_tab_.empty()) editor_tab_ = select_tab_;
  if (editor_tab_ != "#schema" && std::find(open_files_.begin(), open_files_.end(), editor_tab_) == open_files_.end()) editor_tab_ = "#schema";
  // What the tab shows comes first, under the tab bar: the schema over the whole work area
  // (the panels lie on it), a file in the part no panel covers.
  ImVec2 body_min(free_area_.min.x, free_area_.min.y + chrome_h_);
  ImVec2 body_size(std::max(1.0f, free_area_.w()), std::max(1.0f, free_area_.h() - chrome_h_));
  if (editor_tab_ == "#schema") {
    // An empty level explains itself, unless a prototype is drawn on it.
    if (!proto_on_ && (m.snap->blocks.empty() || (m.graph.nodes.empty() && m.req.scope == req_.scope))) {
      ImGui::SetCursorScreenPos(body_min);
      ImGui::BeginChild("##empty", body_size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
      draw_empty(m);
      ImGui::EndChild();
    } else {
      ImGui::SetCursorScreenPos(work_.min);
      draw_graph(m);
    }
  } else {
    ImGui::SetCursorScreenPos(body_min);
    ImGui::BeginChild("##file", body_size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_file_body(m, editor_tab_);
    ImGui::EndChild();
  }
  // The tab bar and the toolbar of the tab, over the schema.
  ImGui::SetCursorScreenPos(free_area_.min);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
  ImGui::BeginChild("##chrome", ImVec2(std::max(1.0f, free_area_.w()), 0), ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  // Tab bar and tab items must see the same frame padding (sizes are computed per item).
  auto begin_tab = [](const std::string& label, bool* open, ImGuiTabItemFlags flags) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 6));
    bool r = ImGui::BeginTabItem(label.c_str(), open, flags);
    ImGui::PopStyleVar();
    return r;
  };
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 6));
  bool tabs = ImGui::BeginTabBar("##editor_tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll);
  ImGui::PopStyleVar();
  if (tabs) {
    ImGuiTabItemFlags sf = select_tab_ == "#schema" ? ImGuiTabItemFlags_SetSelected : 0;
    if (begin_tab(with_icon(Icon::Schema, tr("Schema")) + "###tab_schema", nullptr, sf | ImGuiTabItemFlags_Leading)) {
      editor_tab_ = "#schema";
      ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 8, ImGui::GetCursorPosY() + 4));
      ImGui::BeginGroup();
      ImGui::PushItemWidth(-8);
      draw_schema_toolbar(m);
      ImGui::PopItemWidth();
      ImGui::EndGroup();
      ImGui::Dummy(ImVec2(1, 2));
      ImGui::EndTabItem();
    }
    auto files = open_files_;
    for (const auto& f : files) {
      bool open = true;
      ImGuiTabItemFlags ff = select_tab_ == f ? ImGuiTabItemFlags_SetSelected : 0;
      std::string label = with_icon(f.ends_with(".ksl") ? Icon::Card : Icon::Code, std::filesystem::path(f).filename().string());
      if (begin_tab(label + "###tab_" + f, &open, ff)) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", f.c_str());
        editor_tab_ = f;
        req_.open_file = f;
        draw_file_header(m, f);
        ImGui::EndTabItem();
      }
      if (!open) {
        std::erase(open_files_, f);
        editors_.erase(f);
        editor_loaded_.erase(f);
        if (req_.open_file == f) req_.open_file.clear();
        if (editor_tab_ == f) editor_tab_ = "#schema";
      }
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();
  chrome_h_ = ImGui::GetItemRectSize().y;
  // A thin shadow under the toolbar: the schema goes on beneath it.
  auto* dl = ImGui::GetWindowDrawList();
  float y = free_area_.min.y + chrome_h_;
  dl->AddRectFilledMultiColor(ImVec2(free_area_.min.x, y), ImVec2(free_area_.max.x, y + 6), IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 70),
                              IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
  select_tab_.clear();
  ImGui::End();
}

void Ui::draw_schema_toolbar(const Model& m) {
  draw_address(m);
  ImGui::Spacing();
  bool all = req_.all_levels;
  if (ui::toggle("tb_all", Icon::Layers, tr("All levels"), &all, tr("Open every group in place (wheel zooms)"))) set_all_levels(m, all);
  ImGui::SameLine(0, 4);
  ui::toggle("tb_use", Icon::Link, tr("use links"), &req_.show_use, tr("Show links to shared blocks"));
  ImGui::SameLine(0, 4);
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  ImGui::SameLine(0, 4);
  ImGui::BeginDisabled(!m.can_undo);
  if (ui::icon_button("tb_undo", Icon::Undo, tr("Undo the last edit"))) w_.undo();
  ImGui::EndDisabled();
  ImGui::SameLine(0, 4);
  if (ui::icon_button("tb_add", Icon::Plus, tr("Add a block to this level"), false, tr("Add block"))) {
    dialog_ = "add_block";
    target_ = req_.scope;
  }
  ImGui::SameLine(0, 4);
  if (ui::icon_button("tb_fit", Icon::Fit, tr("Fit to view"))) navigate_ = 1;
  ImGui::SameLine(0, 4);
  if (ui::icon_button("tb_up", Icon::ArrowUp, tr("Up one level"), false) && !req_.scope.empty()) enter(core::parent_id(req_.scope));
  ImGui::SameLine(0, 4);
  // Prototype mode (ADR 0035): sketch ghost blocks and dependencies, then give them to the architect.
  bool proto = proto_on_;
  if (ui::toggle("tb_proto", Icon::Sketch, tr("Prototype"), &proto,
                 tr("Sketch the blocks and dependencies you want as ghosts; on approval the architect builds the schema from "
                    "the idea and the sketch, keeping its main logic"))) {
    set_prototype(proto);
  }
  if (!pinned_.empty()) {
    ImGui::SameLine(0, 12);
    ImGui::TextColored(rgb(255, 230, 120), "%s", with_icon(Icon::Pin, fmt::format(fmt::runtime(tr("Highlighted: {}")), pinned_.size())).c_str());
    ImGui::SameLine(0, 4);
    if (ui::icon_button("tb_clear", Icon::Close, tr("Clear"))) pinned_.clear();
  }
  ImGui::SameLine(0, 4);
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  ImGui::SameLine(0, 4);
  draw_model_chip(m);
  if (!path_error_.empty()) {
    ImGui::SameLine(0, 12);
    ImGui::TextColored(rgb(241, 76, 76), "%s", path_error_.c_str());
  }
  draw_prototype_bar(m);
}

void Ui::draw_file_header(const Model& m, const std::string& file) {
  (void)m;
  ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 10, ImGui::GetCursorPosY() + 4));
  std::string crumbs = core::replace_all(file, "/", "  ›  ");
  ImGui::AlignTextToFramePadding();
  ui::muted("%s", crumbs.c_str());
  ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 1.6f);
  if (ui::icon_button("ext_edit", Icon::FolderOpen, tr("Open in the external editor"))) {
    auto it = editors_.find(file);
    int line = it != editors_.end() && it->second ? it->second->GetCursorPosition().mLine + 1 : 1;
    w_.open_in_editor(file, line);
  }
  ImGui::Dummy(ImVec2(1, 2));
}

void Ui::draw_file_body(const Model& m, const std::string& file) {
  auto& ed = editors_[file];
  if (!ed) {
    ed = std::make_unique<TextEditor>();
    if (!file.ends_with(".ksl")) ed->SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
    ed->SetReadOnly(true);
    ed->SetShowWhitespaces(false);
    auto pal = TextEditor::GetDarkPalette();
    pal[static_cast<int>(TextEditor::PaletteIndex::Background)] = IM_COL32(31, 31, 31, 255);
    pal[static_cast<int>(TextEditor::PaletteIndex::LineNumber)] = IM_COL32(110, 118, 129, 255);
    pal[static_cast<int>(TextEditor::PaletteIndex::CurrentLineFill)] = IM_COL32(255, 255, 255, 10);
    pal[static_cast<int>(TextEditor::PaletteIndex::CurrentLineEdge)] = IM_COL32(255, 255, 255, 20);
    ed->SetPalette(pal);
  }
  if (m.req.open_file == file && editor_loaded_[file] != m.code_text) {
    ed->SetText(m.code_text);
    editor_loaded_[file] = m.code_text;
  }
  ImGui::PushFont(theme::mono_font());
  ed->Render("##code");
  ImGui::PopFont();
}

// --- inspector ------------------------------------------------------------------------------------

// --- editing in place (ADR 0033) -------------------------------------------------------------

void Ui::inspector_card(const Model& m, const index::BlockRow& b) {
  bool current = m.req.selected == b.id;  // the model's card is this block's
  if (card_edit_block_ == b.id && current) {
    if (m.card_text != card_edit_base_) {
      if (card_saved_) {
        card_edit_block_.clear();  // applied: the card on disk is the edited one now
      } else {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(rgb(230, 160, 60), "%s", tr("The card changed on disk while you edit it."));
        ImGui::PopTextWrapPos();
        if (ui::icon_button("card_reload", Icon::Undo, tr("Drop your edits and edit the card as it is on disk now"), false, tr("Reload"))) {
          card_edit_buf_ = card_edit_base_ = m.card_text;
        }
      }
    }
  }
  if (card_edit_block_ != b.id) {
    if (ui::icon_button("card_edit", Icon::Pencil, tr("Edit the card here: checked before it is written, shown as a KSL diff first, can be undone"),
                        false, tr("Edit")) &&
        current) {
      card_edit_block_ = b.id;
      card_edit_buf_ = card_edit_base_ = m.card_text;
      card_saved_ = false;
    }
    ImGui::SameLine(0, 4);
    if (ui::icon_button("in_card2", Icon::Card, tr("Open card in editor"))) w_.open_in_editor(b.dir + "/block.ksl", 1);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
    ImGui::BeginChild("cardtext", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ui::ksl_text(m.card_text);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    return;
  }
  ImGui::PushTextWrapPos(0);
  ui::muted("%s", tr("One directive per line (purpose, kind, in, out, fn, call, use, inv, note...). The id stays: rename the block on the schema."));
  ImGui::PopTextWrapPos();
  int lines = 1;
  for (char c : card_edit_buf_) lines += c == '\n' ? 1 : 0;
  float h = std::clamp(static_cast<float>(lines + 2), 6.0f, 30.0f) * ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2;
  ImGui::PushFont(theme::mono_font());
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::col(theme::kEditorBg));
  // Typing again after a cancelled preview: the next change on disk is not this edit.
  if (ImGui::InputTextMultiline("##card_text", &card_edit_buf_, ImVec2(-1, h), ImGuiInputTextFlags_AllowTabInput)) card_saved_ = false;
  ImGui::PopStyleColor();
  ImGui::PopFont();
  bool changed = card_edit_buf_ != card_edit_base_;
  ImGui::BeginDisabled(!changed);
  ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
  if (ImGui::Button(with_icon(Icon::Check, tr("Check and save")).c_str())) {
    std::string id = b.id;
    std::string text = card_edit_buf_;
    w_.propose([id, text](const core::Schema& s) { return agents::op_replace_card(s, id, text); });
    preview_open_ = true;
    card_saved_ = true;
  }
  ImGui::PopStyleColor();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(tr("Cancel"))) card_edit_block_.clear();
}

void Ui::inspector_guarantees(const Model& m, const index::BlockRow& b) {
  std::vector<std::string> now;
  if (auto it = m.snap->invariants.find(b.id); it != m.snap->invariants.end()) now = it->second;
  bool can_edit = b.kind == "leaf" || b.kind == "shared";
  if (inv_edit_block_ == b.id && now != inv_edit_base_) {
    if (inv_saved_) {
      inv_edit_block_.clear();  // applied
    } else {
      inv_edit_base_ = now;  // changed on disk: the list as it is now is the base
    }
  }
  if (inv_edit_block_ != b.id) {
    if (now.empty()) ui::muted("%s", tr("No guarantees yet: what the block always keeps true (checked by its contract tests)."));
    for (const auto& i : now) {
      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(i.c_str());
      ImGui::PopTextWrapPos();
    }
    if (can_edit && ui::icon_button("inv_edit", Icon::Pencil, tr("Edit the guarantees of this block (shown as a KSL diff first, can be undone)"),
                                    false, tr("Edit guarantees"))) {
      inv_edit_block_ = b.id;
      inv_edit_ = inv_edit_base_ = now;
      if (inv_edit_.empty()) inv_edit_.emplace_back();
      inv_saved_ = false;
    }
    return;
  }
  int remove = -1;
  for (std::size_t i = 0; i < inv_edit_.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 1.3f);
    if (ImGui::InputTextWithHint("##inv", tr("never loses frames, push never blocks..."), &inv_edit_[i])) inv_saved_ = false;
    ImGui::SameLine(0, 4);
    if (ui::icon_button("inv_rm", Icon::Close, tr("Remove this guarantee"))) remove = static_cast<int>(i);
    ImGui::PopID();
  }
  if (remove >= 0) inv_edit_.erase(inv_edit_.begin() + remove);
  if (ui::icon_button("inv_add", Icon::Plus, tr("Add a guarantee"), false, tr("Add"))) inv_edit_.emplace_back();
  std::vector<std::string> cleaned;
  for (const auto& i : inv_edit_) {
    if (!core::trim(i).empty()) cleaned.emplace_back(core::trim(i));
  }
  ImGui::BeginDisabled(cleaned == inv_edit_base_);
  ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
  if (ImGui::Button(with_icon(Icon::Check, tr("Check and save")).c_str())) {
    std::string id = b.id;
    w_.propose([id, cleaned](const core::Schema& s) { return agents::op_set_invariants(s, id, cleaned); });
    preview_open_ = true;
    inv_saved_ = true;
  }
  ImGui::PopStyleColor();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button((tr("Cancel") + std::string("##inv")).c_str())) inv_edit_block_.clear();
}

void Ui::draw_inspector(const Model& m) {
  bool open = true;
  bool visible = begin_tile(Tile::Inspector, tr("Inspector"), &open, theme::kSideBg, ImVec2(10, 8));
  if (!open) {
    settings_.show_inspector = false;
    settings_changed();
  }
  if (!visible) {
    ImGui::End();
    return;
  }
  ui::panel_title(tr("INSPECTOR"));
  ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 1.1f);
  tile_button(Tile::Inspector);
  ImGui::Spacing();
  const auto* b = req_.selected.empty() ? nullptr : m.snap->block(req_.selected);
  if (b == nullptr) {
    // Overview of the project until a block is selected.
    const auto& snap = *m.snap;
    ImFont* big = theme::icon_font_big();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(big, big->FontSize, p0, ImGui::GetColorU32(kFolderColor), icon(Icon::Home));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + big->FontSize + 10, p0.y - 2));
    ImGui::BeginGroup();
    ImGui::TextUnformatted(snap.project.c_str());
    ui::muted("%s", root_.string().c_str());
    ImGui::EndGroup();
    ImGui::Spacing();
    std::map<std::string, int> kinds, statuses;
    for (const auto& x : snap.blocks) {
      kinds[x.kind]++;
      if (x.kind != "group") statuses[x.status]++;
    }
    if (ui::section((std::string(tr("PROJECT")) + "###proj_over").c_str())) {
      if (ImGui::BeginTable("over", 2, ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& [k, n] : kinds) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImVec4 ic = k == "group" ? kFolderColor : k == "entry" ? kEntryColor : k == "shared" ? kSharedColor : rgb(120, 170, 240);
          ImGui::TextColored(ic, "%s", icon(kind_icon(k)));
          ImGui::SameLine();
          ImGui::TextUnformatted(tr_kind(k));
          ImGui::TableNextColumn();
          ImGui::Text("%d", n);
        }
        ImGui::EndTable();
      }
    }
    if (!statuses.empty() && ui::section((std::string(tr("STATUSES")) + "###proj_st").c_str())) {
      for (const auto& [st, n] : statuses) {
        ui::status_pill(tr_status(st), status_color(st));
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("000").x);
        ImGui::Text("%d", n);
      }
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ui::muted("%s", tr("Select a block in the tree or on the schema."));
    ImGui::PopTextWrapPos();
    ImGui::End();
    return;
  }
  // Header: big kind icon in the block's color, name, id.
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImFont* big = theme::icon_font_big();
  ImVec4 ic = b->kind == "group" ? kFolderColor : b->kind == "entry" ? kEntryColor : b->kind == "shared" ? kSharedColor : color_of(m, b->id);
  dl->AddText(big, big->FontSize, p, ImGui::GetColorU32(ic), icon(kind_icon(b->kind)));
  ImGui::SetCursorScreenPos(ImVec2(p.x + big->FontSize + 10, p.y - 2));
  ImGui::BeginGroup();
  ImGui::TextUnformatted(core::last_segment(b->id).c_str());
  ui::muted("%s", b->id.c_str());
  ImGui::EndGroup();
  ImGui::Spacing();
  ui::badge(tr_kind(b->kind), with_alpha(ic, 0.25f), ic);
  ImGui::SameLine(0, 6);
  ui::status_pill(tr_status(b->status), active_blocks_.contains(b->id) ? rgb(240, 200, 30) : status_color(b->status));
  if (!b->thread.empty()) {
    ImGui::SameLine(0, 10);
    ui::muted("%s %s", tr("thread"), b->thread.c_str());
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(b->purpose.c_str());
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  if (ui::icon_button("in_schema", Icon::Schema, tr("Show on the schema"), false, tr("Schema"))) go_to(m, b->id);
  ImGui::SameLine(0, 2);
  if (ui::icon_button("in_card", Icon::Card, tr("Open card in editor"), false, tr("Card"))) w_.open_in_editor(b->dir + "/block.ksl", 1);
  ImGui::SameLine(0, 2);
  if (ui::icon_button("in_folder", Icon::Folder, tr("Open folder"), false, tr("Folder"))) w_.open_block_folder(b->id);
  ImGui::SameLine(0, 2);
  if (ui::icon_button("in_chat", Icon::Chat, tr("Write a request for this block in the chat: the changes stay inside it"), false, tr("Chat"))) {
    chat_scope_ = {ChatScope::Kind::Block, b->id};
    show_chat_ = true;
    chat_focus_ = true;
    chat_window_focus_ = true;
  }
  if (b->kind == "group") {
    ImGui::SameLine(0, 2);
    if (ui::icon_button("in_enter", Icon::Layers, tr("Open this group"), false, tr("Enter"))) enter(b->id);
  }
  ImGui::Spacing();
  ImGui::BeginChild("insp", ImVec2(0, 0), ImGuiChildFlags_None);
  inspector_model(m, b->id);
  if (ui::section((std::string(tr("CARD (KSL)")) + "###card").c_str())) inspector_card(m, *b);
  auto ports = m.snap->ports_of(b->id);
  if (!ports.empty() && ui::section(fmt::format("{} ({})###ports", tr("PORTS"), ports.size()).c_str())) {
    if (ImGui::BeginTable("ports", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      for (const auto* pr : ports) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImVec4 dc = pr->dir == "in" ? rgb(78, 201, 176) : pr->dir == "out" ? rgb(220, 182, 122) : rgb(197, 134, 192);
        ImGui::TextColored(dc, "%s", icon(pr->dir == "fn" ? Icon::Play : Icon::Dot));
        ImGui::SameLine(0, 4);
        ImGui::TextUnformatted(pr->name.c_str());
        ImGui::TableNextColumn();
        ImGui::PushFont(theme::mono_font());
        ImGui::TextDisabled("%s", pr->signature.c_str());
        ImGui::PopFont();
      }
      ImGui::EndTable();
    }
  }
  {
    auto it = m.snap->invariants.find(b->id);
    std::size_t n = it != m.snap->invariants.end() ? it->second.size() : 0;
    // Guarantees belong to leaves and shared blocks (3.8): shown (and editable) for them even when none is written yet.
    bool can_have = b->kind == "leaf" || b->kind == "shared";
    if ((n > 0 || can_have) && ui::section(fmt::format("{} ({})###inv", tr("INVARIANTS"), n).c_str())) inspector_guarantees(m, *b);
  }
  if (auto it = m.snap->history.find(b->id);
      it != m.snap->history.end() && ui::section(fmt::format("{} ({})###hist", tr("HISTORY"), it->second.size()).c_str(), false)) {
    for (const auto& [t, text] : it->second) {
      ImGui::TextColored(rgb(55, 148, 255), "%s", t.c_str());
      ImGui::SameLine();
      ImGui::TextWrapped("%s", text.c_str());
    }
  }
  if (!m.files.empty() && ui::section(fmt::format("{} ({})###files", tr("FILES"), m.files.size()).c_str())) {
    for (const auto& f : m.files) {
      std::string label = with_icon(f.ends_with(".ksl") ? Icon::Card : Icon::Code, f);
      if (ImGui::Selectable((label + "##" + f).c_str(), req_.open_file == f)) open_file_tab(f);
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) w_.open_in_editor(f, 1);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tr("Click: open here; double click: external editor"));
    }
  }
  if (ui::section((std::string(tr("MODEL VIEW (WHAT AGENTS GET)")) + "###view").c_str(), false)) {
    ImGui::PushFont(theme::mono_font());
    ImGui::TextUnformatted(m.view_text.c_str());
    ImGui::PopFont();
  }
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace kk::gui
