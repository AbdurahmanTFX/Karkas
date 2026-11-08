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
    s.links_by_block = j.value("link_colors", std::string("block")) != "kind";
    s.blocks_by_block = j.value("block_colors", std::string("block")) != "status";
    if (j.contains("recent") && j["recent"].is_array()) {
      for (const auto& r : j["recent"]) {
        if (r.is_string()) s.recent.push_back(r.get<std::string>());
      }
    }
  } catch (const json::exception&) {
  }
  return s;
}

void add_recent(GuiSettings& s, const std::filesystem::path& root) {
  auto r = root.string();
  std::erase(s.recent, r);
  s.recent.insert(s.recent.begin(), r);
  if (s.recent.size() > 8) s.recent.resize(8);
}

bool save_settings(const GuiSettings& s) {
  json j = {{"language", lang_code(s.lang)},
            {"block_scale", s.block_scale},
            {"ui_scale", s.ui_scale},
            {"link_colors", s.links_by_block ? "block" : "kind"},
            {"block_colors", s.blocks_by_block ? "block" : "status"},
            {"recent", s.recent}};
  return core::write_file(settings_path(), j.dump(2) + "\n").has_value();
}

}  // namespace kk::gui
