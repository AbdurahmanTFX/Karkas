#pragma once
// dsp.lowpass: one-pole low-pass filter.
#include <memory>
#include <span>

#include "types.hpp"

namespace wk {

struct ILowpass {
  virtual ~ILowpass() = default;
  virtual void process(std::span<Frame> frames) = 0;
  virtual void set_cutoff(float hz, float sample_rate) = 0;
};

std::unique_ptr<ILowpass> make_lowpass();

}  // namespace wk
