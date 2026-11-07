#include "gain.hpp"

namespace wk {

namespace {
class Gain final : public IGain {
 public:
  void process(std::span<Frame> frames) override {
    for (auto& f : frames) {
      f.left *= gain_;
      f.right *= gain_;
    }
  }
  void set_gain(float gain) override { gain_ = gain; }

 private:
  float gain_ = 1.0f;
};
}  // namespace

std::unique_ptr<IGain> make_gain() { return std::make_unique<Gain>(); }

}  // namespace wk
