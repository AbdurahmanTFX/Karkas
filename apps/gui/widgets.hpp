#pragma once
// Small IDE-style widgets on top of ImGui: flat icon buttons, toggles, section headers,
// badges, colored diffs.
#include <imgui.h>

#include <string>

#include "icons.hpp"

namespace kk::gui::ui {

// Flat button with an icon (and optional text); highlighted when `on`.
bool icon_button(const char* id, Icon icon, const char* tooltip = nullptr, bool on = false, const char* text = nullptr);

// A tool bar that fits its panel (ADR 0043). Between begin and end, icon buttons and toggles lose their
// text when the bar with all its texts would be wider than the panel (the text goes to the tooltip),
// and an item that does not fit on the row starts a new row. `*full` keeps the width the bar needs with
// its texts (measured when it was drawn with them).
void tool_row_begin(float* full);
void tool_row_end(float* full);
// Before an item of the bar that is not an icon button (a separator, a text): a new row when it does
// not fit on this one.
void tool_fit(float width);
[[nodiscard]] bool tool_compact();
// A toggle chip: flips `*v` when clicked.
bool toggle(const char* id, Icon icon, const char* text, bool* v, const char* tooltip = nullptr);
// Uppercase-style title of a side panel or a section (heading font, muted); with `reserve`, shortened
// with "…" to leave that much room on its right (the panel's buttons).
void panel_title(const char* text, float reserve = -1.0f);
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

// A splitter at the cursor between two areas: vertical (side by side, `*size` = width of the
// first) or horizontal (one above the other, `*size` = height of the first). Dragging keeps
// `*size` within [min_a, total - min_b]; the cursor shows the resize arrows. True while moved.
bool vsplitter(const char* id, float* size, float min_a, float min_b, float total, float height);
bool hsplitter(const char* id, float* size, float min_a, float min_b, float total, float width);
// The "hand" cursor over the last item (something to click).
void hand_on_hover();
// Mouse cursor for the frame: "hand" over clickable items unless something else set a cursor
// (text input, resize). Call once at the end of the frame; `skip` when the schema canvas owns it.
void update_cursor(bool skip);
// KSL text (ticket, card, change) wrapped to the available width: directive words colored,
// continuation lines aligned after them, "--- section" lines as headers.
void ksl_wrapped(const std::string& text);
// A small "Copy" button: puts `text` on the clipboard and says "Copied" for a moment.
bool copy_button(const char* id, const std::string& text);
// The longest prefix of `s` (whole UTF-8 characters) that fits `width`, with "…" when cut.
std::string ellipsize(const std::string& s, float width);

}  // namespace kk::gui::ui
