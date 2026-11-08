#include "picker.hpp"

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "i18n.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "kk/agents/changes.hpp"
#include "kk/code/import.hpp"
#include "kk/code/init.hpp"
#include "kk/core/text.hpp"

namespace fs = std::filesystem;

namespace kk::gui {

std::string fmt_path_error(const std::string& p);

ProjectPicker::ProjectPicker(const fs::path& start, std::vector<std::string> recent, PickerMode mode)
    : recent_(std::move(recent)), mode_(mode) {
  std::error_code ec;
  fs::path dir = start;
  if (dir.empty() || !fs::is_directory(dir, ec)) dir = fs::current_path(ec);
  go(dir);
}

void ProjectPicker::go(const fs::path& dir_in) {
  std::error_code ec;
  fs::path dir = fs::weakly_canonical(dir_in, ec);
  if (ec || !fs::is_directory(dir, ec)) {
    error_ = fmt_path_error(dir_in.string());
    return;
  }
  dir_ = dir;
  entries_.clear();
  selected_ = -1;
  error_.clear();
  here_is_project_ = fs::exists(dir_ / "project.ksl", ec);
  for (auto it = fs::directory_iterator(dir_, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::directory_iterator(); it.increment(ec)) {
    std::error_code e2;
    if (!it->is_directory(e2)) continue;
    auto name = it->path().filename().string();
    if (!show_hidden_ && name.starts_with(".")) continue;
    entries_.push_back(Entry{name, fs::exists(it->path() / "project.ksl", e2)});
    if (entries_.size() >= 5000) break;
  }
  if (ec) error_ = ec.message();
  std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
    if (a.project != b.project) return a.project;
    return core::to_lower(a.name) < core::to_lower(b.name);
  });
  std::snprintf(path_buf_, sizeof path_buf_, "%s", dir_.string().c_str());
}

void ProjectPicker::draw_browser(float footer, std::optional<fs::path>* chosen) {
  // Path line, up, home.
  if (ImGui::Button(icon(Icon::ArrowUp))) go(dir_.parent_path());
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Parent folder"));
  ImGui::SameLine();
  if (ImGui::Button(with_icon(Icon::Home, tr("Home")).c_str())) {
    if (const char* h = std::getenv("HOME"); h != nullptr) go(h);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputText("##dir", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue)) go(path_buf_);
  if (ImGui::Checkbox(tr("Show hidden folders"), &show_hidden_)) go(dir_);
  if (!error_.empty()) ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%s", error_.c_str());
  // Folders.
  ImGui::BeginChild("dirs", ImVec2(0, -footer), ImGuiChildFlags_Borders);
  for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
    const auto& e = entries_[static_cast<std::size_t>(i)];
    std::string label = std::string(icon(e.project ? Icon::Schema : Icon::Folder)) + "  " + e.name +
                        (e.project ? std::string("   ") + tr("(Karkas project)") : "");
    ImGui::PushStyleColor(ImGuiCol_Text, e.project ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f) : ImVec4(0.86f, 0.71f, 0.48f, 1.0f));
    if (ImGui::Selectable(label.c_str(), selected_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
      selected_ = i;
      if (ImGui::IsMouseDoubleClicked(0)) {
        if (e.project && mode_ == PickerMode::Open) {
          *chosen = dir_ / e.name;
        } else {
          auto next = dir_ / e.name;
          ImGui::PopStyleColor();
          go(next);
          break;
        }
      }
    }
    ImGui::PopStyleColor();
  }
  if (entries_.empty()) ImGui::TextDisabled("%s", tr("(no folders)"));
  ImGui::EndChild();
}

std::optional<fs::path> ProjectPicker::draw(bool* open, bool welcome) {
  std::optional<fs::path> chosen;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(780, 620), ImGuiCond_Appearing);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
  std::string name = std::string(mode_ == PickerMode::Open ? tr("Open project") : tr("New project")) + "###project_picker";
  if (!ImGui::Begin(name.c_str(), welcome ? nullptr : open, fl)) {
    ImGui::End();
    return chosen;
  }
  if (welcome) {
    ImGui::TextWrapped("%s", tr("No Karkas project is open. Open a folder with project.ksl or create a project: in a new "
                                "folder or in a folder that already holds code."));
    ImGui::Spacing();
  }
  if (ImGui::BeginTabBar("picker_tabs")) {
    ImGuiTabItemFlags open_fl = switch_tab_ && mode_ == PickerMode::Open ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags new_fl = switch_tab_ && mode_ == PickerMode::Create ? ImGuiTabItemFlags_SetSelected : 0;
    switch_tab_ = false;
    if (ImGui::BeginTabItem(with_icon(Icon::FolderOpen, tr("Open")).c_str(), nullptr, open_fl)) {
      mode_ = PickerMode::Open;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(with_icon(Icon::Plus, tr("Create")).c_str(), nullptr, new_fl)) {
      mode_ = PickerMode::Create;
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  if (mode_ == PickerMode::Create) {
    chosen = draw_create(open, welcome);
    ImGui::End();
    return chosen;
  }
  // Recent projects.
  std::vector<std::string> recent;
  for (const auto& r : recent_) {
    std::error_code ec;
    if (fs::exists(fs::path(r) / "project.ksl", ec)) recent.push_back(r);
  }
  if (!recent.empty()) {
    ImGui::TextDisabled("%s", tr("Recent projects"));
    for (const auto& r : recent) {
      std::string label = with_icon(Icon::Schema, fs::path(r).filename().string()) + "   " + r;
      if (ImGui::Selectable(label.c_str())) chosen = fs::path(r);
    }
    ImGui::Separator();
  }
  draw_browser(ImGui::GetFrameHeightWithSpacing() * 2.2f, &chosen);
  // What "Open" opens: the selected project folder, or this folder when it is a project.
  fs::path target;
  if (selected_ >= 0 && entries_[static_cast<std::size_t>(selected_)].project) {
    target = dir_ / entries_[static_cast<std::size_t>(selected_)].name;
  } else if (here_is_project_) {
    target = dir_;
  }
  if (target.empty()) {
    ImGui::TextDisabled("%s", tr("Pick a green folder (it holds project.ksl); double click enters a folder."));
  } else {
    ImGui::Text(tr("Project: %s"), target.string().c_str());
  }
  ImGui::BeginDisabled(target.empty());
  ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::accent(1.0f));
  if (ImGui::Button(with_icon(Icon::FolderOpen, tr("Open")).c_str())) chosen = target;
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  if (!welcome) {
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) *open = false;
  }
  ImGui::End();
  return chosen;
}

fs::path ProjectPicker::create_target() const {
  if (in_new_folder_) return name_buf_[0] == '\0' ? fs::path() : dir_ / name_buf_;
  // Existing folder: the selected one, else the folder shown.
  if (selected_ >= 0) return dir_ / entries_[static_cast<std::size_t>(selected_)].name;
  return dir_;
}

std::optional<fs::path> ProjectPicker::draw_create(bool* open, bool welcome) {
  std::optional<fs::path> chosen;
  if (ImGui::RadioButton(tr("In a new folder"), in_new_folder_)) {
    in_new_folder_ = true;
    create_error_.clear();
  }
  ImGui::SameLine(0, 20);
  if (ImGui::RadioButton(tr("In an existing folder (with code or empty)"), !in_new_folder_)) {
    in_new_folder_ = false;
    create_error_.clear();
  }
  ImGui::TextDisabled("%s", in_new_folder_ ? tr("Choose where the new folder goes:") : tr("Choose the folder of the project:"));
  float line = ImGui::GetFrameHeightWithSpacing();
  draw_browser(line * (in_new_folder_ ? 5.6f : 6.0f), &chosen);
  chosen.reset();  // double click only browses here
  // Fields.
  float label_w = ImGui::GetFontSize() * 9;
  auto label = [&](const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text);
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
  };
  if (in_new_folder_) {
    label(tr("Folder name"));
    if (ImGui::InputTextWithHint("##new_name", "my_game", name_buf_, sizeof name_buf_)) create_error_.clear();
  }
  label(tr("Idea (optional)"));
  ImGui::InputTextWithHint("##idea", tr("one line: what the program does"), idea_buf_, sizeof idea_buf_);
  fs::path target = create_target();
  std::error_code ec;
  std::string id = target.empty() ? "" : code::project_id_from(target.filename().string());
  std::string problem;
  if (target.empty()) {
    problem = tr("Enter the name of the new folder.");
  } else if (id.empty()) {
    problem = tr("The folder name must start with a letter.");
  } else if (fs::exists(target / "project.ksl", ec)) {
    problem = tr("This folder already holds a Karkas project: open it instead.");
  } else if (in_new_folder_ && fs::exists(target, ec) && !fs::is_empty(target, ec)) {
    problem = tr("A folder with this name exists and is not empty: choose \"In an existing folder\".");
  }
  if (!in_new_folder_ && problem.empty() && code_checked_ != target) {
    code_checked_ = target;
    target_has_code_ = code::has_code(target);
  }
  if (!problem.empty()) {
    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1), "%s", problem.c_str());
  } else {
    ImGui::Text(tr("Project %s in %s"), id.c_str(), target.string().c_str());
  }
  bool code_here = !in_new_folder_ && problem.empty() && target_has_code_;
  if (code_here) {
    ImGui::Checkbox(tr("Cut the existing code into blocks (karkas import): the proposal appears in Changes"), &import_code_);
  } else {
    ImGui::TextDisabled("%s", tr("Next: describe the idea to the architect (karkas design) or add blocks by hand."));
  }
  if (!create_error_.empty()) ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%s", create_error_.c_str());
  ImGui::BeginDisabled(!problem.empty());
  ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::accent(1.0f));
  if (ImGui::Button(with_icon(Icon::Plus, tr("Create project")).c_str())) {
    auto r = code::init_project(target, idea_buf_);
    if (!r) {
      create_error_ = r.error().message;
    } else {
      if (r->existing_code && code_here && import_code_) {
        // Same as `karkas import`: a change to review, files stay in place.
        auto plan = code::plan_import(target, {});
        if (plan && !plan->blocks.empty()) {
          agents::ChangeStore store(target);
          (void)store.create("import",
                             fmt::format("karkas import: {} blocks over {} existing files", plan->blocks.size(), plan->files.size()),
                             plan->diff);
        }
      }
      chosen = target;
    }
  }
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  if (!welcome) {
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) *open = false;
  }
  return chosen;
}

std::string fmt_path_error(const std::string& p) { return std::string(tr("not a folder: ")) + p; }

}  // namespace kk::gui
