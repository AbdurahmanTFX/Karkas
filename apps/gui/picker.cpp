#include "picker.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "i18n.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "kk/core/text.hpp"

namespace fs = std::filesystem;

namespace kk::gui {

std::string fmt_path_error(const std::string& p);

ProjectPicker::ProjectPicker(const fs::path& start, std::vector<std::string> recent) : recent_(std::move(recent)) {
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

std::optional<fs::path> ProjectPicker::draw(bool* open, bool welcome) {
  std::optional<fs::path> chosen;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_Appearing);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse;
  if (!ImGui::Begin(title("Open project").c_str(), welcome ? nullptr : open, fl)) {
    ImGui::End();
    return chosen;
  }
  if (welcome) {
    ImGui::TextWrapped("%s", tr("No Karkas project is open. Choose a folder with project.ksl (for an existing code base: "
                                "karkas init there, then karkas import)."));
    ImGui::Separator();
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
  float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
  ImGui::BeginChild("dirs", ImVec2(0, -footer), ImGuiChildFlags_Borders);
  for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
    const auto& e = entries_[static_cast<std::size_t>(i)];
    std::string label = std::string(icon(e.project ? Icon::Schema : Icon::Folder)) + "  " + e.name +
                        (e.project ? std::string("   ") + tr("(Karkas project)") : "");
    ImGui::PushStyleColor(ImGuiCol_Text, e.project ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f) : ImVec4(0.86f, 0.71f, 0.48f, 1.0f));
    if (ImGui::Selectable(label.c_str(), selected_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
      selected_ = i;
      if (ImGui::IsMouseDoubleClicked(0)) {
        if (e.project) {
          chosen = dir_ / e.name;
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

std::string fmt_path_error(const std::string& p) { return std::string(tr("not a folder: ")) + p; }

}  // namespace kk::gui
