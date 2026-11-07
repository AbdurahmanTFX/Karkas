#include "settings.hpp"

#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>

#include "kk/core/paths.hpp"

namespace kk::gui {

using json = nlohmann::json;

std::filesystem::path settings_path() {
  std::filesystem::path base;
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x != nullptr && *x != '\0') {
    base = x;
  } else if (const char* h = std::getenv("HOME"); h != nullptr) {
    base = std::filesystem::path(h) / ".config";
  }
  return base / "karkas" / "gui.json";
}

GuiSettings load_settings() {
  GuiSettings s;
  s.lang = lang_from_env();
  auto text = core::read_file(settings_path());
  if (!text) return s;
  try {
    auto j = json::parse(*text);
    if (j.contains("language")) s.lang = lang_from_code(j.value("language", "en"));
    s.block_scale = std::clamp(j.value("block_scale", 1.0f), kMinBlockScale, kMaxBlockScale);
    s.ui_scale = std::clamp(j.value("ui_scale", 1.0f), 0.8f, 1.6f);
  } catch (const json::exception&) {
  }
  return s;
}

bool save_settings(const GuiSettings& s) {
  json j = {{"language", lang_code(s.lang)}, {"block_scale", s.block_scale}, {"ui_scale", s.ui_scale}};
  return core::write_file(settings_path(), j.dump(2) + "\n").has_value();
}

}  // namespace kk::gui
