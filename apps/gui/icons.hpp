#pragma once
// Icons of karkas-gui: small vector shapes (signed distance functions) rasterized into the
// font atlas as private-use glyphs, so they work in any text — tabs, menus, the tree,
// buttons — and take the text color. No icon font is needed.
#include <string>

struct ImFont;

namespace kk::gui {

enum class Icon {
  Folder, FolderOpen, File, Cube, Play, Library, Gear, Search, Layers, Link, Undo, Plus, Close, Check,
  Warning, Error, Info, Ticket, Agent, Hammer, Coins, Changes, Eye, Fit, Home, ArrowUp, Schema, Code,
  Sliders, Files, Wave, Dot, Pin, Card, Stop, Trash, Send, Chat,
  Copy, Windows, Pencil, Bolt, ChevronRight, ChevronDown, Ghost, Float, PopOut, Dock, Sketch, Corner, Focus, Lock, Unlock, Count
};

// UTF-8 text of the icon glyph (U+E000 + index).
const char* icon(Icon i);
// "<icon> text" for labels.
std::string with_icon(Icon i, const std::string& text);

// Adds the icon glyphs to `font` (call before the atlas is built) and, after building,
// rasterizes them into the RGBA32 atlas texture.
void add_icon_glyphs(ImFont* font, float size_px);
void rasterize_icon_glyphs();

}  // namespace kk::gui
