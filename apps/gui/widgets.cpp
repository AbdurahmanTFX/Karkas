#include "widgets.hpp"

#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cstdarg>

#include "i18n.hpp"
#include "kk/core/text.hpp"
#include "theme.hpp"

#include <algorithm>
#include <map>

namespace kk::gui::ui {

bool icon_button(const char* id, Icon icon_id, const char* tooltip, bool on, const char* text) {
  std::string label = text != nullptr && *text != '\0' ? with_icon(icon_id, text) : std::string(icon(icon_id));
  label += "##";
  label += id;
  ImGui::PushStyleColor(ImGuiCol_Button, on ? theme::accent(0.35f) : ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? theme::accent(0.5f) : ImVec4(1, 1, 1, 0.08f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::accent(0.6f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
  bool r = ImGui::Button(label.c_str());
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
  return r;
}

bool toggle(const char* id, Icon icon_id, const char* text, bool* v, const char* tooltip) {
  bool clicked = icon_button(id, icon_id, tooltip, *v, text);
  if (clicked) *v = !*v;
  return clicked;
}

void panel_title(const char* text) {
  ImGui::PushFont(theme::heading_font());
  ImGui::PushStyleColor(ImGuiCol_Text, theme::col(theme::kTextMuted));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

bool section(const char* label, bool default_open) {
  ImGui::PushFont(theme::heading_font());
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1, 1, 1, 0.03f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.06f));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1, 1, 1, 0.08f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
  bool open = ImGui::CollapsingHeader(label, default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  ImGui::PopFont();
  return open;
}

void badge(const std::string& text, ImVec4 bg, ImVec4 fg) {
  ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  float h = ImGui::GetTextLineHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImVec2 size(ts.x + h * 0.8f, h);
  auto* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(bg), h * 0.5f);
  dl->AddText(ImVec2(p.x + h * 0.4f, p.y), ImGui::GetColorU32(fg), text.c_str());
  ImGui::Dummy(size);
}

void status_pill(const char* text, ImVec4 color) {
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(icon(Icon::Dot));
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 2);
  ImGui::TextUnformatted(text);
}

void diff_text(const std::string& text) {
  ImGui::PushFont(theme::mono_font());
  for (const auto& line : core::split(text, '\n', false)) {
    ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (line.starts_with("+++") || line.starts_with("---")) {
      c = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    } else if (line.starts_with("+")) {
      c = ImVec4(0.45f, 0.80f, 0.45f, 1.0f);
    } else if (line.starts_with("-")) {
      c = ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
    } else if (line.starts_with("~")) {
      c = ImVec4(0.55f, 0.70f, 0.95f, 1.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }
  ImGui::PopFont();
}

void ksl_text(const std::string& text) {
  ImGui::PushFont(theme::mono_font());
  const ImVec4 comment(106 / 255.0f, 153 / 255.0f, 85 / 255.0f, 1.0f);
  const ImVec4 record(197 / 255.0f, 134 / 255.0f, 192 / 255.0f, 1.0f);
  const ImVec4 directive(86 / 255.0f, 156 / 255.0f, 214 / 255.0f, 1.0f);
  for (const auto& line : core::split(text, '\n', false)) {
    if (line.empty()) {
      ImGui::NewLine();
      continue;
    }
    if (line[0] == '#') {
      ImGui::TextColored(comment, "%s", line.c_str());
      continue;
    }
    auto sp = line.find(' ');
    std::string head = line.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : line.substr(sp);
    ImGui::TextColored(head[0] == '@' ? record : directive, "%s", head.c_str());
    if (!rest.empty()) {
      ImGui::SameLine(0, 0);
      ImGui::TextUnformatted(rest.c_str());
    }
  }
  ImGui::PopFont();
}

void muted(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextV(fmt, args);
  ImGui::PopStyleColor();
  va_end(args);
}

namespace {

bool splitter(const char* id, bool vertical, float* size, float min_a, float min_b, float total, float length) {
  constexpr float kThick = 6.0f;
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, vertical ? ImVec2(kThick, std::max(1.0f, length)) : ImVec2(std::max(1.0f, length), kThick));
  bool hot = ImGui::IsItemHovered();
  bool active = ImGui::IsItemActive();
  if (hot || active) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  if (hot && !active) ImGui::SetItemTooltip("%s", tr("Drag to resize"));
  auto* dl = ImGui::GetWindowDrawList();
  ImU32 c = active ? ImGui::GetColorU32(theme::accent(0.9f)) : hot ? ImGui::GetColorU32(theme::accent(0.5f)) : IM_COL32(255, 255, 255, 18);
  if (vertical) {
    float x = p.x + kThick * 0.5f;
    dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + length), c, active || hot ? 2.0f : 1.0f);
  } else {
    float y = p.y + kThick * 0.5f;
    dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + length, y), c, active || hot ? 2.0f : 1.0f);
  }
  if (!active) {
    *size = std::clamp(*size, min_a, std::max(min_a, total - min_b));
    return false;
  }
  float d = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
  *size = std::clamp(*size + d, min_a, std::max(min_a, total - min_b));
  return d != 0.0f;
}

}  // namespace

bool vsplitter(const char* id, float* size, float min_a, float min_b, float total, float height) {
  return splitter(id, true, size, min_a, min_b, total, height);
}

bool hsplitter(const char* id, float* size, float min_a, float min_b, float total, float width) {
  return splitter(id, false, size, min_a, min_b, total, width);
}

void hand_on_hover() {
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

void update_cursor(bool skip) {
  ImGuiContext& g = *GImGui;
  if (skip || g.MouseCursor != ImGuiMouseCursor_Arrow || g.HoveredId == 0 || g.ActiveId != 0) return;
  if (g.HoveredIdIsDisabled) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
    return;
  }
  // Window chrome (title bar, scrollbars, dock tabs) keeps the arrow.
  if (g.HoveredWindow != nullptr && (g.HoveredId == g.HoveredWindow->MoveId || g.HoveredId == ImGui::GetWindowScrollbarID(g.HoveredWindow, ImGuiAxis_X) ||
                                     g.HoveredId == ImGui::GetWindowScrollbarID(g.HoveredWindow, ImGuiAxis_Y))) {
    return;
  }
  ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

void ksl_wrapped(const std::string& text) {
  ImGui::PushFont(theme::mono_font());
  const ImVec4 comment(106 / 255.0f, 153 / 255.0f, 85 / 255.0f, 1.0f);
  const ImVec4 record(197 / 255.0f, 134 / 255.0f, 192 / 255.0f, 1.0f);
  const ImVec4 directive(86 / 255.0f, 156 / 255.0f, 214 / 255.0f, 1.0f);
  const ImVec4 level(220 / 255.0f, 180 / 255.0f, 100 / 255.0f, 1.0f);
  ImGui::PushTextWrapPos(0.0f);
  for (const auto& line : core::split(text, '\n', false)) {
    if (line.empty()) {
      ImGui::Spacing();
      continue;
    }
    if (line.starts_with("--- ")) {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, theme::col(theme::kTextMuted));
      ImGui::TextUnformatted(line.substr(4).c_str());
      ImGui::PopStyleColor();
      ImGui::Separator();
      continue;
    }
    if (line[0] == '#') {
      ImGui::PushStyleColor(ImGuiCol_Text, comment);
      ImGui::TextWrapped("%s", line.c_str());
      ImGui::PopStyleColor();
      continue;
    }
    // The head: the directive with its padding (KSL aligns values at column 8), or "L2 worker"
    // of a trail line; the rest wraps aligned after it.
    std::size_t end = line.find(' ');
    bool is_level = line.size() > 1 && line[0] == 'L' && line[1] >= '0' && line[1] <= '9';
    if (is_level && end != std::string::npos) end = line.find(' ', line.find_first_not_of(' ', end));
    if (end == std::string::npos) end = line.size();
    std::size_t rest_at = line.find_first_not_of(' ', end);
    if (rest_at == std::string::npos) rest_at = line.size();
    std::string head = line.substr(0, rest_at);
    std::string rest = line.substr(rest_at);
    ImGui::TextColored(head[0] == '@' ? record : is_level ? level : directive, "%s", head.c_str());
    if (!rest.empty()) {
      ImGui::SameLine(0, 0);
      ImGui::TextWrapped("%s", rest.c_str());
    }
  }
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
}

bool copy_button(const char* id, const std::string& text) {
  static std::map<std::string, double> copied;  // id → time of the last copy
  auto it = copied.find(id);
  bool recent = it != copied.end() && ImGui::GetTime() - it->second < 1.5;
  bool r = icon_button(id, recent ? Icon::Check : Icon::Copy, tr("Copy the whole text"), false, recent ? tr("Copied") : tr("Copy"));
  if (r) {
    ImGui::SetClipboardText(text.c_str());
    copied[id] = ImGui::GetTime();
  }
  return r;
}

std::string ellipsize(const std::string& s, float width) {
  if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
  const std::string ell = "…";
  if (ImGui::CalcTextSize(ell.c_str()).x > width) return {};
  std::size_t lo = 0;
  std::size_t hi = s.size();
  while (lo < hi) {
    std::size_t mid = (lo + hi + 1) / 2;
    while (mid > 0 && (static_cast<unsigned char>(s[mid]) & 0xC0) == 0x80) --mid;
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

void prop(const char* key, const std::string& value) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", key);
  ImGui::TableNextColumn();
  ImGui::TextWrapped("%s", value.c_str());
}

}  // namespace kk::gui::ui
