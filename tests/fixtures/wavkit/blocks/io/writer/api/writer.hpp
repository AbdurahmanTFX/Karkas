#pragma once
// io.writer: writes frames to a WAV file (PCM16 or float32, little-endian).
#include <memory>
#include <span>
#include <string>

#include "types.hpp"

namespace wk {

struct IWriter {
  virtual ~IWriter() = default;
  virtual Result<Unit> open(const std::string& path, AudioFormat format) = 0;
  virtual Result<Unit> write(std::span<const Frame> frames) = 0;
  // Patches the header sizes; the file is valid only after close().
  virtual Result<Unit> close() = 0;
};

std::unique_ptr<IWriter> make_writer();

}  // namespace wk
