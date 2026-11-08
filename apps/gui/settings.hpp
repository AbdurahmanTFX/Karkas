#pragma once
// User settings of karkas-gui: ~/.config/karkas/gui.json (language, sizes).
#include <filesystem>

#include "i18n.hpp"

namespace kk::gui {

struct GuiSettings {
  Lang lang = Lang::En;
  float block_scale = 1.0f;  // default size of blocks on the schema
  float ui_scale = 1.0f;     // text size of the interface (io.FontGlobalScale)
  bool links_by_block = true;  // link color: of the block it starts at (else: by link kind, 14.5)
};

// Limits of block sizes (canvas units): a block never shrinks to a dot.
inline constexpr float kMinBlockW = 120.0f;
inline constexpr float kMinBlockH = 56.0f;
inline constexpr float kMaxBlockW = 1600.0f;
inline constexpr float kMaxBlockH = 1200.0f;
inline constexpr float kMinBlockScale = 0.6f;
inline constexpr float kMaxBlockScale = 2.5f;

std::filesystem::path settings_path();
GuiSettings load_settings();  // defaults (language from LANG) when the file is missing
bool save_settings(const GuiSettings& s);

}  // namespace kk::gui
