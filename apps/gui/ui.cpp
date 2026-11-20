#include "ui.hpp"

#include <cstdint>
#include <TextEditor.h>

#include <fmt/format.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <imgui_internal.h>

#include <cfloat>
#include <imgui_node_editor.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <tuple>

#include "i18n.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "ui_common.hpp"
#include "widgets.hpp"
#include "metrics.hpp"
#include "kk/agents/edit.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/layout/layout.hpp"

namespace ed = ax::NodeEditor;

namespace kk::gui {

namespace {

std::uintptr_t hid(std::string_view s) {
  auto h = static_cast<std::uintptr_t>(core::fnv1a64(s) & 0x7fffffffffffULL);
  return h == 0 ? 1 : h;
}

std::string pin_key(const std::string& block, const std::string& port, std::string_view side) {
  return fmt::format("pin:{}:{}:{}", block, port, side);
}


// Longest prefix of `s` (whole UTF-8 characters) that fits `width` with "…".
std::string fit_uncached(const std::string& s, float width);

// Cached: the same label at the same width (and font size) is fitted once, not every frame.
std::string fit(const std::string& s, float width) {
  static std::unordered_map<std::string, std::string> cache;
  if (cache.size() > 50000) cache.clear();
  std::string key = s;
  key += '\x1f';
  key += std::to_string(static_cast<int>(width * 2.0f));
  key += '\x1f';
  key += std::to_string(static_cast<int>(ImGui::GetFontSize() * 10.0f));
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  return cache.emplace(std::move(key), fit_uncached(s, width)).first->second;
}

std::string fit_uncached(const std::string& s, float width) {
  if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
  const std::string ell = "…";
  if (ImGui::CalcTextSize(ell.c_str()).x > width) return {};
  std::size_t lo = 0;
  std::size_t hi = s.size();
  while (lo < hi) {
    std::size_t mid = (lo + hi + 1) / 2;
    while (mid > 0 && (static_cast<unsigned char>(s[mid]) & 0xC0) == 0x80) --mid;  // UTF-8 boundary
    if (mid <= lo) break;
    if (ImGui::CalcTextSize((s.substr(0, mid) + ell).c_str()).x <= width) {
      lo = mid;
    } else {
      hi = mid - 1;
      while (hi > lo && (static_cast<unsigned char>(s[hi]) & 0xC0) == 0x80) --hi;
    }
  }
  return s.substr(0, lo) + ell;
}

// Text at an absolute position (inside a node), clipped to `width`.
void text_at(float x, float y, const std::string& s, float width, bool disabled = false) {
  auto t = fit(s, width);
  if (t.empty()) return;
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  if (disabled) {
    ImGui::TextDisabled("%s", t.c_str());
  } else {
    ImGui::TextUnformatted(t.c_str());
  }
}

constexpr float kPadX = 6.0f;
constexpr float kPadY = 4.0f;
constexpr float kGrip = 11.0f;

}  // namespace

Ui::Ui(Worker& w, GuiSettings& settings, bool persist, std::function<void()> wake)
    : w_(w), settings_(settings), persist_(persist), wake_(std::move(wake)) {
  ed::Config cfg;
  cfg.SettingsName = nullptr;  // positions and sizes live in .karkas/layout (14.2), not imgui.ini
  ed_ = ed::CreateEditor(&cfg);
  ed::SetCurrentEditor(ed_);
  auto& st = ed::GetStyle();
  st.NodeRounding = 7.0f;
  st.NodeBorderWidth = 1.0f;
  st.HoveredNodeBorderWidth = 2.0f;
  st.SelectedNodeBorderWidth = 2.0f;
  st.PinRounding = 4.0f;
  st.LinkStrength = 120.0f;
  st.Colors[ed::StyleColor_Bg] = ImColor(30, 30, 30);
  st.Colors[ed::StyleColor_Grid] = ImColor(255, 255, 255, 12);
  st.Colors[ed::StyleColor_HovNodeBorder] = theme::accent(0.75f);
  st.Colors[ed::StyleColor_SelNodeBorder] = theme::accent(1.0f);
  st.Colors[ed::StyleColor_HovLinkBorder] = theme::accent(0.6f);
  st.Colors[ed::StyleColor_SelLinkBorder] = theme::accent(1.0f);
  st.Colors[ed::StyleColor_NodeSelRect] = theme::accent(0.15f);
  st.Colors[ed::StyleColor_NodeSelRectBorder] = theme::accent(0.6f);
  ed::SetCurrentEditor(nullptr);
  req_.block_scale = settings_.block_scale;
}

Ui::~Ui() {
  // Notes typed but not saved yet (the pause before the autosave had not passed): written now, here —
  // the worker may stop before it would take a job (ADR 0038).
  for (const auto& [file, at] : note_edited_at_) {
    (void)at;
    if (auto it = editors_.find(file); it != editors_.end() && it->second && !root_.empty()) {
      (void)core::write_file(root_ / file, it->second->GetText());
    }
  }
  ed::DestroyEditor(ed_);
}

void Ui::enter(const std::string& scope) {
  req_.scope = scope == "root" ? "" : scope;
  applied_layout_.clear();
  pinned_.clear();
  req_.expanded.clear();
  req_.all_levels = false;
  // Nothing chosen on the new level yet: the selection is the block the level shows, at the top the
  // whole project (callers that open a level to show a block select it after this).
  select(req_.scope);
}

void Ui::select(const std::string& id) { req_.selected = id; }

void Ui::propose_unlink(std::vector<LinkDesc> links) {
  if (links.empty()) return;
  if (notes_) {  // a line between notes is [[links]] in a text: they become plain text (ADR 0038)
    for (const auto& l : links) {
      if (l.kind == "ref") w_.unlink_notes(l.src_block, l.dst_block);
    }
    return;
  }
  w_.propose([links](const core::Schema& s) -> kk::Result<agents::EditOp> {
    agents::EditOp all;
    for (const auto& l : links) {
      auto op = agents::op_disconnect(s, l.kind, l.src_block, l.src_port, l.dst_block, l.dst_port);
      if (!op) return std::unexpected(op.error());
      all.diff.insert(all.diff.end(), op->diff.begin(), op->diff.end());
      all.title = op->title;
    }
    if (links.size() > 1) {
      all.title = fmt::format("remove {} {} links {} -> {}", links.size(), links.front().kind, links.front().src_block, links.front().dst_block);
    }
    return all;
  });
  preview_open_ = true;
}

void Ui::set_all_levels(const Model& m, bool on) {
  (void)m;
  req_.all_levels = on;
  req_.expanded.clear();
  fit_on_apply_ = true;
}

void Ui::settings_changed() {
  set_lang(settings_.lang);
  ImGui::GetIO().FontGlobalScale = settings_.ui_scale;
  req_.block_scale = settings_.block_scale;
  if (persist_) save_settings(settings_);
}

std::pair<float, float> Ui::base_size(const Model& m, const std::string& id) const {
  const auto* b = m.snap->block(id);
  if (b == nullptr) return {kMinBlockW, kMinBlockH};
  // Port rows (pin_rows of metrics.hpp, from the per-model map instead of a scan of every port).
  int left = 0;
  int right = 0;
  if (b->kind != "group") {
    for (const auto* p : ports_of(m, id)) (p->dir == "out" ? right : left)++;
  }
  auto [w, h] = base_block_size(b->id, b->kind, b->purpose, left, right, req_.char_w, req_.line_h);
  return {static_cast<float>(w), static_cast<float>(h)};
}

std::pair<float, float> Ui::node_size(const Model& m, const std::string& id) const {
  if (resizing_ == id) return live_size_;
  if (auto it = sizes_.find(id); it != sizes_.end()) {
    if (it->second.first > 0) return it->second;
    auto [bw, bh] = base_size(m, id);
    return {std::max(bw * settings_.block_scale, kMinBlockW), std::max(bh * settings_.block_scale, kMinBlockH)};
  }
  if (node_serial_ != m.serial || node_index_.size() != m.graph.nodes.size()) {
    node_index_.clear();
    for (std::size_t i = 0; i < m.graph.nodes.size(); ++i) node_index_[m.graph.nodes[i].id] = i;
    node_serial_ = m.serial;
  }
  if (auto it = node_index_.find(id); it != node_index_.end()) {
    const auto& n = m.graph.nodes[it->second];
    return {static_cast<float>(n.w), static_cast<float>(n.h)};
  }
  return base_size(m, id);
}

const std::vector<const index::PortRow*>& Ui::ports_of(const Model& m, const std::string& id) const {
  if (ports_serial_ != m.serial) {
    ports_cache_.clear();
    for (const auto& p : m.snap->ports) ports_cache_[p.block].push_back(&p);
    ports_serial_ = m.serial;
  }
  static const std::vector<const index::PortRow*> none;
  auto it = ports_cache_.find(id);
  return it == ports_cache_.end() ? none : it->second;
}

void Ui::save_view(const Model& m, bool reset_sizes, const std::string& reset_one) {
  std::map<std::string, layout::Pos> out;
  ed::SetCurrentEditor(ed_);
  for (const auto& n : m.graph.nodes) {
    auto p = ed::GetNodePosition(hid("node:" + n.id));
    layout::Pos pos{p.x, p.y};
    bool manual = false;
    if (auto it = sizes_.find(n.id); it != sizes_.end()) {
      manual = it->second.first > 0;
      if (manual) {
        pos.w = it->second.first;
        pos.h = it->second.second;
      }
    } else if (auto mt = m.manual.find(n.id); mt != m.manual.end() && mt->second.w > 0) {
      manual = true;
      pos.w = mt->second.w;
      pos.h = mt->second.h;
    }
    if (reset_sizes || n.id == reset_one || !manual) {
      pos.w = 0;
      pos.h = 0;
      if (reset_sizes || n.id == reset_one) sizes_[n.id] = {0, 0};
    }
    out[n.id] = pos;
    placed_[n.id] = {p.x, p.y};
  }
  ed::SetCurrentEditor(nullptr);
  sizes_serial_ = m.serial;
  w_.save_positions(m.req.scope.empty() ? "root" : m.req.scope, out);
}

// --- toolbar with breadcrumbs ---------------------------------------------------------------

// Address bar like in a file manager: "project / group / group". A click on a part goes
// there; a click on the empty space turns the bar into a text field for a typed path.
void Ui::draw_address(const Model& m) {
  std::string project = m.snap->project.empty() ? "root" : m.snap->project;
  float w = ImGui::GetContentRegionAvail().x;
  if (editing_path_) {
    ImGui::SetNextItemWidth(w);
    if (focus_path_) {
      ImGui::SetKeyboardFocusHere();
      focus_path_ = false;
    }
    bool go = ImGui::InputText("##path", path_buf_, sizeof path_buf_,
                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (go) {
      navigate_path(m, path_buf_);
      editing_path_ = false;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsItemDeactivated() && !focus_path_)) {
      editing_path_ = false;
    }
    return;
  }
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetFrameHeight();
  ImGui::InvisibleButton("##address", ImVec2(w, h));
  bool clicked = ImGui::IsItemClicked();
  bool bar_hovered = ImGui::IsItemHovered();
  auto* dl = ImGui::GetWindowDrawList();
  const auto& st = ImGui::GetStyle();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bar_hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                    st.FrameRounding);
  std::vector<std::pair<std::string, std::string>> parts = {{project, ""}};  // label, scope
  std::string acc;
  for (const auto& part : core::split(req_.scope, '.')) {
    acc = acc.empty() ? part : acc + "." + part;
    parts.emplace_back(part, acc);
  }
  float x = p.x + st.FramePadding.x;
  float ty = p.y + st.FramePadding.y;
  bool consumed = false;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto& [label, target] = parts[i];
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    ImVec2 a(x - 3, p.y + 2);
    ImVec2 b(x + ts.x + 3, p.y + h - 2);
    bool hot = ImGui::IsMouseHoveringRect(a, b);
    bool last = i + 1 == parts.size();
    if (hot) dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_HeaderHovered), st.FrameRounding);
    dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(ImGuiCol_Text, last ? 1.0f : 0.8f), label.c_str());
    if (hot && clicked) {
      enter(target);
      consumed = true;
    }
    x += ts.x;
    if (!last) {
      const char* sep = " / ";
      dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), sep);
      x += ImGui::CalcTextSize(sep).x;
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !consumed) ImGui::SetTooltip("%s", tr("Click a part of the path to go there, or the empty space to type a path (Enter to go)."));
  if (clicked && !consumed) {
    std::string text = project;
    for (std::size_t i = 1; i < parts.size(); ++i) text += "/" + parts[i].first;
    std::snprintf(path_buf_, sizeof path_buf_, "%s", text.c_str());
    editing_path_ = true;
    focus_path_ = true;
  }
}

void Ui::navigate_path(const Model& m, const std::string& text) {
  auto parts = core::split(core::replace_all(std::string(core::trim(text)), ".", "/"), '/');
  if (!parts.empty() && parts.front() == m.snap->project) parts.erase(parts.begin());
  std::string id = core::join(parts, ".");
  path_error_.clear();
  if (id.empty()) {
    enter("");
    return;
  }
  const auto* b = m.snap->block(id);
  if (b == nullptr) {
    path_error_ = fmt::format(fmt::runtime(tr("no block {}")), id);
  } else if (b->kind == "group") {
    enter(id);
  } else {
    enter(core::parent_id(id));
    select(id);
  }
}

// --- schema of the current level -----------------------------------------------------------------

namespace {
// A bend of a line is kept in the frame of the centers A, B of its two blocks: u along A→B, v
// across it, both in lengths of AB. Moving a block moves, turns and scales the curve with it; the
// side a line leaves on (which follows the bend) does not move the bend.
ImVec2 bend_point(ImVec2 a, ImVec2 b, ImVec2 r) {
  ImVec2 d(b.x - a.x, b.y - a.y);
  return ImVec2(a.x + d.x * r.x - d.y * r.y, a.y + d.y * r.x + d.x * r.y);
}

ImVec2 bend_rel(ImVec2 a, ImVec2 b, ImVec2 p) {
  ImVec2 d(b.x - a.x, b.y - a.y);
  float l2 = d.x * d.x + d.y * d.y;
  if (l2 < 1e-3f) return ImVec2(0.5f, 0.0f);
  ImVec2 r(p.x - a.x, p.y - a.y);
  return ImVec2((r.x * d.x + r.y * d.y) / l2, (r.y * d.x - r.x * d.y) / l2);
}

float segment_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
  ImVec2 ab(b.x - a.x, b.y - a.y);
  float l2 = ab.x * ab.x + ab.y * ab.y;
  float t = l2 > 0 ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
  return std::hypot(p.x - (a.x + ab.x * t), p.y - (a.y + ab.y * t));
}

// A line to draw: data/event port to port, call/use/state between block points (bundled).
struct Planned {
  std::string kind, a, b;              // units (or "ghost:<id>")
  std::string src_port, dst_port;       // data/event: ports at the leaf ends
  bool port_a = false, port_b = false;  // the end is a port row (else the block point)
  char side_a = 'R', side_b = 'L';
  std::vector<const index::LinkRow*> rows;  // the links it shows (bundles: several)
  const GhostLink* ghost_link = nullptr;    // a link the change adds or removes
  // Computed once per plan: pin keys of both ends and the id of the line.
  std::uintptr_t from_port = 0, from_block = 0, to_port = 0, to_fn = 0, to_block = 0;
  std::uintptr_t lid = 0;
  std::string key;  // the line across levels: kind, its two blocks (and ports) — for its bends
};
// A port row of a block, made once per plan.
struct PortRow {
  std::uintptr_t pid = 0;
  std::string label, dir, shape, pin_of;
  bool used = false, added = false, removed = false;
  int col = 0;  // 0 in, 1 out (the block's color), 2 fn
};
// The lines of the level and where they attach: rebuilt only when the model, a block's place
// or size, the ghost of a change or "use links" change — not every frame (ADR 0021).
struct PlanCache {
  std::size_t sig = 0;
  std::vector<Planned> planned;
  std::unordered_map<std::string, std::pair<int, int>> port_votes;  // "block:port:dir" → (left, right)
  std::unordered_map<std::string, std::map<char, std::set<std::string>>> in_points, out_points;
  std::unordered_set<std::string> event_ports;  // "block:port" with event lines (diamond sockets)
  std::unordered_map<std::string, std::pair<std::vector<PortRow>, std::vector<PortRow>>> rows;  // block → left, right
};
PlanCache g_plan;

// Per model: the unit (visible block) of each block, and the ports that have links.
struct ModelCache {
  std::uint64_t serial = ~0ULL;
  const void* snap = nullptr;
  std::unordered_set<std::string> units;
  std::unordered_map<std::string, std::string> unit_of;
  std::uint64_t used_serial = ~0ULL;
  const void* used_snap = nullptr;
  bool used_show_use = false;
  std::unordered_set<std::string> port_used;  // "block:port"
};
ModelCache g_model;

// KARKAS_GUI_PROFILE=1: time of the parts of draw_graph, printed every 120 frames.
struct GraphProfile {
  bool on = std::getenv("KARKAS_GUI_PROFILE") != nullptr;
  std::chrono::steady_clock::time_point t;
  double sum[10] = {};
  int frames = 0;
  void start() {
    if (on) t = std::chrono::steady_clock::now();
  }
  void mark(int i) {
    if (!on) return;
    auto now = std::chrono::steady_clock::now();
    sum[i] += std::chrono::duration<double, std::milli>(now - t).count();
    t = now;
  }
  void frame_done() {
    if (!on || ++frames < 120) return;
    static const char* names[] = {"prep", "frames", "plan", "blocks", "lines", "arrows", "menus", "end", "after", "-"};
    std::fprintf(stderr, "karkas-gui: draw_graph ms/frame:");
    for (int i = 0; i < 9; ++i) std::fprintf(stderr, " %s %.2f", names[i], sum[i] / frames);
    std::fprintf(stderr, "\n");
    std::fill(std::begin(sum), std::end(sum), 0.0);
    frames = 0;
  }
};
GraphProfile g_prof;
}  // namespace

void Ui::draw_graph(const Model& m) {
  g_prof.start();
  {
    // The canvas spans the whole work area; the toolbar (a child window) and the panels lie on
    // it, so only the editor window itself counts as the canvas under the mouse.
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    canvas_min_ = a;
    canvas_max_ = ImVec2(a.x + avail.x, a.y + avail.y);
    canvas_hovered_ = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, canvas_max_);
  }
  ed::SetCurrentEditor(ed_);
  ed::Begin("schema");
  // The editor starts on its underlay channel: over the grid, under lines and blocks. Lines the
  // user bent are drawn there, so they lie like the editor's own lines.
  const int underlay_channel = ImGui::GetWindowDrawList()->_Splitter._Current;
  const auto& snap = *m.snap;
  const std::string& scope = m.req.scope;  // the level this model describes
  std::map<std::uintptr_t, std::string> node_of, pin_of;  // id → block / "block:port:side"
  std::unordered_set<std::uintptr_t> shown_pins;             // pins drawn this frame
  std::string view = scope.empty() ? "root" : scope;
  // Visible blocks ("units") and the unit that shows a given block (itself or an ancestor).
  // Per model: a block's unit is looked up once (walking its parents), then found in a map.
  auto& units = g_model.units;
  auto& unit_cache = g_model.unit_of;
  if (g_model.serial != m.serial || g_model.snap != m.snap.get() || units.size() != m.graph.nodes.size()) {
    units.clear();
    unit_cache.clear();
    for (const auto& n : m.graph.nodes) units.insert(n.id);
    g_model.serial = m.serial;
    g_model.snap = m.snap.get();
  }
  auto unit_of = [&](const std::string& id) -> const std::string& {
    if (auto it = unit_cache.find(id); it != unit_cache.end()) return it->second;
    std::string u;
    for (std::string x = id; !x.empty(); x = core::parent_id(x)) {
      if (units.contains(x)) {
        u = x;
        break;
      }
    }
    return unit_cache.emplace(id, std::move(u)).first->second;
  };
  // Highlight: the hovered block (schema or tree) and the Shift+clicked ones keep their links
  // and partners lit; everything else is dimmed.
  std::set<std::string> focus;
  for (const auto& p : pinned_) {
    if (units.contains(p)) focus.insert(p);
  }
  if (!hover_unit_.empty() && units.contains(hover_unit_)) focus.insert(hover_unit_);
  if (auto t = unit_of(tree_hover_); !t.empty()) focus.insert(t);
  if (!hover_frame_.empty()) {
    for (const auto& u : units) {
      if (u.starts_with(hover_frame_ + ".")) focus.insert(u);
    }
  }
  std::set<std::string> lit_nodes = focus;
  auto lit_pair = [&](const std::string& a, const std::string& b) { return focus.contains(a) || focus.contains(b); };
  for (const auto& l : snap.links) {
    if (l.kind == "use" && !req_.show_use) continue;
    const auto& a = unit_of(l.src_block);
    const auto& b = unit_of(l.dst_block);
    if (a.empty() || b.empty() || a == b || !lit_pair(a, b)) continue;
    lit_nodes.insert(a);
    lit_nodes.insert(b);
  }
  bool dimming = !focus.empty();
  // Ports with links (drawn filled; free ones are hollow): per model and "use links".
  auto& port_used = g_model.port_used;
  if (g_model.used_serial != m.serial || g_model.used_snap != m.snap.get() || g_model.used_show_use != req_.show_use) {
    g_model.used_serial = m.serial;
    g_model.used_snap = m.snap.get();
    g_model.used_show_use = req_.show_use;
    port_used.clear();
    for (const auto& l : snap.links) {
      if (l.kind == "use" && !req_.show_use) continue;
      const auto& a = unit_of(l.src_block);
      const auto& b = unit_of(l.dst_block);
      if (a.empty() || b.empty() || a == b) continue;
      if (l.kind == "data" || l.kind == "event") {
        if (a == l.src_block) port_used.insert(a + ":" + l.src_port);
        if (b == l.dst_block) port_used.insert(b + ":" + l.dst_port);
      } else if (l.kind == "call" && b == l.dst_block) {
        port_used.insert(b + ":" + l.dst_port);
      }
    }
  }
  g_prof.mark(0);
  bool apply_positions = applied_layout_ != m.layout_key;
  if (relayout_ && m.serial > relayout_serial_) {
    apply_positions = true;  // the model after "auto layout" has no manual positions
    relayout_ = false;
  }
  bool resize_done = false;
  bool nested = !m.nested.empty();  // groups opened in place (temporary view, not saved)
  if (apply_positions) {
    for (const auto& n : m.graph.nodes) {
      auto it = m.level.pos.find(n.id);
      if (it == m.level.pos.end()) continue;
      ed::SetNodePosition(hid("node:" + n.id), ImVec2(static_cast<float>(it->second.x), static_cast<float>(it->second.y)));
      placed_[n.id] = {static_cast<float>(it->second.x), static_cast<float>(it->second.y)};
    }
    if (fit_on_apply_) {
      navigate_ = 3;
      fit_on_apply_ = false;
    }
  }
  // Frames of opened groups: around the current rectangles of their blocks (so they follow
  // when blocks are moved); the top edge is a path bar, a click on a part closes that group.
  std::string header_hit;    // group to close if the mouse is pressed now
  std::string header_frame;  // frame whose path bar is under the mouse
  std::string frame_under;   // the innermost opened group around the mouse (a click on the empty place selects it)
  if (nested) {
    constexpr float kPad = static_cast<float>(layout::kFramePad) - 2.0f;
    constexpr float kHead = static_cast<float>(layout::kFrameTitle) - kPad;
    std::vector<const layout::Box*> frames;
    for (const auto& b : m.nested) {
      if (b.group) frames.push_back(&b);
    }
    std::stable_sort(frames.begin(), frames.end(), [](const layout::Box* a, const layout::Box* b) { return a->depth > b->depth; });
    std::map<std::string, ImRect> rects;
    for (const auto* f : frames) {
      ImRect r(ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX));
      bool any = false;
      for (const auto& n : m.graph.nodes) {
        if (!n.id.starts_with(f->id + ".")) continue;
        auto nid = hid("node:" + n.id);
        ImVec2 pos = ed::GetNodePosition(nid);
        ImVec2 size = ed::GetNodeSize(nid);
        if (size.x <= 0) size = ImVec2(static_cast<float>(n.w), static_cast<float>(n.h));
        r.Add(pos);
        r.Add(ImVec2(pos.x + size.x, pos.y + size.y));
        any = true;
      }
      for (const auto& [id, inner] : rects) {
        if (id.starts_with(f->id + ".")) r.Add(inner);
      }
      if (!any) continue;
      r.Min.x -= kPad;
      r.Max.x += kPad;
      r.Max.y += kPad;
      r.Min.y -= kPad + kHead;
      rects[f->id] = r;
    }
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 mouse = ImGui::GetMousePos();  // canvas space inside the editor
    auto inside = [&](ImVec2 a, ImVec2 b) { return mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y; };
    for (const auto* f : frames) {  // the deepest first
      auto rit = rects.find(f->id);
      if (rit != rects.end() && inside(rit->second.Min, rit->second.Max)) {
        frame_under = f->id;
        break;
      }
    }
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {  // outer frames first
      const auto* f = *it;
      auto rit = rects.find(f->id);
      if (rit == rects.end()) continue;
      const ImRect& r = rit->second;
      const auto* gb = snap.block(f->id);
      ImVec4 c = status_color(gb != nullptr ? gb->status : "design");
      bool lit = !dimming;
      for (const auto& u : lit_nodes) lit = lit || u.starts_with(f->id + ".");
      ImVec4 fill = c, edge = c, head = c;
      fill.w = lit ? 0.10f : 0.03f;
      edge.w = lit ? 0.85f : 0.25f;
      head.w = lit ? 0.55f : 0.15f;
      dl->AddRectFilled(r.Min, r.Max, ImGui::GetColorU32(fill), 8.0f);
      dl->AddRect(r.Min, r.Max, ImGui::GetColorU32(edge), 8.0f, 0, 2.0f);
      ImVec2 ha = r.Min;
      ImVec2 hb(r.Max.x, r.Min.y + kHead);
      dl->AddRectFilled(ha, hb, ImGui::GetColorU32(head), 8.0f, ImDrawFlags_RoundCornersTop);
      if (inside(ha, hb) && hover_unit_.empty()) header_frame = f->id;
      // Path from the current level: "group / subgroup"; each part closes its group.
      std::string rel = scope.empty() ? f->id : f->id.substr(scope.size() + 1);
      std::string acc = scope;
      float x = ha.x + 8;
      float ty = ha.y + (kHead - ImGui::GetFontSize()) * 0.5f;
      ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text, lit ? 1.0f : 0.35f);
      auto parts = core::split(rel, '.');
      for (std::size_t i = 0; i < parts.size(); ++i) {
        acc = acc.empty() ? parts[i] : acc + "." + parts[i];
        ImVec2 ts = ImGui::CalcTextSize(parts[i].c_str());
        ImVec2 sa(x - 3, ha.y + 2);
        ImVec2 sb(x + ts.x + 3, hb.y - 2);
        bool hot = inside(sa, sb) && hover_unit_.empty();
        if (hot) {
          dl->AddRectFilled(sa, sb, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4.0f);
          header_hit = acc;
        }
        dl->AddText(ImVec2(x, ty), text_col, parts[i].c_str());
        x += ts.x;
        if (i + 1 < parts.size()) {
          dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), " / ");
          x += ImGui::CalcTextSize(" / ").x;
        }
      }
      // "▲" on the right: close this group, back to the level above.
      const char* up = "▲";
      ImVec2 us = ImGui::CalcTextSize(up);
      ImVec2 ua(hb.x - us.x - 12, ha.y + 2);
      ImVec2 ub(hb.x - 4, hb.y - 2);
      bool up_hot = inside(ua, ub) && hover_unit_.empty();
      if (up_hot) {
        dl->AddRectFilled(ua, ub, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4.0f);
        header_hit = f->id;
      }
      dl->AddText(ImVec2(ua.x + 4, ty), text_col, up);
    }
    if (!header_hit.empty()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  std::unordered_map<std::uintptr_t, std::uintptr_t> folded_to;  // port pin → the "+N" point it folds into
  struct GhostLine {
    std::uintptr_t from, to;
    ImVec4 color;
    bool added;
  };
  std::vector<GhostLine> ghost_lines;
  g_prof.mark(1);
  // --- where lines attach (ADR 0021) ----------------------------------------------------------
  // A line leaves a block on the side that faces the other end: side by side → left/right, one
  // above the other → top/bottom. Ports keep their rows and move to the left or right column by
  // the side most of their lines go to; the block's own in/out points can sit on any side.
  struct Box {
    ImVec2 a, b;
    [[nodiscard]] ImVec2 c() const { return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f); }
  };
  std::unordered_map<std::string, Box> boxes;
  boxes.reserve(m.graph.nodes.size() * 2);
  for (const auto& n : m.graph.nodes) {
    auto nid = hid("node:" + n.id);
    ImVec2 p = ed::GetNodePosition(nid);
    ImVec2 sz = ed::GetNodeSize(nid);
    if (p.x >= FLT_MAX * 0.5f || sz.x <= 0) {
      auto it = m.level.pos.find(n.id);
      p = it != m.level.pos.end() ? ImVec2(static_cast<float>(it->second.x), static_cast<float>(it->second.y)) : ImVec2(0, 0);
      auto [w0, h0] = node_size(m, n.id);
      sz = ImVec2(w0, h0);
    }
    boxes[n.id] = Box{p, ImVec2(p.x + sz.x, p.y + sz.y)};
  }
  // Ghost blocks of the selected change (drawn as dashed nodes; not saved).
  const bool ghost = ghost_on_ && !m.req.change.empty() && m.ghost.id == m.req.change && !m.ghost.empty();
  std::vector<const GhostBlock*> ghost_nodes;
  if (ghost) {
    float right = -FLT_MAX;
    float top = FLT_MAX;
    for (const auto& [id, bx] : boxes) {
      right = std::max(right, bx.b.x);
      top = std::min(top, bx.a.y);
    }
    if (right == -FLT_MAX) {
      right = 0;
      top = 0;
    }
    float y = top;
    for (const auto& g : m.ghost.blocks) {
      // Shown at this level when its parent is the level or a group opened in place.
      std::string parent = core::parent_id(g.id);
      bool here = parent == scope || std::find(m.req.expanded.begin(), m.req.expanded.end(), parent) != m.req.expanded.end();
      if (!here) continue;
      ghost_nodes.push_back(&g);
      float gh = 70.0f + 18.0f * static_cast<float>(g.ports.size());
      boxes["ghost:" + g.id] = Box{ImVec2(right + 90, y), ImVec2(right + 90 + 220, y + gh)};
      y += gh + 30;
    }
  }
  auto side_pair = [&](const std::string& a, const std::string& b, bool rows_only) -> std::pair<char, char> {
    auto ia = boxes.find(a);
    auto ib = boxes.find(b);
    if (ia == boxes.end() || ib == boxes.end()) return {'R', 'L'};
    const Box& A = ia->second;
    const Box& B = ib->second;
    float dx = B.c().x - A.c().x;
    float dy = B.c().y - A.c().y;
    float gap_x = std::max(B.a.x - A.b.x, A.a.x - B.b.x);
    float gap_y = std::max(B.a.y - A.b.y, A.a.y - B.b.y);
    bool horizontal = rows_only || gap_x > 0 ? (rows_only || gap_y <= 0 || gap_x >= gap_y * 0.6f) : gap_y <= 0;
    if (horizontal) return dx >= 0 ? std::pair{'R', 'L'} : std::pair{'L', 'R'};
    return dy >= 0 ? std::pair{'B', 'T'} : std::pair{'T', 'B'};
  };
  // The side of a block that faces a point (a bent line leaves toward its first bend).
  auto side_toward = [&](const std::string& unit, ImVec2 p, bool rows_only) -> char {
    auto it = boxes.find(unit);
    if (it == boxes.end()) return 'R';
    const Box& bx = it->second;
    ImVec2 c = bx.c();
    float gx = std::max(bx.a.x - p.x, p.x - bx.b.x);
    float gy = std::max(bx.a.y - p.y, p.y - bx.b.y);
    if (rows_only || gx >= gy) return p.x >= c.x ? 'R' : 'L';
    return p.y >= c.y ? 'B' : 'T';
  };
  // The first and last bend of a line, placed between the centers of its two blocks.
  auto bend_ends = [&](const std::string& key, const std::string& a, const std::string& b, ImVec2* first, ImVec2* last) {
    auto it = bends_.find(key);
    auto ia = boxes.find(a);
    auto ib = boxes.find(b);
    if (it == bends_.end() || it->second.empty() || ia == boxes.end() || ib == boxes.end()) return false;
    *first = bend_point(ia->second.c(), ib->second.c(), it->second.front());
    *last = bend_point(ia->second.c(), ib->second.c(), it->second.back());
    return true;
  };
  auto ghost_unit = [&](const std::string& id) -> std::string {
    if (!ghost) return {};
    for (const auto* g : ghost_nodes) {
      if (g->id == id) return "ghost:" + g->id;
    }
    return {};
  };
  std::size_t sig = std::hash<std::uint64_t>{}(m.serial) ^ std::hash<const void*>{}(m.snap.get());
  auto mix = [&](std::size_t v) { sig ^= v + 0x9e3779b97f4a7c15ULL + (sig << 6) + (sig >> 2); };
  mix(req_.show_use ? 1 : 2);
  mix(ghost ? 3 : 4);
  mix(std::hash<std::string>{}(m.ghost.id));
  mix(static_cast<std::size_t>(bends_version_));
  for (const auto& n : m.graph.nodes) {
    const Box& bx = boxes[n.id];
    mix(static_cast<std::size_t>(static_cast<long long>(bx.a.x)));
    mix(static_cast<std::size_t>(static_cast<long long>(bx.a.y)));
    mix(static_cast<std::size_t>(static_cast<long long>(bx.b.x)));
    mix(static_cast<std::size_t>(static_cast<long long>(bx.b.y)));
  }
  auto& planned = g_plan.planned;
  auto& port_votes = g_plan.port_votes;
  auto& in_points = g_plan.in_points;
  auto& out_points = g_plan.out_points;
  bool rebuilt = sig != g_plan.sig;
  if (rebuilt) {
  g_plan.sig = sig;
  planned.clear();
  port_votes.clear();
  in_points.clear();
  out_points.clear();
  g_plan.event_ports.clear();
  g_plan.rows.clear();
  std::map<std::tuple<std::string, std::string, std::string>, std::size_t> bundle_at;
  auto add_link = [&](const std::string& kind, const std::string& a, const std::string& b, const std::string& sp, const std::string& dp,
                      bool leaf_a, bool leaf_b, const index::LinkRow* row, const GhostLink* gl) {
    if (kind == "data" || kind == "event") {
      Planned p{kind, a, b, sp, dp, leaf_a, leaf_b, 'R', 'L', {}, gl, {}, {}, {}, {}, {}, 0, {}};
      if (row != nullptr) p.rows.push_back(row);
      auto [sa, sb] = side_pair(a, b, false);
      ImVec2 first;
      ImVec2 last;
      bool bent = row != nullptr &&
                  bend_ends(kind + "|" + a + "|" + b + "|" + row->src_block + ":" + row->src_port + ">" + row->dst_block + ":" + row->dst_port, a,
                            b, &first, &last);
      if (bent) {
        sa = side_toward(a, first, false);
        sb = side_toward(b, last, false);
      }
      if (leaf_a) {
        sa = bent ? side_toward(a, first, true) : side_pair(a, b, true).first;
        auto& v = port_votes[a + ":" + sp + ":out"];
        (sa == 'L' ? v.first : v.second)++;
      }
      if (leaf_b) {
        sb = bent ? side_toward(b, last, true) : side_pair(a, b, true).second;
        auto& v = port_votes[b + ":" + dp + ":in"];
        (sb == 'L' ? v.first : v.second)++;
      }
      p.side_a = sa;
      p.side_b = sb;
      planned.push_back(std::move(p));
      return;
    }
    if (gl != nullptr) {
      auto [sa, sb] = side_pair(a, b, false);
      planned.push_back(Planned{kind, a, b, "", "", false, false, sa, sb, {}, gl, {}, {}, {}, {}, {}, 0, {}});
      return;
    }
    auto key = std::make_tuple(a, b, kind);
    auto it = bundle_at.find(key);
    if (it == bundle_at.end()) {
      auto [sa, sb] = side_pair(a, b, false);
      ImVec2 first;
      ImVec2 last;
      if (bend_ends(kind + "|" + a + "|" + b, a, b, &first, &last)) {
        sa = side_toward(a, first, false);
        sb = side_toward(b, last, false);
      }
      bundle_at[key] = planned.size();
      planned.push_back(Planned{kind, a, b, "", "", false, false, sa, sb, {row}, nullptr, {}, {}, {}, {}, {}, 0, {}});
    } else {
      planned[it->second].rows.push_back(row);
    }
  };
  for (const auto& l : snap.links) {
    if (l.kind == "use" && !req_.show_use) continue;
    const auto& a = unit_of(l.src_block);
    const auto& b = unit_of(l.dst_block);
    if (a.empty() || b.empty() || a == b) continue;
    const auto* ba = snap.block(a);
    const auto* bb = snap.block(b);
    if (ba == nullptr || bb == nullptr) continue;
    add_link(l.kind, a, b, l.src_port, l.dst_port, ba->kind != "group" && a == l.src_block, bb->kind != "group" && b == l.dst_block, &l,
             nullptr);
  }
  if (ghost) {
    for (const auto& gl : m.ghost.links) {
      if (gl.kind == "use" && !req_.show_use) continue;
      std::string a = ghost_unit(gl.src_block);
      if (a.empty()) a = unit_of(gl.src_block);
      std::string b = ghost_unit(gl.dst_block);
      if (b.empty()) b = unit_of(gl.dst_block);
      if (a.empty() || b.empty() || a == b) continue;
      bool leaf_a = a == gl.src_block || a == "ghost:" + gl.src_block;
      bool leaf_b = a.empty() ? false : (b == gl.dst_block || b == "ghost:" + gl.dst_block);
      add_link(gl.kind, a, b, gl.src_port, gl.dst_port, leaf_a && !gl.src_port.empty(), leaf_b && !gl.dst_port.empty(), nullptr, &gl);
    }
  }
  }  // rebuilt
  auto port_side = [&](const std::string& block, const std::string& port, const std::string& dir) {
    char def = dir == "out" ? 'R' : 'L';
    auto it = port_votes.find(block + ":" + port + ":" + (dir == "fn" ? "in" : dir));
    if (it == port_votes.end()) return def;
    if (it->second.first > it->second.second) return 'L';
    if (it->second.second > it->second.first) return 'R';
    return def;
  };
  auto block_key = [](const std::string& unit, bool input, char side) {
    return pin_key(unit, "", input ? "in" : "out") + "@" + std::string(1, side);
  };
  auto port_key = [](const std::string& unit, const std::string& port, const std::string& dir) { return pin_key(unit, port, dir); };
  if (rebuilt) {
    // The block points each block needs: side → kinds of lines there (shape of the socket);
    // pin keys and ids of the lines.
    for (auto& p : planned) {
      if (!p.port_a) out_points[p.a][p.side_a].insert(p.kind);
      if (!p.port_b) in_points[p.b][p.side_b].insert(p.kind);
      if (p.kind == "event") {
        if (p.port_a) g_plan.event_ports.insert(p.a + ":" + p.src_port);
        if (p.port_b) g_plan.event_ports.insert(p.b + ":" + p.dst_port);
      }
      if (p.port_a) p.from_port = hid(port_key(p.a, p.src_port, "out"));
      p.from_block = hid(block_key(p.a, false, p.side_a));
      if (p.port_b) {
        p.to_port = hid(port_key(p.b, p.dst_port, "in"));
        p.to_fn = hid(port_key(p.b, p.dst_port, "fn"));
      }
      p.to_block = hid(block_key(p.b, true, p.side_b));
      p.key = p.kind + "|" + p.a + "|" + p.b;
      if ((p.kind == "data" || p.kind == "event") && !p.rows.empty()) {
        const auto* l = p.rows.front();
        p.key += "|" + l->src_block + ":" + l->src_port + ">" + l->dst_block + ":" + l->dst_port;
      }
      if (p.ghost_link != nullptr) {
        p.lid = 1;  // not drawn by the editor
      } else if (p.kind == "data" || p.kind == "event" || p.rows.size() == 1) {
        p.lid = hid(fmt::format("link:{}", p.rows.front()->id));
      } else {
        p.lid = hid(fmt::format("bundle:{}>{}:{}", p.a, p.b, p.kind));
      }
    }
  }
  // Socket centers and directions of every pin drawn (canvas units): the flow arrows follow
  // the same curves as the editor's lines.
  struct Socket {
    ImVec2 c;
    ImVec2 dir;
  };
  std::unordered_map<std::uintptr_t, Socket> sockets;
  auto dir_of = [](char side) {
    switch (side) {
      case 'L': return ImVec2(-1, 0);
      case 'T': return ImVec2(0, -1);
      case 'B': return ImVec2(0, 1);
      default: return ImVec2(1, 0);
    }
  };

  g_prof.mark(2);
  // --- blocks ----------------------------------------------------------------------------------
  std::set<std::string> touched(m.ghost.touched.begin(), m.ghost.touched.end());
  std::unordered_set<std::string> card_units;  // blocks drawn as cards this frame (their pins are hidden)
  std::map<std::string, std::vector<const GhostPort*>> ghost_ports;  // block → ports the change adds or removes
  if (ghost) {
    for (const auto& gp : m.ghost.ports) ghost_ports[gp.block].push_back(&gp);
  }
  // One block (a real one, or a ghost of the change): `gb` is set for a ghost.
  auto draw_block = [&](const std::string& unit, const index::BlockRow* b, const GhostBlock* gb) {
    auto nid = hid("node:" + unit);
    if (b != nullptr) node_of[nid] = unit;
    std::string kind = b != nullptr ? b->kind : gb->kind;
    std::string purpose = b != nullptr ? b->purpose : gb->purpose;
    std::string status = b != nullptr ? b->status : "design";
    std::string name = core::last_segment(b != nullptr ? b->id : gb->id);
    if (notes_ && !purpose.empty()) name = purpose;  // a note or topic is called by its title (ADR 0038)
    // Size of the block and the font scale that makes its text fit (text follows size).
    float w = 0;
    float h = 0;
    float bw = 0;
    float bh = 0;
    if (b != nullptr) {
      std::tie(w, h) = node_size(m, unit);
      std::tie(bw, bh) = base_size(m, unit);
    } else {
      const auto& bx = boxes["ghost:" + gb->id];
      w = bw = bx.b.x - bx.a.x;
      h = bh = bx.b.y - bx.a.y;
    }
    float font = std::clamp(std::min(w / bw, h / bh), 0.55f, 2.5f);
    float inner_w = w - 2 * kPadX;
    float inner_h = h - 2 * kPadY;
    bool is_ghost = gb != nullptr;
    bool active = b != nullptr && active_blocks_.contains(unit);
    ImVec4 tint = is_ghost ? rgb(110, 220, 140) : settings_.blocks_by_block ? color_of(m, unit) : status_color(status);
    ImVec4 fill = ImVec4(0.12f + tint.x * 0.16f, 0.12f + tint.y * 0.16f, 0.12f + tint.z * 0.16f, is_ghost ? 0.55f : 0.96f);
    ImVec4 border = with_alpha(tint, 0.55f);
    float border_w = 1.0f;
    if (status == "failing" || status == "drift") {
      border = status_color(status);
      border_w = 2.5f;
    }
    if (status == "needs_review") {
      border = rgb(255, 140, 0);
      border_w = 3.0f;
    }
    if (active) {
      border = rgb(240, 200, 30, static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0)));
      border_w = 3.0f;
    }
    bool dim = dimming && !lit_nodes.contains(unit) && !is_ghost;
    if (pinned_.contains(unit)) {
      border = rgb(255, 230, 120);
      border_w = 3.0f;
    } else if (dimming && focus.contains(unit)) {
      border = rgb(255, 255, 255, 0.9f);
      border_w = 2.5f;
    }
    if (dim) {
      fill.w = 0.35f;
      border.w = 0.15f;
    }
    bool dashed = is_ghost || (ghost && touched.contains(unit));
    if (dashed) border.w = 0.0f;  // drawn dashed below
    ed::PushStyleColor(ed::StyleColor_NodeBg, fill);
    ed::PushStyleColor(ed::StyleColor_NodeBorder, border);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, border_w);
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(kPadX, kPadY, kPadX, kPadY));
    if (is_ghost) ed::SetNodePosition(nid, boxes["ghost:" + gb->id].a);
    ed::BeginNode(nid);
    ImVec4 text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImVec4 text_dis = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (dim) {
      text_col.w = 0.25f;
      text_dis.w = 0.18f;
    }
    if (is_ghost) text_col = rgb(170, 240, 190);
    ImGui::PushStyleColor(ImGuiCol_Text, text_col);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, text_dis);
    // Text, line spacing and gaps all follow the block size.
    ImGui::SetWindowFontScale(font);
    ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing.x * font, spacing.y * font));
    ImVec2 o = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeightWithSpacing();
    float y = o.y;
    float text_h = ImGui::GetTextLineHeight();
    float sock_r = std::max(3.0f, text_h * 0.24f);
    // Level of detail (ADR 0023): when the block's text would be smaller than ~8 px on screen
    // (a zoomed out schema, "All levels"), the block is a card with only its name, as large as
    // the block allows; zooming in brings the inside back. The pins keep their places, so the
    // lines do not move when a block switches.
    constexpr float kOverviewTextPx = 8.0f;  // line height on screen; letters are about half of it
    const bool overview = text_h * ed::GetCurrentZoom() < kOverviewTextPx;
    if (overview) card_units.insert(unit);
    float left_x = o.x - kPadX;
    float right_x = o.x + inner_w + kPadX;
    float top_y = o.y - kPadY;
    float bottom_y = o.y + inner_h + kPadY;
    // Sockets: the shape tells the kind of line — circle data, diamond event, square a
    // function others call, arrow a call (block point), open diamond use, double ring state.
    auto socket = [&](std::uintptr_t pid, ImVec2 c, char side, bool input, bool used, ImVec4 col, const std::string& shape) {
      if (overview) return;  // a card shows its name only
      auto* dl = ImGui::GetWindowDrawList();
      bool hot = pid == hover_pin_;
      float r = hot ? sock_r * 1.45f : sock_r;
      if (dim) col.w *= 0.35f;
      ImU32 c32 = ImGui::GetColorU32(col);
      ImU32 halo = ImGui::GetColorU32(ImVec4(0.09f, 0.09f, 0.09f, dim ? 0.4f : 1.0f));
      auto fill_or_line = [&](const std::vector<ImVec2>& pts) {
        dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), halo);
        std::vector<ImVec2> in = pts;
        for (auto& p : in) p = ImVec2(c.x + (p.x - c.x) * 0.75f, c.y + (p.y - c.y) * 0.75f);
        if (used) {
          dl->AddConvexPolyFilled(in.data(), static_cast<int>(in.size()), c32);
        } else {
          dl->AddPolyline(in.data(), static_cast<int>(in.size()), c32, ImDrawFlags_Closed, 1.6f);
        }
      };
      if (shape == "arrow") {
        // Points into the block for an "in" point, out of it for an "out" point.
        ImVec2 d = dir_of(side);
        if (input) d = ImVec2(-d.x, -d.y);
        ImVec2 n(-d.y, d.x);
        float th = r * 1.6f;
        fill_or_line({ImVec2(c.x + d.x * th, c.y + d.y * th), ImVec2(c.x - d.x * th * 0.7f + n.x * th, c.y - d.y * th * 0.7f + n.y * th),
                      ImVec2(c.x - d.x * th * 0.7f - n.x * th, c.y - d.y * th * 0.7f - n.y * th)});
      } else if (shape == "diamond" || shape == "use") {
        float d = r * 1.5f;
        if (shape == "use") {
          dl->AddQuadFilled(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), halo);
          dl->AddQuad(ImVec2(c.x, c.y - d * 0.75f), ImVec2(c.x + d * 0.75f, c.y), ImVec2(c.x, c.y + d * 0.75f), ImVec2(c.x - d * 0.75f, c.y), c32,
                      1.6f);
        } else {
          fill_or_line({ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y)});
        }
      } else if (shape == "square") {
        float d = r * 1.2f;
        fill_or_line({ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y - d), ImVec2(c.x + d, c.y + d), ImVec2(c.x - d, c.y + d)});
      } else if (shape == "state") {
        dl->AddCircleFilled(c, r + 2.5f, halo);
        dl->AddCircle(c, r + 1.2f, c32, 0, 1.4f);
        if (used) dl->AddCircleFilled(c, r * 0.55f, c32);
      } else {
        dl->AddCircleFilled(c, r + 1.5f, halo);
        if (used) {
          dl->AddCircleFilled(c, r, c32);
        } else {
          dl->AddCircle(c, r, c32, 0, 1.6f);
        }
      }
    };
    // A pin of the node editor at socket `c`; `side` gives the direction its lines leave in.
    auto make_pin = [&](std::uintptr_t pid, bool input, ImVec2 c, char side, ImVec2 rect_a, ImVec2 rect_b) {
      ImVec2 d = dir_of(side);
      ed::PushStyleVar(input ? ed::StyleVar_TargetDirection : ed::StyleVar_SourceDirection, d);
      ed::BeginPin(pid, input ? ed::PinKind::Input : ed::PinKind::Output);
      ed::PinRect(rect_a, rect_b);
      ed::PinPivotRect(c, c);
      ed::EndPin();
      ed::PopStyleVar();
      sockets[pid] = Socket{c, d};
    };
    // A port row: label inside the block, socket on the left or right border.
    auto pin = [&](std::uintptr_t pid, const std::string& label, float py, char side, bool input, float width, bool used, ImVec4 col,
                   const std::string& shape, bool added, bool removed) {
      shown_pins.insert(pid);
      bool left = side == 'L';
      ImVec2 c(left ? left_x : right_x, py + text_h * 0.5f);
      ImVec4 tc = added ? rgb(140, 235, 160) : removed ? rgb(241, 110, 110) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
      std::string text;
      float tw = width * 0.6f;
      float tx = left ? o.x + sock_r * 0.9f : o.x + inner_w - tw - sock_r * 0.9f;
      if (!overview) {
        text = fit((added ? "+ " : "") + label, width);
        tw = ImGui::CalcTextSize(text.c_str()).x;
        tx = left ? o.x + sock_r * 0.9f : o.x + inner_w - tw - sock_r * 0.9f;
        ImGui::SetCursorScreenPos(ImVec2(tx, py));
        ImGui::TextColored(tc, "%s", text.c_str());
      }
      if (removed && !overview) {
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(tx, py + text_h * 0.55f), ImVec2(tx + tw, py + text_h * 0.55f), ImGui::GetColorU32(tc), 1.5f);
      }
      ImVec2 a = left ? ImVec2(c.x - sock_r * 2, py) : ImVec2(tx, py);
      ImVec2 z = left ? ImVec2(tx + tw, py + text_h) : ImVec2(c.x + sock_r * 2, py + text_h);
      make_pin(pid, input, c, side, a, z);
      socket(pid, c, side, input, used, added ? rgb(110, 220, 140) : col, shape);
      return pid;
    };
    // The block's own in/out points: one per side its lines use.
    auto block_points = [&](bool input, const std::map<char, std::set<std::string>>& sides_used, ImVec4 col) {
      auto draw_point = [&](char side, const std::set<std::string>& kinds) {
        auto pid = hid(block_key(unit, input, side));
        shown_pins.insert(pid);
        // In on the left and out on the right stay on the title line; a point moved to the
        // other side sits one line lower. Top/bottom: in at 35 %, out at 65 % of the width.
        ImVec2 c;
        bool natural = (input && side == 'L') || (!input && side == 'R');
        switch (side) {
          case 'L': c = ImVec2(left_x, o.y + text_h * 0.5f + (natural ? 0.0f : lh)); break;
          case 'R': c = ImVec2(right_x, o.y + text_h * 0.5f + (natural ? 0.0f : lh)); break;
          case 'T': c = ImVec2(o.x + inner_w * (input ? 0.35f : 0.65f), top_y); break;
          default: c = ImVec2(o.x + inner_w * (input ? 0.35f : 0.65f), bottom_y); break;
        }
        float pr = sock_r * 2.2f;
        make_pin(pid, input, c, side, ImVec2(c.x - pr, c.y - pr), ImVec2(c.x + pr, c.y + pr));
        std::string shape = "arrow";
        if (!kinds.empty() && !kinds.contains("call") && !kinds.contains("data") && !kinds.contains("event")) {
          shape = kinds.contains("state") ? "state" : "use";
        }
        socket(pid, c, side, input, !kinds.empty(), col, shape);
        pin_of[pid] = unit + "::" + (input ? "in" : "out");
      };
      if (sides_used.empty()) {
        draw_point(input ? 'L' : 'R', {});
        return;
      }
      for (const auto& [side, kinds] : sides_used) draw_point(side, kinds);
    };
    // Line 1 (on a band of the block's color): kind icon, name, status dot on the right.
    Icon kind_icon = kind == "group" ? Icon::Folder : kind == "entry" ? Icon::Play : kind == "shared" ? Icon::Library : notes_ ? Icon::File : Icon::Cube;
    float dot_w = ImGui::CalcTextSize(icon(Icon::Dot)).x;
    float icon_w = ImGui::CalcTextSize(icon(kind_icon)).x + ImGui::GetStyle().ItemSpacing.x * 0.6f;
    float title_x = o.x + sock_r * 0.8f;
    if (!overview) {
      ImGui::SetCursorScreenPos(ImVec2(title_x, y));
      ImGui::TextColored(with_alpha(ImVec4(1, 1, 1, 1), dim ? 0.3f : 0.9f), "%s", icon(kind_icon));
      text_at(title_x + icon_w, y, (is_ghost ? "+ " : "") + name, inner_w - icon_w - dot_w - sock_r * 2.5f);
      if (!notes_) {  // notes have no status
        ImGui::SetCursorScreenPos(ImVec2(o.x + inner_w - dot_w - sock_r * 0.8f, y));
        ImGui::TextColored(with_alpha(active ? rgb(240, 200, 30) : status_color(status), dim ? 0.3f : 1.0f), "%s", icon(Icon::Dot));
      }
    }
    const ImVec4 in_col = rgb(205, 205, 205);
    auto ip = in_points.find(unit);
    auto op = out_points.find(unit);
    static const std::map<char, std::set<std::string>> kNone;
    if (kind != "entry" || ip != in_points.end()) block_points(true, ip != in_points.end() ? ip->second : kNone, in_col);
    if (kind != "shared" || op != out_points.end()) block_points(false, op != out_points.end() ? op->second : kNone, with_alpha(tint, 1.0f));
    float band_h = lh + kPadY * 0.5f;
    y += lh + ImGui::GetStyle().ItemSpacing.y * 0.5f;
    // Notes (ADR 0038): a note shows the first lines of its text, a topic how many notes it holds.
    if (notes_ && b != nullptr && !overview) {
      if (kind == "group") {
        if (y + lh <= o.y + inner_h) {
          text_at(o.x + sock_r * 0.6f, y, fmt::format(fmt::runtime(tr("notes: {}")), b->children.size()), inner_w - sock_r * 1.2f);
        }
      } else {
        const auto* n = note_of(m, unit);
        std::vector<std::string> lines;
        if (n != nullptr) {
          for (const auto& l : core::split(n->text, '\n')) {
            if (!core::trim(l).empty()) lines.emplace_back(core::trim(l));
            if (lines.size() >= 12) break;
          }
        }
        if (lines.empty()) lines.emplace_back(tr("(empty)"));
        for (const auto& l : lines) {
          if (y + lh > o.y + inner_h) break;
          text_at(o.x + sock_r * 0.6f, y, l, inner_w - sock_r * 1.2f, true);
          y += lh;
        }
      }
    } else if (y + lh <= o.y + inner_h) {
      // Line 2: purpose.
      if (!overview) {  // the line keeps its place: ports do not move with the zoom
        std::string p = purpose;
        if (kind == "group" && b != nullptr) p += fmt::format(" ({})", b->children.size());
        text_at(o.x + sock_r * 0.6f, y, p, inner_w - sock_r * 1.2f);
      }
      y += lh;
    }
    // Ports in two columns by the side their lines go to. Ports that do not fit fold into
    // "+N" points; lines to them attach there.
    float half = inner_w / 2 - sock_r * 1.5f;
    int rows_fit = std::max(1, static_cast<int>((o.y + inner_h - y) / lh));
    if (kind != "group") {
      auto& cached = g_plan.rows[unit];
      if (cached.first.empty() && cached.second.empty()) {
        std::set<std::string> removed_names;
        std::vector<std::tuple<std::string, std::string, bool>> port_list;  // dir, name, added
        if (b != nullptr) {
          for (const auto* p : ports_of(m, unit)) port_list.emplace_back(p->dir, p->name, false);
          if (auto gp = ghost_ports.find(unit); gp != ghost_ports.end()) {
            for (const auto* p : gp->second) {
              if (p->added) {
                port_list.emplace_back(p->dir, p->name, true);
              } else {
                removed_names.insert(p->dir + ":" + p->name);
              }
            }
          }
        } else {
          for (const auto& p : gb->ports) port_list.emplace_back(p.dir, p.name, true);
        }
        for (const auto& [dir, pname, added] : port_list) {
          PortRow r;
          r.pid = hid(port_key(unit, pname, dir));
          r.label = dir == "fn" ? pname + "()" : pname;
          r.dir = dir;
          r.shape = dir == "fn" ? "square" : g_plan.event_ports.contains(unit + ":" + pname) ? "diamond" : "circle";
          r.pin_of = unit + ":" + pname + ":" + dir;
          r.used = port_used.contains(unit + ":" + pname);
          r.added = added;
          r.removed = removed_names.contains(dir + ":" + pname);
          r.col = dir == "fn" ? 2 : dir == "out" ? 1 : 0;
          (port_side(unit, pname, dir) == 'L' ? cached.first : cached.second).push_back(std::move(r));
        }
      }
      const auto& left = cached.first;
      const auto& right = cached.second;
      const ImVec4 fn_col = rgb(197, 134, 192);
      auto row_col = [&](const PortRow& r) { return r.col == 2 ? fn_col : r.col == 1 ? with_alpha(tint, 1.0f) : in_col; };
      int need = static_cast<int>(std::max(left.size(), right.size()));
      int shown = need <= rows_fit ? need : std::max(0, rows_fit - 1);
      for (int i = 0; i < shown; ++i) {
        float ry = y + static_cast<float>(i) * lh;
        if (i < static_cast<int>(left.size())) {
          const auto& r = left[static_cast<std::size_t>(i)];
          auto pid = pin(r.pid, r.label, ry, 'L', r.dir != "out", half, r.used, row_col(r), r.shape, r.added, r.removed);
          pin_of[pid] = r.pin_of;
        }
        if (i < static_cast<int>(right.size())) {
          const auto& r = right[static_cast<std::size_t>(i)];
          auto pid = pin(r.pid, r.label, ry, 'R', r.dir != "out", half, r.used, row_col(r), r.shape, r.added, r.removed);
          pin_of[pid] = r.pin_of;
        }
      }
      if (shown < need) {
        float ry = y + static_cast<float>(shown) * lh;
        int hidden_l = std::max(0, static_cast<int>(left.size()) - shown);
        int hidden_r = std::max(0, static_cast<int>(right.size()) - shown);
        if (hidden_l > 0) {
          bool used = false;
          for (std::size_t k = static_cast<std::size_t>(shown); k < left.size(); ++k) used = used || left[k].used;
          auto pid = pin(hid(pin_key(unit, "+", "in")), fmt::format("+{}", hidden_l), ry, 'L', true, half, used, in_col, "circle", false, false);
          pin_of[pid] = unit + ":+:in";
          for (std::size_t k = static_cast<std::size_t>(shown); k < left.size(); ++k) folded_to[left[k].pid] = pid;
        }
        if (hidden_r > 0) {
          bool used = false;
          for (std::size_t k = static_cast<std::size_t>(shown); k < right.size(); ++k) used = used || right[k].used;
          auto pid = pin(hid(pin_key(unit, "+", "out")), fmt::format("+{}", hidden_r), ry, 'R', false, half, used, with_alpha(tint, 1.0f), "circle",
                         false, false);
          pin_of[pid] = unit + ":+:out";
          for (std::size_t k = static_cast<std::size_t>(shown); k < right.size(); ++k) folded_to[right[k].pid] = pid;
        }
      }
    }
    if (overview) {
      // The card: the name alone, centered, as large as the block allows.
      std::string card = (is_ghost ? "+ " : "") + name;
      ImFont* f = theme::heading_font();  // medium weight: crisp when small
      float base = f->FontSize;
      ImVec2 one = f->CalcTextSizeA(base, FLT_MAX, 0.0f, card.c_str());
      float k = std::min((inner_w + 2 * kPadX) * 0.86f / std::max(1.0f, one.x), (inner_h + 2 * kPadY) * 0.78f / std::max(1.0f, one.y));
      k = std::max(k, 0.5f);
      ImVec2 ts(one.x * k, one.y * k);
      ImVec2 c(o.x + inner_w * 0.5f, o.y + inner_h * 0.5f);
      ImVec4 col = is_ghost ? rgb(170, 240, 190) : ImVec4(1, 1, 1, 1);
      // Shown larger than the small font was rasterized: the large one keeps it sharp.
      if (base * k * ed::GetCurrentZoom() > base * 1.3f) f = theme::display_font();
      ImGui::GetWindowDrawList()->AddText(f, base * k, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                                          ImGui::GetColorU32(with_alpha(col, dim ? 0.3f : 0.95f)), card.c_str());
    }
    // The block keeps exactly its size.
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(inner_w, inner_h));
    ImGui::PopStyleVar();
    ImGui::SetWindowFontScale(1.0f);
    // Resize grip in the bottom-right corner (min/max sizes, 14.6); same size on screen at any zoom.
    if (!nested && !is_ghost && !overview) {
      float grip = std::min({kGrip / std::max(ed::GetCurrentZoom(), 0.05f), inner_w / 4, inner_h / 3});
      ImGui::SetCursorScreenPos(ImVec2(o.x + inner_w - grip, o.y + inner_h - grip));
      ImGui::InvisibleButton(("##grip" + unit).c_str(), ImVec2(grip, grip));
      bool grip_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
      if (ImGui::IsItemActivated()) {
        resizing_ = unit;
        resize_start_ = {w, h};
        auto mp = ImGui::GetMousePos();
        resize_mouse_ = {mp.x, mp.y};
        live_size_ = {w, h};
      }
      if (ImGui::IsItemActive() && resizing_ == unit) {
        auto mp = ImGui::GetMousePos();
        live_size_ = {std::clamp(resize_start_.first + mp.x - resize_mouse_.first, kMinBlockW, kMaxBlockW),
                      std::clamp(resize_start_.second + mp.y - resize_mouse_.second, kMinBlockH, kMaxBlockH)};
      }
      if (ImGui::IsItemDeactivated() && resizing_ == unit) {
        sizes_[unit] = live_size_;
        resizing_.clear();
        resize_done = true;
      }
      if (grip_hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
      auto* dl = ImGui::GetWindowDrawList();
      ImVec2 c(o.x + inner_w, o.y + inner_h);
      ImU32 gc = ImGui::GetColorU32(grip_hot ? rgb(255, 255, 255, 0.9f) : rgb(255, 255, 255, 0.35f));
      for (int k = 1; k <= 3; ++k) {
        float d = grip * 0.27f * static_cast<float>(k);
        dl->AddLine(ImVec2(c.x - d, c.y), ImVec2(c.x, c.y - d), gc, 1.2f * grip / kGrip);
      }
    }
    ImGui::PopStyleColor(2);
    ed::EndNode();
    ed::PopStyleVar(2);
    ed::PopStyleColor(2);
    // The title band behind the first line, in the block's color; a dashed border for the
    // blocks of the selected change (new: green, edited: accent).
    if (auto* bg = ed::GetNodeBackgroundDrawList(nid)) {
      ImVec2 np = ed::GetNodePosition(nid);
      ImVec2 ns = ed::GetNodeSize(nid);
      float r = ed::GetStyle().NodeRounding;
      // (A card is the band over the whole block.)
      float band_bottom = overview ? np.y + ns.y - 1 : np.y + kPadY + band_h;
      bg->AddRectFilled(ImVec2(np.x + 1, np.y + 1), ImVec2(np.x + ns.x - 1, band_bottom),
                        ImGui::GetColorU32(with_alpha(tint, dim ? 0.10f : overview ? 0.55f : 0.42f)), r,
                        overview ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop);
      if (dashed) {
        ImU32 dc = ImGui::GetColorU32(is_ghost ? rgb(110, 220, 140, 0.95f) : theme::accent(1.0f));
        float t = static_cast<float>(std::fmod(pulse_ * 18.0, 16.0));
        auto dash = [&](ImVec2 a, ImVec2 z) {
          float len = std::hypot(z.x - a.x, z.y - a.y);
          if (len <= 0) return;
          ImVec2 d((z.x - a.x) / len, (z.y - a.y) / len);
          for (float s = -t; s < len; s += 16.0f) {
            float s0 = std::max(0.0f, s);
            float s1 = std::min(len, s + 9.0f);
            if (s1 > s0) bg->AddLine(ImVec2(a.x + d.x * s0, a.y + d.y * s0), ImVec2(a.x + d.x * s1, a.y + d.y * s1), dc, 2.0f);
          }
        };
        ImVec2 p0(np.x - 2, np.y - 2);
        ImVec2 p1(np.x + ns.x + 2, np.y + ns.y + 2);
        dash(p0, ImVec2(p1.x, p0.y));
        dash(ImVec2(p1.x, p0.y), p1);
        dash(p1, ImVec2(p0.x, p1.y));
        dash(ImVec2(p0.x, p1.y), p0);
      }
    }
  };
  for (const auto& n : m.graph.nodes) {
    const auto* b = snap.block(n.id);
    if (b == nullptr) continue;
    draw_block(n.id, b, nullptr);
  }
  for (const auto* g : ghost_nodes) draw_block("ghost:" + g->id, nullptr, g);
  applied_layout_ = m.layout_key;

  g_prof.mark(3);
  // --- lines ---------------------------------------------------------------------------------
  auto resolve = [&](std::uintptr_t id) -> std::uintptr_t {
    if (id == 0) return 0;
    if (shown_pins.contains(id)) return id;
    if (auto it = folded_to.find(id); it != folded_to.end()) return it->second;
    return 0;
  };
  // Every drawn line → its plan entry; the links it shows and a bundle's label are made only
  // when they are needed (a menu, a tooltip, a delete).
  std::unordered_map<std::uintptr_t, std::size_t> line_of;
  auto links_of = [&](std::uintptr_t lid) {
    std::vector<LinkDesc> out;
    auto it = line_of.find(lid);
    if (it == line_of.end()) return out;
    for (const auto* l : planned[it->second].rows) out.push_back(LinkDesc{l->kind, l->src_block, l->src_port, l->dst_block, l->dst_port});
    return out;
  };
  auto bundle_text = [&](std::uintptr_t lid) -> std::string {
    auto it = line_of.find(lid);
    if (it == line_of.end()) return {};
    const auto& p = planned[it->second];
    if (p.rows.size() < 2) return {};
    std::vector<std::string> names;
    for (const auto* l : p.rows) {
      if (names.size() == 12) {
        names.emplace_back("…");
        break;
      }
      names.push_back(l->dst_port.empty() ? l->dst_block : l->dst_port);
    }
    return fmt::format(fmt::runtime(tr("{} → {}: {} {} links")), core::last_segment(p.a), core::last_segment(p.b), p.rows.size(), p.kind) +
           "\n" + core::join(names, ", ");
  };
  // Color: of the block the line starts at (data source, called/used block) or of the kind;
  // dimmed when another block is in focus, brighter and thicker when it belongs to the focus.
  auto style = [&](const std::string& kind, const std::string& start, const std::string& a, const std::string& b, float thick) {
    ImVec4 c = settings_.links_by_block && !start.starts_with("ghost:") ? with_alpha(color_of(m, start), kind == "use" ? 0.6f : 0.9f)
                                                                      : link_color(kind);
    if (dimming) {
      if (lit_pair(a, b)) {
        c.w = 1.0f;
        thick += 1.0f;
      } else {
        c.w = 0.07f;
      }
    }
    return std::make_pair(c, thick);
  };
  struct Drawn {
    std::uintptr_t from, to;
    ImVec4 color;
    float thick;
    bool lit;
    std::size_t pi;  // its plan entry
    bool bent;       // drawn here through the user's points, not by the editor
  };
  std::vector<Drawn> drawn;
  drawn.reserve(planned.size());
  for (std::size_t pi = 0; pi < planned.size(); ++pi) {
    const auto& p = planned[pi];
    std::uintptr_t from = 0;
    std::uintptr_t to = 0;
    if (p.port_a) from = resolve(p.from_port);
    if (from == 0) from = resolve(p.from_block);
    if (p.port_b) {
      to = resolve(p.to_port);
      if (to == 0) to = resolve(p.to_fn);
    }
    if (to == 0) to = resolve(p.to_block);
    if (from == 0 || to == 0) continue;
    if (p.ghost_link != nullptr) {
      // A line the change adds (green, dashed) or removes (red): drawn over, not selectable.
      ImVec4 c = p.ghost_link->added ? rgb(110, 220, 140, 0.95f) : rgb(241, 90, 90, 0.95f);
      ghost_lines.push_back({from, to, c, p.ghost_link->added});
      continue;
    }
    std::uintptr_t lid = p.lid;
    line_of[lid] = pi;
    const std::string& start = p.kind == "data" || p.kind == "event" ? p.a : p.b;
    float thick = 2.0f;
    if (p.kind != "data" && p.kind != "event") {
      float base = p.kind == "use" ? 1.0f : (p.kind == "state" ? 3.0f : 2.0f);
      thick = base + std::min(3.0f, std::log2(static_cast<float>(p.rows.size())));
    }
    auto [c, t] = style(p.kind, start, p.a, p.b, thick);
    if (ghost && !p.rows.empty()) {
      // A line the change removes is marked red.
      for (const auto& gl : m.ghost.links) {
        if (gl.added) continue;
        for (const auto* l : p.rows) {
          if (l->kind == gl.kind && l->src_block == gl.src_block && l->dst_block == gl.dst_block &&
              (gl.dst_port.empty() || l->dst_port == gl.dst_port)) {
            c = rgb(241, 90, 90, 0.95f);
          }
        }
      }
    }
    bool bent = bends_.contains(p.key);
    if (!bent) ed::Link(lid, from, to, c, t);
    drawn.push_back({from, to, c, t, dimming && lit_pair(p.a, p.b), pi, bent});
  }
  g_prof.mark(4);
  // Geometry of every line (canvas units): one cubic for a line the editor draws (the same curve
  // as the editor's), a smooth curve through the user's points for a bent one (ADR 0022).
  const float strength = ed::GetStyle().LinkStrength;
  auto ease = [&](ImVec2 a, ImVec2 b) {
    float dist = std::hypot(b.x - a.x, b.y - a.y);
    float half_d = dist * 0.5f;
    return half_d < strength ? strength * std::sin(IM_PI * 0.5f * half_d / strength) : strength;
  };
  using Seg = std::array<ImVec2, 4>;
  auto at = [](const ImVec2* p, float t) {
    float u = 1 - t;
    return ImVec2(u * u * u * p[0].x + 3 * u * u * t * p[1].x + 3 * u * t * t * p[2].x + t * t * t * p[3].x,
                  u * u * u * p[0].y + 3 * u * u * t * p[1].y + 3 * u * t * t * p[2].y + t * t * t * p[3].y);
  };
  // The frame of a line's bends: the centers of its two blocks.
  auto bend_frame = [&](const Drawn& d, ImVec2* ca, ImVec2* cb) {
    auto ia = boxes.find(planned[d.pi].a);
    auto ib = boxes.find(planned[d.pi].b);
    if (ia == boxes.end() || ib == boxes.end()) return false;
    *ca = ia->second.c();
    *cb = ib->second.c();
    return true;
  };
  // The points a line passes through: its two sockets and the user's bends between them.
  auto line_points = [&](const Drawn& d, std::vector<ImVec2>& q, ImVec2& da, ImVec2& db) -> bool {
    q.clear();
    auto fa = sockets.find(d.from);
    auto tb = sockets.find(d.to);
    if (fa == sockets.end() || tb == sockets.end()) return false;
    ImVec2 a = fa->second.c;
    ImVec2 b = tb->second.c;
    da = fa->second.dir;
    db = tb->second.dir;
    q.push_back(a);
    ImVec2 ca;
    ImVec2 cb;
    if (d.bent && bend_frame(d, &ca, &cb)) {
      if (auto it = bends_.find(planned[d.pi].key); it != bends_.end()) {
        for (const auto& r : it->second) q.push_back(bend_point(ca, cb, r));
      }
    }
    q.push_back(b);
    return true;
  };
  auto spline = [&](const std::vector<ImVec2>& q, ImVec2 da, ImVec2 db, std::vector<Seg>& out) {
    out.clear();
    if (q.size() == 2) {
      float e = ease(q[0], q[1]);
      out.push_back({q[0], ImVec2(q[0].x + da.x * e, q[0].y + da.y * e), ImVec2(q[1].x + db.x * e, q[1].y + db.y * e), q[1]});
      return;
    }
    // Through every point (Catmull-Rom); the ends leave and enter along their sockets.
    for (std::size_t i = 0; i + 1 < q.size(); ++i) {
      ImVec2 p0 = q[i];
      ImVec2 p3 = q[i + 1];
      float e = std::min(strength, std::hypot(p3.x - p0.x, p3.y - p0.y) * 0.5f);
      ImVec2 c1 = i == 0 ? ImVec2(p0.x + da.x * e, p0.y + da.y * e)
                         : ImVec2(p0.x + (q[i + 1].x - q[i - 1].x) / 6.0f, p0.y + (q[i + 1].y - q[i - 1].y) / 6.0f);
      ImVec2 c2 = i + 2 == q.size() ? ImVec2(p3.x + db.x * e, p3.y + db.y * e)
                                    : ImVec2(p3.x - (q[i + 2].x - q[i].x) / 6.0f, p3.y - (q[i + 2].y - q[i].y) / 6.0f);
      out.push_back({p0, c1, c2, p3});
    }
  };
  std::vector<std::vector<Seg>> geo(drawn.size());
  {
    std::vector<ImVec2> q;
    ImVec2 da;
    ImVec2 db;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
      if (line_points(drawn[i], q, da, db)) spline(q, da, db, geo[i]);
    }
  }
  // The line under the mouse (~7 px on screen) — or the one whose bend is dragged.
  const float zoom = std::max(ed::GetCurrentZoom(), 0.05f);
  const float hr = 5.0f / zoom;               // radius of a bend handle (5 px on screen)
  const ImVec2 mouse = ImGui::GetMousePos();  // canvas units inside the editor
  int hover_line = -1;
  if (!bend_drag_key_.empty()) {
    for (std::size_t i = 0; i < drawn.size(); ++i) {
      if (planned[drawn[i].pi].key == bend_drag_key_) hover_line = static_cast<int>(i);
    }
    if (hover_line < 0) {
      bend_drag_key_.clear();  // the line is gone (another model)
      bend_drag_index_ = -1;
    }
  } else if (canvas_hovered_ && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
             (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) &&
             (!ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
    // (Also in the frame of a press: the handle under it must exist to take it. Not while a block
    // is dragged or the view panned.)
    // A block covers the lines under it: over a block the block keeps the hover (its links light
    // up), and a line is taken only at one of its bends, so a bend left over a block can still be
    // moved. (Zoomed out, the lines near a small block would otherwise cover most of it.)
    bool over_block = false;
    for (const auto& [id, bx] : boxes) {
      if (mouse.x >= bx.a.x && mouse.x < bx.b.x && mouse.y >= bx.a.y && mouse.y < bx.b.y) {
        over_block = true;
        break;
      }
    }
    float best = over_block ? hr * 1.6f : 7.0f / zoom;
    for (std::size_t i = 0; over_block && i < drawn.size(); ++i) {
      ImVec2 ca;
      ImVec2 cb;
      auto it = bends_.find(planned[drawn[i].pi].key);
      if (!drawn[i].bent || it == bends_.end() || !bend_frame(drawn[i], &ca, &cb)) continue;
      for (const auto& r : it->second) {
        ImVec2 c = bend_point(ca, cb, r);
        float dd = std::hypot(mouse.x - c.x, mouse.y - c.y);
        if (dd < best) {
          best = dd;
          hover_line = static_cast<int>(i);
        }
      }
    }
    for (std::size_t i = 0; !over_block && i < drawn.size(); ++i) {
      for (const auto& sg : geo[i]) {
        float x0 = std::min({sg[0].x, sg[1].x, sg[2].x, sg[3].x}) - best;
        float x1 = std::max({sg[0].x, sg[1].x, sg[2].x, sg[3].x}) + best;
        float y0 = std::min({sg[0].y, sg[1].y, sg[2].y, sg[3].y}) - best;
        float y1 = std::max({sg[0].y, sg[1].y, sg[2].y, sg[3].y}) + best;
        if (mouse.x < x0 || mouse.x > x1 || mouse.y < y0 || mouse.y > y1) continue;
        ImVec2 prev = sg[0];
        for (int k = 1; k <= 24; ++k) {
          ImVec2 cur = at(sg.data(), static_cast<float>(k) / 24.0f);
          float dd = segment_distance(mouse, prev, cur);
          if (dd < best) {
            best = dd;
            hover_line = static_cast<int>(i);
          }
          prev = cur;
        }
      }
    }
  }
  // Bent lines lie like the editor's own: on the underlay, under the blocks.
  {
    auto* dl = ImGui::GetWindowDrawList();
    int prev_channel = dl->_Splitter._Current;
    dl->ChannelsSetCurrent(underlay_channel);
    for (std::size_t i = 0; i < drawn.size(); ++i) {
      const auto& d = drawn[i];
      if (!d.bent) continue;
      bool hot = static_cast<int>(i) == hover_line;
      if (hot) {
        for (const auto& sg : geo[i]) dl->AddBezierCubic(sg[0], sg[1], sg[2], sg[3], ImGui::GetColorU32(with_alpha(d.color, 0.25f)), d.thick + 6.0f);
      }
      ImU32 col = ImGui::GetColorU32(hot ? with_alpha(d.color, std::max(d.color.w, 0.9f)) : d.color);
      for (const auto& sg : geo[i]) dl->AddBezierCubic(sg[0], sg[1], sg[2], sg[3], col, hot ? d.thick + 1.0f : d.thick);
    }
    dl->ChannelsSetCurrent(prev_channel);
  }
  // Handles of that line: its bends (drag moves one, a double click removes it) and the middle of
  // each piece (drag bends the line there). A pressed handle holds the mouse: the editor neither
  // pans nor selects meanwhile; the same id each frame keeps the dragged one pressed.
  std::string handle_tip;
  if (hover_line >= 0) {
    const auto& d = drawn[static_cast<std::size_t>(hover_line)];
    const std::string& key = planned[d.pi].key;
    std::vector<ImVec2> q;
    ImVec2 da;
    ImVec2 db;
    ImVec2 a;
    ImVec2 b;
    if (line_points(d, q, da, db) && bend_frame(d, &a, &b)) {
      auto* dl = ImGui::GetWindowDrawList();
      const float px = 1.0f / zoom;
      ImU32 line_col = ImGui::GetColorU32(with_alpha(d.color, 1.0f));
      auto button = [&](const std::string& label, ImVec2 c) {
        ImGui::SetCursorScreenPos(ImVec2(c.x - hr * 1.6f, c.y - hr * 1.6f));
        ImGui::InvisibleButton(label.c_str(), ImVec2(hr * 3.2f, hr * 3.2f));
      };
      auto draw_bend = [&](ImVec2 c, bool hot) {
        float r = hot ? hr * 1.25f : hr;
        dl->AddCircleFilled(c, r + 1.5f * px, IM_COL32(20, 20, 20, 230));
        dl->AddCircleFilled(c, r, line_col);
        dl->AddCircle(c, r, IM_COL32(255, 255, 255, 230), 0, 1.5f * px);
      };
      if (!bend_drag_key_.empty()) {
        auto& pts = bends_[key];
        if (bend_drag_index_ >= 0 && bend_drag_index_ < static_cast<int>(pts.size())) {
          button(bend_drag_label_, bend_point(a, b, pts[static_cast<std::size_t>(bend_drag_index_)]));
          if (ImGui::IsItemActive()) {
            pts[static_cast<std::size_t>(bend_drag_index_)] = bend_rel(a, b, mouse);
            ++bends_version_;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
          } else {
            bend_drag_key_.clear();
            bend_drag_index_ = -1;
            save_bends();
          }
          if (bend_drag_index_ >= 0) draw_bend(bend_point(a, b, pts[static_cast<std::size_t>(bend_drag_index_)]), true);
        } else {
          bend_drag_key_.clear();
          bend_drag_index_ = -1;
        }
      } else {
        int remove = -1;
        if (auto it = bends_.find(key); it != bends_.end()) {
          for (std::size_t k = 0; k < it->second.size(); ++k) {
            ImVec2 c = bend_point(a, b, it->second[k]);
            std::string label = "##bend" + key + "#" + std::to_string(k);
            button(label, c);
            bool hot = ImGui::IsItemHovered();
            if (hot) {
              ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
              handle_tip = tr("Drag to move the bend; a double click removes it");
            }
            if (ImGui::IsItemActivated()) {
              if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                remove = static_cast<int>(k);
              } else {
                bend_drag_key_ = key;
                bend_drag_index_ = static_cast<int>(k);
                bend_drag_label_ = label;
              }
            }
            draw_bend(c, hot);
          }
        }
        if (remove >= 0) {
          auto& pts = bends_[key];
          pts.erase(pts.begin() + remove);
          if (pts.empty()) bends_.erase(key);
          ++bends_version_;
          save_bends();
        } else {
          const auto& segs = geo[static_cast<std::size_t>(hover_line)];
          for (std::size_t j = 0; j < segs.size(); ++j) {
            ImVec2 c = at(segs[j].data(), 0.5f);
            std::string label = "##mid" + key + "#" + std::to_string(j);
            button(label, c);
            bool hot = ImGui::IsItemHovered();
            if (hot) {
              ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
              handle_tip = tr("Drag to bend the line here");
            }
            if (ImGui::IsItemActivated() && bend_drag_key_.empty()) {
              auto& pts = bends_[key];
              pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(std::min(j, pts.size())), bend_rel(a, b, c));
              ++bends_version_;
              bend_drag_key_ = key;
              bend_drag_index_ = static_cast<int>(std::min(j, pts.size() - 1));
              bend_drag_label_ = label;
            }
            float r = hot ? hr * 1.15f : hr * 0.8f;
            dl->AddCircleFilled(c, r, IM_COL32(30, 30, 30, hot ? 240 : 190));
            dl->AddCircle(c, r, line_col, 0, 1.5f * px);
            dl->AddLine(ImVec2(c.x - r * 0.55f, c.y), ImVec2(c.x + r * 0.55f, c.y), line_col, 1.4f * px);
            dl->AddLine(ImVec2(c.x, c.y - r * 0.55f), ImVec2(c.x, c.y + r * 0.55f), line_col, 1.4f * px);
          }
        }
      }
    }
  }
  // Flow arrows along each line (where it flows), running along the lines in focus; ghost
  // lines of the selected change as dashes.
  {
    auto* dl = ImGui::GetWindowDrawList();
    auto curve = [&](std::uintptr_t from, std::uintptr_t to, ImVec2* p) {
      auto fa = sockets.find(from);
      auto tb = sockets.find(to);
      if (fa == sockets.end() || tb == sockets.end()) return false;
      ImVec2 a = fa->second.c;
      ImVec2 b = tb->second.c;
      float e = ease(a, b);
      p[0] = a;
      p[1] = ImVec2(a.x + fa->second.dir.x * e, a.y + fa->second.dir.y * e);
      p[2] = ImVec2(b.x + tb->second.dir.x * e, b.y + tb->second.dir.y * e);
      p[3] = b;
      return true;
    };
    auto chevron = [&](const ImVec2* p, float t, ImU32 col, float size) {
      ImVec2 q = at(p, t);
      ImVec2 q2 = at(p, std::min(1.0f, t + 0.02f));
      float len = std::hypot(q2.x - q.x, q2.y - q.y);
      if (len <= 0.0001f) return;
      ImVec2 dd((q2.x - q.x) / len, (q2.y - q.y) / len);
      ImVec2 n(-dd.y, dd.x);
      ImVec2 tip(q.x + dd.x * size, q.y + dd.y * size);
      dl->AddTriangleFilled(tip, ImVec2(q.x - dd.x * size * 0.6f + n.x * size * 0.75f, q.y - dd.y * size * 0.6f + n.y * size * 0.75f),
                            ImVec2(q.x - dd.x * size * 0.6f - n.x * size * 0.75f, q.y - dd.y * size * 0.6f - n.y * size * 0.75f), col);
    };
    // A point `f` (0..1) of the way along a line of several pieces.
    // Blocks by cells of a grid, to find quickly whether a point lies under one.
    constexpr float kCell = 256.0f;
    auto cell = [](float x, float y) {
      auto cx = static_cast<std::int64_t>(std::floor(x / kCell));
      auto cy = static_cast<std::int64_t>(std::floor(y / kCell));
      return (cx << 32) ^ (cy & 0xffffffffLL);
    };
    std::unordered_map<std::int64_t, std::vector<const Box*>> grid;
    for (const auto& [id, bx] : boxes) {
      for (float y = std::floor(bx.a.y / kCell) * kCell; y <= bx.b.y; y += kCell) {
        for (float x = std::floor(bx.a.x / kCell) * kCell; x <= bx.b.x; x += kCell) grid[cell(x, y)].push_back(&bx);
      }
    }
    auto under_block = [&](ImVec2 q) {
      auto it = grid.find(cell(q.x, q.y));
      if (it == grid.end()) return false;
      return std::any_of(it->second.begin(), it->second.end(),
                         [&](const Box* bx) { return q.x > bx->a.x && q.x < bx->b.x && q.y > bx->a.y && q.y < bx->b.y; });
    };
    // A point `f` (0..1) of the way along a line of several pieces; not where a block covers it
    // (lines pass under blocks, the arrows are drawn over them).
    auto chevron_on = [&](const std::vector<Seg>& g, float f, ImU32 col, float size) {
      if (g.empty()) return;
      float x = std::clamp(f, 0.0f, 0.999f) * static_cast<float>(g.size());
      auto si = static_cast<std::size_t>(x);
      float t = std::min(0.98f, x - static_cast<float>(si));
      if (under_block(at(g[si].data(), t))) return;
      chevron(g[si].data(), t, col, size);
    };
    float size = std::clamp(4.5f / zoom, 3.0f, 14.0f);
    float phase = static_cast<float>(std::fmod(pulse_ * 0.35, 1.0));
    for (std::size_t i = 0; i < drawn.size(); ++i) {
      const auto& d = drawn[i];
      if (d.color.w < 0.2f || geo[i].empty()) continue;
      ImVec4 c = d.color;
      c.w = std::min(1.0f, c.w + 0.05f);
      ImU32 col = ImGui::GetColorU32(c);
      if (d.lit) {
        for (int k = 0; k < 3; ++k) chevron_on(geo[i], std::fmod(phase + static_cast<float>(k) / 3.0f, 1.0f) * 0.9f + 0.05f, col, size * 1.2f);
      } else {
        chevron_on(geo[i], 0.5f, col, size);
      }
    }
    ImVec2 p[4];
    for (const auto& g : ghost_lines) {
      if (!curve(g.from, g.to, p)) continue;
      ImU32 col = ImGui::GetColorU32(g.color);
      ImVec2 prev = p[0];
      for (int k = 1; k <= 40; ++k) {
        ImVec2 q = at(p, static_cast<float>(k) / 40.0f);
        if (k % 2 == 1) dl->AddLine(prev, q, col, 2.5f);
        prev = q;
      }
      chevron(p, 0.5f, col, size * 1.3f);
    }
  }
  g_prof.mark(5);

  // The prototype of this level (ADR 0035): ghost blocks and dependencies the human sketches.
  if (proto_on_ && sketch_.scope == scope) {
    draw_sketch(m, underlay_channel);
  } else {
    sketch_nodes_.clear();
    sketch_pin_of_.clear();
  }

  // Draw a link from an out port or fn to a consumer (14.6).
  if (ed::BeginCreate(rgb(255, 255, 255), 2.0f)) {
    ed::PinId a;
    ed::PinId b;
    bool queried = ed::QueryNewLink(&a, &b) && a && b;
    if (queried && (sketch_pin_of_.contains(a.Get()) || sketch_pin_of_.contains(b.Get()))) {
      sketch_new_link(a.Get(), b.Get(), pin_of);  // a dependency of the prototype
    } else if (queried && notes_) {
      // Notes (ADR 0038): from a note's point to another note or topic — a [[link]] in the first note's
      // text. From an "in" point backwards: the other note links to this one.
      auto pa = core::split(pin_of[a.Get()], ':', false);
      auto pb = core::split(pin_of[b.Get()], ':', false);
      bool ok = pa.size() == 3 && pb.size() == 3 && pa[0] != pb[0];
      std::string src = ok ? pa[0] : "", dst = ok ? pb[0] : "";
      if (ok && pa[2] == "in" && pb[2] == "out") std::swap(src, dst);
      const auto* sb = snap.block(src);
      ok = ok && sb != nullptr && sb->kind != "group";
      if (!ok) {
        ed::Suspend();
        ImGui::SetTooltip("%s", tr("A link starts in a note and goes to another note or topic"));
        ed::Resume();
        ed::RejectNewItem(rgb(255, 80, 80), 2.0f);
      } else if (ed::AcceptNewItem(rgb(120, 255, 120), 3.0f)) {
        w_.link_notes(src, dst);
      }
    } else if (queried) {
      auto pa = core::split(pin_of[a.Get()], ':', false);
      auto pb = core::split(pin_of[b.Get()], ':', false);
      // Provider: an out port or a fn. Consumer: an in port; a block's "out" point (it calls the
      // fn); a block's "in" point (it gets a new in port for the data).
      auto provider = [](const std::vector<std::string>& p) { return p.size() == 3 && !p[1].empty() && p[1] != "+" && (p[2] == "out" || p[2] == "fn"); };
      auto consumer = [](const std::vector<std::string>& p) { return p.size() == 3 && p[1] != "+" && (p[2] == "in" || (p[1].empty() && p[2] == "out")); };
      if (!provider(pa)) std::swap(pa, pb);
      bool call = provider(pa) && pa[2] == "fn" && consumer(pb) && pb[1].empty() && pb[2] == "out";
      bool data = provider(pa) && pa[2] == "out" && consumer(pb) && pb[2] == "in";
      bool ok = (call || data) && pa[0] != pb[0];
      if (!ok) {
        bool block_to_block = pa.size() == 3 && pb.size() == 3 && pa[1].empty() && pb[1].empty();
        ed::Suspend();
        ImGui::SetTooltip("%s", block_to_block ? tr("Drop it on a function of the other block to add a call")
                                               : tr("These two points cannot be linked"));
        ed::Resume();
      }
      if (ok && ed::AcceptNewItem(rgb(120, 255, 120), 3.0f)) {
        std::string fa = pa[0], fp = pa[1], tb = pb[0], tp = pb[2] == "in" ? pb[1] : "";
        w_.propose([=](const core::Schema& s) { return agents::op_connect(s, fa, fp, tb, tp); });
        preview_open_ = true;
      } else if (!ok) {
        ed::RejectNewItem(rgb(255, 80, 80), 2.0f);
      }
    }
    ed::EndCreate();
  }
  if (ed::BeginDelete()) {
    ed::LinkId lid;
    while (ed::QueryDeletedLink(&lid)) {
      // Delete on a selected line removes every link it shows (after the KSL preview).
      if (ed::AcceptDeletedItem(false) && line_of.contains(lid.Get())) propose_unlink(links_of(lid.Get()));
    }
    ed::NodeId nid;
    while (ed::QueryDeletedNode(&nid)) {
      if (auto sk = sketch_nodes_.find(nid.Get()); sk != sketch_nodes_.end()) {
        if (ed::AcceptDeletedItem(true)) {  // a block of the prototype goes with its dependencies
          auto id = sk->second;
          std::erase_if(sketch_.blocks, [&](const agents::SketchBlock& b) { return sketch_.id_of(b) == id; });
          std::erase_if(sketch_.links, [&](const agents::SketchLink& l) { return l.from == id || l.to == id; });
          sketch_placed_.erase(id);
          save_prototype();
        }
        continue;
      }
      if (ed::AcceptDeletedItem(false) && node_of.contains(nid.Get())) {
        auto id = node_of[nid.Get()];
        w_.propose([=](const core::Schema& s) { return agents::op_remove_block(s, id); });
        preview_open_ = true;
      }
    }
    ed::EndDelete();
  }

  // Double click: group → enter, leaf → code (14.3).
  auto dbl = ed::GetDoubleClickedNode();
  std::string go_into;
  bool shift = ImGui::GetIO().KeyShift;
  if (dbl && sketch_nodes_.contains(dbl.Get())) {
    sketch_target_ = sketch_nodes_[dbl.Get()];  // edit the prototype block
    sketch_dialog_ = true;
  } else if (dbl && node_of.contains(dbl.Get()) && !shift) {
    const auto* b = snap.block(node_of[dbl.Get()]);
    if (b->kind == "group") {
      go_into = b->id;
    } else if (notes_) {
      select(b->id);
      open_note(m, b->id);  // a note opens its text (ADR 0038)
    } else {
      select(b->id);
      req_.open_file.clear();
    }
  }
  // A selection made elsewhere (the tree, the search, a level entered, a click on the empty place)
  // becomes the canvas's too: the block still selected on the canvas would take it back otherwise.
  if (req_.selected != canvas_selected_) {
    ed::ClearSelection();
    for (const auto& [nid, id] : node_of) {
      if (id == req_.selected) {
        ed::SelectNode(ed::NodeId(nid));
        break;
      }
    }
    canvas_selected_ = req_.selected;
  }
  std::vector<ed::NodeId> sel(8);
  int nsel = ed::GetSelectedNodes(sel.data(), static_cast<int>(sel.size()));
  if (nsel == 1 && node_of.contains(sel[0].Get())) {
    auto id = node_of[sel[0].Get()];
    if (req_.selected != id && !ImGui::IsMouseDragging(0)) {
      select(id);
      canvas_selected_ = id;
    }
  }
  ed::Suspend();
  bool menu_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
  if (auto hl = ed::GetHoveredLink(); hl && !menu_open) {
    if (auto text = bundle_text(hl.Get()); !text.empty()) ImGui::SetTooltip("%s", text.c_str());
  }
  // A bent line is not the editor's: its tooltip and menu come from here.
  const bool bent_hovered = hover_line >= 0 && drawn[static_cast<std::size_t>(hover_line)].bent;
  if (!menu_open && !handle_tip.empty()) {
    ImGui::SetTooltip("%s", handle_tip.c_str());
  } else if (!menu_open && bent_hovered) {
    if (auto text = bundle_text(planned[drawn[static_cast<std::size_t>(hover_line)].pi].lid); !text.empty()) ImGui::SetTooltip("%s", text.c_str());
  }
  hover_pin_ = 0;
  if (auto hp = ed::GetHoveredPin(); hp && pin_of.contains(hp.Get())) {
    hover_pin_ = hp.Get();
    auto parts = core::split(pin_of[hp.Get()], ':', false);
    if (parts.size() == 3 && !ImGui::IsMouseDragging(0) && !menu_open && !card_units.contains(parts[0])) {
      if (parts[1].empty() && parts[2] == "in") {
        ImGui::SetTooltip("%s", tr("In: links from blocks that call or use this one. Drop a data output here to add an input."));
      } else if (parts[1].empty() && parts[2] == "out") {
        ImGui::SetTooltip("%s", tr("Out: what this block calls or uses. Drag from here to a function of another block to add a call."));
      } else if (parts[2] == "fn") {
        ImGui::SetTooltip("%s", fmt::format(fmt::runtime(tr("Function {}(): drop a block's out point here to add a call")), parts[1]).c_str());
      }
    }
  }
  ed::NodeId ctx;
  ed::LinkId link_ctx;
  if (bent_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !menu_open) {
    const auto& p = planned[drawn[static_cast<std::size_t>(hover_line)].pi];
    menu_links_ = links_of(p.lid);
    menu_line_key_ = p.key;
    ImGui::OpenPopup("link_menu");
    suppress_bg_menu_ = 3;  // the editor saw a right click on its background there
  }
  if (suppress_bg_menu_ > 0) {
    --suppress_bg_menu_;
    (void)ed::ShowBackgroundContextMenu();
  } else if (ed::ShowNodeContextMenu(&ctx) && (node_of.contains(ctx.Get()) || sketch_nodes_.contains(ctx.Get()))) {
    if (sketch_nodes_.contains(ctx.Get())) {
      sketch_target_ = sketch_nodes_[ctx.Get()];
      ImGui::OpenPopup("sketch_menu");
    } else {
      target_ = node_of[ctx.Get()];
      ImGui::OpenPopup("node_menu");
    }
  } else if (ed::ShowLinkContextMenu(&link_ctx) && line_of.contains(link_ctx.Get())) {
    menu_links_ = links_of(link_ctx.Get());
    menu_line_key_ = planned[line_of[link_ctx.Get()]].key;
    ImGui::OpenPopup("link_menu");
  } else if (ed::ShowBackgroundContextMenu()) {
    ImGui::OpenPopup("bg_menu");
  }
  if (ImGui::BeginPopup("link_menu")) {
    // Remove one link of the line, or all of them.
    if (!menu_links_.empty()) {
      const auto& f = menu_links_.front();
      ImGui::TextDisabled("%s → %s · %s", f.src_block.c_str(), f.dst_block.c_str(), f.kind.c_str());
      ImGui::Separator();
      if (bends_.contains(menu_line_key_) && ImGui::MenuItem(with_icon(Icon::Undo, tr("Straighten the line")).c_str())) {
        bends_.erase(menu_line_key_);
        ++bends_version_;
        save_bends();
      }
      auto label = [](const LinkDesc& l) {
        if (l.kind == "data" || l.kind == "event") return fmt::format("{}:{} → {}:{}", l.src_block, l.src_port, l.dst_block, l.dst_port);
        if (l.kind == "call") return fmt::format("{}()", l.dst_port);
        return l.kind + " " + l.dst_block;
      };
      ImGui::PushStyleColor(ImGuiCol_Text, rgb(241, 110, 110));
      if (menu_links_.size() == 1) {
        if (ImGui::MenuItem(with_icon(Icon::Close, tr("Remove the link")).c_str())) propose_unlink(menu_links_);
      } else {
        for (std::size_t i = 0; i < menu_links_.size() && i < 30; ++i) {
          std::string item = with_icon(Icon::Close, fmt::format(fmt::runtime(tr("Remove {}")), label(menu_links_[i]))) + "##" + std::to_string(i);
          if (ImGui::MenuItem(item.c_str())) propose_unlink({menu_links_[i]});
        }
        ImGui::Separator();
        if (ImGui::MenuItem(with_icon(Icon::Close, fmt::format(fmt::runtime(tr("Remove all {} links")), menu_links_.size())).c_str())) {
          propose_unlink(menu_links_);
        }
      }
      ImGui::PopStyleColor();
    }
    ImGui::EndPopup();
  }
  bool reset_one = false;
  bool auto_layout = false;
  if (notes_ && ImGui::BeginPopup("node_menu")) {  // a note or a topic (ADR 0038)
    const auto* b = snap.block(target_);
    bool topic = b != nullptr && b->kind == "group";
    ImGui::TextDisabled("%s", block_title(m, target_).c_str());
    ImGui::Separator();
    if (topic && ImGui::MenuItem(with_icon(Icon::Layers, tr("Open")).c_str())) go_into = target_;
    if (!topic && b != nullptr && ImGui::MenuItem(with_icon(Icon::Pencil, tr("Edit the text")).c_str())) {
      select(target_);
      open_note(m, target_);
    }
    if (b != nullptr && ImGui::MenuItem(with_icon(Icon::Card, tr("Rename...")).c_str())) {
      note_dialog_ = "retitle";
      note_target_ = target_;
      note_title_buf_ = b->purpose;
      note_dialog_focus_ = true;
    }
    if (topic && ImGui::MenuItem(with_icon(Icon::Plus, tr("New note in this topic...")).c_str())) {
      note_dialog_ = "add_note";
      note_target_ = target_;
      note_title_buf_.clear();
      note_dialog_focus_ = true;
    }
    if (ImGui::MenuItem(with_icon(Icon::Fit, tr("Reset size")).c_str())) reset_one = true;
    if (b != nullptr && ImGui::MenuItem(with_icon(Icon::Folder, tr("Open folder")).c_str())) w_.open_block_folder(b->id);
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(241, 110, 110));
    if (ImGui::MenuItem(with_icon(Icon::Trash, tr("Remove...")).c_str())) {
      note_dialog_ = "remove";
      note_target_ = target_;
    }
    ImGui::PopStyleColor();
    ImGui::EndPopup();
  } else if (ImGui::BeginPopup("node_menu")) {
    const auto* b = snap.block(target_);
    ImGui::TextDisabled("%s", target_.c_str());
    ImGui::Separator();
    if (b != nullptr && b->kind == "group" && ImGui::MenuItem(with_icon(Icon::Layers, tr("Open")).c_str())) go_into = target_;
    if (b != nullptr && b->kind != "group" && ImGui::MenuItem(with_icon(Icon::Plus, tr("Add port...")).c_str())) dialog_ = "add_port";
    if (ImGui::MenuItem(with_icon(Icon::Card, tr("Rename...")).c_str())) dialog_ = "rename";
    if (b != nullptr && b->kind != "group" && ImGui::MenuItem(with_icon(Icon::Changes, tr("Split...")).c_str())) dialog_ = "split";
    if (b != nullptr && b->kind != "group" && ImGui::MenuItem(with_icon(Icon::Link, tr("Merge into...")).c_str())) dialog_ = "merge";
    if (ImGui::MenuItem(with_icon(Icon::Fit, tr("Reset size")).c_str())) reset_one = true;
    ImGui::Separator();
    if (b != nullptr && ImGui::MenuItem(with_icon(Icon::Card, tr("Open card in editor")).c_str())) w_.open_in_editor(b->dir + "/block.ksl", 1);
    if (b != nullptr && ImGui::MenuItem(with_icon(Icon::Folder, tr("Open folder")).c_str())) w_.open_block_folder(b->id);
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(241, 110, 110));
    if (ImGui::MenuItem(with_icon(Icon::Close, tr("Remove")).c_str())) {
      auto id = target_;
      w_.propose([=](const core::Schema& s) { return agents::op_remove_block(s, id); });
      preview_open_ = true;
    }
    ImGui::PopStyleColor();
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("bg_menu")) {
    if (proto_on_ && ImGui::MenuItem(with_icon(Icon::Sketch, tr("Add a prototype block here")).c_str())) {
      sketch_new_pos_ = ed::ScreenToCanvas(ImGui::GetMousePosOnOpeningCurrentPopup());
      sketch_target_.clear();
      sketch_dialog_ = true;
    }
    if (notes_) {
      if (ImGui::MenuItem(with_icon(Icon::Plus, tr("New note...")).c_str())) {
        note_dialog_ = "add_note";
        note_target_ = scope;
        note_title_buf_.clear();
        note_dialog_focus_ = true;
      }
      if (ImGui::MenuItem(with_icon(Icon::Folder, tr("New topic...")).c_str())) {
        note_dialog_ = "add_topic";
        note_target_ = scope;
        note_title_buf_.clear();
        note_dialog_focus_ = true;
      }
    } else if (ImGui::MenuItem(with_icon(Icon::Plus, tr("Add block...")).c_str())) {
      dialog_ = "add_block";
      target_ = scope;
    }
    if (ImGui::MenuItem(with_icon(Icon::Fit, tr("Fit to view")).c_str())) navigate_ = 1;
    if (ImGui::MenuItem(with_icon(Icon::Schema, tr("Auto layout (reset positions and sizes)")).c_str())) auto_layout = true;
    if (std::any_of(drawn.begin(), drawn.end(), [](const Drawn& d) { return d.bent; }) &&
        ImGui::MenuItem(with_icon(Icon::Undo, tr("Straighten every line of this level")).c_str())) {
      for (const auto& d : drawn) {
        if (d.bent) bends_.erase(planned[d.pi].key);
      }
      ++bends_version_;
      save_bends();
    }
    if (ImGui::MenuItem(with_icon(Icon::Folder, tr("Show project folder")).c_str())) w_.open_folder("");
    ImGui::EndPopup();
  }
  sketch_popups(m);
  ed::Resume();
  // Fit the view when a level is opened (not after every move or resize).
  if (navigated_view_ != view) {
    navigated_view_ = view;
    navigate_ = 3;
  }
  if (navigate_ > 0 && --navigate_ == 0) fit_view();
  // The block under the mouse (or under a hovered pin) is the hover focus of the next frame.
  std::string hovered_now;
  if (hover_line >= 0) {
    // A line (or its bend over a block) is under the mouse: no block takes the focus.
  } else if (auto hn = ed::GetHoveredNode(); hn && node_of.contains(hn.Get())) {
    hovered_now = node_of[hn.Get()];
  } else if (auto hp = ed::GetHoveredPin(); hp && pin_of.contains(hp.Get())) {
    hovered_now = core::split(pin_of[hp.Get()], ':', false).front();
  }

  // Cursor: a hand over what can be clicked or dragged into a link, the move arrows while a
  // block or the canvas is dragged.
  if (canvas_hovered_ || ImGui::IsMouseDragging(0) || ImGui::IsMouseDragging(1) || ImGui::IsMouseDragging(2)) {
    if (ImGui::IsMouseClicked(0)) drag_from_node_ = !hovered_now.empty() && !ed::GetHoveredPin();
    if (ImGui::IsMouseReleased(0)) drag_from_node_ = false;
    if ((ImGui::IsMouseDragging(0) && drag_from_node_ && resizing_.empty()) || ImGui::IsMouseDragging(1) || ImGui::IsMouseDragging(2)) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (ed::GetHoveredPin() || ed::GetHoveredLink()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
  }
  std::string collapse_now;
  if (!header_hit.empty() && ImGui::IsMouseClicked(0) && !shift) collapse_now = header_hit;
  hover_frame_ = header_frame;
  g_prof.mark(6);
  ed::End();
  g_prof.mark(7);
  // The highlight is cleared only by a plain left click on the background. The editor reports
  // a background "click" on the release of any button, also after a drag, and only in End():
  // panning with the right button or drawing a selection rectangle must keep the highlight.
  {
    const auto& io = ImGui::GetIO();
    bool plain = io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < io.MouseDragThreshold * io.MouseDragThreshold;
    if (ed::GetBackgroundClickButtonIndex() == ImGuiMouseButton_Left && plain && !shift && header_hit.empty()) {
      pinned_.clear();
      // A click on the empty place selects the block it lies in: an opened group around it, else
      // the group whose level this is, at the top the whole project.
      select(frame_under.empty() ? scope : frame_under);
    }
  }
  ed::SetCurrentEditor(nullptr);
  // Shift+click keeps a block's links lit (again: off); Shift+double-click opens a group in place.
  std::string expand_now;
  if (shift && ImGui::IsMouseClicked(0) && !hovered_now.empty() && resizing_.empty()) {
    if (!pinned_.erase(hovered_now)) pinned_.insert(hovered_now);
    if (ImGui::IsMouseDoubleClicked(0)) expand_now = hovered_now;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) pinned_.clear();
  hover_unit_ = hovered_now;
  // Open a group in place with everything inside it; close a frame (and what is inside).
  if (const auto* g = expand_now.empty() ? nullptr : snap.block(expand_now); g != nullptr && g->kind == "group" && !g->children.empty()) {
    std::set<std::string> open(req_.expanded.begin(), req_.expanded.end());
    open.insert(g->id);
    for (const auto& b : snap.blocks) {
      if (b.kind == "group" && b.id.starts_with(g->id + ".")) open.insert(b.id);
    }
    req_.expanded.assign(open.begin(), open.end());
    pinned_.erase(g->id);
    fit_on_apply_ = true;
  }
  if (!collapse_now.empty()) {
    if (req_.all_levels) {
      // "All levels" is every group opened; closing one turns it into an explicit list.
      req_.all_levels = false;
      req_.expanded.clear();
      for (const auto& blk : snap.blocks) {
        if (blk.kind == "group" && (scope.empty() || blk.id.starts_with(scope + "."))) req_.expanded.push_back(blk.id);
      }
      std::sort(req_.expanded.begin(), req_.expanded.end());
    }
    std::erase_if(req_.expanded, [&](const std::string& id) { return id == collapse_now || id.starts_with(collapse_now + "."); });
    fit_on_apply_ = true;
  }
  // Manual moves and sizes are saved and win over the automatic layout (14.2).
  bool moved = false;
  if (ImGui::IsMouseReleased(0) && !node_of.empty() && resizing_.empty() && !nested) {
    ed::SetCurrentEditor(ed_);
    for (const auto& [nid2, id] : node_of) {
      auto p = ed::GetNodePosition(nid2);
      auto it = placed_.find(id);
      if (it == placed_.end() || std::fabs(it->second.first - p.x) > 1 || std::fabs(it->second.second - p.y) > 1) moved = true;
    }
    ed::SetCurrentEditor(nullptr);
  }
  if (moved || resize_done) save_view(m);
  if (reset_one) save_view(m, false, target_);
  if (auto_layout) {
    for (const auto& n : m.graph.nodes) sizes_[n.id] = {0, 0};
    sizes_serial_ = m.serial;
    relayout_ = true;
    relayout_serial_ = m.serial;
    navigate_ = 4;
    w_.save_positions(view, {});
  }
  if (!go_into.empty()) enter(go_into);
  g_prof.mark(8);
  g_prof.frame_done();
}

// --- empty level -------------------------------------------------------------------------------

void Ui::load_bends() {
  bends_.clear();
  ++bends_version_;
  std::ifstream in(root_ / ".karkas" / "bends.json");
  if (!in) return;
  auto j = nlohmann::json::parse(in, nullptr, false);
  if (!j.is_object()) return;
  for (const auto& [key, v] : j.items()) {
    if (!v.is_array()) continue;
    std::vector<ImVec2> pts;
    for (const auto& p : v) {
      if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number()) pts.emplace_back(p[0].get<float>(), p[1].get<float>());
    }
    if (!pts.empty()) bends_[key] = std::move(pts);
  }
}

void Ui::save_bends() {
  // .karkas/bends.json: {"<kind>|<block a>|<block b>[|ports]": [[u, v], ...]} (ADR 0022).
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [key, pts] : bends_) {
    auto a = nlohmann::json::array();
    for (const auto& p : pts) {
      a.push_back({std::round(static_cast<double>(p.x) * 1000.0) / 1000.0, std::round(static_cast<double>(p.y) * 1000.0) / 1000.0});
    }
    j[key] = std::move(a);
  }
  std::error_code ec;
  auto dir = root_ / ".karkas";
  std::filesystem::create_directories(dir, ec);
  auto tmp = dir / "bends.json.tmp";
  {
    std::ofstream out(tmp);
    if (!out) return;
    out << j.dump(2) << "\n";
  }
  std::filesystem::rename(tmp, dir / "bends.json", ec);
}

// --- prototype on the canvas (ADR 0035) ------------------------------------------------------

namespace {

const ImVec4 kSketch = rgb(186, 126, 255);   // a block or dependency of the prototype
const ImVec4 kSketchKept = rgb(110, 220, 140);  // ... that the schema keeps

// A dashed curve from `a` (leaving to the right) to `b` (entering from the left), with an arrow.
void dashed_curve(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick, bool dotted) {
  float dx = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f);
  ImVec2 c1(a.x + dx, a.y);
  ImVec2 c2(b.x - dx, b.y);
  auto at = [&](float t) {
    float u = 1 - t;
    return ImVec2(u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x,
                  u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y);
  };
  constexpr int kSeg = 48;
  ImVec2 prev = a;
  for (int k = 1; k <= kSeg; ++k) {
    ImVec2 q = at(static_cast<float>(k) / kSeg);
    bool on = dotted ? (k % 3 == 0) : (k % 4 != 3);
    if (on) dl->AddLine(prev, q, col, thick);
    prev = q;
  }
  ImVec2 t0 = at(0.96f);
  ImVec2 d(b.x - t0.x, b.y - t0.y);
  float len = std::max(0.001f, std::sqrt(d.x * d.x + d.y * d.y));
  d = ImVec2(d.x / len, d.y / len);
  ImVec2 n(-d.y, d.x);
  float s = 7.0f + thick;
  dl->AddTriangleFilled(b, ImVec2(b.x - d.x * s + n.x * s * 0.55f, b.y - d.y * s + n.y * s * 0.55f),
                        ImVec2(b.x - d.x * s - n.x * s * 0.55f, b.y - d.y * s - n.y * s * 0.55f), col);
}

}  // namespace

void Ui::draw_sketch(const Model& m, int underlay_channel) {
  for (const auto& [id, p] : sketch_apply_) {
    ed::SetNodePosition(hid("node:" + id), p);  // a realized block goes where the human drew it
    placed_[id] = {p.x, p.y};
  }
  sketch_apply_.clear();
  sketch_nodes_.clear();
  sketch_pin_of_.clear();
  sketch_socks_.clear();
  const auto& snap = *m.snap;
  // What the schema keeps of the prototype, once it was given to the agents.
  agents::SketchCheck check;
  bool checked = !sketch_.sent.empty();
  if (checked) check = agents::check_sketch(sketch_, snapshot_shape(snap));
  float scale = std::max(0.6f, settings_.block_scale);
  float w = 230.0f * scale;
  for (const auto& sb : sketch_.blocks) {
    auto id = sketch_.id_of(sb);
    // A block the schema has now is drawn by the schema (at the place the human gave it).
    if (checked && !check.found[id].empty()) continue;
    auto nid = hid("sketch:" + id);
    sketch_nodes_[nid] = id;
    if (!sketch_placed_.contains(id)) {
      ed::SetNodePosition(nid, ImVec2(sb.x, sb.y));
      sketch_placed_.insert(id);
    }
    ed::PushStyleColor(ed::StyleColor_NodeBg, with_alpha(kSketch, 0.14f));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0, 0, 0, 0));  // dashed, below
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(12, 8, 12, 10));
    ed::BeginNode(nid);
    ImVec2 o = ImGui::GetCursorScreenPos();
    float inner = w - 24;
    float lh = ImGui::GetTextLineHeight();
    // Title: the sketch icon, the name, the kind.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.86f, 1.0f, 1.0f));
    ImGui::TextUnformatted(with_icon(Icon::Sketch, sb.name).c_str());
    ImGui::PopStyleColor();
    if (!sb.kind.empty()) {
      ImGui::SameLine(0, 6);
      ImGui::TextColored(with_alpha(kSketch, 0.9f), "· %s", tr_kind(sb.kind));
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    if (!sb.purpose.empty()) {
      ImGui::TextColored(rgb(200, 190, 215), "%s", sb.purpose.c_str());
    } else {
      ImGui::TextDisabled("%s", tr("(the architect writes its purpose)"));
    }
    ImGui::PopTextWrapPos();
    if (checked) ImGui::TextColored(rgb(241, 120, 120), "%s", tr("not in the schema"));
    ImGui::Dummy(ImVec2(inner, 1));
    // Pins: in on the left, out on the right of the title line.
    float cy = o.y + lh * 0.5f;
    ImVec2 in_c(o.x - 12, cy);
    ImVec2 out_c(o.x + inner + 12, cy);
    auto pin = [&](const std::string& side, ImVec2 c, bool input) {
      auto pid = hid("sketchpin:" + id + ":" + side);
      sketch_pin_of_[pid] = id;
      ed::PushStyleVar(input ? ed::StyleVar_TargetDirection : ed::StyleVar_SourceDirection, ImVec2(input ? -1.0f : 1.0f, 0.0f));
      ed::BeginPin(pid, input ? ed::PinKind::Input : ed::PinKind::Output);
      ed::PinRect(ImVec2(c.x - 9, c.y - 9), ImVec2(c.x + 9, c.y + 9));
      ed::PinPivotRect(c, c);
      ed::EndPin();
      ed::PopStyleVar();
      ImGui::GetWindowDrawList()->AddCircleFilled(c, 5.0f, ImGui::GetColorU32(kSketch));
      ImGui::GetWindowDrawList()->AddCircle(c, 5.0f, IM_COL32(30, 20, 40, 255), 0, 1.5f);
    };
    pin("in", in_c, true);
    pin("out", out_c, false);
    sketch_socks_[id] = {in_c, out_c};
    ed::EndNode();
    ed::PopStyleVar();
    ed::PopStyleColor(2);
    // A dashed border: a block that is not there yet.
    if (auto* bg = ed::GetNodeBackgroundDrawList(nid)) {
      ImVec2 a = ed::GetNodePosition(nid);
      ImVec2 sz = ed::GetNodeSize(nid);
      ImVec2 b(a.x + sz.x, a.y + sz.y);
      ImU32 c = ImGui::GetColorU32(kSketch);
      auto dash = [&](ImVec2 p, ImVec2 q) {
        float len = std::max(std::fabs(q.x - p.x), std::fabs(q.y - p.y));
        int n = std::max(1, static_cast<int>(len / 7.0f));
        for (int i = 0; i < n; i += 2) {
          float t0 = static_cast<float>(i) / n;
          float t1 = std::min(1.0f, static_cast<float>(i + 1) / n);
          bg->AddLine(ImVec2(p.x + (q.x - p.x) * t0, p.y + (q.y - p.y) * t0), ImVec2(p.x + (q.x - p.x) * t1, p.y + (q.y - p.y) * t1), c, 2.0f);
        }
      };
      dash(a, ImVec2(b.x, a.y));
      dash(ImVec2(b.x, a.y), b);
      dash(b, ImVec2(a.x, b.y));
      dash(ImVec2(a.x, b.y), a);
    }
    // The place the human gives it, kept when the drag ends.
    ImVec2 np = ed::GetNodePosition(nid);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && (std::fabs(np.x - sb.x) > 0.5f || std::fabs(np.y - sb.y) > 0.5f)) {
      for (auto& b2 : sketch_.blocks) {
        if (sketch_.id_of(b2) == id) {
          b2.x = np.x;
          b2.y = np.y;
        }
      }
      save_prototype();
    }
  }
  // Dependencies: dashed, from the out point of one block to the in point of the other; a real
  // block is reached at its border (or the border of the block that shows it on this level).
  std::set<std::string> shown;
  for (const auto& n : m.graph.nodes) shown.insert(n.id);
  auto end_point = [&](const std::string& id, bool out) -> std::optional<ImVec2> {
    if (auto it = sketch_socks_.find(id); it != sketch_socks_.end()) return out ? it->second.second : it->second.first;
    std::string target = id;
    if (checked) {
      if (auto f = check.found.find(id); f != check.found.end() && !f->second.empty()) target = f->second;
    }
    for (std::string x = target; !x.empty(); x = core::parent_id(x)) {
      if (!shown.contains(x)) continue;
      auto nid = hid("node:" + x);
      ImVec2 p = ed::GetNodePosition(nid);
      ImVec2 sz = ed::GetNodeSize(nid);
      if (sz.x <= 0) return std::nullopt;
      return ImVec2(out ? p.x + sz.x : p.x, p.y + std::min(sz.y * 0.5f, 22.0f));
    }
    return std::nullopt;
  };
  auto* dl = ImGui::GetWindowDrawList();
  int prev = dl->_Splitter._Current;
  dl->_Splitter.SetCurrentChannel(dl, underlay_channel);
  for (std::size_t i = 0; i < sketch_.links.size(); ++i) {
    const auto& l = sketch_.links[i];
    auto a = end_point(l.from, true);
    auto b = end_point(l.to, false);
    if (!a || !b) continue;
    int state = checked && i < check.links.size() ? check.links[i] : 0;
    ImVec4 col = checked && state == 1 ? kSketchKept : kSketch;
    dashed_curve(dl, *a, *b, ImGui::GetColorU32(with_alpha(col, 0.95f)), 2.5f, checked && state == -1);
  }
  dl->_Splitter.SetCurrentChannel(dl, prev);
  // "+ block" in the bar: the dialog opens with the block at the middle of the free part.
  if (sketch_add_center_) {
    sketch_add_center_ = false;
    ImVec2 c((free_area_.min.x + free_area_.max.x) * 0.5f, (free_area_.min.y + free_area_.max.y) * 0.5f);
    sketch_new_pos_ = ed::ScreenToCanvas(c);
    sketch_new_pos_.x -= 115 * scale;
    sketch_target_.clear();
    sketch_dialog_ = true;
  }
}

void Ui::sketch_new_link(std::uintptr_t a, std::uintptr_t b, const std::map<std::uintptr_t, std::string>& pin_of) {
  // The block of a pin: a sketch block, or the real block of a port or block point.
  auto owner = [&](std::uintptr_t p) -> std::string {
    if (auto it = sketch_pin_of_.find(p); it != sketch_pin_of_.end()) return it->second;
    if (auto it = pin_of.find(p); it != pin_of.end()) {
      auto parts = core::split(it->second, ':', false);
      return parts.empty() ? std::string() : parts[0];
    }
    return {};
  };
  // Dragged from an in point: the dependency goes the other way.
  bool reverse = false;
  for (const auto& [pid, sid] : sketch_pin_of_) {
    if (pid == a && hid("sketchpin:" + sid + ":in") == a) reverse = true;
  }
  std::string from = owner(a);
  std::string to = owner(b);
  if (reverse) std::swap(from, to);
  bool dup = std::any_of(sketch_.links.begin(), sketch_.links.end(), [&](const agents::SketchLink& l) { return l.from == from && l.to == to; });
  bool ok = !from.empty() && !to.empty() && from != to && !dup;
  if (!ok) {
    ed::Suspend();
    ImGui::SetTooltip("%s", dup ? tr("This dependency is drawn already") : tr("These two points cannot be linked"));
    ed::Resume();
    ed::RejectNewItem(rgb(255, 80, 80), 2.0f);
    return;
  }
  if (ed::AcceptNewItem(kSketch, 3.0f)) {
    sketch_.links.push_back({from, to, ""});
    save_prototype();
  }
}

void reset_canvas_caches() {
  g_plan = PlanCache{};
  g_model = ModelCache{};
}

void Ui::fit_view() {
  // Into the part of the canvas below the toolbar that no panel covers.
  fit_canvas(ed_, canvas_min_, canvas_max_, ImVec2(free_area_.min.x, free_area_.min.y + chrome_h_), free_area_.max, 1.5f);
}

void Ui::draw_empty(const Model& m) {
  std::vector<std::string> orphans;
  for (const auto& d : m.snap->drift) {
    if (d.code == "D06" && d.block.empty()) orphans.push_back(d.file);
  }
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float width = std::min(avail.x - 40.0f, 640.0f);
  ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - width) / 2, ImGui::GetCursorPosY() + avail.y * 0.15f));
  ImGui::BeginGroup();
  {
    ImFont* big = theme::icon_font_big();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(big, big->FontSize * 2.0f, ImVec2(p.x + width * 0.5f - big->FontSize, p.y),
                                        ImGui::GetColorU32(ImGuiCol_TextDisabled), icon(Icon::Schema));
    ImGui::Dummy(ImVec2(width, big->FontSize * 2.4f));
  }
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  if (notes_) {  // ADR 0038
    ImGui::TextUnformatted(m.snap->blocks.empty() ? tr("No notes yet.") : tr("No notes in this topic yet."));
    ImGui::TextDisabled("%s", tr("\"+ Note\" above (or right click) writes a note; \"+ Topic\" groups notes. In a text, [[title]] "
                                 "links it to another note."));
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
    if (ImGui::Button(with_icon(Icon::Plus, tr("New note")).c_str())) {
      note_dialog_ = "add_note";
      note_target_ = req_.scope;
      note_title_buf_.clear();
      note_dialog_focus_ = true;
    }
    ImGui::PopStyleColor();
  } else if (!m.snap->blocks.empty()) {
    ImGui::TextUnformatted(tr("No blocks on this level."));
    ImGui::TextDisabled("%s", tr("Right click → Add block, or the \"Add block\" button above."));
  } else {
    ImGui::TextUnformatted(tr("The schema has no blocks yet."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("karkas init only creates the project frame (project.ksl, karkas.toml, .karkas/). "
                                "Blocks appear when the architect designs the project (karkas design) or when you add "
                                "them here (\"Add block\")."));
    if (!orphans.empty()) {
      ImGui::Spacing();
      ImGui::TextColored(rgb(230, 180, 60), tr("%zu code files of this project are outside blocks (D06):"), orphans.size());
      std::size_t shown = std::min<std::size_t>(orphans.size(), 8);
      for (std::size_t i = 0; i < shown; ++i) ImGui::BulletText("%s", orphans[i].c_str());
      if (orphans.size() > shown) ImGui::TextDisabled(tr("... and %zu more"), orphans.size() - shown);
      ImGui::Spacing();
      // A proposed import (karkas import, "New project" with import): point at it.
      const core::Change* pending = nullptr;
      for (const auto& c : m.changes) {
        if (c.status.value == "pending" && c.from.value == "import") pending = &c;
      }
      if (pending != nullptr) {
        ImGui::TextWrapped(tr("The import is proposed as %s: review the blocks and approve it."), pending->id.c_str());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
        if (ImGui::Button(with_icon(Icon::Changes, tr("Open schema changes")).c_str())) side_ = SideView::Changes;
        ImGui::PopStyleColor();
      } else {
        ImGui::TextWrapped("%s", tr("Existing code becomes blocks without moving files: run karkas import, then "
                                    "approve the proposed change in the Changes tab."));
      }
    }
  }
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
}

// --- edit dialogs: form → KSL diff preview → apply (14.6) -------------------------------------------

void Ui::draw_dialogs(const Model& m) {
  // A form → KSL diff preview → apply. Labels above the fields, the main action highlighted.
  auto field = [](const char* label) {
    ui::muted("%s", label);
    ImGui::SetNextItemWidth(-1);
  };
  auto primary = [](const std::string& label) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::accent(1.0f));
    bool r = ImGui::Button(label.c_str());
    ImGui::PopStyleColor(2);
    return r;
  };
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 center(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);
  if (!dialog_.empty() && !ImGui::IsPopupOpen(title("Edit").c_str())) {
    ImGui::OpenPopup(title("Edit").c_str());
    preview_open_ = false;
  }
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, 0), ImGuiCond_Appearing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
  if (ImGui::BeginPopupModal(title("Edit").c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
    auto close = [&] {
      dialog_.clear();
      ImGui::CloseCurrentPopup();
    };
    auto heading = [&](Icon ic, const std::string& text) {
      ImGui::TextColored(theme::accent(1.0f), "%s", icon(ic));
      ImGui::SameLine();
      ImGui::TextUnformatted(text.c_str());
      ImGui::Separator();
      ImGui::Spacing();
    };
    bool go = false;
    if (dialog_ == "add_block") {
      heading(Icon::Plus, fmt::format(fmt::runtime(core::replace_all(tr("New block in %s"), "%s", "{}")),
                                      target_.empty() ? tr("the project") : target_));
      field(tr("name"));
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      go |= ImGui::InputText("##name", buf_name_, sizeof buf_name_, ImGuiInputTextFlags_EnterReturnsTrue);
      std::string kinds = std::string(tr_kind("group")) + '\0' + tr_kind("leaf") + '\0' + tr_kind("entry") + '\0' +
                          tr_kind("shared") + '\0' + '\0';
      field(tr("kind"));
      ImGui::Combo("##kind", &kind_, kinds.c_str());
      field(tr("purpose"));
      ImGui::InputText("##purpose", buf_purpose_, sizeof buf_purpose_);
      field(tr("thread"));
      ImGui::InputText("##thread", buf_thread_, sizeof buf_thread_);
      ImGui::Spacing();
      if (primary(with_icon(Icon::Eye, tr("Preview"))) || go) {
        std::string parent = target_, name = buf_name_, purpose = buf_purpose_, thread = buf_thread_;
        auto kind = static_cast<core::BlockKind>(kind_);
        w_.propose([=](const core::Schema& s) { return agents::op_add_block(s, parent, name, kind, purpose, thread); });
        preview_open_ = true;
        close();
      }
    } else if (dialog_ == "add_port") {
      heading(Icon::Plus, fmt::format(fmt::runtime(core::replace_all(tr("New port of %s"), "%s", "{}")), target_));
      field(tr("dir"));
      ImGui::Combo("##dir", &dir_, "in\0out\0fn\0");
      if (dir_ != 2) {
        field(tr("name"));
        ImGui::InputText("##name", buf_name_, sizeof buf_name_);
      }
      field(dir_ == 2 ? tr("signature: name(params) -> R") : tr("type"));
      go |= ImGui::InputText("##spec", buf_spec_, sizeof buf_spec_, ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::Spacing();
      if (primary(with_icon(Icon::Eye, tr("Preview"))) || go) {
        std::string id = target_, name = buf_name_, spec = buf_spec_;
        auto dir = static_cast<core::PortDir>(dir_);
        w_.propose([=](const core::Schema& s) { return agents::op_add_port(s, id, dir, name, spec); });
        preview_open_ = true;
        close();
      }
    } else if (dialog_ == "rename" || dialog_ == "merge" || dialog_ == "split") {
      const char* head = dialog_ == "rename" ? "Rename %s" : dialog_ == "merge" ? "Merge %s into" : "Split %s";
      const char* label = dialog_ == "rename" ? "new name" : dialog_ == "merge" ? "merge into (block id)" : "new block name";
      heading(dialog_ == "rename" ? Icon::Card : Icon::Changes, fmt::format(fmt::runtime(core::replace_all(tr(head), "%s", "{}")), target_));
      field(tr(label));
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      go |= ImGui::InputText("##name", buf_name_, sizeof buf_name_, ImGuiInputTextFlags_EnterReturnsTrue);
      if (dialog_ == "split") {
        field(tr("ports to move (comma separated)"));
        ImGui::InputText("##spec", buf_spec_, sizeof buf_spec_);
      }
      ImGui::Spacing();
      if (primary(with_icon(Icon::Eye, tr("Preview"))) || go) {
        std::string id = target_, name = buf_name_, spec = buf_spec_, what = dialog_;
        w_.propose([=](const core::Schema& s) -> kk::Result<agents::EditOp> {
          if (what == "rename") return agents::op_rename(s, id, name);
          if (what == "merge") return agents::op_merge(s, name, id);
          return agents::op_split(s, id, name, core::split(spec, ','));
        });
        preview_open_ = true;
        close();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
  if (preview_open_ && (m.has_pending || !m.pending_error.empty()) && !ImGui::IsPopupOpen(title("KSL diff").c_str())) {
    ImGui::OpenPopup(title("KSL diff").c_str());
  }
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool failed = !m.pending_error.empty();
  ImGui::SetNextWindowSize(failed ? ImVec2(ImGui::GetFontSize() * 30, 0) : ImVec2(ImGui::GetFontSize() * 42, ImGui::GetFontSize() * 30),
                           ImGuiCond_Appearing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
  if (ImGui::BeginPopupModal(title("KSL diff").c_str(), nullptr, failed ? ImGuiWindowFlags_AlwaysAutoResize : 0)) {
    float footer = ImGui::GetFrameHeightWithSpacing() + 8;
    if (!m.pending_error.empty()) {
      ImGui::TextColored(rgb(241, 76, 76), "%s", icon(Icon::Error));
      ImGui::SameLine();
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(m.pending_error.c_str());
      ImGui::PopTextWrapPos();
    } else {
      ImGui::TextColored(theme::accent(1.0f), "%s", icon(Icon::Changes));
      ImGui::SameLine();
      ImGui::TextUnformatted(m.pending_title.c_str());
      ImGui::Separator();
      ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
      ImGui::BeginChild("pdiff", ImVec2(0, -footer), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
      ui::diff_text(m.pending_preview);
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }
    bool can_apply = m.has_pending && m.pending_error.empty();
    if (!can_apply) ImGui::Spacing();
    if (can_apply && primary(with_icon(Icon::Check, tr("Apply")))) {
      w_.confirm();
      preview_open_ = false;
      ImGui::CloseCurrentPopup();
    }
    if (can_apply) ImGui::SameLine();
    if (ImGui::Button(tr("Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      w_.cancel();
      preview_open_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

}  // namespace kk::gui
