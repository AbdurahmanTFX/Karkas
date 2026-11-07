#include "writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace wk {

namespace {

void le(std::vector<unsigned char>& b, std::uint32_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xffu));
}

class WavWriter final : public IWriter {
 public:
  Result<Unit> open(const std::string& path, AudioFormat format) override {
    if (!((format.bits == 16 && !format.is_float) || (format.bits == 32 && format.is_float))) {
      return Result<Unit>::fail("only PCM16 or float32 output");
    }
    fmt_ = format;
    fmt_.channels = 2;
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) return Result<Unit>::fail("cannot write " + path);
    std::vector<unsigned char> h(44, 0);
    out_.write(reinterpret_cast<const char*>(h.data()), 44);  // placeholder, patched in close()
    frames_ = 0;
    return Unit{};
  }

  Result<Unit> write(std::span<const Frame> frames) override {
    std::vector<unsigned char> b;
    b.reserve(frames.size() * 8);
    for (const auto& f : frames) {
      for (float s : {f.left, f.right}) {
        if (fmt_.is_float) {
          std::uint32_t bits = 0;
          std::memcpy(&bits, &s, 4);
          le(b, bits, 4);
        } else {
          float c = std::clamp(s, -1.0f, 1.0f);
          auto v = static_cast<std::int16_t>(std::lround(c * 32767.0f));
          le(b, static_cast<std::uint16_t>(v), 2);
        }
      }
    }
    out_.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    frames_ += frames.size();
    return out_ ? Result<Unit>(Unit{}) : Result<Unit>::fail("write failed");
  }

  Result<Unit> close() override {
    const std::uint32_t block = static_cast<std::uint32_t>(2 * fmt_.bits / 8);
    const std::uint32_t data = static_cast<std::uint32_t>(frames_) * block;
    std::vector<unsigned char> h;
    for (char c : std::string("RIFF")) h.push_back(static_cast<unsigned char>(c));
    le(h, 36 + data, 4);
    for (char c : std::string("WAVEfmt ")) h.push_back(static_cast<unsigned char>(c));
    le(h, 16, 4);
    le(h, fmt_.is_float ? 3 : 1, 2);
    le(h, 2, 2);
    le(h, static_cast<std::uint32_t>(fmt_.sample_rate), 4);
    le(h, static_cast<std::uint32_t>(fmt_.sample_rate) * block, 4);
    le(h, block, 2);
    le(h, static_cast<std::uint32_t>(fmt_.bits), 2);
    for (char c : std::string("data")) h.push_back(static_cast<unsigned char>(c));
    le(h, data, 4);
    out_.seekp(0);
    out_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
    out_.close();
    return Unit{};
  }

 private:
  std::ofstream out_;
  AudioFormat fmt_;
  std::size_t frames_ = 0;
};

}  // namespace

std::unique_ptr<IWriter> make_writer() { return std::make_unique<WavWriter>(); }

}  // namespace wk
