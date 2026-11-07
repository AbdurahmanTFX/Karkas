#include "ui.hpp"

#include <TextEditor.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <cmath>
#include <map>

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

std::uintptr_t hid(std::string_view s) {
  auto h = static_cast<std::uintptr_t>(core::fnv1a64(s) & 0x7fffffffffffULL);
  return h == 0 ? 1 : h;
}

std::string pin_key(const std::string& block, const std::string& port, const char* side) {
  return fmt::format("pin:{}:{}:{}", block, port, side);
}

std::string child_of(const std::string& scope, const std::string& id) {
  if (!index::Snapshot::under(scope, id)) return {};
  std::string rest = id;
  if (!scope.empty()) {
    if (id.size() == scope.size()) return {};
    rest = id.substr(scope.size() + 1);
  }
  auto head = rest.substr(0, rest.find('.'));
  return scope.empty() ? head : scope + "." + head;
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

}  // namespace

Ui::Ui(Worker& w) : w_(w), code_(std::make_unique<TextEditor>()) {
  ed::Config cfg;
  cfg.SettingsName = nullptr;  // positions live in .karkas/layout (14.2), not imgui.ini
  ed_ = ed::CreateEditor(&cfg);
  code_->SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
  code_->SetReadOnly(true);
  code_->SetPalette(TextEditor::GetDarkPalette());
}

Ui::~Ui() { ed::DestroyEditor(ed_); }

void Ui::enter(const std::string& scope) {
  req_.scope = scope == "root" ? "" : scope;
  applied_layout_.clear();
}

void Ui::select(const std::string& id) {
  req_.selected = id;
}

bool Ui::draw(const std::shared_ptr<const Model>& mp, bool first_frame) {
  pulse_ = ImGui::GetTime();
  ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
  if (first_frame && ImGui::DockBuilderGetNode(dock) != nullptr && ImGui::DockBuilderGetNode(dock)->IsLeafNode()) {
    ImGui::DockBuilderRemoveNodeChildNodes(dock);
    ImGuiID center = dock;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Blocks", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Code", right);
    ImGui::DockBuilderDockWindow("Schema", center);
    ImGui::DockBuilderDockWindow("Details", bottom);
    ImGui::DockBuilderFinish(dock);
  }
  if (!mp || !mp->snap) {
    ImGui::Begin("Schema");
    ImGui::TextUnformatted(mp ? mp->message.c_str() : "indexing...");
    ImGui::End();
    return true;
  }
  const Model& m = *mp;
  active_blocks_.clear();
  for (const auto& a : m.active) active_blocks_.insert(a.block);
  draw_tree(m);
  ImGui::Begin("Schema");
  draw_toolbar(m);
  if (req_.all_levels) {
    draw_all_levels(m);
  } else {
    draw_graph(m);
  }
  ImGui::End();
  draw_inspector(m);
  draw_code(m);
  draw_bottom(m);
  draw_dialogs(m);
  return !m.active.empty();
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
  ImGui::Checkbox("All levels", &req_.all_levels);
  if (req_.all_levels) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("zoom", &zoom_, 0.2f, 1.5f, "%.2f");
  }
  ImGui::SameLine();
  ImGui::Checkbox("use links", &req_.show_use);
  ImGui::SameLine();
  ImGui::BeginDisabled(!m.can_undo);
  if (ImGui::SmallButton("Undo")) w_.undo();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::SmallButton("Add block")) {
    dialog_ = "add_block";
    target_ = req_.scope;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s · index %.0f ms · %s", m.engine.c_str(), m.index_ms, m.message.c_str());
}

// --- tree ---------------------------------------------------------------------------------------

void Ui::draw_tree(const Model& m) {
  ImGui::Begin("Blocks");
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s [%s] %s\n%s", id.c_str(), b->kind.c_str(), b->status.c_str(), b->purpose.c_str());
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
  std::map<std::uintptr_t, std::string> node_of, pin_of;  // id → block / "block:port:side"
  bool apply_positions = applied_layout_ != m.layout_key;
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
    bool active = active_blocks_.contains(n.id);
    ImVec4 fill = status_color(b->status);
    fill.w = 0.35f;
    ImVec4 border = active ? rgb(240, 200, 30, static_cast<float>(0.6 + 0.4 * std::sin(pulse_ * 5.0))) : status_color(b->status);
    if (b->status == "needs_review") border = rgb(255, 140, 0);
    ed::PushStyleColor(ed::StyleColor_NodeBg, fill);
    ed::PushStyleColor(ed::StyleColor_NodeBorder, border);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, active || b->status == "needs_review" ? 3.0f : 1.5f);
    ed::BeginNode(nid);
    ImGui::TextUnformatted(core::last_segment(n.id).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s · %s", b->kind.c_str(), b->status.c_str());
    std::string purpose = b->purpose.size() > 34 ? b->purpose.substr(0, 33) + "…" : b->purpose;
    if (b->kind == "group") purpose += fmt::format(" ({})", b->children.size());
    ImGui::TextUnformatted(purpose.c_str());
    if (b->kind == "group") {
      auto in = hid(pin_key(n.id, "", "in"));
      auto out = hid(pin_key(n.id, "", "out"));
      pin_of[in] = n.id + "::in";
      pin_of[out] = n.id + "::out";
      ed::BeginPin(in, ed::PinKind::Input);
      ImGui::TextUnformatted("▶");
      ed::EndPin();
      ImGui::SameLine(0, 60);
      ed::BeginPin(out, ed::PinKind::Output);
      ImGui::TextUnformatted("▶");
      ed::EndPin();
    } else {
      std::vector<const index::PortRow*> ins, outs;
      for (const auto* p : snap.ports_of(n.id)) (p->dir == "in" ? ins : outs).push_back(p);
      // Calls and uses arrive on an implicit "links" pin.
      auto calls = hid(pin_key(n.id, "", "calls"));
      pin_of[calls] = n.id + "::calls";
      std::size_t rows = std::max(ins.size() + 1, outs.size());
      for (std::size_t i = 0; i < rows; ++i) {
        if (i < ins.size()) {
          auto pid = hid(pin_key(n.id, ins[i]->name, "in"));
          pin_of[pid] = n.id + ":" + ins[i]->name + ":in";
          ed::BeginPin(pid, ed::PinKind::Input);
          ImGui::Text("● %s", ins[i]->name.c_str());
          ed::EndPin();
        } else if (i == ins.size()) {
          ed::BeginPin(calls, ed::PinKind::Input);
          ImGui::TextDisabled("○ links");
          ed::EndPin();
        } else {
          ImGui::TextUnformatted(" ");
        }
        if (i < outs.size()) {
          ImGui::SameLine(0, 30);
          auto pid = hid(pin_key(n.id, outs[i]->name, "out"));
          pin_of[pid] = n.id + ":" + outs[i]->name + ":out";
          ed::BeginPin(pid, ed::PinKind::Output);
          ImGui::Text("%s %s ●", outs[i]->dir == "fn" ? "fn" : "", outs[i]->name.c_str());
          ed::EndPin();
        }
      }
    }
    ed::EndNode();
    ed::PopStyleVar(1);
    ed::PopStyleColor(2);
  }
  applied_layout_ = m.layout_key;

  // Links: port to port between leaves, otherwise lifted to group pins (3.6).
  std::map<std::uintptr_t, const index::LinkRow*> link_of;
  for (const auto& l : snap.links) {
    if (l.kind == "use" && !req_.show_use) continue;
    auto a = child_of(req_.scope, l.src_block);
    auto b = child_of(req_.scope, l.dst_block);
    if (a.empty() || b.empty() || a == b) continue;
    if (!node_of.contains(hid("node:" + a)) || !node_of.contains(hid("node:" + b))) continue;
    const auto* ba = snap.block(a);
    const auto* bb = snap.block(b);
    std::uintptr_t from = 0;
    std::uintptr_t to = 0;
    bool leaf_a = ba->kind != "group" && a == l.src_block;
    bool leaf_b = bb->kind != "group" && b == l.dst_block;
    if (l.kind == "data" || l.kind == "event") {
      from = leaf_a ? hid(pin_key(a, l.src_port, "out")) : hid(pin_key(a, "", "out"));
      to = leaf_b ? hid(pin_key(b, l.dst_port, "in")) : hid(pin_key(b, "", "in"));
    } else {
      // call/use/state: provider (callee, shared) → user; drawn into the "links" pin.
      from = leaf_b && !l.dst_port.empty() ? hid(pin_key(b, l.dst_port, "out")) : hid(pin_key(b, "", bb->kind == "group" ? "out" : "calls"));
      to = leaf_a ? hid(pin_key(a, "", "calls")) : hid(pin_key(a, "", "in"));
      if (!leaf_b || l.dst_port.empty()) from = bb->kind == "group" ? hid(pin_key(b, "", "out")) : hid(pin_key(b, "", "calls"));
    }
    auto lid = hid(fmt::format("link:{}", l.id));
    link_of[lid] = &l;
    float thick = l.kind == "use" ? 1.0f : (l.kind == "state" ? 3.0f : 2.0f);
    ed::Link(lid, from, to, link_color(l.kind), thick);
  }

  // Draw a link from an out port or fn to a consumer (14.6).
  if (ed::BeginCreate(rgb(255, 255, 255), 2.0f)) {
    ed::PinId a;
    ed::PinId b;
    if (ed::QueryNewLink(&a, &b) && a && b) {
      auto pa = pin_of[a.Get()];
      auto pb = pin_of[b.Get()];
      auto parts_a = core::split(pa, ':', false);
      auto parts_b = core::split(pb, ':', false);
      bool ok = parts_a.size() == 3 && parts_b.size() == 3 && !parts_a[1].empty() && parts_a[0] != parts_b[0];
      if (ok && ed::AcceptNewItem(rgb(120, 255, 120), 3.0f)) {
        std::string fa = parts_a[0], fp = parts_a[1], tb = parts_b[0], tp = parts_b[2] == "in" ? parts_b[1] : "";
        if (parts_a[2] == "in") std::swap(fa, tb), fp = parts_b[1], tp = parts_a[1];
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
  if (dbl && node_of.contains(dbl.Get())) {
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
  // Manual moves are saved and win over the automatic layout (14.2).
  if (ImGui::IsMouseReleased(0) && !node_of.empty()) {
    std::map<std::string, layout::Pos> moved;
    bool any = false;
    for (const auto& [nid2, id] : node_of) {
      auto p = ed::GetNodePosition(nid2);
      auto it = placed_.find(id);
      if (it == placed_.end() || std::fabs(it->second.first - p.x) > 1 || std::fabs(it->second.second - p.y) > 1) any = true;
      moved[id] = layout::Pos{p.x, p.y};
      placed_[id] = {p.x, p.y};
    }
    if (any) w_.save_positions(req_.scope.empty() ? "root" : req_.scope, moved);
  }
  ed::Suspend();
  ed::NodeId ctx;
  if (ed::ShowNodeContextMenu(&ctx) && node_of.contains(ctx.Get())) {
    target_ = node_of[ctx.Get()];
    ImGui::OpenPopup("node_menu");
  } else if (ed::ShowBackgroundContextMenu()) {
    ImGui::OpenPopup("bg_menu");
  }
  if (ImGui::BeginPopup("node_menu")) {
    const auto* b = snap.block(target_);
    ImGui::TextDisabled("%s", target_.c_str());
    if (b != nullptr && b->kind == "group" && ImGui::MenuItem("Open")) go_into = target_;
    if (b != nullptr && b->kind != "group" && ImGui::MenuItem("Add port...")) dialog_ = "add_port";
    if (ImGui::MenuItem("Rename...")) dialog_ = "rename";
    if (b != nullptr && b->kind != "group" && ImGui::MenuItem("Split...")) dialog_ = "split";
    if (b != nullptr && b->kind != "group" && ImGui::MenuItem("Merge into...")) dialog_ = "merge";
    if (ImGui::MenuItem("Remove")) {
      auto id = target_;
      w_.propose([=](const core::Schema& s) { return agents::op_remove_block(s, id); });
      preview_open_ = true;
    }
    if (b != nullptr && ImGui::MenuItem("Open card in editor")) w_.open_in_editor(b->dir + "/block.ksl", 1);
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("bg_menu")) {
    if (ImGui::MenuItem("Add block...")) {
      dialog_ = "add_block";
      target_ = req_.scope;
    }
    if (ImGui::MenuItem("Fit to view")) ed::NavigateToContent();
    ImGui::EndPopup();
  }
  ed::Resume();
  if (apply_positions) navigate_ = 3;
  if (navigate_ > 0 && --navigate_ == 0) ed::NavigateToContent(0.0f);
  ed::End();
  ed::SetCurrentEditor(nullptr);
  if (!go_into.empty()) enter(go_into);
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
    if (b.group) {
      dl->AddRect(a, z, ImGui::GetColorU32(c), 6.0f, 0, 1.5f);
      dl->AddText(ImVec2(a.x + 8, a.y + 6), ImGui::GetColorU32(ImGuiCol_Text), b.id.c_str());
    } else {
      ImVec4 f = c;
      f.w = 0.45f;
      dl->AddRectFilled(a, z, ImGui::GetColorU32(f), 4.0f);
      dl->AddRect(a, z, ImGui::GetColorU32(c), 4.0f, 0, active_blocks_.contains(b.id) ? 3.0f : 1.0f);
      dl->AddText(ImVec2(a.x + 6, a.y + 6), ImGui::GetColorU32(ImGuiCol_Text), core::last_segment(b.id).c_str());
      dl->AddText(ImVec2(a.x + 6, a.y + 6 + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled), row->status.c_str());
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
  ImGui::Begin("Inspector");
  const auto* b = req_.selected.empty() ? nullptr : m.snap->block(req_.selected);
  if (b == nullptr) {
    ImGui::TextDisabled("Select a block in the tree or on the schema.");
    ImGui::End();
    return;
  }
  status_dot(b->status, active_blocks_.contains(b->id), pulse_);
  ImGui::Text("%s", b->id.c_str());
  ImGui::TextDisabled("%s · %s%s%s", b->kind.c_str(), b->status.c_str(), b->thread.empty() ? "" : " · thread ",
                      b->thread.c_str());
  ImGui::TextWrapped("%s", b->purpose.c_str());
  if (ImGui::Button("Open card in editor")) w_.open_in_editor(b->dir + "/block.ksl", 1);
  if (b->kind == "group") {
    ImGui::SameLine();
    if (ImGui::Button("Enter")) enter(b->id);
  }
  if (ImGui::CollapsingHeader("Card (KSL)", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextUnformatted(m.card_text.c_str());
  }
  auto ports = m.snap->ports_of(b->id);
  if (!ports.empty() && ImGui::CollapsingHeader("Ports", ImGuiTreeNodeFlags_DefaultOpen)) {
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
  if (auto it = m.snap->invariants.find(b->id); it != m.snap->invariants.end() && ImGui::CollapsingHeader("Invariants")) {
    for (const auto& i : it->second) ImGui::BulletText("%s", i.c_str());
  }
  if (auto it = m.snap->history.find(b->id); it != m.snap->history.end() && ImGui::CollapsingHeader("History")) {
    for (const auto& [t, text] : it->second) ImGui::BulletText("%s %s", t.c_str(), text.c_str());
  }
  if (!m.files.empty() && ImGui::CollapsingHeader("Files", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (const auto& f : m.files) {
      if (ImGui::Selectable(f.c_str(), req_.open_file == f)) req_.open_file = f;
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) w_.open_in_editor(f, 1);
    }
  }
  if (ImGui::CollapsingHeader("Model view (what agents get)")) ImGui::TextUnformatted(m.view_text.c_str());
  ImGui::End();
}

// --- code -------------------------------------------------------------------------------------------

void Ui::draw_code(const Model& m) {
  ImGui::Begin("Code");
  if (req_.open_file.empty()) {
    ImGui::TextDisabled("Pick a file in the inspector (double click opens the external editor).");
    ImGui::End();
    return;
  }
  ImGui::TextUnformatted(req_.open_file.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Open in editor")) w_.open_in_editor(req_.open_file, code_->GetCursorPosition().mLine + 1);
  if (m.req.open_file == req_.open_file && code_loaded_ != req_.open_file + "\n" + m.code_text) {
    code_->SetText(m.code_text);
    code_loaded_ = req_.open_file + "\n" + m.code_text;
  }
  code_->Render("code");
  ImGui::End();
}

// --- bottom tabs (14.4) ------------------------------------------------------------------------------

void Ui::draw_bottom(const Model& m) {
  ImGui::Begin("Details");
  if (ImGui::BeginTabBar("tabs")) {
    if (ImGui::BeginTabItem("Tickets")) {
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
      ImGui::TextUnformatted(m.ticket_text.empty() ? "(select a ticket)" : m.ticket_text.c_str());
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Agents")) {
      ImGui::Text("Working now: %zu", m.active.size());
      for (const auto& a : m.active) {
        status_dot("", true, pulse_);
        ImGui::Text("%s %s (%s) %s", a.role.c_str(), a.block.c_str(), a.model.c_str(), a.ticket.c_str());
      }
      ImGui::Separator();
      if (ImGui::BeginTable("runs", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        for (const char* h : {"role", "block", "tier", "outcome", "tokens in/out", "cache read", "usd"}) ImGui::TableSetupColumn(h);
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
    if (ImGui::BeginTabItem("Build & tests")) {
      if (ImGui::BeginTable("tests", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        for (const char* h : {"block", "contract", "unit", "integration", "failures"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const auto& t : m.snap->tests) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextColored(t.fresh ? ImVec4(1, 1, 1, 1) : rgb(150, 150, 150), "%s%s", t.block.c_str(), t.fresh ? "" : " (stale)");
          for (const auto& c : {t.contract, t.unit, t.integration}) {
            ImGui::TableNextColumn();
            if (c.total > 0) ImGui::TextColored(c.pass == c.total ? rgb(90, 200, 90) : rgb(230, 80, 80), "%d/%d", c.pass, c.total);
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(t.build_failed ? "build failed" : (t.failures.empty() ? "" : t.failures.front().c_str()));
        }
        ImGui::EndTable();
      }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Costs")) {
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
      ImGui::Text("total $%.4f · %zu runs · cache read share %.1f%%", total, m.runs.size(), share);
      auto table = [](const char* title, const std::map<std::string, double>& v) {
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", title);
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
    if (ImGui::BeginTabItem("Drift")) {
      for (const auto& d : m.snap->drift) {
        ImGui::TextColored(d.severity == "E" ? rgb(230, 80, 80) : rgb(230, 180, 60), "%s", d.str().c_str());
        if (ImGui::IsItemClicked() && !d.file.empty()) w_.open_in_editor(d.file, d.line);
      }
      if (m.snap->drift.empty()) ImGui::TextDisabled("no drift");
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Changes")) {
      ImGui::BeginChild("clist", ImVec2(ImGui::GetContentRegionAvail().x * 0.4f, 0));
      for (const auto& c : m.changes) {
        std::string label = fmt::format("{} [{}] {}", c.id, c.status.value, c.reason.value);
        if (ImGui::Selectable(label.c_str(), req_.change == c.id)) req_.change = c.id;
      }
      if (m.changes.empty()) ImGui::TextDisabled("no proposed changes");
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("cview");
      for (const auto& c : m.changes) {
        if (c.id != req_.change || c.status.value != "pending") continue;
        if (ImGui::Button("Approve")) w_.approve_change(c.id);
        ImGui::SameLine();
        if (ImGui::Button("Reject")) w_.reject_change(c.id);
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
  if (!dialog_.empty()) {
    ImGui::OpenPopup("Edit");
    preview_open_ = false;
  }
  if (ImGui::BeginPopupModal("Edit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    auto close = [&] {
      dialog_.clear();
      ImGui::CloseCurrentPopup();
    };
    if (dialog_ == "add_block") {
      ImGui::Text("New block in %s", target_.empty() ? "the project" : target_.c_str());
      ImGui::InputText("name", buf_name_, sizeof buf_name_);
      ImGui::Combo("kind", &kind_, "group\0leaf\0entry\0shared\0");
      ImGui::InputText("purpose", buf_purpose_, sizeof buf_purpose_);
      ImGui::InputText("thread", buf_thread_, sizeof buf_thread_);
      if (ImGui::Button("Preview")) {
        std::string parent = target_, name = buf_name_, purpose = buf_purpose_, thread = buf_thread_;
        auto kind = static_cast<core::BlockKind>(kind_);
        w_.propose([=](const core::Schema& s) { return agents::op_add_block(s, parent, name, kind, purpose, thread); });
        preview_open_ = true;
        close();
      }
    } else if (dialog_ == "add_port") {
      ImGui::Text("New port of %s", target_.c_str());
      ImGui::Combo("dir", &dir_, "in\0out\0fn\0");
      if (dir_ != 2) ImGui::InputText("name", buf_name_, sizeof buf_name_);
      ImGui::InputText(dir_ == 2 ? "signature: name(params) -> R" : "type", buf_spec_, sizeof buf_spec_);
      if (ImGui::Button("Preview")) {
        std::string id = target_, name = buf_name_, spec = buf_spec_;
        auto dir = static_cast<core::PortDir>(dir_);
        w_.propose([=](const core::Schema& s) { return agents::op_add_port(s, id, dir, name, spec); });
        preview_open_ = true;
        close();
      }
    } else if (dialog_ == "rename" || dialog_ == "merge" || dialog_ == "split") {
      const char* label = dialog_ == "rename" ? "new name" : dialog_ == "merge" ? "merge into (block id)" : "new block name";
      ImGui::Text("%s %s", dialog_.c_str(), target_.c_str());
      ImGui::InputText(label, buf_name_, sizeof buf_name_);
      if (dialog_ == "split") ImGui::InputText("ports to move (comma separated)", buf_spec_, sizeof buf_spec_);
      if (ImGui::Button("Preview")) {
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
    if (ImGui::Button("Cancel")) close();
    ImGui::EndPopup();
  }
  if (preview_open_ && (m.has_pending || !m.pending_error.empty())) ImGui::OpenPopup("KSL diff");
  if (ImGui::BeginPopupModal("KSL diff", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (!m.pending_error.empty()) {
      ImGui::TextColored(rgb(230, 80, 80), "%s", m.pending_error.c_str());
    } else {
      ImGui::TextUnformatted(m.pending_preview.c_str());
    }
    if (m.has_pending && m.pending_error.empty() && ImGui::Button("Apply")) {
      w_.confirm();
      preview_open_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      w_.cancel();
      preview_open_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace kk::gui
