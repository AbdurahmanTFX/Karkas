#pragma once
// Colors shared by the panels of karkas-gui.
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>

#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/index/index.hpp"

namespace kk::gui {

inline ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
}

// Status colors (14.5), bright enough for a dark theme.
inline ImVec4 status_color(const std::string& s) {
  if (s == "passing") return rgb(115, 201, 145);
  if (s == "failing") return rgb(241, 76, 76);
  if (s == "drift") return rgb(180, 128, 215);
  if (s == "api_frozen" || s == "tests_ready") return rgb(55, 148, 255);
  if (s == "implemented") return rgb(78, 201, 176);
  if (s == "needs_review") return rgb(230, 150, 40);
  return rgb(139, 139, 139);  // design
}

inline ImVec4 link_color(const std::string& kind) {
  if (kind == "data") return rgb(66, 135, 245);
  if (kind == "event") return rgb(245, 145, 40);
  if (kind == "call") return rgb(170, 170, 170);
  if (kind == "state") return rgb(220, 50, 50);
  return rgb(120, 120, 120, 0.6f);  // use
}

// The i-th color of a sequence of well-separated hues (golden ratio steps).
inline ImVec4 palette_color(std::size_t i, float alpha = 1.0f) {
  double h = 0.08 + static_cast<double>(i) * 0.6180339887;
  h -= static_cast<double>(static_cast<long>(h));
  float r = 0, g = 0, b = 0;
  ImGui::ColorConvertHSVtoRGB(static_cast<float>(h), i % 2 == 0 ? 0.62f : 0.48f, 0.98f, r, g, b);
  return ImVec4(r, g, b, alpha);
}

// A color per block: siblings get well-separated hues (shifted per parent), the same in the
// tree, on block backgrounds and on the links that start at the block.
class BlockColors {
 public:
  ImVec4 get(const index::Snapshot& s, std::uint64_t serial, const std::string& id) {
    if (serial != serial_) {
      cache_.clear();
      serial_ = serial;
    }
    auto it = cache_.find(id);
    if (it != cache_.end()) return it->second;
    std::string parent = core::parent_id(id);
    static const std::vector<std::string> none;
    const auto* pb = parent.empty() ? nullptr : s.block(parent);
    const auto& kids = parent.empty() ? s.top_level : (pb != nullptr ? pb->children : none);
    std::size_t idx = 0;
    for (std::size_t i = 0; i < kids.size(); ++i) {
      if (kids[i] == id) idx = i;
    }
    std::size_t shift = parent.empty() ? 0 : static_cast<std::size_t>(core::fnv1a64(parent) % 7);
    return cache_[id] = palette_color(idx + shift);
  }

 private:
  std::map<std::string, ImVec4> cache_;
  std::uint64_t serial_ = ~0ULL;
};

// A status dot at the cursor (pulsing yellow while an agent works on the block).
inline void status_dot(const std::string& status, bool active, double pulse) {
  auto* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float r = ImGui::GetTextLineHeight() * 0.30f;
  ImVec4 c = active ? rgb(240, 200, 30, static_cast<float>(0.6 + 0.4 * std::sin(pulse * 5.0))) : status_color(status);
  dl->AddCircleFilled(ImVec2(p.x + r + 1, p.y + ImGui::GetTextLineHeight() * 0.5f), r, ImGui::GetColorU32(c));
  ImGui::Dummy(ImVec2(r * 2 + 4, ImGui::GetTextLineHeight()));
  ImGui::SameLine();
}

// Local time of a unix moment: "14:05" today, "12.11 14:05" this year, "12.11.2025" before; "" for 0.
inline std::string when(std::int64_t unix_s) {
  if (unix_s <= 0) return {};
  std::time_t t = static_cast<std::time_t>(unix_s);
  std::time_t now = std::time(nullptr);
  std::tm a{};
  std::tm b{};
  localtime_r(&t, &a);
  localtime_r(&now, &b);
  char buf[32];
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) {
    std::strftime(buf, sizeof buf, "%H:%M", &a);
  } else if (a.tm_year == b.tm_year) {
    std::strftime(buf, sizeof buf, "%d.%m %H:%M", &a);
  } else {
    std::strftime(buf, sizeof buf, "%d.%m.%Y", &a);
  }
  return buf;
}

// "45 s", "3 min 12 s", "1 h 05 min".
inline std::string duration_text(std::int64_t s) {
  if (s < 0) s = 0;
  char buf[32];
  if (s < 60) {
    std::snprintf(buf, sizeof buf, "%lld s", static_cast<long long>(s));
  } else if (s < 3600) {
    std::snprintf(buf, sizeof buf, "%lld min %02lld s", static_cast<long long>(s / 60), static_cast<long long>(s % 60));
  } else {
    std::snprintf(buf, sizeof buf, "%lld h %02lld min", static_cast<long long>(s / 3600), static_cast<long long>(s % 3600 / 60));
  }
  return buf;
}

inline ImVec4 with_alpha(ImVec4 c, float a) {
  c.w = a;
  return c;
}

}  // namespace kk::gui
