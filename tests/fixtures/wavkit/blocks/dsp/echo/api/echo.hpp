#pragma once
// dsp.echo: feedback delay line.
#include <cstddef>
#include <memory>
#include <span>

#include "types.hpp"

namespace wk {

struct IEcho {
  virtual ~IEcho() = default;
  virtual void process(std::span<Frame> frames) = 0;
  // delay in frames, feedback in [0, 1), mix in [0, 1]
  virtual void set_params(std::size_t delay_frames, float feedback, float mix) = 0;
};

std::unique_ptr<IEcho> make_echo();

}  // namespace wk
