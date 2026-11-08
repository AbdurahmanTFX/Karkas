// The IDE shell of karkas-gui: menu, activity bar, side bar views, editor tabs, inspector,
// bottom panel and status bar. The schema itself is drawn in ui.cpp.
#include <cstdint>
#include <TextEditor.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <functional>

#include "i18n.hpp"
#include "icons.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace kk::gui {

namespace {

// A docked panel without the dock tab bar (the shell draws its own headers and tabs).
bool begin_panel(const char* name, ImU32 bg, ImVec2 padding = ImVec2(10, 8)) {
  ImGuiWindowClass wc;
  wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
  ImGui::SetNextWindowClass(&wc);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::col(bg));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
  bool open = ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  return open;
}

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

// Text of a KSL card with the directive words colored (like a code editor).
void card_text(const std::string& text) {
  ImGui::PushFont(theme::mono_font());
  for (const auto& line : core::split(text, '\n', false)) {
    if (line.empty()) {
      ImGui::NewLine();
      continue;
    }
    if (line[0] == '#') {
      ImGui::TextColored(rgb(106, 153, 85), "%s", line.c_str());
      continue;
    }
    auto sp = line.find(' ');
    std::string head = line.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : line.substr(sp);
    ImGui::TextColored(head[0] == '@' ? rgb(197, 134, 192) : rgb(86, 156, 214), "%s", head.c_str());
    if (!rest.empty()) {
      ImGui::SameLine(0, 0);
      ImGui::TextUnformatted(rest.c_str());
    }
  }
  ImGui::PopFont();
}

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
  draw_menu();
  draw_status_bar(m);
  draw_activity_bar(m);
  ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
  if (first_frame && ImGui::DockBuilderGetNode(dock) != nullptr && ImGui::DockBuilderGetNode(dock)->IsLeafNode()) {
    ImGui::DockBuilderRemoveNodeChildNodes(dock);
    ImGuiID center = dock;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.19f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGui::DockBuilderDockWindow("###side", left);
    ImGui::DockBuilderDockWindow("###Inspector", right);
    ImGui::DockBuilderDockWindow("###editor", center);
    ImGui::DockBuilderDockWindow("###panel", bottom);
    ImGui::DockBuilderFinish(dock);
  }
  draw_side_bar(m);
  draw_settings_window(m);
  if (m == nullptr) {
    if (begin_panel("###editor", theme::kEditorBg)) ui::muted("%s", mp ? mp->message.c_str() : tr("indexing..."));
    ImGui::End();
    if (begin_panel("###Inspector", theme::kSideBg)) ui::panel_title(tr("INSPECTOR"));
    ImGui::End();
    if (begin_panel("###panel", theme::kSideBg)) ImGui::TextUnformatted("");
    ImGui::End();
    return true;
  }
  draw_editor(*m);
  draw_inspector(*m);
  draw_bottom(*m);
  draw_dialogs(*m);
  return !m->active.empty();
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

void Ui::show_picker() {
  picker_ = std::make_unique<ProjectPicker>(root_.empty() ? std::filesystem::path() : root_.parent_path(), settings_.recent);
  picker_open_ = true;
}

void Ui::draw_menu() {
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) show_picker();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Comma)) show_settings_ = !show_settings_;
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
  if (ImGui::BeginMenu(tr("Go"))) {
    if (ImGui::MenuItem(with_icon(Icon::ArrowUp, tr("Up one level")).c_str(), "Alt+Up", false, !req_.scope.empty())) {
      enter(core::parent_id(req_.scope));
    }
    if (ImGui::MenuItem(with_icon(Icon::Home, tr("Project root")).c_str())) enter("");
    ImGui::EndMenu();
  }
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
      bool on = side_ == v;
      if (v == SideView::Settings) on = show_settings_;
      if (ImGui::IsItemClicked()) {
        if (v == SideView::Search && side_ != v) focus_search_ = true;
        if (v == SideView::Settings) {
          show_settings_ = !show_settings_;  // a separate window, not a side bar view
        } else {
          side_ = v;
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
      if (!m->active.empty()) {
        float a = static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0));
        if (status_item("st_agents", with_icon(Icon::Agent, fmt::format(fmt::runtime(tr("agents working: {}")), m->active.size())),
                        rgb(240, 200, 30, a))) {
          bottom_select_ = "agents";
        }
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
  if (!begin_panel("Side###side", theme::kSideBg, ImVec2(8, 8))) {
    ImGui::End();
    return;
  }
  const char* titles[] = {"EXPLORER", "SEARCH", "SCHEMA CHANGES", "SETTINGS"};
  ui::panel_title(tr(titles[static_cast<int>(side_)]));
  if (side_ == SideView::Explorer) {
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 2.2f);
    if (ui::icon_button("ex_open", Icon::FolderOpen, tr("Open project..."))) show_picker();
    ImGui::SameLine(0, 2);
    if (ui::icon_button("ex_folder", Icon::Folder, tr("Show project folder"))) w_.open_folder("");
  }
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
  for (const auto& c : m.changes) {
    ImGui::PushID(c.id.c_str());
    bool sel = req_.change == c.id;
    if (ImGui::Selectable("##c", sel, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2.1f))) req_.change = c.id;
    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    auto* dl = ImGui::GetWindowDrawList();
    ImVec4 sc = c.status.value == "pending" ? rgb(230, 180, 60) : c.status.value == "approved" ? rgb(115, 201, 145) : rgb(140, 140, 140);
    dl->AddText(ImVec2(mn.x + 2, mn.y), ImGui::GetColorU32(sc), icon(Icon::Changes));
    float tx = mn.x + ImGui::CalcTextSize(icon(Icon::Changes)).x + 8;
    dl->AddText(ImVec2(tx, mn.y), ImGui::GetColorU32(ImGuiCol_Text), c.id.c_str());
    std::string st = tr_status(c.status.value);
    if (c.status.value == "pending") st = tr("pending");
    if (c.status.value == "approved") st = tr("approved");
    if (c.status.value == "rejected") st = tr("rejected");
    ImVec2 sts = ImGui::CalcTextSize(st.c_str());
    dl->AddText(ImVec2(mx.x - sts.x - 4, mn.y), ImGui::GetColorU32(sc), st.c_str());
    dl->PushClipRect(mn, mx, true);
    dl->AddText(ImVec2(tx, mn.y + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled), c.reason.value.c_str());
    dl->PopClipRect();
    ImGui::PopID();
  }
  for (const auto& c : m.changes) {
    if (c.id != req_.change) continue;
    ImGui::Separator();
    if (c.status.value == "pending") {
      ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::accent(1.0f));
      if (ImGui::Button(with_icon(Icon::Check, tr("Approve")).c_str())) w_.approve_change(c.id);
      ImGui::PopStyleColor(2);
      ImGui::SameLine();
      if (ImGui::Button(with_icon(Icon::Close, tr("Reject")).c_str())) w_.reject_change(c.id);
    }
    ImGui::BeginChild("diff", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ui::diff_text(m.change_preview);
    ImGui::EndChild();
  }
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
  if (!begin_panel("Editor###editor", theme::kEditorBg, ImVec2(0, 0))) {
    ImGui::End();
    return;
  }
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
      ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 8, ImGui::GetCursorPosY() + 4));
      ImGui::BeginGroup();
      ImGui::PushItemWidth(-8);
      draw_schema_toolbar(m);
      ImGui::PopItemWidth();
      ImGui::EndGroup();
      if (m.snap->blocks.empty() || (m.graph.nodes.empty() && m.req.scope == req_.scope)) {
        draw_empty(m);
      } else {
        draw_graph(m);
      }
      ImGui::EndTabItem();
    }
    auto files = open_files_;
    for (const auto& f : files) {
      bool open = true;
      ImGuiTabItemFlags ff = select_tab_ == f ? ImGuiTabItemFlags_SetSelected : 0;
      std::string label = with_icon(f.ends_with(".ksl") ? Icon::Card : Icon::Code, std::filesystem::path(f).filename().string());
      if (begin_tab(label + "###tab_" + f, &open, ff)) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", f.c_str());
        req_.open_file = f;
        draw_file_tab(m, f);
        ImGui::EndTabItem();
      }
      if (!open) {
        std::erase(open_files_, f);
        editors_.erase(f);
        editor_loaded_.erase(f);
        if (req_.open_file == f) req_.open_file.clear();
      }
    }
    ImGui::EndTabBar();
  }
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
}

void Ui::draw_file_tab(const Model& m, const std::string& file) {
  ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 10, ImGui::GetCursorPosY() + 4));
  std::string crumbs = core::replace_all(file, "/", "  ›  ");
  ui::muted("%s", crumbs.c_str());
  ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 1.6f);
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
  if (ui::icon_button("ext_edit", Icon::FolderOpen, tr("Open in the external editor"))) w_.open_in_editor(file, ed->GetCursorPosition().mLine + 1);
  if (m.req.open_file == file && editor_loaded_[file] != m.code_text) {
    ed->SetText(m.code_text);
    editor_loaded_[file] = m.code_text;
  }
  ImGui::PushFont(theme::mono_font());
  ed->Render("##code");
  ImGui::PopFont();
}

// --- inspector ------------------------------------------------------------------------------------

void Ui::draw_inspector(const Model& m) {
  if (!begin_panel("Inspector###Inspector", theme::kSideBg, ImVec2(10, 8))) {
    ImGui::End();
    return;
  }
  ui::panel_title(tr("INSPECTOR"));
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
  if (b->kind == "group") {
    ImGui::SameLine(0, 2);
    if (ui::icon_button("in_enter", Icon::Layers, tr("Open this group"), false, tr("Enter"))) enter(b->id);
  }
  ImGui::Spacing();
  ImGui::BeginChild("insp", ImVec2(0, 0), ImGuiChildFlags_None);
  inspector_model(m, b->id);
  if (ui::section((std::string(tr("CARD (KSL)")) + "###card").c_str())) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
    ImGui::BeginChild("cardtext", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    card_text(m.card_text);
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }
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
  if (auto it = m.snap->invariants.find(b->id);
      it != m.snap->invariants.end() && ui::section(fmt::format("{} ({})###inv", tr("INVARIANTS"), it->second.size()).c_str())) {
    for (const auto& i : it->second) ImGui::BulletText("%s", i.c_str());
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

// --- bottom panel -----------------------------------------------------------------------------------

void Ui::draw_bottom(const Model& m) {
  if (!begin_panel("Panel###panel", theme::kSideBg, ImVec2(10, 4))) {
    ImGui::End();
    return;
  }
  int errors = 0;
  int warnings = 0;
  for (const auto& d : m.snap->drift) (d.severity == "E" ? errors : warnings)++;
  auto tab = [&](const char* key, Icon ic, const std::string& text) {
    ImGuiTabItemFlags f = bottom_select_ == key ? ImGuiTabItemFlags_SetSelected : 0;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
    bool r = ImGui::BeginTabItem((with_icon(ic, text) + "###" + key).c_str(), nullptr, f);
    ImGui::PopStyleVar();
    return r;
  };
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 5));
  bool tabs = ImGui::BeginTabBar("##panel_tabs");
  ImGui::PopStyleVar();
  if (tabs) {
    if (tab("tickets", Icon::Ticket, fmt::format("{} {}", tr("Tickets"), m.snap->tickets.size()))) {
      if (m.snap->tickets.empty()) {
        ui::muted("%s", tr("no tickets yet: karkas task / karkas debug create them"));
      } else if (ImGui::BeginTable("t", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable,
                            ImVec2(ImGui::GetContentRegionAvail().x * 0.48f, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"id", "kind", "status", "title"}) ImGui::TableSetupColumn(tr(h));
        ImGui::TableHeadersRow();
        for (const auto& t : m.snap->tickets) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (ImGui::Selectable(t.id.c_str(), req_.ticket == t.id, ImGuiSelectableFlags_SpanAllColumns)) req_.ticket = t.id;
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(t.kind.c_str());
          ImGui::TableNextColumn();
          ImGui::TextColored(t.status == "closed" ? rgb(115, 201, 145) : rgb(230, 180, 60), "%s", t.status.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(t.title.c_str());
        }
        ImGui::EndTable();
        ImGui::SameLine();
        ImGui::BeginChild("ticket", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (m.ticket_text.empty()) {
          ui::muted("%s", tr("(select a ticket)"));
        } else {
          card_text(m.ticket_text);
        }
        ImGui::EndChild();
      }
      ImGui::EndTabItem();
    }
    if (tab("agents", Icon::Agent, m.active.empty() ? std::string(tr("Agents")) : fmt::format("{} {}", tr("Agents"), m.active.size()))) {
      for (const auto& a : m.active) {
        status_dot("", true, pulse_);
        ImGui::Text("%s  %s  (%s)  %s", a.role.c_str(), a.block.c_str(), a.model.c_str(), a.ticket.c_str());
      }
      if (m.active.empty()) ui::muted("%s", tr("no agent is working now"));
      ImGui::Spacing();
      if (ImGui::BeginTable("runs", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"role", "block", "tier", "outcome", "tokens in/out", "cache read", "usd"}) ImGui::TableSetupColumn(tr(h));
        ImGui::TableHeadersRow();
        for (auto it = m.runs.rbegin(); it != m.runs.rend() && it - m.runs.rbegin() < 50; ++it) {
          ImGui::TableNextRow();
          for (const auto& cell : {it->role, it->block, it->tier, it->outcome, fmt::format("{}/{}", it->input_tokens, it->output_tokens),
                                   std::to_string(it->cache_read_tokens), fmt::format("{:.4f}", it->cost_usd)}) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cell.c_str());
          }
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }
    if (tab("build", Icon::Hammer, tr("Build & tests"))) {
      if (m.snap->tests.empty()) ui::muted("%s", tr("no test results yet: karkas test"));
      if (ImGui::BeginTable("tests", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {"block", "contract", "unit", "integration", "failures"}) ImGui::TableSetupColumn(tr(h));
        ImGui::TableHeadersRow();
        for (const auto& t : m.snap->tests) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextColored(t.fresh ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : rgb(140, 140, 140), "%s%s%s", t.block.c_str(),
                             t.fresh ? "" : " ", t.fresh ? "" : tr("(stale)"));
          for (const auto& c : {t.contract, t.unit, t.integration}) {
            ImGui::TableNextColumn();
            if (c.total > 0) {
              ImGui::TextColored(c.pass == c.total ? rgb(115, 201, 145) : rgb(241, 76, 76), "%s %d/%d", icon(c.pass == c.total ? Icon::Check : Icon::Close),
                                 c.pass, c.total);
            }
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(t.build_failed ? tr("build failed") : (t.failures.empty() ? "" : t.failures.front().c_str()));
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }
    if (tab("costs", Icon::Coins, tr("Costs"))) {
      double total = 0;
      std::int64_t in = 0, cr = 0, cw = 0;
      std::map<std::string, double> by_block, by_model, by_ticket;
      for (const auto& r : m.runs) {
        total += r.cost_usd;
        in += r.input_tokens;
        cr += r.cache_read_tokens;
        cw += r.cache_write_tokens;
        by_block[r.block] += r.cost_usd;
        by_model[r.model] += r.cost_usd;
        if (!r.ticket.empty()) by_ticket[r.ticket] += r.cost_usd;
      }
      double share = in + cr + cw > 0 ? 100.0 * static_cast<double>(cr) / static_cast<double>(in + cr + cw) : 0.0;
      ImGui::Text(tr("total $%.4f · %zu runs · cache read share %.1f%%"), total, m.runs.size(), share);
      ImGui::Spacing();
      if (ImGui::BeginTable("costs", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        for (const auto& [name, v] : {std::pair{"by block", &by_block}, std::pair{"by model", &by_model}, std::pair{"by ticket", &by_ticket}}) {
          ImGui::TableNextColumn();
          ui::panel_title(tr(name));
          for (const auto& [k, c] : *v) {
            ImGui::TextUnformatted(k.c_str());
            ImGui::SameLine(ImGui::GetColumnWidth() - ImGui::CalcTextSize("$0.0000").x - 4);
            ImGui::Text("$%.4f", c);
          }
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }
    std::string drift_label = m.snap->drift.empty() ? std::string(tr("Drift")) : fmt::format("{} {}·{}", tr("Drift"), errors, warnings);
    if (tab("drift", Icon::Wave, drift_label)) {
      if (m.snap->drift.empty()) ui::muted("%s", tr("no drift"));
      for (std::size_t i = 0; i < m.snap->drift.size(); ++i) {
        const auto& d = m.snap->drift[i];
        bool err = d.severity == "E";
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextColored(err ? rgb(241, 76, 76) : rgb(230, 180, 60), "%s", icon(err ? Icon::Error : Icon::Warning));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", d.code.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(d.message.c_str());
        if (!d.block.empty()) {
          ImGui::SameLine();
          ImGui::TextDisabled("%s", d.block.c_str());
        }
        if (!d.file.empty()) {
          ImGui::SameLine();
          std::string where = d.line > 0 ? fmt::format("{}:{}", d.file, d.line) : d.file;
          if (ImGui::TextLink(where.c_str())) w_.open_in_editor(d.file, d.line);
        }
        ImGui::PopID();
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  bottom_select_.clear();
  ImGui::End();
}

}  // namespace kk::gui
