#include "ui.hpp"

#include <cstdint>
#include <TextEditor.h>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cfloat>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include "i18n.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "ui_common.hpp"
#include "widgets.hpp"
#include "metrics.hpp"
#include "kk/agents/edit.hpp"
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
std::string fit(const std::string& s, float width) {
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

Ui::Ui(Worker& w, GuiSettings& settings, bool persist) : w_(w), settings_(settings), persist_(persist) {
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

Ui::~Ui() { ed::DestroyEditor(ed_); }

void Ui::enter(const std::string& scope) {
  req_.scope = scope == "root" ? "" : scope;
  applied_layout_.clear();
  pinned_.clear();
  req_.expanded.clear();
  req_.all_levels = false;
}

void Ui::select(const std::string& id) { req_.selected = id; }

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
  int in = 0;
  int out = 0;
  (void)in;
  (void)out;
  if (used_serial_ != m.serial) {
    used_ = used_blocks(*m.snap);
    used_serial_ = m.serial;
  }
  auto [left, right] = pin_rows(*m.snap, *b, used_);
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
  for (const auto& n : m.graph.nodes) {
    if (n.id == id) return {static_cast<float>(n.w), static_cast<float>(n.h)};
  }
  return base_size(m, id);
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

void Ui::draw_graph(const Model& m) {
  ed::SetCurrentEditor(ed_);
  ed::Begin("schema");
  const auto& snap = *m.snap;
  const std::string& scope = m.req.scope;  // the level this model describes
  std::map<std::uintptr_t, std::string> node_of, pin_of;  // id → block / "block:port:side"
  std::set<std::string> shown_pins;                         // pin keys drawn this frame
  std::string view = scope.empty() ? "root" : scope;
  // Visible blocks ("units") and the unit that shows a given block (itself or an ancestor).
  std::set<std::string> units;
  for (const auto& n : m.graph.nodes) units.insert(n.id);
  auto unit_of = [&](const std::string& id) -> std::string {
    for (std::string x = id; !x.empty(); x = core::parent_id(x)) {
      if (units.contains(x)) return x;
    }
    return {};
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
    auto a = unit_of(l.src_block);
    auto b = unit_of(l.dst_block);
    if (a.empty() || b.empty() || a == b || !lit_pair(a, b)) continue;
    lit_nodes.insert(a);
    lit_nodes.insert(b);
  }
  bool dimming = !focus.empty();
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
  for (const auto& n : m.graph.nodes) {
    const auto* b = snap.block(n.id);
    if (b == nullptr) continue;
    auto nid = hid("node:" + n.id);
    node_of[nid] = n.id;
    // Size of the block and the font scale that makes its text fit (text follows size).
    auto [w, h] = node_size(m, n.id);
    auto [bw, bh] = base_size(m, n.id);
    float font = std::clamp(std::min(w / bw, h / bh), 0.55f, 2.5f);
    float inner_w = w - 2 * kPadX;
    float inner_h = h - 2 * kPadY;

    // Background in the block's own color (setting), status as a dot; the border warns.
    bool active = active_blocks_.contains(n.id);
    ImVec4 tint = settings_.blocks_by_block ? color_of(m, n.id) : status_color(b->status);
    ImVec4 fill = ImVec4(0.12f + tint.x * 0.16f, 0.12f + tint.y * 0.16f, 0.12f + tint.z * 0.16f, 0.96f);
    ImVec4 border = with_alpha(tint, 0.55f);
    float border_w = 1.0f;
    if (b->status == "failing" || b->status == "drift") {
      border = status_color(b->status);
      border_w = 2.5f;
    }
    if (b->status == "needs_review") {
      border = rgb(255, 140, 0);
      border_w = 3.0f;
    }
    if (active) {
      border = rgb(240, 200, 30, static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0)));
      border_w = 3.0f;
    }
    bool dim = dimming && !lit_nodes.contains(n.id);
    if (pinned_.contains(n.id)) {
      border = rgb(255, 230, 120);
      border_w = 3.0f;
    } else if (dimming && focus.contains(n.id)) {
      border = rgb(255, 255, 255, 0.9f);
      border_w = 2.5f;
    }
    if (dim) {
      fill.w = 0.35f;
      border.w = 0.15f;
    }
    ed::PushStyleColor(ed::StyleColor_NodeBg, fill);
    ed::PushStyleColor(ed::StyleColor_NodeBorder, border);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, border_w);
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(kPadX, kPadY, kPadX, kPadY));
    ed::BeginNode(nid);
    ImVec4 text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImVec4 text_dis = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (dim) {
      text_col.w = 0.25f;
      text_dis.w = 0.18f;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, text_col);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, text_dis);
    // Text, line spacing and gaps all follow the block size.
    ImGui::SetWindowFontScale(font);
    ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing.x * font, spacing.y * font));
    ImVec2 o = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeightWithSpacing();
    float y = o.y;
    // Line 1 (on a band of the block's color): kind icon, name, status dot on the right.
    Icon kind_icon = b->kind == "group" ? Icon::Folder : b->kind == "entry" ? Icon::Play : b->kind == "shared" ? Icon::Library : Icon::Cube;
    float dot_w = ImGui::CalcTextSize(icon(Icon::Dot)).x;
    float icon_w = ImGui::CalcTextSize(icon(kind_icon)).x + ImGui::GetStyle().ItemSpacing.x * 0.6f;
    ImGui::SetCursorScreenPos(ImVec2(o.x, y));
    ImGui::TextColored(with_alpha(ImVec4(1, 1, 1, 1), dim ? 0.3f : 0.9f), "%s", icon(kind_icon));
    text_at(o.x + icon_w, y, core::last_segment(n.id), inner_w - icon_w - dot_w - 4);
    ImGui::SetCursorScreenPos(ImVec2(o.x + inner_w - dot_w, y));
    ImGui::TextColored(with_alpha(active ? rgb(240, 200, 30) : status_color(b->status), dim ? 0.3f : 1.0f), "%s", icon(Icon::Dot));
    float band_h = lh + kPadY * 0.5f;
    y += lh + ImGui::GetStyle().ItemSpacing.y * 0.5f;
    // Line 2: purpose.
    if (y + lh <= o.y + inner_h) {
      std::string purpose = b->purpose;
      if (b->kind == "group") purpose += fmt::format(" ({})", b->children.size());
      text_at(o.x, y, purpose, inner_w);
      y += lh;
    }
    // Pins. Ports that do not fit fold into "+N" pins; links to them attach there.
    auto pin = [&](const std::string& key, const std::string& label, float px, float py, bool input, float width) {
      auto pid = hid(key);
      shown_pins.insert(key);
      std::string text = fit(label, width);
      if (text.empty()) text = "●";
      ImGui::SetCursorScreenPos(ImVec2(px, py));
      ed::BeginPin(pid, input ? ed::PinKind::Input : ed::PinKind::Output);
      ImGui::TextUnformatted(text.c_str());
      ed::EndPin();
      return pid;
    };
    float half = inner_w / 2 - 4;
    int rows_fit = std::max(1, static_cast<int>((o.y + inner_h - y) / lh));
    if (b->kind == "group") {
      auto in = pin(pin_key(n.id, "", "in"), "▶", o.x, y, true, half);
      auto out_label = std::string("▶");
      float tw = ImGui::CalcTextSize(out_label.c_str()).x;
      auto out = pin(pin_key(n.id, "", "out"), out_label, o.x + inner_w - tw, y, false, half);
      pin_of[in] = n.id + "::in";
      pin_of[out] = n.id + "::out";
    } else {
      // Left: inputs (in, fn, "used"); right: outputs (out, "calls") — links run left to right.
      if (used_serial_ != m.serial) {
        used_ = used_blocks(snap);
        used_serial_ = m.serial;
      }
      std::vector<std::pair<std::string, std::string>> left, right;  // key, label
      for (const auto* p : snap.ports_of(n.id)) {
        if (p->dir == "in") left.emplace_back(pin_key(n.id, p->name, "in"), "● " + p->name);
      }
      for (const auto* p : snap.ports_of(n.id)) {
        if (p->dir == "fn") left.emplace_back(pin_key(n.id, p->name, "fn"), "▸ " + p->name + "()");
      }
      if (used_.contains(n.id)) left.emplace_back(pin_key(n.id, "", "use"), std::string("○ ") + tr("used"));
      for (const auto* p : snap.ports_of(n.id)) {
        if (p->dir == "out") right.emplace_back(pin_key(n.id, p->name, "out"), p->name + " ●");
      }
      if (b->kind != "shared") right.emplace_back(pin_key(n.id, "", "calls"), std::string(tr("calls")) + " ○");
      auto side_of = [](const std::string& key) { return core::split(key, ':', false)[3]; };
      auto port_of = [](const std::string& key) { return core::split(key, ':', false)[2]; };
      int need = static_cast<int>(std::max(left.size(), right.size()));
      int shown = need <= rows_fit ? need : std::max(0, rows_fit - 1);
      for (int i = 0; i < shown; ++i) {
        float ry = y + static_cast<float>(i) * lh;
        if (i < static_cast<int>(left.size())) {
          const auto& [key, label] = left[static_cast<std::size_t>(i)];
          auto pid = pin(key, label, o.x, ry, true, half);
          pin_of[pid] = n.id + ":" + port_of(key) + ":" + side_of(key);
        }
        if (i < static_cast<int>(right.size())) {
          const auto& [key, label] = right[static_cast<std::size_t>(i)];
          std::string t = fit(label, half);
          float tw = ImGui::CalcTextSize(t.c_str()).x;
          auto pid = pin(key, label, o.x + inner_w - tw, ry, false, half);
          pin_of[pid] = n.id + ":" + port_of(key) + ":" + side_of(key);
        }
      }
      if (shown < need) {
        float ry = y + static_cast<float>(shown) * lh;
        int hidden_l = std::max(0, static_cast<int>(left.size()) - shown);
        int hidden_r = std::max(0, static_cast<int>(right.size()) - shown);
        if (hidden_l > 0) {
          auto pid = pin(pin_key(n.id, "+", "in"), fmt::format("+{}", hidden_l), o.x, ry, true, half);
          pin_of[pid] = n.id + ":+:in";
        }
        if (hidden_r > 0) {
          std::string label = fmt::format("+{}", hidden_r);
          float tw = ImGui::CalcTextSize(label.c_str()).x;
          auto pid = pin(pin_key(n.id, "+", "out"), label, o.x + inner_w - tw, ry, false, half);
          pin_of[pid] = n.id + ":+:out";
        }
      }
    }
    // The block keeps exactly its size.
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(inner_w, inner_h));
    // Resize grip in the bottom-right corner (min/max sizes, 14.6); same size on screen at any zoom.
    ImGui::PopStyleVar();
    ImGui::SetWindowFontScale(1.0f);
    if (!nested) {
    float grip = std::min({kGrip / std::max(ed::GetCurrentZoom(), 0.05f), inner_w / 4, inner_h / 3});
    ImGui::SetCursorScreenPos(ImVec2(o.x + inner_w - grip, o.y + inner_h - grip));
    ImGui::InvisibleButton(("##grip" + n.id).c_str(), ImVec2(grip, grip));
    bool grip_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (ImGui::IsItemActivated()) {
      resizing_ = n.id;
      resize_start_ = {w, h};
      auto mp = ImGui::GetMousePos();
      resize_mouse_ = {mp.x, mp.y};
      live_size_ = {w, h};
    }
    if (ImGui::IsItemActive() && resizing_ == n.id) {
      auto mp = ImGui::GetMousePos();
      live_size_ = {std::clamp(resize_start_.first + mp.x - resize_mouse_.first, kMinBlockW, kMaxBlockW),
                    std::clamp(resize_start_.second + mp.y - resize_mouse_.second, kMinBlockH, kMaxBlockH)};
    }
    if (ImGui::IsItemDeactivated() && resizing_ == n.id) {
      sizes_[n.id] = live_size_;
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
    // The title band behind the first line, in the block's color.
    if (auto* bg = ed::GetNodeBackgroundDrawList(nid)) {
      ImVec2 np = ed::GetNodePosition(nid);
      ImVec2 ns = ed::GetNodeSize(nid);
      float r = ed::GetStyle().NodeRounding;
      bg->AddRectFilled(ImVec2(np.x + 1, np.y + 1), ImVec2(np.x + ns.x - 1, np.y + kPadY + band_h), 
                        ImGui::GetColorU32(with_alpha(tint, dim ? 0.10f : 0.42f)), r, ImDrawFlags_RoundCornersTop);
    }
  }
  applied_layout_ = m.layout_key;

  // Links: port to port between leaves, otherwise lifted to group pins (3.6).
  // A pin of a block: the port's own pin, else its "+N" pin, else the group pin of that side.
  auto endpoint = [&](const std::string& block, const std::string& port, const std::string& side) {
    auto key = pin_key(block, port, side);
    if (shown_pins.contains(key)) return hid(key);
    bool input = side == "in" || side == "fn" || side == "use";
    auto folded = pin_key(block, "+", input ? "in" : "out");
    if (shown_pins.contains(folded)) return hid(folded);
    return hid(pin_key(block, "", input ? "in" : "out"));
  };
  std::map<std::uintptr_t, const index::LinkRow*> link_of;
  // call/use/state links between the same two blocks are drawn as one line (thicker with
  // more links); the hover tooltip names them. data/event links stay port to port.
  struct Bundle {
    std::uintptr_t from = 0, to = 0;
    std::string kind, a, b;
    std::vector<const index::LinkRow*> links;
  };
  std::map<std::tuple<std::string, std::string, std::string>, Bundle> bundles;
  // Color: of the block the line starts at (data source, called/used block) or of the kind;
  // dimmed when another block is in focus, brighter and thicker when it belongs to the focus.
  auto style = [&](const std::string& kind, const std::string& start, const std::string& a, const std::string& b,
                   float thick) {
    ImVec4 c = settings_.links_by_block ? with_alpha(color_of(m, start), kind == "use" ? 0.6f : 0.9f) : link_color(kind);
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
  for (const auto& l : snap.links) {
    if (l.kind == "use" && !req_.show_use) continue;
    auto a = unit_of(l.src_block);
    auto b = unit_of(l.dst_block);
    if (a.empty() || b.empty() || a == b) continue;
    if (!node_of.contains(hid("node:" + a)) || !node_of.contains(hid("node:" + b))) continue;
    const auto* ba = snap.block(a);
    const auto* bb = snap.block(b);
    bool leaf_a = ba->kind != "group" && a == l.src_block;
    bool leaf_b = bb->kind != "group" && b == l.dst_block;
    std::uintptr_t from = 0;
    std::uintptr_t to = 0;
    if (l.kind == "data" || l.kind == "event") {
      from = leaf_a ? endpoint(a, l.src_port, "out") : hid(pin_key(a, "", "out"));
      to = leaf_b ? endpoint(b, l.dst_port, "in") : hid(pin_key(b, "", "in"));
    } else {
      // call/use/state: from the caller's "calls" pin to the called fn (or "used" pin).
      from = leaf_a ? endpoint(a, "", "calls") : hid(pin_key(a, "", "out"));
      if (!leaf_b) {
        to = hid(pin_key(b, "", "in"));
      } else if (l.kind == "call" && !l.dst_port.empty()) {
        to = endpoint(b, l.dst_port, "fn");
      } else {
        to = endpoint(b, "", "use");
      }
      auto& bundle = bundles[{a, b, l.kind}];
      if (bundle.links.empty()) bundle = Bundle{from, to, l.kind, a, b, {}};
      bundle.links.push_back(&l);
      continue;
    }
    auto lid = hid(fmt::format("link:{}", l.id));
    link_of[lid] = &l;
    auto [c, thick] = style(l.kind, a, a, b, 2.0f);
    ed::Link(lid, from, to, c, thick);
  }
  std::map<std::uintptr_t, std::string> bundle_text;
  for (const auto& [key, bd] : bundles) {
    auto n = bd.links.size();
    auto lid = n == 1 ? hid(fmt::format("link:{}", bd.links[0]->id)) : hid(fmt::format("bundle:{}>{}:{}", bd.a, bd.b, bd.kind));
    float base = bd.kind == "use" ? 1.0f : (bd.kind == "state" ? 3.0f : 2.0f);
    float thick = base + std::min(3.0f, std::log2(static_cast<float>(n)));
    if (n == 1) {
      link_of[lid] = bd.links[0];  // a single link can be deleted on the picture
    } else {
      std::vector<std::string> names;
      for (const auto* l : bd.links) {
        if (names.size() == 12) {
          names.emplace_back("…");
          break;
        }
        names.push_back(l->dst_port.empty() ? l->dst_block : l->dst_port);
      }
      bundle_text[lid] = fmt::format(fmt::runtime(tr("{} → {}: {} {} links")), core::last_segment(bd.a),
                                     core::last_segment(bd.b), n, bd.kind) +
                         "\n" + core::join(names, ", ");
    }
    auto [c, t] = style(bd.kind, bd.b, bd.a, bd.b, thick);
    ed::Link(lid, bd.from, bd.to, c, t);
  }

  // Draw a link from an out port or fn to a consumer (14.6).
  if (ed::BeginCreate(rgb(255, 255, 255), 2.0f)) {
    ed::PinId a;
    ed::PinId b;
    if (ed::QueryNewLink(&a, &b) && a && b) {
      auto pa = core::split(pin_of[a.Get()], ':', false);
      auto pb = core::split(pin_of[b.Get()], ':', false);
      // Provider: an out port or a fn; consumer: an in port, or the caller's "calls"/"used" pin.
      auto provider = [](const std::vector<std::string>& p) { return p.size() == 3 && !p[1].empty() && p[1] != "+" && (p[2] == "out" || p[2] == "fn"); };
      auto consumer = [](const std::vector<std::string>& p) {
        return p.size() == 3 && p[1] != "+" && ((p[2] == "in" && !p[1].empty()) || p[2] == "calls" || p[2] == "use");
      };
      if (!provider(pa)) std::swap(pa, pb);
      bool ok = provider(pa) && consumer(pb) && pa[0] != pb[0] && !(pa[2] == "fn" && pb[2] == "in");
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
      if (ed::AcceptDeletedItem(false) && link_of.contains(lid.Get())) {
        const auto* l = link_of[lid.Get()];
        auto k = l->kind, sb = l->src_block, sp = l->src_port, db = l->dst_block, dp = l->dst_port;
        w_.propose([=](const core::Schema& s) { return agents::op_disconnect(s, k, sb, sp, db, dp); });
        preview_open_ = true;
      }
    }
    ed::NodeId nid;
    while (ed::QueryDeletedNode(&nid)) {
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
  if (dbl && node_of.contains(dbl.Get()) && !shift) {
    const auto* b = snap.block(node_of[dbl.Get()]);
    if (b->kind == "group") {
      go_into = b->id;
    } else {
      select(b->id);
      req_.open_file.clear();
    }
  }
  std::vector<ed::NodeId> sel(8);
  int nsel = ed::GetSelectedNodes(sel.data(), static_cast<int>(sel.size()));
  if (nsel == 1 && node_of.contains(sel[0].Get())) {
    auto id = node_of[sel[0].Get()];
    if (req_.selected != id && !ImGui::IsMouseDragging(0)) select(id);
  }
  ed::Suspend();
  if (auto hl = ed::GetHoveredLink(); hl && bundle_text.contains(hl.Get())) ImGui::SetTooltip("%s", bundle_text[hl.Get()].c_str());
  ed::NodeId ctx;
  if (ed::ShowNodeContextMenu(&ctx) && node_of.contains(ctx.Get())) {
    target_ = node_of[ctx.Get()];
    ImGui::OpenPopup("node_menu");
  } else if (ed::ShowBackgroundContextMenu()) {
    ImGui::OpenPopup("bg_menu");
  }
  bool reset_one = false;
  bool auto_layout = false;
  if (ImGui::BeginPopup("node_menu")) {
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
    if (ImGui::MenuItem(with_icon(Icon::Plus, tr("Add block...")).c_str())) {
      dialog_ = "add_block";
      target_ = scope;
    }
    if (ImGui::MenuItem(with_icon(Icon::Fit, tr("Fit to view")).c_str())) navigate_ = 1;
    if (ImGui::MenuItem(with_icon(Icon::Schema, tr("Auto layout (reset positions and sizes)")).c_str())) auto_layout = true;
    if (ImGui::MenuItem(with_icon(Icon::Folder, tr("Show project folder")).c_str())) w_.open_folder("");
    ImGui::EndPopup();
  }
  ed::Resume();
  // Fit the view when a level is opened (not after every move or resize).
  if (navigated_view_ != view) {
    navigated_view_ = view;
    navigate_ = 3;
  }
  if (navigate_ > 0 && --navigate_ == 0) ed::NavigateToContent(0.0f);
  // The block under the mouse (or under a hovered pin) is the hover focus of the next frame.
  std::string hovered_now;
  if (auto hn = ed::GetHoveredNode(); hn && node_of.contains(hn.Get())) {
    hovered_now = node_of[hn.Get()];
  } else if (auto hp = ed::GetHoveredPin(); hp && pin_of.contains(hp.Get())) {
    hovered_now = core::split(pin_of[hp.Get()], ':', false).front();
  }
  if (ed::IsBackgroundClicked() && !shift && header_hit.empty()) pinned_.clear();
  std::string collapse_now;
  if (!header_hit.empty() && ImGui::IsMouseClicked(0) && !shift) collapse_now = header_hit;
  hover_frame_ = header_frame;
  ed::End();
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
}

// --- empty level -------------------------------------------------------------------------------

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
  if (!m.snap->blocks.empty()) {
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
      ImGui::TextWrapped("%s", tr("Existing code becomes blocks without moving files: run karkas import, then "
                                  "approve the proposed change in the Changes tab."));
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
