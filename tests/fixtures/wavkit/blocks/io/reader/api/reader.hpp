#pragma once
// io.reader: pull source of frames from a WAV file (PCM16 or float32).
#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include "types.hpp"

namespace wk {

struct IReader {
  virtual ~IReader() = default;
  // Parses the header; unsupported formats return an error, never crash.
  virtual Result<AudioFormat> open(const std::string& path) = 0;
  // Fills up to out.size() frames normalized to [-1, 1]; returns the count, 0 at the end.
  virtual std::size_t frames(std::span<Frame> out) = 0;
};

std::unique_ptr<IReader> make_reader();

}  // namespace wk
