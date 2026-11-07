#pragma once
// dsp.chain: runs the configured effects over frames in a fixed order (gain, lowpass, echo).
#include <memory>
#include <span>

#include "types.hpp"

namespace wk {

struct IGain;
struct ILowpass;
struct IEcho;

struct IChain {
  virtual ~IChain() = default;
  virtual void process(std::span<Frame> frames) = 0;
};

// nullptr = effect off.
std::unique_ptr<IChain> make_chain(IGain* gain, ILowpass* lowpass, IEcho* echo);

}  // namespace wk
