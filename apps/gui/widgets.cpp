#include "widgets.hpp"

#include <cstdarg>

#include "kk/core/text.hpp"
#include "theme.hpp"

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
    ImGui::TextColored(c, "%s", line.c_str());
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

void prop(const char* key, const std::string& value) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", key);
  ImGui::TableNextColumn();
  ImGui::TextWrapped("%s", value.c_str());
}

}  // namespace kk::gui::ui
