#pragma once
// Scripted mouse and keyboard input for headless checks (karkas-gui --script "..."):
//   move X Y | click X Y [shift] | dclick X Y [shift] | drag X1 Y1 X2 Y2 | wait N | key esc
// separated by ';'. Every action is spread over frames, one event batch per frame.
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiIO;

namespace kk::gui {

class InputScript {
 public:
  // nullopt and a message in `error` when the text does not parse.
  static std::optional<InputScript> parse(std::string_view text, std::string* error = nullptr);
  [[nodiscard]] bool done() const { return next_ >= frames_.size(); }
  [[nodiscard]] bool empty() const { return frames_.empty(); }
  void step(ImGuiIO& io);  // feeds the events of the current frame

 private:
  std::vector<std::function<void(ImGuiIO&)>> frames_;
  std::size_t next_ = 0;
};

}  // namespace kk::gui
