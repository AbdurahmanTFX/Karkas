#pragma once
// mix: mixes N tracks with per-track gain, clip-safe.
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "types.hpp"

namespace wk {

struct IMix {
  virtual ~IMix() = default;
  virtual void set_track_gain(std::size_t track, float gain) = 0;
  // `out` gets the length of the longest track; samples stay within [-1, 1].
  virtual void mix(std::span<const std::vector<Frame>> tracks, std::vector<Frame>& out) = 0;
};

std::unique_ptr<IMix> make_mix();

}  // namespace wk
