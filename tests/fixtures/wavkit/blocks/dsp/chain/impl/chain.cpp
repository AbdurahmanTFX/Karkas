#include "chain.hpp"

#include "echo.hpp"
#include "gain.hpp"
#include "lowpass.hpp"

namespace wk {

namespace {
class Chain final : public IChain {
 public:
  Chain(IGain* g, ILowpass* l, IEcho* e) : gain_(g), lowpass_(l), echo_(e) {}
  void process(std::span<Frame> frames) override {
    if (gain_ != nullptr) gain_->process(frames);
    if (lowpass_ != nullptr) lowpass_->process(frames);
    if (echo_ != nullptr) echo_->process(frames);
  }

 private:
  IGain* gain_;
  ILowpass* lowpass_;
  IEcho* echo_;
};
}  // namespace

std::unique_ptr<IChain> make_chain(IGain* gain, ILowpass* lowpass, IEcho* echo) {
  return std::make_unique<Chain>(gain, lowpass, echo);
}

}  // namespace wk
