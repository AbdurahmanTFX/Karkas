#pragma once
// stream.ring: lock-free single-producer single-consumer ring of audio frames.
#include <cstddef>
#include <memory>
#include <span>

#include "types.hpp"

namespace wk {

struct IRing {
  virtual ~IRing() = default;
  // All or nothing; never blocks: false when there is not enough room.
  virtual bool push(std::span<const Frame> frames) = 0;
  // Takes up to out.size() frames; never blocks; returns the count.
  virtual std::size_t pop(std::span<Frame> out) = 0;
  [[nodiscard]] virtual std::size_t capacity() const = 0;
  [[nodiscard]] virtual std::size_t fill() const = 0;
};

// `capacity` is rounded up to a power of two.
std::unique_ptr<IRing> make_ring(std::size_t capacity);

}  // namespace wk
