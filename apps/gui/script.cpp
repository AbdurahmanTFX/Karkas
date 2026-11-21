#include "script.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>

#include "drop.hpp"
#include "kk/core/text.hpp"

namespace kk::gui {

namespace {

// Script positions are in the main window; with windows of the desktop on, ImGui's mouse is
// in desktop coordinates.
void mouse_at(ImGuiIO& io, float x, float y) {
  bool windows = (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
  ImVec2 o = windows ? ImGui::GetMainViewport()->Pos : ImVec2(0, 0);
  io.AddMousePosEvent(o.x + x, o.y + y);
  // The hidden window of a headless run is under no real cursor: say the mouse is over it.
  if (windows) io.AddMouseViewportEvent(ImGui::GetMainViewport()->ID);
}

}  // namespace

std::optional<InputScript> InputScript::parse(std::string_view text, std::string* error) {
  InputScript s;
  auto fail = [&](const std::string& what) -> std::optional<InputScript> {
    if (error != nullptr) *error = what;
    return std::nullopt;
  };
  auto num = [](const std::string& v) { return std::strtof(v.c_str(), nullptr); };
  auto& f = s.frames_;
  float lx = 0, ly = 0;  // where the mouse is after the commands so far (glide starts there)
  for (const auto& raw : core::split(text, ';')) {
    auto w = core::split_ws(raw);
    if (w.empty()) continue;
    const auto& cmd = w[0];
    bool shift = w.back() == "shift";
    auto key_shift = [&](bool down) {
      if (shift) f.emplace_back([down](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, down); });
    };
    if (w.size() >= 3 && cmd != "key" && cmd != "type" && cmd != "drop") {
      bool two_points = (cmd == "drag" || cmd == "rdrag") && w.size() == 5;
      float px = num(w[two_points ? 3 : 1]), py = num(w[two_points ? 4 : 2]);
      if (cmd != "glide") {
        lx = px;
        ly = py;
      }
    }
    if (cmd == "glide" && w.size() == 4) {
      // Eased, so a recorded demo shows where the mouse goes (karkas-gui --show).
      float x0 = lx, y0 = ly, x = num(w[1]), y = num(w[2]);
      int n = std::max(1, static_cast<int>(num(w[3])));
      for (int i = 1; i <= n; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(n);
        float e = t * t * (3 - 2 * t);
        f.emplace_back([=](ImGuiIO& io) { mouse_at(io, x0 + (x - x0) * e, y0 + (y - y0) * e); });
      }
      lx = x;
      ly = y;
    } else if (cmd == "move" && w.size() == 3) {
      float x = num(w[1]), y = num(w[2]);
      f.emplace_back([x, y](ImGuiIO& io) { mouse_at(io, x, y); });
    } else if ((cmd == "click" || cmd == "dclick") && w.size() >= 3) {
      float x = num(w[1]), y = num(w[2]);
      f.emplace_back([x, y](ImGuiIO& io) { mouse_at(io, x, y); });
      key_shift(true);
      for (int i = 0; i < (cmd == "dclick" ? 2 : 1); ++i) {
        f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
        f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
      }
      key_shift(false);
    } else if (cmd == "rclick" && w.size() == 3) {
      float x = num(w[1]), y = num(w[2]);
      f.emplace_back([x, y](ImGuiIO& io) { mouse_at(io, x, y); });
      f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(1, true); });
      f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(1, false); });
    } else if (cmd == "drag" && w.size() == 5) {
      float x1 = num(w[1]), y1 = num(w[2]), x2 = num(w[3]), y2 = num(w[4]);
      f.emplace_back([x1, y1](ImGuiIO& io) { mouse_at(io, x1, y1); });
      f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
      for (int i = 1; i <= 10; ++i) {
        float t = static_cast<float>(i) / 10.0f;
        f.emplace_back([=](ImGuiIO& io) { mouse_at(io, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t); });
      }
      f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    } else if (cmd == "wheel" && w.size() == 4) {
      float x = num(w[1]), y = num(w[2]), d = num(w[3]);
      f.emplace_back([x, y](ImGuiIO& io) { mouse_at(io, x, y); });
      f.emplace_back([d](ImGuiIO& io) { io.AddMouseWheelEvent(0, d); });
    } else if (cmd == "rdrag" && w.size() == 5) {
      float x1 = num(w[1]), y1 = num(w[2]), x2 = num(w[3]), y2 = num(w[4]);
      f.emplace_back([x1, y1](ImGuiIO& io) { mouse_at(io, x1, y1); });
      f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(1, true); });
      for (int i = 1; i <= 10; ++i) {
        float t = static_cast<float>(i) / 10.0f;
        f.emplace_back([=](ImGuiIO& io) { mouse_at(io, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t); });
      }
      f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(1, false); });
    } else if (cmd == "wait" && w.size() == 2) {
      for (int i = 0; i < std::atoi(w[1].c_str()); ++i) f.emplace_back([](ImGuiIO&) {});
    } else if (cmd == "key" && w.size() == 2 && w[1] == "ctrl+o") {
      f.emplace_back([](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
      });
      f.emplace_back([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_O, true); });
      f.emplace_back([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_O, false); });
      f.emplace_back([](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
      });
    } else if (cmd == "key" && w.size() == 2 &&
               (w[1] == "esc" || w[1] == "enter" || w[1] == "end" || w[1] == "home" || w[1] == "backspace" || w[1] == "down" ||
                w[1] == "f5" || w[1] == "f11")) {
      ImGuiKey k = w[1] == "esc"       ? ImGuiKey_Escape
                   : w[1] == "f5"      ? ImGuiKey_F5
                   : w[1] == "f11"     ? ImGuiKey_F11
                   : w[1] == "enter"   ? ImGuiKey_Enter
                   : w[1] == "end"     ? ImGuiKey_End
                   : w[1] == "home"    ? ImGuiKey_Home
                   : w[1] == "down"    ? ImGuiKey_DownArrow
                                       : ImGuiKey_Backspace;
      f.emplace_back([k](ImGuiIO& io) { io.AddKeyEvent(k, true); });
      f.emplace_back([k](ImGuiIO& io) { io.AddKeyEvent(k, false); });
    } else if (cmd == "key" && w.size() == 2 && w[1] == "ctrl+v") {
      f.emplace_back([](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
      });
      f.emplace_back([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_V, true); });
      f.emplace_back([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_V, false); });
      f.emplace_back([](ImGuiIO& io) {
        io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
      });
    } else if (cmd == "drop" && w.size() >= 2) {  // a file dropped on the window (ADR 0041)
      std::string path(core::trim(std::string_view(raw).substr(raw.find("drop") + 4)));
      f.emplace_back([path](ImGuiIO&) { push_dropped({path}); });
    } else if (cmd == "type" && w.size() >= 2) {
      std::string typed(core::trim(std::string_view(raw).substr(raw.find("type") + 4)));
      f.emplace_back([typed](ImGuiIO& io) { io.AddInputCharactersUTF8(typed.c_str()); });
    } else {
      return fail("cannot parse script action '" + std::string(core::trim(raw)) + "'");
    }
  }
  return s;
}

void InputScript::step(ImGuiIO& io) {
  if (!done()) frames_[next_++](io);
}

}  // namespace kk::gui
