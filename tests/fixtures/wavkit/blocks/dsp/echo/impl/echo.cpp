#include "echo.hpp"

#include <algorithm>
#include <vector>

namespace wk {

namespace {
class Echo final : public IEcho {
 public:
  void process(std::span<Frame> frames) override {
    if (line_.empty()) return;
    for (auto& f : frames) {
      Frame delayed = line_[pos_];
      line_[pos_] = Frame{f.left + delayed.left * feedback_, f.right + delayed.right * feedback_};
      pos_ = (pos_ + 1) % line_.size();
      f.left = f.left * (1.0f - mix_) + delayed.left * mix_;
      f.right = f.right * (1.0f - mix_) + delayed.right * mix_;
    }
  }
  void set_params(std::size_t delay_frames, float feedback, float mix) override {
    line_.assign(std::max<std::size_t>(delay_frames, 1), Frame{});
    pos_ = 0;
    feedback_ = std::clamp(feedback, 0.0f, 0.99f);
    mix_ = std::clamp(mix, 0.0f, 1.0f);
  }

 private:
  std::vector<Frame> line_;
  std::size_t pos_ = 0;
  float feedback_ = 0.0f;
  float mix_ = 0.0f;
};
}  // namespace

std::unique_ptr<IEcho> make_echo() { return std::make_unique<Echo>(); }

}  // namespace wk
