// Prototype mode of karkas-gui (docs/adr/0035): the human sketches ghost blocks and their
// dependencies on a level, writes the idea, and approves: the architect designs the level from
// both (karkas design|expand --sketch) — or, on a level that has blocks, the orchestrator adds
// them (karkas task) — and the program checks the sketch is kept. The sketch stays on the
// schema: blocks the schema has now take the places they were drawn at, the rest stay ghosts.
#include <fmt/format.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>

#include "i18n.hpp"
#include "icons.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace kk::gui {

namespace {

const ImVec4 kSketchCol = rgb(186, 126, 255);
constexpr std::array<const char*, 5> kKinds = {"", "group", "leaf", "entry", "shared"};

std::set<std::string> block_ids(const index::Snapshot& snap) {
  std::set<std::string> out;
  for (const auto& b : snap.blocks) out.insert(b.id);
  return out;
}

bool level_empty(const index::Snapshot& snap, const std::string& scope) {
  return std::none_of(snap.blocks.begin(), snap.blocks.end(), [&](const index::BlockRow& b) { return b.parent == scope; });
}

}  // namespace

void Ui::load_prototype() {
  auto s = agents::load_sketch(root_);
  sketch_ = s ? *s : agents::Sketch{};
  if (!s) sketch_msg_ = s.error().message;
  sketch_placed_.clear();
  sketch_positioned_.clear();
}

void Ui::save_prototype() {
  if (auto r = agents::save_sketch(root_, sketch_); !r) sketch_msg_ = r.error().message;
}

void Ui::set_prototype(bool on) {
  proto_on_ = on;
  if (!on) return;
  load_prototype();
  // A new sketch is drawn on the level shown; an existing one opens its own level.
  if (sketch_.empty()) {
    sketch_.scope = req_.scope;
    sketch_.sent.clear();
    sketch_.map.clear();
  } else if (sketch_.scope != req_.scope) {
    enter(sketch_.scope);
    sketch_msg_ = fmt::format(fmt::runtime(tr("The prototype is drawn on {}: opened it.")), sketch_.scope.empty() ? std::string("/") : sketch_.scope);
  }
  select_tab_ = "#schema";
}

void Ui::draw_prototype_bar(const Model& m) {
  if (!proto_on_) return;
  if (sketch_.scope != req_.scope) {
    ImGui::TextColored(kSketchCol, "%s", with_icon(Icon::Sketch, tr("PROTOTYPE")).c_str());
    ImGui::SameLine();
    ui::muted("%s", fmt::format(fmt::runtime(tr("drawn on {}")), sketch_.scope.empty() ? std::string("/") : sketch_.scope).c_str());
    ImGui::SameLine();
    if (ui::icon_button("pr_go", Icon::ArrowUp, tr("Open the level of the prototype"), false, tr("Open"))) enter(sketch_.scope);
    return;
  }
  place_realized(m);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(kSketchCol, "%s", with_icon(Icon::Sketch, tr("PROTOTYPE")).c_str());
  ImGui::SameLine(0, 10);
  // The idea: what is built (the architect reads it with the sketch).
  ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.42f));
  if (ImGui::InputTextWithHint("##idea", tr("the idea: what the project does..."), &sketch_.idea)) save_prototype();
  ImGui::SameLine(0, 6);
  if (ui::icon_button("pr_add", Icon::Plus, tr("Add a ghost block (or right click on the schema)"), false, tr("Block"))) sketch_add_center_ = true;
  ImGui::SameLine(0, 4);
  ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(kSketchCol, 0.55f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, with_alpha(kSketchCol, 0.75f));
  std::string send_label = with_icon(Icon::Check, sketch_.sent.empty() ? tr("Approve: to the architect") : tr("Send again"));
  if (ImGui::Button(send_label.c_str())) send_prototype(m);
  ImGui::PopStyleColor(2);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("%s", tr("The architect builds the level from the idea and this sketch: it keeps the blocks and dependencies "
                               "you drew (the program checks it), develops the rest, and offers better variants of critical places "
                               "as changes you approve or reject."));
  }
  ImGui::SameLine(0, 4);
  if (ui::icon_button("pr_clear", Icon::Trash, tr("Start a new prototype (the sketch is removed)"))) {
    (void)agents::clear_sketch(root_);
    sketch_ = agents::Sketch{};
    sketch_.scope = req_.scope;
    sketch_placed_.clear();
    sketch_positioned_.clear();
    sketch_msg_.clear();
  }
  ImGui::SameLine(0, 4);
  if (ui::icon_button("pr_done", Icon::Close, tr("Leave the prototype mode (the sketch is kept)"))) proto_on_ = false;
  // Counts and, once given to the agents, what the schema keeps.
  std::string info = fmt::format(fmt::runtime(tr("{} blocks, {} dependencies")), sketch_.blocks.size(), sketch_.links.size());
  if (!sketch_.sent.empty()) {
    agents::SchemaShape shape;
    for (const auto& b : m.snap->blocks) shape.blocks.insert(b.id);
    for (const auto& l : m.snap->links) shape.links.emplace_back(l.src_block, l.dst_block);
    auto c = agents::check_sketch(sketch_, shape);
    info += " · " + fmt::format(fmt::runtime(tr("kept: {}/{} blocks, {}/{} dependencies")), c.kept_blocks(), sketch_.blocks.size(),
                                c.kept_links(), sketch_.links.size());
  }
  ImGui::PushTextWrapPos(0);
  ui::muted("%s", (info + "  ·  " + tr("Drag from a ghost's right point to a block to draw a dependency; double click: edit; Delete removes.")).c_str());
  ImGui::PopTextWrapPos();
  if (!sketch_msg_.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(rgb(230, 170, 90), "%s", sketch_msg_.c_str());
    ImGui::PopTextWrapPos();
  }
  // The dependencies, each removable (they are lines, not items of the canvas).
  if (!sketch_.links.empty()) {
    int remove = -1;
    for (std::size_t i = 0; i < sketch_.links.size(); ++i) {
      const auto& l = sketch_.links[i];
      std::string chip = fmt::format("{} → {}", core::last_segment(l.from), core::last_segment(l.to));
      if (i > 0) ImGui::SameLine(0, 4);
      if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(chip.c_str()).x + 40 > ImGui::GetContentRegionMax().x) ImGui::NewLine();
      ImGui::PushID(static_cast<int>(i));
      ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(kSketchCol, 0.18f));
      if (ImGui::SmallButton((chip + "  " + icon(Icon::Close)).c_str())) remove = static_cast<int>(i);
      ImGui::PopStyleColor();
      ImGui::SetItemTooltip("%s", tr("Remove this dependency"));
      ImGui::PopID();
    }
    if (remove >= 0) {
      sketch_.links.erase(sketch_.links.begin() + remove);
      save_prototype();
    }
  }
}

void Ui::sketch_popups(const Model& m) {
  (void)m;
  if (ImGui::BeginPopup("sketch_menu")) {
    ImGui::TextDisabled("%s", sketch_target_.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(with_icon(Icon::Pencil, tr("Edit...")).c_str())) sketch_dialog_ = true;
    if (ImGui::MenuItem(with_icon(Icon::Link, tr("Remove its dependencies")).c_str())) {
      std::erase_if(sketch_.links, [&](const agents::SketchLink& l) { return l.from == sketch_target_ || l.to == sketch_target_; });
      save_prototype();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(241, 110, 110));
    if (ImGui::MenuItem(with_icon(Icon::Close, tr("Remove from the prototype")).c_str())) {
      auto id = sketch_target_;
      std::erase_if(sketch_.blocks, [&](const agents::SketchBlock& b) { return sketch_.id_of(b) == id; });
      std::erase_if(sketch_.links, [&](const agents::SketchLink& l) { return l.from == id || l.to == id; });
      save_prototype();
    }
    ImGui::PopStyleColor();
    ImGui::EndPopup();
  }
  // The block dialog: name, purpose, kind (new at sketch_new_pos_, or the block of sketch_target_).
  if (sketch_dialog_) {
    sketch_dialog_ = false;
    const auto* b = sketch_target_.empty() ? nullptr : sketch_.find(sketch_target_);
    sk_name_ = b != nullptr ? b->name : "";
    sk_purpose_ = b != nullptr ? b->purpose : "";
    sk_kind_ = 0;
    for (std::size_t k = 0; k < kKinds.size(); ++k) {
      if (b != nullptr && b->kind == kKinds[k]) sk_kind_ = static_cast<int>(k);
    }
    ImGui::OpenPopup("###sketch_block");
  }
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, 0), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal((with_icon(Icon::Sketch, tr("Prototype block")) + "###sketch_block").c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    bool editing = !sketch_target_.empty();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputTextWithHint("##sk_name", tr("name: audio, ui, storage..."), &sk_name_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth(-1);
    enter |= ImGui::InputTextWithHint("##sk_purpose", tr("what it does (optional: the architect writes it)"), &sk_purpose_,
                                      ImGuiInputTextFlags_EnterReturnsTrue);
    std::array<std::string, 5> labels = {tr("the architect decides"), tr_kind("group"), tr_kind("leaf"), tr_kind("entry"), tr_kind("shared")};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##sk_kind", labels[static_cast<std::size_t>(sk_kind_)].c_str())) {
      for (std::size_t k = 0; k < labels.size(); ++k) {
        if (ImGui::Selectable(labels[k].c_str(), sk_kind_ == static_cast<int>(k))) sk_kind_ = static_cast<int>(k);
      }
      ImGui::EndCombo();
    }
    std::string name(core::trim(sk_name_));
    std::string id = sketch_.scope.empty() ? name : sketch_.scope + "." + name;
    std::string problem;
    if (!name.empty() && !core::is_ident(name)) problem = tr("a name is a-z, 0-9 and _, starting with a letter");
    if (problem.empty() && id != sketch_target_ && sketch_.find(id) != nullptr) problem = tr("the prototype has a block of this name");
    if (problem.empty() && m.snap->block(id) != nullptr) problem = tr("a block of this name exists: link to it instead");
    if (!problem.empty()) ImGui::TextColored(rgb(241, 120, 120), "%s", problem.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(name.empty() || !problem.empty());
    ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
    bool ok = ImGui::Button(editing ? tr("Save") : tr("Add")) || (enter && !name.empty() && problem.empty());
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ok && !name.empty() && problem.empty()) {
      std::string kind = kKinds[static_cast<std::size_t>(sk_kind_)];
      std::string purpose(core::trim(sk_purpose_));
      if (editing) {
        for (auto& sb : sketch_.blocks) {
          if (sketch_.id_of(sb) != sketch_target_) continue;
          sb.name = name;
          sb.purpose = purpose;
          sb.kind = kind;
        }
        for (auto& l : sketch_.links) {  // a renamed block keeps its dependencies
          if (l.from == sketch_target_) l.from = id;
          if (l.to == sketch_target_) l.to = id;
        }
        sketch_placed_.erase(sketch_target_);
      } else {
        sketch_.blocks.push_back({name, purpose, kind, sketch_new_pos_.x, sketch_new_pos_.y});
      }
      save_prototype();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void Ui::send_prototype(const Model& m) {
  const auto& snap = *m.snap;
  auto existing = block_ids(snap);
  if (!sketch_.sent.empty()) {
    // Sent before: what the schema does not have yet goes to the orchestrator as a task.
    agents::SchemaShape shape;
    shape.blocks = existing;
    for (const auto& l : snap.links) shape.links.emplace_back(l.src_block, l.dst_block);
    auto c = agents::check_sketch(sketch_, shape);
    agents::Sketch rest = sketch_;
    rest.blocks.clear();
    rest.links.clear();
    for (const auto& b : sketch_.blocks) {
      if (c.found[sketch_.id_of(b)].empty()) rest.blocks.push_back(b);
    }
    for (std::size_t i = 0; i < sketch_.links.size() && i < c.links.size(); ++i) {
      if (c.links[i] == 0) rest.links.push_back(sketch_.links[i]);
    }
    if (rest.empty()) {
      sketch_msg_ = tr("Everything of the prototype is in the schema (dependencies through groups come when they are expanded).");
      return;
    }
    if (!chat_ || chat_->busy()) {
      sketch_msg_ = tr("The chat runs a command now: wait for it, then approve again.");
      return;
    }
    ChatScope where = sketch_.scope.empty() ? ChatScope{} : ChatScope{ChatScope::Kind::Block, sketch_.scope};
    if (chat_->send(ChatMode::Task, agents::sketch_task(rest), where)) {
      sketch_.sent_at = core::unix_now();
      save_prototype();
      sketch_msg_ = tr("What the schema lacks of the prototype went to the orchestrator: follow it in the chat.");
    }
    show_chat_ = true;
    chat_window_focus_ = true;
    return;
  }
  if (auto why = agents::sketch_problems(sketch_, existing); !why.empty()) {
    sketch_msg_ = why;
    return;
  }
  if (!chat_ || chat_->busy()) {
    sketch_msg_ = tr("The chat runs a command now: wait for it, then approve again.");
    return;
  }
  const std::string& scope = sketch_.scope;
  bool empty = level_empty(snap, scope);
  const auto* group = scope.empty() ? nullptr : snap.block(scope);
  if (scope.empty() && empty && core::trim(sketch_.idea).empty() && snap.idea.empty()) {
    sketch_msg_ = tr("Write the idea first: the architect builds from the idea and the sketch.");
    return;
  }
  save_prototype();
  bool sent = false;
  if (scope.empty() && empty) {
    sent = chat_->send(ChatMode::Task, "/design --sketch");  // the architect designs the top level from the sketch
    sketch_.sent = "design";
  } else if (group != nullptr && group->kind == "group" && empty) {
    sent = chat_->send(ChatMode::Task, "/expand " + scope + " --sketch");
    sketch_.sent = "expand";
  } else {
    // A level that has blocks: the orchestrator adds what the sketch shows (a change to approve).
    ChatScope where = scope.empty() ? ChatScope{} : ChatScope{ChatScope::Kind::Block, scope};
    sent = chat_->send(ChatMode::Task, agents::sketch_task(sketch_), where);
    sketch_.sent = "task";
  }
  if (!sent) {
    sketch_.sent.clear();
    sketch_msg_ = tr("The chat could not start the command (see the chat).");
  } else {
    sketch_.sent_at = core::unix_now();
    save_prototype();
    sketch_msg_ = tr("Given to the agents: follow it in the chat. Blocks the schema gets take the places you drew them at.");
  }
  show_chat_ = true;
  chat_window_focus_ = true;
}

void Ui::place_realized(const Model& m) {
  // Once the schema has the blocks of the prototype, each takes the place it was drawn at (once;
  // the human moves it freely after), so the level looks the way the human drew it.
  if (sketch_.sent.empty() || m.req.scope != sketch_.scope) return;
  std::map<std::string, layout::Pos> add;
  for (const auto& sb : sketch_.blocks) {
    auto id = sketch_.id_of(sb);
    auto it = sketch_.map.find(id);
    std::string real = it != sketch_.map.end() ? it->second : id;
    if (m.snap->block(real) == nullptr || sketch_positioned_.contains(real) || m.manual.contains(real)) continue;
    bool shown = std::any_of(m.graph.nodes.begin(), m.graph.nodes.end(), [&](const layout::Node& n) { return n.id == real; });
    if (!shown) continue;
    add[real] = layout::Pos{sb.x, sb.y, 0, 0};
  }
  if (add.empty()) return;
  auto all = m.manual;
  for (const auto& [id, p] : add) {
    all[id] = p;
    sketch_positioned_.insert(id);
    sketch_apply_[id] = ImVec2(static_cast<float>(p.x), static_cast<float>(p.y));  // at once, not at the next model
  }
  w_.save_positions(m.req.scope.empty() ? "root" : m.req.scope, all);
}

}  // namespace kk::gui
