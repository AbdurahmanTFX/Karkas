#pragma once
// Small IDE-style widgets on top of ImGui: flat icon buttons, toggles, section headers,
// badges, colored diffs.
#include <imgui.h>

#include <string>

#include "icons.hpp"

namespace kk::gui::ui {

// A docked panel without the dock tab bar (the shell draws its own headers and tabs).
// Call ImGui::End() whatever it returns.
bool begin_docked(const char* name, ImU32 bg, ImVec2 padding = ImVec2(10, 8));
// Flat button with an icon (and optional text); highlighted when `on`.
bool icon_button(const char* id, Icon icon, const char* tooltip = nullptr, bool on = false, const char* text = nullptr);
// A toggle chip: flips `*v` when clicked.
bool toggle(const char* id, Icon icon, const char* text, bool* v, const char* tooltip = nullptr);
// Uppercase-style title of a side panel or a section (heading font, muted).
void panel_title(const char* text);
// Collapsible section header like the side bar of an IDE.
bool section(const char* label, bool default_open = true);
// Rounded pill with text.
void badge(const std::string& text, ImVec4 bg, ImVec4 fg = ImVec4(1, 1, 1, 1));
// Status as a colored dot + text.
void status_pill(const char* text, ImVec4 color);
// Text with + / - lines colored like a diff, in the monospace font.
void diff_text(const std::string& text);
// KSL text (card, ticket) with the directive words colored like a code editor.
void ksl_text(const std::string& text);
// Muted text.
void muted(const char* fmt, ...) IM_FMTARGS(1);
// Key-value row inside a two-column table.
void prop(const char* key, const std::string& value);

}  // namespace kk::gui::ui
