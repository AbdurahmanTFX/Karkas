#include "script.hpp"

#include <imgui.h>

#include <cstdlib>

#include "kk/core/text.hpp"

namespace kk::gui {

std::optional<InputScript> InputScript::parse(std::string_view text, std::string* error) {
  InputScript s;
  auto fail = [&](const std::string& what) -> std::optional<InputScript> {
    if (error != nullptr) *error = what;
    return std::nullopt;
  };
  auto num = [](const std::string& v) { return std::strtof(v.c_str(), nullptr); };
  auto& f = s.frames_;
  for (const auto& raw : core::split(text, ';')) {
    auto w = core::split_ws(raw);
    if (w.empty()) continue;
    const auto& cmd = w[0];
    bool shift = w.back() == "shift";
    auto key_shift = [&](bool down) {
      if (shift) f.emplace_back([down](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Shift, down); });
    };
    if (cmd == "move" && w.size() == 3) {
      float x = num(w[1]), y = num(w[2]);
      f.emplace_back([x, y](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
    } else if ((cmd == "click" || cmd == "dclick") && w.size() >= 3) {
      float x = num(w[1]), y = num(w[2]);
      f.emplace_back([x, y](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
      key_shift(true);
      for (int i = 0; i < (cmd == "dclick" ? 2 : 1); ++i) {
        f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
        f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
      }
      key_shift(false);
    } else if (cmd == "drag" && w.size() == 5) {
      float x1 = num(w[1]), y1 = num(w[2]), x2 = num(w[3]), y2 = num(w[4]);
      f.emplace_back([x1, y1](ImGuiIO& io) { io.AddMousePosEvent(x1, y1); });
      f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
      for (int i = 1; i <= 10; ++i) {
        float t = static_cast<float>(i) / 10.0f;
        f.emplace_back([=](ImGuiIO& io) { io.AddMousePosEvent(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t); });
      }
      f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
    } else if (cmd == "wheel" && w.size() == 4) {
      float x = num(w[1]), y = num(w[2]), d = num(w[3]);
      f.emplace_back([x, y](ImGuiIO& io) { io.AddMousePosEvent(x, y); });
      f.emplace_back([d](ImGuiIO& io) { io.AddMouseWheelEvent(0, d); });
    } else if (cmd == "rdrag" && w.size() == 5) {
      float x1 = num(w[1]), y1 = num(w[2]), x2 = num(w[3]), y2 = num(w[4]);
      f.emplace_back([x1, y1](ImGuiIO& io) { io.AddMousePosEvent(x1, y1); });
      f.emplace_back([](ImGuiIO& io) { io.AddMouseButtonEvent(1, true); });
      for (int i = 1; i <= 10; ++i) {
        float t = static_cast<float>(i) / 10.0f;
        f.emplace_back([=](ImGuiIO& io) { io.AddMousePosEvent(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t); });
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
    } else if (cmd == "key" && w.size() == 2 && (w[1] == "esc" || w[1] == "enter")) {
      ImGuiKey k = w[1] == "esc" ? ImGuiKey_Escape : ImGuiKey_Enter;
      f.emplace_back([k](ImGuiIO& io) { io.AddKeyEvent(k, true); });
      f.emplace_back([k](ImGuiIO& io) { io.AddKeyEvent(k, false); });
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
