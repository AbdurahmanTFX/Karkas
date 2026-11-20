// Build and run in karkas-gui (docs/adr/0037): the green triangle and the stop square at the right of
// the menu bar, the Run menu (F5, Ctrl+F5, Shift+F5), the Run tab — the program to run and its editor,
// the output of the build and of the program, a line of input for the program — and the run's state in
// the status bar. The run itself is `karkas run` in a terminal of its own (run.cpp).
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>

#include "i18n.hpp"
#include "icons.hpp"
#include "kk/core/text.hpp"
#include "theme.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace kk::gui {

namespace {

const ImVec4 kRunGreen = rgb(89, 196, 99);
const ImVec4 kStopRed = rgb(232, 80, 80);

std::string duration_text(double s) {
  if (s < 60) return fmt::format("{:.1f} s", s);
  auto t = static_cast<long>(s);
  return t < 3600 ? fmt::format("{}:{:02}", t / 60, t % 60) : fmt::format("{}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60);
}

enum class Glyph { Play, Stop };

// A flat square button with a drawn triangle or square: the green "run" and the red "stop" of an IDE.
bool glyph_button(const char* id, Glyph g, ImVec4 color, bool enabled, const char* tooltip) {
  float h = ImGui::GetFrameHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!enabled);
  bool clicked = ImGui::InvisibleButton(id, ImVec2(h, h));
  ImGui::EndDisabled();
  bool hot = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  auto* dl = ImGui::GetWindowDrawList();
  if (hot && enabled) {
    dl->AddRectFilled(p, ImVec2(p.x + h, p.y + h), ImGui::IsItemActive() ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 22), 4.0f);
  }
  ImVec4 c = enabled ? color : rgb(110, 110, 110);
  ImU32 col = ImGui::GetColorU32(c);
  ImVec2 m(p.x + h * 0.5f, p.y + h * 0.5f);
  float r = std::round(h * 0.27f);
  if (g == Glyph::Play) {
    // A triangle pointing right, its weight centered (the centroid sits at a third of its width).
    dl->AddTriangleFilled(ImVec2(m.x - r * 0.75f, m.y - r), ImVec2(m.x - r * 0.75f, m.y + r), ImVec2(m.x + r * 1.05f, m.y), col);
  } else {
    float q = r * 0.82f;
    dl->AddRectFilled(ImVec2(m.x - q, m.y - q), ImVec2(m.x + q, m.y + q), col, 1.5f);
  }
  if (hot && tooltip != nullptr) ImGui::SetTooltip("%s", tooltip);
  if (enabled && hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  return clicked && enabled;
}

}  // namespace

// --- starting and stopping ---------------------------------------------------------------------

void Ui::show_run_tab() {
  if (!settings_.show_bottom) {
    settings_.show_bottom = true;
    settings_changed();
  }
  if (!tab_visible("run")) set_tab_visible("run", true);
  bottom_select_ = "run";
}

void Ui::start_run(bool build) {
  if (!run_) return;
  run_follow_ = true;
  show_run_tab();
  if (run_->busy()) {
    // Like "Rerun" of an IDE: the run going on ends first, the new one starts when it has.
    run_restart_ = true;
    run_restart_build_ = build;
    run_->stop();
    return;
  }
  run_->start(build);
}

void Ui::run_shortcuts() {
  if (!run_) return;
  if (run_restart_ && !run_->busy()) {
    run_restart_ = false;
    run_->start(run_restart_build_);
  }
  // The run ended: the build folder holds new programs (the editor offers them).
  bool busy = run_->busy();
  if (run_was_busy_ && !busy) w_.refresh_run();
  run_was_busy_ = busy;
  if (ImGui::IsKeyChordPressed(ImGuiKey_F5)) start_run(true);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F5)) start_run(false);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F5)) {
    run_restart_ = false;
    run_->stop();
  }
}

void Ui::save_run_command(const std::string& command) {
  auto cmd = std::string(core::trim(command));
  auto r = core::write_config_value(root_, "project", "run_command", cmd.empty() ? std::nullopt : std::optional<std::string>(cmd));
  run_msg_ = r ? "" : r.error().message;
  if (r) run_edit_ = false;
  w_.reload_config();
  w_.refresh_run();
}

std::string Ui::run_status_text() const {
  if (!run_) return "";
  auto t = duration_text(run_->elapsed());
  switch (run_->phase()) {
    case RunPhase::Building:
      return run_->stopping() ? std::string(tr("Stopping...")) : fmt::format(fmt::runtime(tr("Building... {}")), t);
    case RunPhase::Running:
      return run_->stopping() ? std::string(tr("Stopping..."))
                              : fmt::format(fmt::runtime(tr("Running: {} · {}")), run_->program(), t);
    case RunPhase::Idle:
      break;
  }
  auto e = run_->last_end();
  if (!e.ended) return tr("Not run yet: F5 builds the project and runs its program");
  if (e.nothing_to_run) return tr("Nothing to run: name the program below");
  if (e.build_failed) return fmt::format(fmt::runtime(tr("Build failed · {}")), t);
  if (e.stopped) return fmt::format(fmt::runtime(tr("Stopped · {}")), t);
  if (e.signal != 0) return fmt::format(fmt::runtime(tr("Ended by signal {} · {}")), e.signal, t);
  return fmt::format(fmt::runtime(tr("Finished with exit code {} · {}")), e.exit_code, t);
}

// --- menu bar and status bar -------------------------------------------------------------------

void Ui::run_menu(const Model* m) {
  if (!ImGui::BeginMenu(tr("Run"))) return;
  bool can = m != nullptr && run_ != nullptr;
  bool busy = can && run_->busy();
  if (ImGui::MenuItem(with_icon(Icon::Play, busy ? tr("Build and run again") : tr("Build and run")).c_str(), "F5", false, can)) start_run(true);
  if (ImGui::MenuItem(with_icon(Icon::Play, tr("Run without building")).c_str(), "Ctrl+F5", false, can)) start_run(false);
  if (ImGui::MenuItem(with_icon(Icon::Stop, tr("Stop")).c_str(), "Shift+F5", false, busy)) {
    run_restart_ = false;
    run_->stop();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(with_icon(Icon::Pencil, tr("Program to run...")).c_str(), nullptr, false, can)) {
    show_run_tab();
    run_edit_ = true;
    run_edit_focus_ = true;
    run_edit_buf_ = m->run.run_command.empty() ? m->run.shown : m->run.run_command;
    w_.refresh_run();
  }
  if (ImGui::MenuItem(with_icon(Icon::Play, tr("Show the Run tab")).c_str(), nullptr, false, can)) show_run_tab();
  ImGui::EndMenu();
}

void Ui::draw_run_buttons(const Model* m) {
  if (m == nullptr || !run_) return;
  if (!run_info_asked_) {
    run_info_asked_ = true;
    w_.refresh_run();
  }
  bool busy = run_->busy();
  // At the right end of the menu bar: the program, then the triangle and the square.
  const auto& info = m->run;
  std::string label = !info.known ? "" : info.error.empty() ? info.shown : std::string(tr("no program to run"));
  float h = ImGui::GetFrameHeight();
  float label_w = label.empty() ? 0.0f : std::min(ImGui::CalcTextSize(label.c_str()).x, ImGui::GetFontSize() * 16.0f);
  float right = label_w + h * 2.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f + 8.0f;
  float x = ImGui::GetWindowWidth() - right;
  if (x <= ImGui::GetCursorPosX()) return;  // no room left on a narrow window
  // Centered in the height of the menu bar (its frame padding is larger than the buttons').
  const float y = std::max(0.0f, std::round((ImGui::GetWindowHeight() - h) * 0.5f));
  ImGui::SetCursorPos(ImVec2(x, y));
  if (!label.empty()) {
    auto shown = ui::ellipsize(label, label_w);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##run_program", ImVec2(label_w, h));
    bool hot = ImGui::IsItemHovered();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                                        ImGui::GetColorU32(info.error.empty() ? (hot ? rgb(230, 230, 230) : rgb(160, 160, 160)) : rgb(230, 160, 60)),
                                        shown.c_str());
    if (hot) {
      ImGui::SetTooltip("%s", info.error.empty() ? fmt::format(fmt::runtime(tr("Runs: {}. Click: the Run tab")), info.shown).c_str()
                                                 : info.error.c_str());
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemClicked()) show_run_tab();
    ImGui::SameLine();
    ImGui::SetCursorPosY(y);
  }
  ImGui::PushID("run_buttons");
  if (glyph_button("##run", Glyph::Play, kRunGreen, true, busy ? tr("Build and run again (F5)") : tr("Build and run (F5)"))) start_run(true);
  ImGui::SameLine();
  ImGui::SetCursorPosY(y);
  if (glyph_button("##stop", Glyph::Stop, kStopRed, busy, tr("Stop (Shift+F5)"))) {
    run_restart_ = false;
    run_->stop();
  }
  ImGui::PopID();
}

void Ui::run_status_item(const Model* m) {
  (void)m;
  if (!run_ || !run_->busy()) return;
  // The same flat item as the rest of the status bar.
  std::string text = with_icon(Icon::Play, run_status_text());
  ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  ImVec2 p = ImGui::GetCursorScreenPos();
  float h = ImGui::GetWindowHeight();
  ImVec2 size(ts.x + 12, h);
  ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetWindowPos().y));
  ImGui::InvisibleButton("st_run", size);
  bool hot = ImGui::IsItemHovered();
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 a = ImGui::GetItemRectMin();
  if (hot) dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), IM_COL32(255, 255, 255, 22));
  dl->AddText(ImVec2(a.x + 6, a.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(run_->phase() == RunPhase::Running ? kRunGreen : rgb(240, 200, 30)),
              text.c_str());
  if (hot) ImGui::SetTooltip("%s", tr("The program of the project (the Run tab)"));
  if (ImGui::IsItemClicked()) show_run_tab();
  ImGui::SameLine(0, 2);
}

// --- the Run tab -----------------------------------------------------------------------------------

void Ui::run_program_line(const Model& m) {
  const auto& info = m.run;
  if (!run_edit_) {
    ImGui::AlignTextToFramePadding();
    if (!info.known) {
      ui::muted("%s", tr("looking for the program..."));
      return;
    }
    if (!info.error.empty()) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(rgb(230, 160, 60), "%s", tr("Karkas does not know which program to run: the project has its own build, or no "
                                                     "code of the entry block yet. Name the program."));
      ImGui::PopTextWrapPos();
    } else {
      ui::muted("%s", tr("Program:"));
      ImGui::SameLine();
      ImGui::PushFont(theme::mono_font());
      ImGui::TextUnformatted(info.shown.c_str());
      ImGui::PopFont();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", info.run_command.empty() ? tr("The program of the entry block that Karkas's CMake builds")
                                                         : tr("[project] run_command of karkas.toml, run by sh from the project root"));
      }
    }
    ImGui::SameLine();
    if (ui::icon_button("run_edit", Icon::Pencil, tr("Choose the program and its arguments ([project] run_command in karkas.toml)"), false,
                        info.error.empty() ? tr("Change") : tr("Name the program"))) {
      run_edit_ = true;
      run_edit_focus_ = true;
      run_edit_buf_ = info.run_command.empty() ? info.shown : info.run_command;
      w_.refresh_run();
    }
    return;
  }
  // The editor: one command line, run by sh from the project root after the build.
  ui::muted("%s", tr("Command, run by sh from the project root after the build (arguments may follow):"));
  ImGui::PushFont(theme::mono_font());
  ImGui::SetNextItemWidth(-1);
  if (run_edit_focus_) {
    ImGui::SetKeyboardFocusHere();
    run_edit_focus_ = false;
  }
  bool enter = ImGui::InputTextWithHint("##run_cmd", "build/<program> --argument", &run_edit_buf_, ImGuiInputTextFlags_EnterReturnsTrue);
  bool escape = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::PopFont();
  ImGui::PushStyleColor(ImGuiCol_Button, theme::accent(0.9f));
  bool save = ImGui::Button(with_icon(Icon::Check, tr("Save")).c_str()) || enter;
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::Button(tr("Cancel")) || escape) {
    run_edit_ = false;
    run_msg_.clear();
  }
  if (!info.generated.empty()) {
    ImGui::SameLine();
    if (ImGui::Button(with_icon(Icon::Undo, tr("The program Karkas builds")).c_str())) save_run_command("");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", fmt::format(fmt::runtime(tr("Remove run_command: run {}")), info.generated).c_str());
    }
  }
  if (save) {
    // The generated program itself needs no run_command: it stays the default.
    std::string cmd(core::trim(run_edit_buf_));
    save_run_command(cmd == info.generated ? std::string() : cmd);
  }
  if (!run_msg_.empty()) ImGui::TextColored(rgb(241, 76, 76), "%s", run_msg_.c_str());
  if (!info.candidates.empty()) {
    ui::muted("%s", tr("Programs in the build folder:"));
    for (const auto& c : info.candidates) {
      ImGui::SameLine();
      if (ImGui::SmallButton(c.c_str())) run_edit_buf_ = c;
    }
  } else if (!info.error.empty()) {
    ui::muted("%s", tr("No program in the build folder yet: Run builds the project first, then offers what it made."));
  }
}

void Ui::bottom_run(const Model& m) {
  if (!run_) return;
  bool busy = run_->busy();
  // First line: run, stop, the state; clear and copy on the right.
  if (ui::icon_button("run_go", Icon::Play, tr("Build the project and run its program (F5)"), false, busy ? tr("Run again") : tr("Run"))) {
    start_run(true);
  }
  ImGui::SameLine(0, 4);
  ImGui::BeginDisabled(!busy);
  if (ui::icon_button("run_stop", Icon::Stop, tr("Stop the build or the program (Shift+F5)"), false, tr("Stop"))) {
    run_restart_ = false;
    run_->stop();
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0, 12);
  ImGui::AlignTextToFramePadding();
  auto e = run_->last_end();
  ImVec4 sc = rgb(204, 204, 204);
  if (busy) {
    sc = run_->phase() == RunPhase::Running ? kRunGreen : rgb(240, 200, 30);
  } else if (e.ended) {
    bool good = e.started && !e.stopped && e.signal == 0 && e.exit_code == 0;
    sc = good ? kRunGreen : (e.stopped ? rgb(204, 204, 204) : kStopRed);
  }
  ImGui::TextColored(sc, "%s", run_status_text().c_str());
  if (run_serial_ != run_->serial()) {
    run_serial_ = run_->serial();
    auto before = run_cache_.size();
    run_cache_ = run_->lines();
    if (run_cache_.size() < before) {  // a new run, or cleared: from the top, following again
      run_follow_ = true;
      run_last_scroll_ = 0.0f;
    }
  }
  std::string partial = run_->partial_line();
  {
    float bw = ImGui::GetFrameHeight() * 1.2f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8, ImGui::GetContentRegionMax().x - bw * 2 - 8));
    if (ui::icon_button("run_copy", Icon::Copy, tr("Copy the output"))) {
      std::string all;
      for (const auto& l : run_cache_) all += l.text + "\n";
      ImGui::SetClipboardText((all + partial).c_str());
    }
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(busy || run_cache_.empty());
    if (ui::icon_button("run_clear", Icon::Trash, tr("Clear the output"))) run_->clear();
    ImGui::EndDisabled();
  }
  run_program_line(m);
  ImGui::Separator();
  // The output; with a line of input under it while the program runs.
  float input_h = run_->phase() == RunPhase::Running ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::col(theme::kEditorBg));
  ImGui::BeginChild("run_out", ImVec2(0, -input_h), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PushFont(theme::mono_font());
  auto color = [](RunLine::Kind k) {
    switch (k) {
      case RunLine::Kind::Info: return rgb(120, 180, 255);
      case RunLine::Kind::Warning: return rgb(230, 190, 90);
      case RunLine::Kind::Error: return rgb(241, 100, 100);
      case RunLine::Kind::Out: break;
    }
    return rgb(212, 212, 212);
  };
  std::string end_line;
  ImVec4 end_color = rgb(160, 160, 160);
  if (!busy && e.ended && !run_cache_.empty()) {
    end_line = run_status_text();
    end_color = sc;
  }
  int extra = (partial.empty() ? 0 : 1) + (end_line.empty() ? 0 : 1);
  ImGuiListClipper clip;
  clip.Begin(static_cast<int>(run_cache_.size()) + extra);
  while (clip.Step()) {
    for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
      auto n = static_cast<int>(run_cache_.size());
      if (i < n) {
        const auto& l = run_cache_[static_cast<std::size_t>(i)];
        ImGui::PushStyleColor(ImGuiCol_Text, color(l.kind));
        ImGui::TextUnformatted(l.kind == RunLine::Kind::Info ? ("· " + l.text).c_str() : l.text.c_str());
        ImGui::PopStyleColor();
      } else if (i == n && !partial.empty()) {
        ImGui::TextUnformatted(partial.c_str());
      } else {
        ImGui::TextColored(end_color, "%s", ("— " + end_line).c_str());
      }
    }
  }
  if (run_cache_.empty() && partial.empty() && !busy) {
    ui::muted("%s", tr("F5 or the green triangle builds the project and runs its program; its output shows here."));
  }
  // Follow the newest lines while the view is at the bottom; scrolling up (wheel or scroll bar)
  // stops following, scrolling back to the bottom follows again.
  float scroll = ImGui::GetScrollY();
  if (scroll < run_last_scroll_ - 1.0f) run_follow_ = false;
  if (scroll >= ImGui::GetScrollMaxY() - 2.0f) run_follow_ = true;
  if (run_follow_) ImGui::SetScrollHereY(1.0f);
  run_last_scroll_ = scroll;
  ImGui::PopFont();
  if (ImGui::BeginPopupContextWindow("run_menu")) {
    std::string all;
    for (const auto& l : run_cache_) all += l.text + "\n";
    if (ImGui::MenuItem(with_icon(Icon::Copy, tr("Copy the output")).c_str())) ImGui::SetClipboardText(all.c_str());
    if (ImGui::MenuItem(with_icon(Icon::Trash, tr("Clear the output")).c_str(), nullptr, false, !busy)) run_->clear();
    ImGui::EndPopup();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  if (run_->phase() == RunPhase::Running) {
    ImGui::PushFont(theme::mono_font());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##run_input", tr("input for the program: Enter sends the line"), &run_input_,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
      if (run_->send_input(run_input_)) run_input_.clear();
      run_follow_ = true;
      ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::PopFont();
  }
}

}  // namespace kk::gui
