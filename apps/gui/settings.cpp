#include "settings.hpp"

#include "kk/core/model.hpp"
#include "kk/core/notes.hpp"

#include <algorithm>
#include <cctype>
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

std::filesystem::path layout_path() { return settings_path().parent_path() / "gui_layout.ini"; }

std::string clean_layout_ini(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    auto end = text.find('\n', pos);
    auto line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos + 1);
    pos = end == std::string_view::npos ? text.size() : end + 1;
    // A value after '=' that is nan or inf (any case, any sign) is not a size.
    bool bad = false;
    for (std::size_t eq = line.find('='); eq != std::string_view::npos && !bad; eq = line.find('=', eq + 1)) {
      std::string v;
      for (std::size_t i = eq + 1; i < line.size() && v.size() < 5; ++i) {
        char c = line[i];
        if (c == '-' || c == '+') continue;
        if (!std::isalpha(static_cast<unsigned char>(c))) break;
        v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      bad = v.starts_with("nan") || v.starts_with("inf");
    }
    if (!bad) out.append(line);
  }
  return out;
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
    s.tickets_list_w = j.value("tickets_list_w", 0.0f);
    s.changes_list_h = j.value("changes_list_h", 0.0f);
    s.show_side = j.value("show_side", true);
    s.show_inspector = j.value("show_inspector", true);
    s.show_bottom = j.value("show_bottom", true);
    s.chat_steps = j.value("chat_steps", true);
    s.show_activity = j.value("show_activity", true);
    s.show_status = j.value("show_status", true);
    // 0.08 was the floor of a size read while the layout was being rebuilt: not a user's size.
    auto frac = [&](const char* key, float def, float lo, float hi) {
      float v = j.value(key, def);
      return v <= 0.0801f ? def : std::clamp(v, lo, hi);
    };
    s.side_frac = frac("side_frac", 0.19f, 0.12f, 0.6f);
    s.inspector_frac = frac("inspector_frac", 0.26f, 0.12f, 0.6f);
    s.bottom_frac = frac("bottom_frac", 0.28f, 0.10f, 0.8f);
    if (j.contains("hidden_tabs") && j["hidden_tabs"].is_array()) {
      for (const auto& t : j["hidden_tabs"]) {
        if (t.is_string()) s.hidden_tabs.push_back(t.get<std::string>());
      }
    }
    if (j.contains("os_windows") && j["os_windows"].is_array()) {
      for (const auto& t : j["os_windows"]) {
        if (t.is_string()) s.os_windows.push_back(t.get<std::string>());
      }
    }
    s.last_code = j.value("last_code", std::string());
    s.last_notes = j.value("last_notes", std::string());
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
  if (s.recent.size() > 16) s.recent.resize(16);
  (is_notes_project_dir(root) ? s.last_notes : s.last_code) = r;
}

bool is_project_dir(const std::filesystem::path& root) {
  std::error_code ec;
  return std::filesystem::is_regular_file(root / "project.ksl", ec);
}

bool is_notes_project_dir(const std::filesystem::path& root) {
  auto text = core::read_file(root / "project.ksl");
  if (!text) return false;
  auto p = core::parse_project_text(*text, "project.ksl");
  return p && core::is_notes_project(*p);
}

bool save_settings(const GuiSettings& s) {
  json j = {{"language", lang_code(s.lang)},
            {"block_scale", s.block_scale},
            {"ui_scale", s.ui_scale},
            {"link_colors", s.links_by_block ? "block" : "kind"},
            {"block_colors", s.blocks_by_block ? "block" : "status"},
            {"recent", s.recent},
            {"last_code", s.last_code},
            {"last_notes", s.last_notes},
            {"tickets_list_w", s.tickets_list_w},
            {"changes_list_h", s.changes_list_h},
            {"show_side", s.show_side},
            {"show_inspector", s.show_inspector},
            {"show_bottom", s.show_bottom},
            {"hidden_tabs", s.hidden_tabs},
            {"chat_steps", s.chat_steps},
            {"show_activity", s.show_activity},
            {"show_status", s.show_status},
            {"side_frac", s.side_frac},
            {"inspector_frac", s.inspector_frac},
            {"bottom_frac", s.bottom_frac},
            {"os_windows", s.os_windows}};
  return core::write_file(settings_path(), j.dump(2) + "\n").has_value();
}

}  // namespace kk::gui
