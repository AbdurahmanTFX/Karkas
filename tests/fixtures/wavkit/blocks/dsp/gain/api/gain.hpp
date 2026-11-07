#pragma once
// dsp.gain: multiplies samples by a gain factor.
#include <memory>
#include <span>

#include "types.hpp"

namespace wk {

struct IGain {
  virtual ~IGain() = default;
  virtual void process(std::span<Frame> frames) = 0;
  virtual void set_gain(float gain) = 0;
};

std::unique_ptr<IGain> make_gain();

}  // namespace wk
