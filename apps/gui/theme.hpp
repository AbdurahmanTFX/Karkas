#pragma once
// Look of karkas-gui: a dark IDE theme (after VSCode Dark Modern and Godot), fonts (UI,
// headings, monospace for code) and the icon glyphs.
#include <imgui.h>

#include <string>

namespace kk::gui::theme {

// Palette.
inline constexpr ImU32 kAccent = IM_COL32(0, 120, 212, 255);      // #0078D4
inline constexpr ImU32 kActivityBg = IM_COL32(24, 24, 24, 255);   // #181818
inline constexpr ImU32 kSideBg = IM_COL32(24, 24, 24, 255);
inline constexpr ImU32 kEditorBg = IM_COL32(31, 31, 31, 255);     // #1F1F1F
inline constexpr ImU32 kBorder = IM_COL32(43, 43, 43, 255);       // #2B2B2B
inline constexpr ImU32 kStatusBg = IM_COL32(24, 24, 24, 255);
inline constexpr ImU32 kTextMuted = IM_COL32(157, 157, 157, 255);
ImVec4 accent(float alpha = 1.0f);
ImVec4 col(ImU32 c);

void apply(float scale);  // ImGui style and colors

// Fonts: call once before the backend creates the font texture.
void load_fonts(float scale, const std::string& configured_ui_font);
ImFont* ui_font();
ImFont* heading_font();  // small medium-weight font for section titles
ImFont* display_font();  // the same, rasterized large (names of blocks drawn as cards)
ImFont* mono_font();     // code
ImFont* icon_font_big(); // icons only, large (activity bar)
// Markdown of notes (ADR 0040): the text font bold, italic or both (the UI font when none is found), and
// headings — levels 1..3 bold and larger, 4..6 bold.
ImFont* md_font(bool bold, bool italic);
ImFont* md_heading_font(int level);

}  // namespace kk::gui::theme
