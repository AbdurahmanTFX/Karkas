#pragma once
// stream.pump: reader thread that moves frames from a source into a sink.
#include <cstddef>
#include <memory>
#include <span>

#include "types.hpp"

namespace wk {

struct IReader;

// Where the pump puts frames (implemented by the consumer, e.g. the ring buffer).
struct IFrameSink {
  virtual ~IFrameSink() = default;
  // All or nothing; false when there is no room right now.
  virtual bool push(std::span<const Frame> frames) = 0;
};

struct IPump {
  virtual ~IPump() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  // True after the source reached its end and every frame was delivered.
  [[nodiscard]] virtual bool finished() const = 0;
};

std::unique_ptr<IPump> make_pump(IReader& source, IFrameSink& sink, std::size_t chunk = 256);

}  // namespace wk
