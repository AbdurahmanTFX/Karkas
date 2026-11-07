#include "mix.hpp"

#include <algorithm>

namespace wk {

namespace {
class Mix final : public IMix {
 public:
  void set_track_gain(std::size_t track, float gain) override {
    if (gains_.size() <= track) gains_.resize(track + 1, 1.0f);
    gains_[track] = gain;
  }
  void mix(std::span<const std::vector<Frame>> tracks, std::vector<Frame>& out) override {
    std::size_t len = 0;
    for (const auto& t : tracks) len = std::max(len, t.size());
    out.assign(len, Frame{});
    for (std::size_t k = 0; k < tracks.size(); ++k) {
      float g = k < gains_.size() ? gains_[k] : 1.0f;
      for (std::size_t i = 0; i < tracks[k].size(); ++i) {
        out[i].left += tracks[k][i].left * g;
        out[i].right += tracks[k][i].right * g;
      }
    }
    for (auto& f : out) {
      f.left = std::clamp(f.left, -1.0f, 1.0f);
      f.right = std::clamp(f.right, -1.0f, 1.0f);
    }
  }

 private:
  std::vector<float> gains_;
};
}  // namespace

std::unique_ptr<IMix> make_mix() { return std::make_unique<Mix>(); }

}  // namespace wk
