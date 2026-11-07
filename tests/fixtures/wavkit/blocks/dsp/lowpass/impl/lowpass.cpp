#include "lowpass.hpp"

#include <cmath>

namespace wk {

namespace {
class Lowpass final : public ILowpass {
 public:
  void process(std::span<Frame> frames) override {
    for (auto& f : frames) {
      zl_ += a_ * (f.left - zl_);
      zr_ += a_ * (f.right - zr_);
      f.left = zl_;
      f.right = zr_;
    }
  }
  void set_cutoff(float hz, float sample_rate) override {
    const float x = std::exp(-2.0f * 3.14159265f * hz / sample_rate);
    a_ = 1.0f - x;
  }

 private:
  float a_ = 1.0f;
  float zl_ = 0.0f;
  float zr_ = 0.0f;
};
}  // namespace

std::unique_ptr<ILowpass> make_lowpass() { return std::make_unique<Lowpass>(); }

}  // namespace wk
