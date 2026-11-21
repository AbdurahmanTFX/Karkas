// Notes projects in karkas-gui (docs/adr/0038): "Programming | Notes" in the Karkas menu (two workspaces,
// each with its last project and its recent ones); in a notes project the schema holds topics and
// notes, a note's text is edited in an editor tab (saved by itself after a pause, Ctrl+S, on closing;
// a Markdown note is also shown rendered, docs/adr/0040), [[links]] are the lines between notes (drawn
// on the schema they are written into the text), and the inspector shows a note's text, its links and
// the notes that link to it.
#include <cstdint>
#include <TextEditor.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <filesystem>
#include <limits>

#include "drop.hpp"
#include "i18n.hpp"
#include "icons.hpp"
#include "kk/agents/edit.hpp"
#include "kk/core/markdown.hpp"
#include "kk/core/notes.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace kk::gui {

namespace fs = std::filesystem;

namespace {

constexpr double kAutosaveAfter = 1.5;  // seconds without typing

const ImVec4 kNoteColor = rgb(120, 170, 240);
const ImVec4 kTopicColor = rgb(220, 182, 122);

}  // namespace

std::string editor_text(const TextEditor& ed) {
  auto text = ed.GetText();
  if (text.ends_with('\n')) text.pop_back();
  return text;
}

// --- the two workspaces --------------------------------------------------------------------------

void Ui::draw_mode_items() {
  if (ImGui::MenuItem(with_icon(Icon::Code, tr("Programming")).c_str(), nullptr, !notes_ && !root_.empty())) switch_mode(false);
  ImGui::SetItemTooltip("%s", tr("Code projects: the schema of blocks, agents, build and run"));
  if (ImGui::MenuItem(with_icon(Icon::Pencil, tr("Notes")).c_str(), nullptr, notes_)) switch_mode(true);
  ImGui::SetItemTooltip("%s", tr("Notes: text files in topics, linked by [[name]] in their text"));
}

void Ui::show_picker_for(bool create, bool notes) {
  picker_ = std::make_unique<ProjectPicker>(root_.empty() ? std::filesystem::path() : root_.parent_path(), settings_.recent,
                                            create ? PickerMode::Create : PickerMode::Open, notes ? PickerKind::Notes : PickerKind::Code);
  picker_open_ = true;
}

void Ui::switch_mode(bool notes) {
  if (notes == notes_ && !root_.empty()) return;
  // The last project of that workspace; none yet: its picker (Create when it has no project at all).
  const std::string& last = notes ? settings_.last_notes : settings_.last_code;
  if (!last.empty() && fs::path(last) != root_ && is_project_dir(last) && is_notes_project_dir(last) == notes) {
    open_request_ = fs::path(last);
    return;
  }
  bool any = false;
  for (const auto& r : settings_.recent) any = any || (is_project_dir(r) && is_notes_project_dir(r) == notes);
  show_picker_for(!any, notes);
}

// --- notes and their titles -------------------------------------------------------------------------

const NoteText* Ui::note_of(const Model& m, const std::string& id) const {
  if (!m.note_texts) return nullptr;
  auto it = m.note_texts->find(id);
  return it == m.note_texts->end() ? nullptr : &it->second;
}

std::string Ui::note_id_of_file(const Model& m, const std::string& file) const {
  if (!m.note_texts) return {};
  for (const auto& [id, n] : *m.note_texts) {
    if (n.file == file) return id;
  }
  return {};
}

std::string Ui::block_title(const Model& m, const std::string& id) const {
  if (notes_) {
    if (const auto* b = m.snap->block(id); b != nullptr && !b->purpose.empty()) return b->purpose;
  }
  return core::last_segment(id);
}

std::string Ui::new_note_id(const Model& m, const std::string& parent, const std::string& title) const {
  auto seg = core::ident_from_title(title);
  auto id = parent.empty() ? seg : parent + "." + seg;
  for (int n = 2; m.snap->block(id) != nullptr; ++n) id = (parent.empty() ? seg : parent + "." + seg) + "_" + std::to_string(n);
  return id;
}

void Ui::open_note(const Model& m, const std::string& id) {
  if (const auto* n = note_of(m, id)) {
    open_file_tab(n->file);
    req_.open_file = n->file;
    note_focus_file_ = n->file;
  }
}

bool Ui::note_dirty(const std::string& file) const { return note_edited_at_.contains(file); }

void Ui::autosave_notes(bool all) {
  double now = ImGui::GetTime();
  for (auto it = note_edited_at_.begin(); it != note_edited_at_.end();) {
    auto ed = editors_.find(it->first);
    if (ed == editors_.end() || !ed->second) {
      it = note_edited_at_.erase(it);
      continue;
    }
    if (!all && now - it->second < kAutosaveAfter) {
      ++it;
      continue;
    }
    auto text = editor_text(*ed->second);
    w_.save_note(it->first, text);
    editor_loaded_[it->first] = text;
    note_saved_text_[it->first] = text;
    it = note_edited_at_.erase(it);
  }
}

// --- Markdown notes (ADR 0040) ------------------------------------------------------------------------

Ui::NoteView Ui::note_view_of(const std::string& file) {
  auto [it, made] = note_view_.try_emplace(file, static_cast<NoteView>(std::clamp(settings_.note_view, 0, 2)));
  return it->second;
}

void Ui::set_note_view(const std::string& file, NoteView v) {
  note_view_[file] = v;
  if (settings_.note_view != static_cast<int>(v)) {
    settings_.note_view = static_cast<int>(v);  // the next note opens the same way
    settings_changed();
  }
}

void Ui::note_view_buttons(const std::string& file) {
  NoteView v = note_view_of(file);
  ImGui::SameLine(0, 14);
  if (ui::icon_button("nv_edit", Icon::Pencil, tr("The text as written (Markdown)"), v == NoteView::Edit, tr("Text"))) {
    set_note_view(file, NoteView::Edit);
  }
  ImGui::SameLine(0, 2);
  if (ui::icon_button("nv_split", Icon::Windows, tr("The text and the view side by side: the view follows the typing"), v == NoteView::Split,
                      tr("Both"))) {
    set_note_view(file, NoteView::Split);
  }
  ImGui::SameLine(0, 2);
  if (ui::icon_button("nv_view", Icon::Eye, tr("The note rendered: links open, a double click edits that place"), v == NoteView::Preview,
                      tr("Rendered"))) {
    set_note_view(file, NoteView::Preview);
  }
}

void Ui::note_preview(const Model& m, const std::string& file, TextEditor& ed, ImVec2 size) {
  auto& pv = note_preview_[file];
  if (pv.stale) {
    pv.doc = core::md::parse(editor_text(ed));
    pv.stale = false;
  }
  const float em = ImGui::GetFontSize();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.4f, em * 0.9f));
  if (ImGui::BeginChild("##note_view", size, ImGuiChildFlags_AlwaysUseWindowPadding)) {
    // A column one can read: centered when the tab is wide.
    const float column = em * 46;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > column) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - column) * 0.5f);
    if (pv.doc.empty()) {
      ui::muted("%s", tr("(empty: Text writes it)"));
    } else if (auto click = markdown_view("note_md", pv.doc, note_link_kind(m, file), std::numeric_limits<std::size_t>::max(), column,
                                          note_image_fn(file))) {
      if (click->dbl) {
        note_jump_file_ = file;
        note_jump_line_ = click->line;
      } else {
        follow_note_link(m, file, *click, true);
      }
    }
    ImGui::Dummy(ImVec2(1, em * 2));
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

std::string Ui::note_link_target(const Model& m, const std::string& from_file, const std::string& href, bool wiki) const {
  if (link_cache_key_[0] != m.snap.get() || link_cache_key_[1] != m.note_texts.get()) {
    link_cache_.clear();
    link_cache_key_[0] = m.snap.get();
    link_cache_key_[1] = m.note_texts.get();
  }
  auto key = from_file + "\n" + href + (wiki ? "\nw" : "\nm");
  if (auto it = link_cache_.find(key); it != link_cache_.end()) return it->second;
  std::string found;
  if (!wiki) {
    if (auto f = core::note_link_file(from_file, href)) found = note_id_of_file(m, *f);
  } else if (auto id = core::replace_all(std::string(core::trim(href)), "/", "."); m.snap->block(id) != nullptr) {
    found = id;  // as resolve_note: an id, else the one note or topic of that title or last id part
  } else {
    auto folded = core::fold_title(href);
    int n = 0;
    for (const auto& b : m.snap->blocks) {
      if (core::fold_title(b.purpose) == folded || core::fold_title(core::last_segment(b.id)) == folded) {
        found = b.id;
        ++n;
      }
    }
    if (n != 1) found.clear();
  }
  link_cache_[key] = found;
  return found;
}

MdLinkKind Ui::note_link_kind(const Model& m, const std::string& from_file) const {
  return [this, &m, from_file](const std::string& href, bool wiki) {
    if (!note_link_target(m, from_file, href, wiki).empty()) return kMdNote;
    if (wiki || core::note_link_file(from_file, href)) return kMdNowhere;  // names a note that is not there
    return kMdOut;
  };
}

void Ui::follow_note_link(const Model& m, const std::string& from_file, const MdClick& c, bool in_tab) {
  if (c.href.empty()) return;
  if (c.image) {  // an image opens in its program; a web image in the browser (it is never fetched here)
    if (!c.file.empty()) {
      w_.open_link("", "/" + c.file);
    } else if (!c.wiki && core::md::is_url(c.href)) {
      w_.open_link(from_file, c.href);
    }
    return;
  }
  if (auto id = c.image ? std::string() : note_link_target(m, from_file, c.href, c.wiki); !id.empty()) {
    const auto* b = m.snap->block(id);
    if (!in_tab || b == nullptr || b->kind == "group") {
      go_to(m, id);
    } else {
      enter(core::parent_id(id));
      select(id);
      open_note(m, id);
    }
    return;
  }
  if (c.wiki || c.href.starts_with('#') || core::note_link_file(from_file, c.href)) return;  // no such note
  w_.open_link(from_file, c.href);
}

// --- images (ADR 0041) -------------------------------------------------------------------------------

MdImageFn Ui::note_image_fn(const std::string& note_file) {
  return [this, note_file](const core::md::Run& r) {
    MdImage out;
    if (!r.wiki && core::md::is_url(r.href)) return out;  // a web image: a link, never fetched
    auto im = images_.get(note_file, r.href, r.wiki);
    using S = NoteImage::State;
    out.state = im.state == S::Ready     ? MdImage::State::Ready
                : im.state == S::Missing ? MdImage::State::Missing
                : im.state == S::Failed  ? MdImage::State::Failed
                                         : MdImage::State::Loading;
    out.tex = im.tex;
    out.w = static_cast<float>(im.w);
    out.h = static_cast<float>(im.h);
    out.file = im.file;
    out.error = im.state == S::Missing ? std::string(tr("the file is not there")) : im.error;
    return out;
  };
}

void Ui::note_files_frame(const Model& m) {
  // Files dropped on the window go into the Markdown note whose tab is open, else the note selected.
  if (auto dropped = take_dropped(); !dropped.empty()) {
    std::string target;
    if (notes_ && editor_tab_ != "#schema" && core::md::is_markdown_file(editor_tab_) && !note_id_of_file(m, editor_tab_).empty()) {
      target = editor_tab_;
    } else if (const auto* n = notes_ ? note_of(m, req_.selected) : nullptr; n != nullptr && n->markdown) {
      target = n->file;
    }
    if (target.empty()) {
      w_.say(tr("drop files on a Markdown note: its open tab or the note selected"));
    } else {
      w_.attach_files(target, std::move(dropped));
    }
  }
  // Links the worker made (dropped files, a pasted image): into the note's editor, its tab opened if needed.
  for (const auto& ins : m.note_inserts) {
    if (ins.id <= note_insert_seen_) continue;
    note_insert_seen_ = ins.id;
    note_pending_insert_[ins.note_file].push_back(ins.text);
    if (auto id = note_id_of_file(m, ins.note_file); !id.empty() && editor_tab_ != ins.note_file) open_note(m, id);
  }
}

// --- toolbar and dialogs ------------------------------------------------------------------------------

void Ui::notes_toolbar(const Model& m) {
  (void)m;
  draw_address(m);
  ImGui::Spacing();
  bool all = req_.all_levels;
  if (ui::toggle("tb_all", Icon::Layers, tr("All levels"), &all, tr("Open every topic in place (wheel zooms)"))) set_all_levels(m, all);
  ImGui::SameLine(0, 4);
  ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
  ImGui::SameLine(0, 4);
  ImGui::BeginDisabled(!m.can_undo);
  if (ui::icon_button("tb_undo", Icon::Undo, tr("Undo the last edit"))) w_.undo();
  ImGui::EndDisabled();
  ImGui::SameLine(0, 4);
  if (ui::icon_button("tb_note", Icon::Plus, tr("A new note on this level"), false, tr("Note"))) {
    note_dialog_ = "add_note";
    note_target_ = req_.scope;
    note_title_buf_.clear();
    note_dialog_focus_ = true;
  }
  ImGui::SameLine(0, 4);
  if (ui::icon_button("tb_topic", Icon::Folder, tr("A new topic on this level: notes go into it"), false, tr("Topic"))) {
    note_dialog_ = "add_topic";
    note_target_ = req_.scope;
    note_title_buf_.clear();
    note_dialog_focus_ = true;
  }
  ImGui::SameLine(0, 4);
  if (ui::icon_button("tb_fit", Icon::Fit, tr("Fit to view"))) navigate_ = 1;
  ImGui::SameLine(0, 4);
  if (ui::icon_button("tb_up", Icon::ArrowUp, tr("Up one level"), false) && !req_.scope.empty()) enter(core::parent_id(req_.scope));
  if (!path_error_.empty()) {
    ImGui::SameLine(0, 12);
    ImGui::TextColored(rgb(241, 76, 76), "%s", path_error_.c_str());
  }
}

void Ui::draw_note_dialogs(const Model& m) {
  // A note made by "+ Note" opens in the editor once the model has it.
  if (!note_open_next_.empty() && note_of(m, note_open_next_) != nullptr) {
    select(note_open_next_);
    open_note(m, note_open_next_);
    note_open_next_.clear();
  }
  if (note_dialog_.empty()) return;
  if (!ImGui::IsPopupOpen("###note_dialog")) ImGui::OpenPopup("###note_dialog");
  const char* head = note_dialog_ == "add_note"    ? tr("New note")
                     : note_dialog_ == "add_topic" ? tr("New topic")
                     : note_dialog_ == "retitle"   ? tr("Rename")
                                                   : tr("Remove");
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, 0), ImGuiCond_Appearing);
  bool open = true;
  if (!ImGui::BeginPopupModal((std::string(head) + "###note_dialog").c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    note_dialog_.clear();
    return;
  }
  auto close = [&] {
    note_dialog_.clear();
    ImGui::CloseCurrentPopup();
  };
  if (note_dialog_ == "remove") {
    const auto* b = m.snap->block(note_target_);
    bool topic = b != nullptr && b->kind == "group";
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
    ImGui::TextUnformatted(fmt::format(fmt::runtime(topic ? tr("Remove the topic \"{}\"?") : tr("Remove the note \"{}\"?")),
                                       block_title(m, note_target_))
                               .c_str());
    const auto* n = note_of(m, note_target_);
    bool own_file = n != nullptr && b != nullptr && n->file.starts_with(b->dir + "/");
    ui::muted("%s", topic ? tr("A topic is removed when it holds no notes.")
                          : own_file ? tr("It leaves the schema; Undo brings it back with its text.")
                                     : tr("Its file stays where it is: only the note leaves the schema; Undo brings it back."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, rgb(180, 60, 60));
    if (ImGui::Button(with_icon(Icon::Trash, tr("Remove")).c_str())) {
      auto id = note_target_;
      w_.apply_now([id](const core::Schema& s) { return agents::op_remove_block(s, id); });
      if (req_.selected == id) select(core::parent_id(id));
      close();
    }
    ImGui::PopStyleColor();
  } else {
    if (note_dialog_ == "add_note" || note_dialog_ == "add_topic") {
      ui::muted("%s", fmt::format(fmt::runtime(tr("in {}")), note_target_.empty() ? tr("the top level") : block_title(m, note_target_)).c_str());
    }
    if (note_dialog_focus_) {
      ImGui::SetKeyboardFocusHere();
      note_dialog_focus_ = false;
    }
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputTextWithHint("##note_title", tr("title"), &note_title_buf_, ImGuiInputTextFlags_EnterReturnsTrue);
    std::string title(core::trim(note_title_buf_));
    bool bad = title.find_first_of("[]|\n") != std::string::npos;
    if (bad) ImGui::TextColored(rgb(230, 160, 60), "%s", tr("A title cannot hold [ ] or |: [[links]] are written with them."));
    ImGui::Spacing();
    ImGui::BeginDisabled(title.empty() || bad);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
    bool go = ImGui::Button(with_icon(Icon::Check, note_dialog_ == "retitle" ? tr("Save") : tr("Create")).c_str()) || (enter && !title.empty() && !bad);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (go) {
      if (note_dialog_ == "retitle") {
        w_.retitle_note(note_target_, title);
      } else {
        bool group = note_dialog_ == "add_topic";
        auto parent = note_target_;
        auto id = new_note_id(m, parent, title);
        auto name = core::last_segment(id);
        w_.apply_now([=](const core::Schema& s) -> kk::Result<agents::EditOp> {
          // A folder left by a removed note keeps its old text: a new note takes another name.
          std::string n = name;
          auto dir_of = [&](const std::string& nm) { return s.blocks_root() / core::id_to_rel_path(parent.empty() ? nm : parent + "." + nm); };
          for (int k = 2; fs::exists(dir_of(n)); ++k) n = name + "_" + std::to_string(k);
          auto op = agents::op_add_block(s, parent, n, group ? core::BlockKind::Group : core::BlockKind::Leaf, title);
          // Notes run in no thread.
          if (op) std::erase_if(op->diff, [](const core::DiffLine& l) { return l.text.starts_with("thread"); });
          return op;
        });
        if (!group) note_open_next_ = id;
      }
      close();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
  ImGui::EndPopup();
  if (!open) note_dialog_.clear();
}

// --- the assistant's proposals (ADR 0039) -------------------------------------------------------------

void Ui::proposal_buttons(const Model* m, const std::string& id) {
  bool pending = false;
  if (m != nullptr) {
    for (const auto& p : m->proposals) pending |= p.id == id && p.status == "pending";
  }
  ImGui::PushID(id.c_str());
  if (ui::icon_button("show_p", Icon::Eye, tr("Show what the proposal changes, note by note"), false, id.c_str())) {
    side_ = SideView::Changes;
    settings_.show_side = true;
    req_.change = id;
  }
  if (pending) {
    ImGui::SameLine(0, 4);
    if (ui::icon_button("approve_p", Icon::Check, tr("Approve: the notes are written"), false, tr("Approve"))) w_.approve_proposal(id);
    ImGui::SameLine(0, 4);
    if (ui::icon_button("reject_p", Icon::Close, tr("Reject"), false, tr("Reject"))) w_.reject_proposal(id);
  }
  ImGui::PopID();
}

void Ui::side_proposals(const Model& m) {
  if (m.proposals.empty()) {
    ui::muted("%s", tr("no proposals yet"));
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ui::muted("%s", tr("Ask the notes assistant in the chat: what it would write or link comes here as a proposal; nothing is "
                       "written before you approve it."));
    ImGui::PopTextWrapPos();
    return;
  }
  // The list, then the selected proposal: its request, its edits as diffs, approve / reject.
  float list_h = std::min(static_cast<float>(m.proposals.size()) * ImGui::GetTextLineHeightWithSpacing() * 2.2f + 8,
                          ImGui::GetContentRegionAvail().y * 0.4f);
  ImGui::BeginChild("plist", ImVec2(0, list_h));
  for (const auto& p : m.proposals) {
    ImGui::PushID(p.id.c_str());
    bool sel = req_.change == p.id;
    if (ImGui::Selectable("##p", sel, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2.1f))) req_.change = sel ? "" : p.id;
    ImVec2 mn = ImGui::GetItemRectMin();
    float w = ImGui::GetItemRectSize().x;
    auto* dl = ImGui::GetWindowDrawList();
    ImVec4 sc = p.status == "pending" ? rgb(240, 200, 30) : p.status == "approved" ? rgb(115, 201, 145) : rgb(150, 150, 150);
    const char* st = p.status == "pending" ? tr("waits") : p.status == "approved" ? tr("approved") : tr("rejected");
    std::string head = fmt::format("{}  {}", p.id, st);
    dl->AddText(ImVec2(mn.x + 4, mn.y), ImGui::GetColorU32(sc), head.c_str());
    dl->AddText(ImVec2(mn.x + 4, mn.y + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                ui::ellipsize(p.summary.empty() ? p.request : p.summary, w - 8).c_str());
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::Separator();
  const agents::NotesProposal* cur = nullptr;
  for (const auto& p : m.proposals) {
    if (p.id == req_.change) cur = &p;
  }
  if (cur == nullptr) {
    ui::muted("%s", tr("Select a proposal to see what it changes."));
    return;
  }
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(cur->summary.c_str());
  ui::muted("%s", fmt::format(fmt::runtime(tr("request: {}")), cur->request).c_str());
  ImGui::PopTextWrapPos();
  proposal_buttons(&m, cur->id);
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
  ImGui::BeginChild("pdiff", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  if (m.req.change == cur->id) {
    ui::diff_text(m.proposal_preview);
  } else {
    ui::muted("%s", tr("loading..."));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// --- inspector ----------------------------------------------------------------------------------------

void Ui::inspector_note(const Model& m, const index::BlockRow& b) {
  bool topic = b.kind == "group";
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImFont* big = theme::icon_font_big();
  dl->AddText(big, big->FontSize, p, ImGui::GetColorU32(topic ? kTopicColor : kNoteColor), icon(topic ? Icon::Folder : Icon::File));
  ImGui::SetCursorScreenPos(ImVec2(p.x + big->FontSize + 10, p.y - 2));
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(block_title(m, b.id).c_str());
  ImGui::PopTextWrapPos();
  // Where it is: the titles of its topics.
  std::string path;
  for (auto id = core::parent_id(b.id); !id.empty(); id = core::parent_id(id)) path = block_title(m, id) + (path.empty() ? "" : " / " + path);
  ui::muted("%s", (path.empty() ? std::string(tr_kind(b.kind)) : std::string(tr_kind(b.kind)) + " · " + path).c_str());
  ImGui::EndGroup();
  ImGui::Spacing();
  const auto* n = note_of(m, b.id);
  if (!topic && ui::icon_button("nt_open", Icon::Pencil, tr("Edit the text in a tab"), false, tr("Open"))) open_note(m, b.id);
  if (topic && ui::icon_button("nt_enter", Icon::Layers, tr("Open this topic"), false, tr("Enter"))) enter(b.id);
  ImGui::SameLine(0, 2);
  if (ui::icon_button("nt_rename", Icon::Card, tr("Change the title (links that name it follow)"), false, tr("Rename"))) {
    note_dialog_ = "retitle";
    note_target_ = b.id;
    note_title_buf_ = b.purpose;
    note_dialog_focus_ = true;
  }
  ImGui::SameLine(0, 2);
  if (ui::icon_button("nt_folder", Icon::Folder, tr("Open folder"))) w_.open_block_folder(b.id);
  ImGui::SameLine(0, 2);
  if (ui::icon_button("nt_remove", Icon::Trash, tr("Remove"))) {
    note_dialog_ = "remove";
    note_target_ = b.id;
  }
  ImGui::SameLine(0, 2);
  if (ui::icon_button("nt_ask", Icon::Chat, tr("Ask the notes assistant about this (the chat, limited to it)"), false, tr("Ask"))) {
    chat_scope_ = {ChatScope::Kind::Block, b.id};
    scope_seen_block_ = req_.selected;
    show_chat_ = true;
    chat_focus_ = true;
  }
  ImGui::Spacing();
  auto link_row = [&](const std::string& id, const char* key) {
    ImGui::PushID(key);
    ImGui::PushID(id.c_str());
    const auto* x = m.snap->block(id);
    bool is_topic = x != nullptr && x->kind == "group";
    ImGui::TextColored(is_topic ? kTopicColor : kNoteColor, "%s", icon(is_topic ? Icon::Folder : Icon::File));
    ImGui::SameLine();
    if (ImGui::Selectable(block_title(m, id).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
      if (ImGui::IsMouseDoubleClicked(0) && !is_topic) {
        open_note(m, id);
      } else {
        go_to(m, id);
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", id.c_str());
    ImGui::PopID();
    ImGui::PopID();
  };
  if (topic) {
    auto kids = m.snap->children_of(b.id);
    if (ui::section(fmt::format("{} ({})###nt_kids", tr("IN THIS TOPIC"), kids.size()).c_str())) {
      for (const auto& k : kids) link_row(k, "kid");
      if (kids.empty()) ui::muted("%s", tr("no notes yet: + Note on the toolbar"));
    }
  } else if (ui::section((std::string(tr("TEXT")) + "###nt_text").c_str())) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
    ImGui::BeginChild("nt_text", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    std::string text = n != nullptr ? n->text : "";
    if (core::trim(text).empty()) {
      ui::muted("%s", tr("(empty: Open writes it)"));
    } else if (n->markdown && n->doc) {
      // Markdown rendered (ADR 0040): links go to their notes, a double click edits that place.
      constexpr std::size_t kShownBlocks = 80;
      if (auto click = markdown_view("nt_md", *n->doc, note_link_kind(m, n->file), kShownBlocks, 0.0f, note_image_fn(n->file))) {
        if (click->dbl) {
          open_note(m, b.id);
          note_jump_file_ = n->file;
          note_jump_line_ = click->line;
        } else {
          follow_note_link(m, n->file, *click, false);
        }
      }
    } else {
      constexpr std::size_t kShown = 4000;
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(text.size() > kShown ? (core::truncate_utf8(text, kShown) + " …").c_str() : text.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (!(n != nullptr && n->markdown) && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) open_note(m, b.id);
  }
  // Its [[links]], the ones that name nothing, and the notes that link to it.
  std::vector<std::string> out, in;
  for (const auto& l : m.snap->links) {
    if (l.kind != "ref") continue;
    if (l.src_block == b.id) out.push_back(l.dst_block);
    if (l.dst_block == b.id) in.push_back(l.src_block);
  }
  std::vector<std::string> broken;
  for (const auto& d : m.snap->drift) {
    if (d.block == b.id && d.code == "W08") broken.push_back(d.message);
  }
  if (!topic && ui::section(fmt::format("{} ({})###nt_out", tr("LINKS"), out.size() + broken.size()).c_str())) {
    for (const auto& id : out) link_row(id, "out");
    for (const auto& msg : broken) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(rgb(230, 160, 60), "%s %s", icon(Icon::Warning), msg.c_str());
      ImGui::PopTextWrapPos();
    }
    if (out.empty() && broken.empty()) ui::muted("%s", tr("none: write [[title of a note]] in the text, or draw a line on the schema"));
  }
  if (ui::section(fmt::format("{} ({})###nt_in", tr("LINKED FROM"), in.size()).c_str())) {
    for (const auto& id : in) link_row(id, "in");
    if (in.empty()) ui::muted("%s", tr("no note links here yet"));
  }
  // Plain words that name a title (ADR 0039): links to suggest, made in one click — no model.
  if (m.req.selected == b.id && !m.mentions.empty() &&
      ui::section(fmt::format("{} ({})###nt_ment", tr("MENTIONS WITHOUT A LINK"), m.mentions.size()).c_str())) {
    for (std::size_t i = 0; i < m.mentions.size(); ++i) {
      const auto& mn = m.mentions[i];
      ImGui::PushID(static_cast<int>(i));
      bool here = mn.src == b.id;  // this note names another one, or another note names this one
      if (ui::icon_button("ment_link", Icon::Link, tr("Make these words a [[link]]"), false, tr("Link"))) w_.link_mention(mn.src, mn.dst);
      ImGui::SameLine();
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(here ? fmt::format(fmt::runtime(tr("names \"{}\": {}")), block_title(m, mn.dst), mn.text).c_str()
                                  : fmt::format(fmt::runtime(tr("\"{}\" names it: {}")), block_title(m, mn.src), mn.text).c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopID();
    }
  }
  if (n != nullptr) {
    ImGui::Spacing();
    ui::muted("%s", n->file.c_str());
  }
}

}  // namespace kk::gui
