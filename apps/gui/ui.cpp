#include "ui.hpp"

#include <TextEditor.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "i18n.hpp"
#include "metrics.hpp"
#include "kk/agents/edit.hpp"
#include "kk/core/text.hpp"

namespace ed = ax::NodeEditor;

namespace kk::gui {

namespace {

// --- colors (14.5) ------------------------------------------------------------------------

ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
}

ImVec4 status_color(const std::string& s) {
  if (s == "passing") return rgb(56, 142, 60);
  if (s == "failing") return rgb(198, 40, 40);
  if (s == "drift") return rgb(123, 31, 162);
  if (s == "api_frozen" || s == "tests_ready") return rgb(30, 100, 200);
  if (s == "implemented") return rgb(0, 121, 107);
  if (s == "needs_review") return rgb(90, 90, 90);
  return rgb(110, 110, 110);  // design
}

ImVec4 link_color(const std::string& kind) {
  if (kind == "data") return rgb(66, 135, 245);
  if (kind == "event") return rgb(245, 145, 40);
  if (kind == "call") return rgb(170, 170, 170);
  if (kind == "state") return rgb(220, 50, 50);
  return rgb(120, 120, 120, 0.6f);  // use
}

// The i-th color of a sequence of well-separated hues (golden ratio steps).
ImVec4 palette_color(std::size_t i, float alpha = 1.0f) {
  double h = 0.08 + static_cast<double>(i) * 0.6180339887;
  h -= static_cast<double>(static_cast<long>(h));
  float r = 0, g = 0, b = 0;
  ImGui::ColorConvertHSVtoRGB(static_cast<float>(h), i % 2 == 0 ? 0.72f : 0.55f, 1.0f, r, g, b);
  return ImVec4(r, g, b, alpha);
}

std::uintptr_t hid(std::string_view s) {
  auto h = static_cast<std::uintptr_t>(core::fnv1a64(s) & 0x7fffffffffffULL);
  return h == 0 ? 1 : h;
}

std::string pin_key(const std::string& block, const std::string& port, const char* side) {
  return fmt::format("pin:{}:{}:{}", block, port, side);
}


void status_dot(const std::string& status, bool active, double pulse) {
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float r = ImGui::GetTextLineHeight() * 0.32f;
  ImVec4 c = active ? rgb(240, 200, 30, static_cast<float>(0.6 + 0.4 * std::sin(pulse * 5.0))) : status_color(status);
  dl->AddCircleFilled(ImVec2(p.x + r + 1, p.y + ImGui::GetTextLineHeight() * 0.5f), r, ImGui::GetColorU32(c));
  if (status == "needs_review") {
    dl->AddCircle(ImVec2(p.x + r + 1, p.y + ImGui::GetTextLineHeight() * 0.5f), r + 1.5f, ImGui::GetColorU32(rgb(255, 140, 0)), 0, 2.0f);
  }
  ImGui::Dummy(ImVec2(r * 2 + 4, ImGui::GetTextLineHeight()));
  ImGui::SameLine();
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

Ui::Ui(Worker& w, GuiSettings& settings, bool persist)
    : w_(w), settings_(settings), persist_(persist), code_(std::make_unique<TextEditor>()) {
  ed::Config cfg;
  cfg.SettingsName = nullptr;  // positions and sizes live in .karkas/layout (14.2), not imgui.ini
  ed_ = ed::CreateEditor(&cfg);
  code_->SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
  code_->SetReadOnly(true);
  code_->SetPalette(TextEditor::GetDarkPalette());
  req_.block_scale = settings_.block_scale;
}

Ui::~Ui() { ed::DestroyEditor(ed_); }

void Ui::enter(const std::string& scope) {
  req_.scope = scope == "root" ? "" : scope;
  applied_layout_.clear();
  pinned_.clear();
}

void Ui::select(const std::string& id) { req_.selected = id; }

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
  for (const auto* p : m.snap->ports_of(id)) (p->dir == "in" ? in : out)++;
  auto [w, h] = base_block_size(b->id, b->kind, b->purpose, in, out, req_.char_w, req_.line_h);
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

bool Ui::draw(const std::shared_ptr<const Model>& mp, bool first_frame) {
  pulse_ = ImGui::GetTime();
  draw_menu();
  ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
  if (first_frame && ImGui::DockBuilderGetNode(dock) != nullptr && ImGui::DockBuilderGetNode(dock)->IsLeafNode()) {
    ImGui::DockBuilderRemoveNodeChildNodes(dock);
    ImGuiID center = dock;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGui::DockBuilderDockWindow("###Blocks", left);
    ImGui::DockBuilderDockWindow("###Inspector", right);
    ImGui::DockBuilderDockWindow("###Code", right);
    ImGui::DockBuilderDockWindow("###Schema", center);
    ImGui::DockBuilderDockWindow("###Details", bottom);
    ImGui::DockBuilderFinish(dock);
  }
  if (!mp || !mp->snap) {
    ImGui::Begin(title("Schema").c_str());
    ImGui::TextUnformatted(mp ? mp->message.c_str() : tr("indexing..."));
    ImGui::End();
    draw_settings(mp.get());
    return true;
  }
  const Model& m = *mp;
  if (m.serial > sizes_serial_ && resizing_.empty()) sizes_.clear();  // the model has them now
  active_blocks_.clear();
  for (const auto& a : m.active) active_blocks_.insert(a.block);
  draw_tree(m);
  ImGui::Begin(title("Schema").c_str());
  draw_toolbar(m);
  if (m.snap->blocks.empty() || (!req_.all_levels && m.graph.nodes.empty() && m.req.scope == req_.scope)) {
    draw_empty(m);
  } else if (req_.all_levels) {
    draw_all_levels(m);
  } else {
    draw_graph(m);
  }
  ImGui::End();
  draw_inspector(m);
  draw_code(m);
  draw_bottom(m);
  draw_dialogs(m);
  draw_settings(&m);
  return !m.active.empty();
}

// --- menu and settings ---------------------------------------------------------------------

void Ui::draw_menu() {
  if (!ImGui::BeginMainMenuBar()) return;
  if (ImGui::BeginMenu("Karkas")) {
    if (ImGui::MenuItem(tr("Settings..."))) show_settings_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Quit"))) quit_ = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(tr("View"))) {
    ImGui::MenuItem(tr("All levels"), nullptr, &req_.all_levels);
    ImGui::MenuItem(tr("use links"), nullptr, &req_.show_use);
    if (ImGui::MenuItem(tr("Fit to view"))) navigate_ = 1;
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();
}

void Ui::draw_settings(const Model* m) {
  if (!show_settings_) return;
  ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
  if (!ImGui::Begin(title("Settings").c_str(), &show_settings_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  bool changed = false;
  if (ImGui::BeginCombo(tr("Language"), lang_name(settings_.lang))) {
    for (auto l : {Lang::En, Lang::Ru}) {
      if (ImGui::Selectable(lang_name(l), settings_.lang == l)) {
        settings_.lang = l;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  changed |= ImGui::SliderFloat(tr("Default block size"), &settings_.block_scale, kMinBlockScale, kMaxBlockScale, "%.2f");
  changed |= ImGui::SliderFloat(tr("Interface text size"), &settings_.ui_scale, 0.8f, 1.6f, "%.2f");
  ImGui::TextUnformatted(tr("Link colors"));
  ImGui::SameLine();
  if (ImGui::RadioButton(tr("by block"), settings_.links_by_block)) {
    settings_.links_by_block = true;
    changed = true;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton(tr("by link kind"), !settings_.links_by_block)) {
    settings_.links_by_block = false;
    changed = true;
  }
  ImGui::TextWrapped("%s", tr("Drag the corner of a block to resize it; the text follows the size."));
  ImGui::TextWrapped("%s", tr("Hover a block to see only its links. Shift+click keeps a block's links lit (several "
                              "blocks can be lit); Shift+double-click opens a group in place."));
  if (m != nullptr && m->snap && ImGui::Button(tr("Reset block sizes in this view"))) save_view(*m, true);
  if (m != nullptr) ImGui::TextDisabled(tr("Layout engine: %s"), m->engine.c_str());
  ImGui::TextDisabled(tr("Settings are saved to %s"), settings_path().string().c_str());
  if (changed) settings_changed();
  ImGui::End();
}

// --- toolbar with breadcrumbs ---------------------------------------------------------------

void Ui::draw_toolbar(const Model& m) {
  if (ImGui::SmallButton(m.snap->project.empty() ? "root" : m.snap->project.c_str())) enter("");
  std::string acc;
  for (const auto& part : core::split(req_.scope, '.')) {
    acc = acc.empty() ? part : acc + "." + part;
    ImGui::SameLine();
    ImGui::TextUnformatted("›");
    ImGui::SameLine();
    if (ImGui::SmallButton(part.c_str())) enter(acc);
  }
  ImGui::SameLine(0, 24);
  ImGui::Checkbox(tr("All levels"), &req_.all_levels);
  if (req_.all_levels) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat(tr("zoom"), &zoom_, 0.2f, 1.5f, "%.2f");
  }
  ImGui::SameLine();
  ImGui::Checkbox(tr("use links"), &req_.show_use);
  ImGui::SameLine();
  ImGui::BeginDisabled(!m.can_undo);
  if (ImGui::SmallButton(tr("Undo"))) w_.undo();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::SmallButton(tr("Add block"))) {
    dialog_ = "add_block";
    target_ = req_.scope;
  }
  if (!pinned_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(rgb(255, 230, 120), tr("Highlighted: %zu"), pinned_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) pinned_.clear();
  }
  ImGui::SameLine();
  std::string status = m.engine + " · " + fmt::format(fmt::runtime(tr("index {:.0f} ms")), m.index_ms);
  if (!m.message.empty()) status += " · " + m.message;
  ImGui::TextDisabled("%s", status.c_str());
}

// --- tree ---------------------------------------------------------------------------------------

void Ui::draw_tree(const Model& m) {
  tree_hover_.clear();
  ImGui::Begin(title("Blocks").c_str());
  std::function<void(const std::string&)> node = [&](const std::string& id) {
    const auto* b = m.snap->block(id);
    if (b == nullptr) return;
    status_dot(b->status, active_blocks_.contains(id), pulse_);
    bool leaf = b->kind != "group";
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (leaf) fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (req_.selected == id) fl |= ImGuiTreeNodeFlags_Selected;
    if (index::Snapshot::under(id, req_.scope) && !leaf) fl |= ImGuiTreeNodeFlags_DefaultOpen;
    std::string label = core::last_segment(id) + "##" + id;
    bool open = ImGui::TreeNodeEx(label.c_str(), fl);
    if (ImGui::IsItemClicked()) select(id);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !leaf) enter(id);
    if (ImGui::IsItemHovered()) {
      tree_hover_ = id;
      ImGui::SetTooltip("%s [%s] %s\n%s", id.c_str(), tr_kind(b->kind), tr_status(b->status), b->purpose.c_str());
    }
    if (open && !leaf) {
      for (const auto& c : b->children) node(c);
      ImGui::TreePop();
    }
  };
  for (const auto& id : m.snap->top_level) node(id);
  ImGui::End();
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
  for (const auto& n : m.graph.nodes) {
    const auto* b = snap.block(n.id);
    if (b == nullptr) continue;
    auto nid = hid("node:" + n.id);
    node_of[nid] = n.id;
    if (apply_positions) {
      auto it = m.level.pos.find(n.id);
      if (it != m.level.pos.end()) {
        ed::SetNodePosition(nid, ImVec2(static_cast<float>(it->second.x), static_cast<float>(it->second.y)));
        placed_[n.id] = {static_cast<float>(it->second.x), static_cast<float>(it->second.y)};
      }
    }
    // Size of the block and the font scale that makes its text fit (text follows size).
    auto [w, h] = node_size(m, n.id);
    auto [bw, bh] = base_size(m, n.id);
    float font = std::clamp(std::min(w / bw, h / bh), 0.55f, 2.5f);
    float inner_w = w - 2 * kPadX;
    float inner_h = h - 2 * kPadY;

    bool active = active_blocks_.contains(n.id);
    ImVec4 fill = status_color(b->status);
    fill.w = 0.35f;
    ImVec4 border = active ? rgb(240, 200, 30, static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0))) : status_color(b->status);
    if (b->status == "needs_review") border = rgb(255, 140, 0);
    float border_w = active || b->status == "needs_review" ? 3.0f : 1.5f;
    bool dim = dimming && !lit_nodes.contains(n.id);
    if (pinned_.contains(n.id)) {
      border = rgb(255, 230, 120);
      border_w = 3.0f;
    } else if (dimming && focus.contains(n.id)) {
      border = rgb(255, 255, 255, 0.9f);
      border_w = 2.5f;
    }
    if (dim) {
      fill.w = 0.08f;
      border.w = 0.2f;
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
    // Line 1: name + kind · status (as much as fits).
    std::string name = fit(core::last_segment(n.id), inner_w);
    text_at(o.x, y, name, inner_w);
    float used = ImGui::CalcTextSize(name.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
    if (inner_w - used > ImGui::CalcTextSize("…").x * 3) {
      text_at(o.x + used, y, fmt::format("{} · {}", tr_kind(b->kind), tr_status(b->status)), inner_w - used, true);
    }
    y += lh;
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
      std::vector<const index::PortRow*> ins, outs;
      for (const auto* p : snap.ports_of(n.id)) (p->dir == "in" ? ins : outs).push_back(p);
      // Left column: in ports, then the implicit "links" pin (calls, uses).
      std::vector<std::pair<std::string, std::string>> left;  // key, label
      for (const auto* p : ins) left.emplace_back(pin_key(n.id, p->name, "in"), "● " + p->name);
      left.emplace_back(pin_key(n.id, "", "calls"), std::string("○ ") + tr("links"));
      std::vector<std::pair<std::string, std::string>> right;
      for (const auto* p : outs) right.emplace_back(pin_key(n.id, p->name, "out"), (p->dir == "fn" ? "fn " : "") + p->name + " ●");
      int need = static_cast<int>(std::max(left.size(), right.size()));
      int shown = need <= rows_fit ? need : std::max(0, rows_fit - 1);
      for (int i = 0; i < shown; ++i) {
        float ry = y + static_cast<float>(i) * lh;
        if (i < static_cast<int>(left.size())) {
          auto pid = pin(left[static_cast<std::size_t>(i)].first, left[static_cast<std::size_t>(i)].second, o.x, ry, true, half);
          auto parts = core::split(left[static_cast<std::size_t>(i)].first, ':', false);
          pin_of[pid] = n.id + ":" + parts[2] + ":" + (parts[3] == "calls" ? "calls" : "in");
        }
        if (i < static_cast<int>(right.size())) {
          const auto& [key, label] = right[static_cast<std::size_t>(i)];
          std::string t = fit(label, half);
          float tw = ImGui::CalcTextSize(t.c_str()).x;
          auto pid = pin(key, label, o.x + inner_w - tw, ry, false, half);
          auto parts = core::split(key, ':', false);
          pin_of[pid] = n.id + ":" + parts[2] + ":out";
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
    ImGui::PopStyleColor(2);
    ed::EndNode();
    ed::PopStyleVar(2);
    ed::PopStyleColor(2);
  }
  applied_layout_ = m.layout_key;

  // Links: port to port between leaves, otherwise lifted to group pins (3.6).
  auto endpoint = [&](const std::string& block, const std::string& port, const char* side) {
    auto key = pin_key(block, port, side);
    if (shown_pins.contains(key)) return hid(key);
    auto folded = pin_key(block, "+", std::string(side) == "calls" ? "in" : side);
    if (shown_pins.contains(folded)) return hid(folded);
    return hid(pin_key(block, "", side));
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
    ImVec4 c = link_color(kind);
    if (settings_.links_by_block) {
      auto idx = static_cast<std::size_t>(std::distance(units.begin(), units.find(start)));
      c = palette_color(idx, kind == "use" ? 0.6f : 0.9f);
    }
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
      // call/use/state: provider (callee, shared) → user, into its "links" pin.
      if (bb->kind == "group") {
        from = hid(pin_key(b, "", "out"));
      } else if (leaf_b && !l.dst_port.empty()) {
        from = endpoint(b, l.dst_port, "out");
      } else {
        from = endpoint(b, "", "calls");
      }
      to = leaf_a ? endpoint(a, "", "calls") : hid(pin_key(a, "", "in"));
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
      auto parts_a = core::split(pin_of[a.Get()], ':', false);
      auto parts_b = core::split(pin_of[b.Get()], ':', false);
      bool ok = parts_a.size() == 3 && parts_b.size() == 3 && parts_a[0] != parts_b[0] && parts_a[1] != "+" &&
                parts_b[1] != "+" && (!parts_a[1].empty() || !parts_b[1].empty());
      if (ok && ed::AcceptNewItem(rgb(120, 255, 120), 3.0f)) {
        std::string fa = parts_a[0], fp = parts_a[1], tb = parts_b[0], tp = parts_b[2] == "in" ? parts_b[1] : "";
        if (parts_a[2] == "in" || parts_a[1].empty()) {
          fa = parts_b[0];
          fp = parts_b[1];
          tb = parts_a[0];
          tp = parts_a[2] == "in" ? parts_a[1] : "";
        }
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
    if (b != nullptr && b->kind == "group" && ImGui::MenuItem(tr("Open"))) go_into = target_;
    if (b != nullptr && b->kind != "group" && ImGui::MenuItem(tr("Add port..."))) dialog_ = "add_port";
    if (ImGui::MenuItem(tr("Rename..."))) dialog_ = "rename";
    if (b != nullptr && b->kind != "group" && ImGui::MenuItem(tr("Split..."))) dialog_ = "split";
    if (b != nullptr && b->kind != "group" && ImGui::MenuItem(tr("Merge into..."))) dialog_ = "merge";
    if (ImGui::MenuItem(tr("Reset size"))) reset_one = true;
    if (ImGui::MenuItem(tr("Remove"))) {
      auto id = target_;
      w_.propose([=](const core::Schema& s) { return agents::op_remove_block(s, id); });
      preview_open_ = true;
    }
    if (b != nullptr && ImGui::MenuItem(tr("Open card in editor"))) w_.open_in_editor(b->dir + "/block.ksl", 1);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("bg_menu")) {
    if (ImGui::MenuItem(tr("Add block..."))) {
      dialog_ = "add_block";
      target_ = scope;
    }
    if (ImGui::MenuItem(tr("Fit to view"))) navigate_ = 1;
    if (ImGui::MenuItem(tr("Auto layout (reset positions and sizes)"))) auto_layout = true;
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
  if (ed::IsBackgroundClicked() && !shift) pinned_.clear();
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
  // Manual moves and sizes are saved and win over the automatic layout (14.2).
  bool moved = false;
  if (ImGui::IsMouseReleased(0) && !node_of.empty() && resizing_.empty()) {
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

// --- all levels (nested boxes) ----------------------------------------------------------------

void Ui::draw_all_levels(const Model& m) {
  ImGui::BeginChild("all", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float maxx = 0;
  float maxy = 0;
  std::map<std::string, ImVec2> center;
  for (const auto& b : m.boxes) {
    const auto* row = m.snap->block(b.id);
    if (row == nullptr) continue;
    ImVec2 a(o.x + static_cast<float>(b.x) * zoom_, o.y + static_cast<float>(b.y) * zoom_);
    ImVec2 z(a.x + static_cast<float>(b.w) * zoom_, a.y + static_cast<float>(b.h) * zoom_);
    maxx = std::max(maxx, static_cast<float>(b.x + b.w) * zoom_);
    maxy = std::max(maxy, static_cast<float>(b.y + b.h) * zoom_);
    ImVec4 c = status_color(row->status);
    float tw = z.x - a.x - 12;
    if (b.group) {
      dl->AddRect(a, z, ImGui::GetColorU32(c), 6.0f, 0, 1.5f);
      dl->AddText(ImVec2(a.x + 8, a.y + 6), ImGui::GetColorU32(ImGuiCol_Text), fit(b.id, tw).c_str());
    } else {
      ImVec4 f = c;
      f.w = 0.45f;
      dl->AddRectFilled(a, z, ImGui::GetColorU32(f), 4.0f);
      dl->AddRect(a, z, ImGui::GetColorU32(c), 4.0f, 0, active_blocks_.contains(b.id) ? 3.0f : 1.0f);
      dl->AddText(ImVec2(a.x + 6, a.y + 6), ImGui::GetColorU32(ImGuiCol_Text), fit(core::last_segment(b.id), tw).c_str());
      if (z.y - a.y > ImGui::GetTextLineHeight() * 2 + 8) {
        dl->AddText(ImVec2(a.x + 6, a.y + 6 + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    fit(tr_status(row->status), tw).c_str());
      }
      center[b.id] = ImVec2((a.x + z.x) / 2, (a.y + z.y) / 2);
      if (ImGui::IsMouseHoveringRect(a, z) && ImGui::IsMouseClicked(0)) select(b.id);
    }
  }
  for (const auto& l : m.snap->links) {
    if (l.kind == "use" && !req_.show_use) continue;
    if (!center.contains(l.src_block) || !center.contains(l.dst_block)) continue;
    auto a = center[l.src_block];
    auto b = center[l.dst_block];
    dl->AddBezierCubic(a, ImVec2(a.x + 40, a.y), ImVec2(b.x - 40, b.y), b, ImGui::GetColorU32(link_color(l.kind)), 1.8f);
  }
  ImGui::Dummy(ImVec2(maxx + 20, maxy + 20));
  ImGui::EndChild();
}

// --- inspector --------------------------------------------------------------------------------

void Ui::draw_inspector(const Model& m) {
  ImGui::Begin(title("Inspector").c_str());
  const auto* b = req_.selected.empty() ? nullptr : m.snap->block(req_.selected);
  if (b == nullptr) {
    ImGui::TextDisabled("%s", tr("Select a block in the tree or on the schema."));
    ImGui::End();
    return;
  }
  status_dot(b->status, active_blocks_.contains(b->id), pulse_);
  ImGui::Text("%s", b->id.c_str());
  if (b->thread.empty()) {
    ImGui::TextDisabled("%s · %s", tr_kind(b->kind), tr_status(b->status));
  } else {
    ImGui::TextDisabled("%s · %s · %s %s", tr_kind(b->kind), tr_status(b->status), tr("thread"), b->thread.c_str());
  }
  ImGui::TextWrapped("%s", b->purpose.c_str());
  if (ImGui::Button(tr("Open card in editor"))) w_.open_in_editor(b->dir + "/block.ksl", 1);
  if (b->kind == "group") {
    ImGui::SameLine();
    if (ImGui::Button(tr("Enter"))) enter(b->id);
  }
  if (ImGui::CollapsingHeader(title("Card (KSL)").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextUnformatted(m.card_text.c_str());
  }
  auto ports = m.snap->ports_of(b->id);
  if (!ports.empty() && ImGui::CollapsingHeader(title("Ports").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::BeginTable("ports", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      for (const auto* p : ports) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p->dir.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p->name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p->signature.c_str());
      }
      ImGui::EndTable();
    }
  }
  if (auto it = m.snap->invariants.find(b->id); it != m.snap->invariants.end() && ImGui::CollapsingHeader(title("Invariants").c_str())) {
    for (const auto& i : it->second) ImGui::BulletText("%s", i.c_str());
  }
  if (auto it = m.snap->history.find(b->id); it != m.snap->history.end() && ImGui::CollapsingHeader(title("History").c_str())) {
    for (const auto& [t, text] : it->second) ImGui::BulletText("%s %s", t.c_str(), text.c_str());
  }
  if (!m.files.empty() && ImGui::CollapsingHeader(title("Files").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
    for (const auto& f : m.files) {
      if (ImGui::Selectable(f.c_str(), req_.open_file == f)) req_.open_file = f;
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) w_.open_in_editor(f, 1);
    }
  }
  if (ImGui::CollapsingHeader(title("Model view (what agents get)").c_str())) ImGui::TextUnformatted(m.view_text.c_str());
  ImGui::End();
}

// --- code -------------------------------------------------------------------------------------------

void Ui::draw_code(const Model& m) {
  ImGui::Begin(title("Code").c_str());
  if (req_.open_file.empty()) {
    ImGui::TextDisabled("%s", tr("Pick a file in the inspector (double click opens the external editor)."));
    ImGui::End();
    return;
  }
  ImGui::TextUnformatted(req_.open_file.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton(tr("Open in editor"))) w_.open_in_editor(req_.open_file, code_->GetCursorPosition().mLine + 1);
  if (m.req.open_file == req_.open_file && code_loaded_ != req_.open_file + "\n" + m.code_text) {
    code_->SetText(m.code_text);
    code_loaded_ = req_.open_file + "\n" + m.code_text;
  }
  code_->Render("code");
  ImGui::End();
}

// --- bottom tabs (14.4) ------------------------------------------------------------------------------

void Ui::draw_bottom(const Model& m) {
  ImGui::Begin(title("Details").c_str());
  if (ImGui::BeginTabBar("tabs")) {
    if (ImGui::BeginTabItem(title("Tickets").c_str())) {
      if (ImGui::BeginTable("t", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                            ImVec2(ImGui::GetContentRegionAvail().x * 0.45f, 0))) {
        for (const auto& t : m.snap->tickets) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (ImGui::Selectable(t.id.c_str(), req_.ticket == t.id, ImGuiSelectableFlags_SpanAllColumns)) req_.ticket = t.id;
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(t.kind.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(t.status.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(t.title.c_str());
        }
        ImGui::EndTable();
      }
      ImGui::SameLine();
      ImGui::BeginChild("ticket");
      ImGui::TextUnformatted(m.ticket_text.empty() ? tr("(select a ticket)") : m.ticket_text.c_str());
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(title("Agents").c_str())) {
      ImGui::Text(tr("Working now: %zu"), m.active.size());
      for (const auto& a : m.active) {
        status_dot("", true, pulse_);
        ImGui::Text("%s %s (%s) %s", a.role.c_str(), a.block.c_str(), a.model.c_str(), a.ticket.c_str());
      }
      ImGui::Separator();
      if (ImGui::BeginTable("runs", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
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
    if (ImGui::BeginTabItem(title("Build & tests").c_str())) {
      if (ImGui::BeginTable("tests", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        for (const char* h : {"block", "contract", "unit", "integration", "failures"}) ImGui::TableSetupColumn(tr(h));
        ImGui::TableHeadersRow();
        for (const auto& t : m.snap->tests) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextColored(t.fresh ? ImVec4(1, 1, 1, 1) : rgb(150, 150, 150), "%s%s%s", t.block.c_str(), t.fresh ? "" : " ",
                             t.fresh ? "" : tr("(stale)"));
          for (const auto& c : {t.contract, t.unit, t.integration}) {
            ImGui::TableNextColumn();
            if (c.total > 0) ImGui::TextColored(c.pass == c.total ? rgb(90, 200, 90) : rgb(230, 80, 80), "%d/%d", c.pass, c.total);
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(t.build_failed ? tr("build failed") : (t.failures.empty() ? "" : t.failures.front().c_str()));
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(title("Costs").c_str())) {
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
      auto table = [](const char* name, const std::map<std::string, double>& v) {
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", tr(name));
        for (const auto& [k, c] : v) ImGui::Text("%-20s $%.4f", k.c_str(), c);
        ImGui::EndGroup();
        ImGui::SameLine(0, 40);
      };
      table("by block", by_block);
      table("by model", by_model);
      table("by ticket", by_ticket);
      ImGui::NewLine();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(title("Drift").c_str())) {
      for (const auto& d : m.snap->drift) {
        ImGui::TextColored(d.severity == "E" ? rgb(230, 80, 80) : rgb(230, 180, 60), "%s", d.str().c_str());
        if (ImGui::IsItemClicked() && !d.file.empty()) w_.open_in_editor(d.file, d.line);
      }
      if (m.snap->drift.empty()) ImGui::TextDisabled("%s", tr("no drift"));
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(title("Changes").c_str())) {
      ImGui::BeginChild("clist", ImVec2(ImGui::GetContentRegionAvail().x * 0.4f, 0));
      for (const auto& c : m.changes) {
        std::string label = fmt::format("{} [{}] {}", c.id, c.status.value, c.reason.value);
        if (ImGui::Selectable(label.c_str(), req_.change == c.id)) req_.change = c.id;
      }
      if (m.changes.empty()) ImGui::TextDisabled("%s", tr("no proposed changes"));
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("cview");
      for (const auto& c : m.changes) {
        if (c.id != req_.change || c.status.value != "pending") continue;
        if (ImGui::Button(tr("Approve"))) w_.approve_change(c.id);
        ImGui::SameLine();
        if (ImGui::Button(tr("Reject"))) w_.reject_change(c.id);
      }
      ImGui::TextUnformatted(m.change_preview.c_str());
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
}

// --- edit dialogs: form → KSL diff preview → apply (14.6) -------------------------------------------

void Ui::draw_dialogs(const Model& m) {
  if (!dialog_.empty() && !ImGui::IsPopupOpen(title("Edit").c_str())) {
    ImGui::OpenPopup(title("Edit").c_str());
    preview_open_ = false;
  }
  if (ImGui::BeginPopupModal(title("Edit").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    auto close = [&] {
      dialog_.clear();
      ImGui::CloseCurrentPopup();
    };
    if (dialog_ == "add_block") {
      ImGui::Text(tr("New block in %s"), target_.empty() ? tr("the project") : target_.c_str());
      ImGui::InputText(tr("name"), buf_name_, sizeof buf_name_);
      std::string kinds = std::string(tr_kind("group")) + '\0' + tr_kind("leaf") + '\0' + tr_kind("entry") + '\0' +
                          tr_kind("shared") + '\0' + '\0';
      ImGui::Combo(tr("kind"), &kind_, kinds.c_str());
      ImGui::InputText(tr("purpose"), buf_purpose_, sizeof buf_purpose_);
      ImGui::InputText(tr("thread"), buf_thread_, sizeof buf_thread_);
      if (ImGui::Button(tr("Preview"))) {
        std::string parent = target_, name = buf_name_, purpose = buf_purpose_, thread = buf_thread_;
        auto kind = static_cast<core::BlockKind>(kind_);
        w_.propose([=](const core::Schema& s) { return agents::op_add_block(s, parent, name, kind, purpose, thread); });
        preview_open_ = true;
        close();
      }
    } else if (dialog_ == "add_port") {
      ImGui::Text(tr("New port of %s"), target_.c_str());
      ImGui::Combo(tr("dir"), &dir_, "in\0out\0fn\0");
      if (dir_ != 2) ImGui::InputText(tr("name"), buf_name_, sizeof buf_name_);
      ImGui::InputText(dir_ == 2 ? tr("signature: name(params) -> R") : tr("type"), buf_spec_, sizeof buf_spec_);
      if (ImGui::Button(tr("Preview"))) {
        std::string id = target_, name = buf_name_, spec = buf_spec_;
        auto dir = static_cast<core::PortDir>(dir_);
        w_.propose([=](const core::Schema& s) { return agents::op_add_port(s, id, dir, name, spec); });
        preview_open_ = true;
        close();
      }
    } else if (dialog_ == "rename" || dialog_ == "merge" || dialog_ == "split") {
      const char* head = dialog_ == "rename" ? "Rename %s" : dialog_ == "merge" ? "Merge %s into" : "Split %s";
      const char* label = dialog_ == "rename" ? "new name" : dialog_ == "merge" ? "merge into (block id)" : "new block name";
      ImGui::Text(tr(head), target_.c_str());
      ImGui::InputText(tr(label), buf_name_, sizeof buf_name_);
      if (dialog_ == "split") ImGui::InputText(tr("ports to move (comma separated)"), buf_spec_, sizeof buf_spec_);
      if (ImGui::Button(tr("Preview"))) {
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
    if (ImGui::Button(tr("Cancel"))) close();
    ImGui::EndPopup();
  }
  if (preview_open_ && (m.has_pending || !m.pending_error.empty()) && !ImGui::IsPopupOpen(title("KSL diff").c_str())) {
    ImGui::OpenPopup(title("KSL diff").c_str());
  }
  if (ImGui::BeginPopupModal(title("KSL diff").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (!m.pending_error.empty()) {
      ImGui::TextColored(rgb(230, 80, 80), "%s", m.pending_error.c_str());
    } else {
      ImGui::TextUnformatted(m.pending_preview.c_str());
    }
    if (m.has_pending && m.pending_error.empty() && ImGui::Button(tr("Apply"))) {
      w_.confirm();
      preview_open_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Close"))) {
      w_.cancel();
      preview_open_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace kk::gui
