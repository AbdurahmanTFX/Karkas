#include "reader.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace wk {

namespace {

std::uint32_t le32(const unsigned char* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

class WavReader final : public IReader {
 public:
  Result<AudioFormat> open(const std::string& path) override {
    in_.open(path, std::ios::binary);
    if (!in_) return Result<AudioFormat>::fail("cannot open " + path);
    unsigned char riff[12];
    if (!in_.read(reinterpret_cast<char*>(riff), 12) || std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
      return Result<AudioFormat>::fail("not a RIFF/WAVE file");
    }
    bool have_fmt = false;
    while (true) {
      unsigned char hdr[8];
      if (!in_.read(reinterpret_cast<char*>(hdr), 8)) return Result<AudioFormat>::fail("no data chunk");
      std::uint32_t size = le32(hdr + 4);
      if (std::memcmp(hdr, "fmt ", 4) == 0) {
        if (size < 16 || size > 64) return Result<AudioFormat>::fail("bad fmt chunk");
        std::vector<unsigned char> f(size);
        if (!in_.read(reinterpret_cast<char*>(f.data()), size)) return Result<AudioFormat>::fail("short fmt chunk");
        std::uint16_t tag = le16(f.data());
        fmt_.channels = le16(f.data() + 2);
        fmt_.sample_rate = static_cast<int>(le32(f.data() + 4));
        fmt_.bits = le16(f.data() + 14);
        fmt_.is_float = tag == 3;
        bool pcm16 = tag == 1 && fmt_.bits == 16;
        bool f32 = tag == 3 && fmt_.bits == 32;
        if (!pcm16 && !f32) return Result<AudioFormat>::fail("unsupported format (only PCM16 and float32)");
        if (fmt_.channels < 1 || fmt_.channels > 2) return Result<AudioFormat>::fail("only mono or stereo");
        have_fmt = true;
      } else if (std::memcmp(hdr, "data", 4) == 0) {
        if (!have_fmt) return Result<AudioFormat>::fail("data before fmt");
        remaining_ = size;
        return fmt_;
      } else {
        in_.seekg(size + (size & 1u), std::ios::cur);
      }
    }
  }

  std::size_t frames(std::span<Frame> out) override {
    const std::size_t bytes_per_sample = static_cast<std::size_t>(fmt_.bits / 8);
    const std::size_t frame_bytes = bytes_per_sample * static_cast<std::size_t>(fmt_.channels);
    if (frame_bytes == 0) return 0;
    std::size_t want = std::min<std::size_t>(out.size(), remaining_ / frame_bytes);
    std::vector<unsigned char> buf(want * frame_bytes);
    if (want == 0 || !in_.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))) return 0;
    remaining_ -= buf.size();
    for (std::size_t i = 0; i < want; ++i) {
      float ch[2] = {0.0f, 0.0f};
      for (int c = 0; c < fmt_.channels; ++c) {
        const unsigned char* p = buf.data() + i * frame_bytes + static_cast<std::size_t>(c) * bytes_per_sample;
        if (fmt_.is_float) {
          std::uint32_t bits = le32(p);
          std::memcpy(&ch[c], &bits, 4);
        } else {
          ch[c] = static_cast<float>(static_cast<std::int16_t>(le16(p))) / 32768.0f;
        }
      }
      out[i] = Frame{ch[0], fmt_.channels == 1 ? ch[0] : ch[1]};
    }
    return want;
  }

 private:
  std::ifstream in_;
  AudioFormat fmt_;
  std::size_t remaining_ = 0;
};

}  // namespace

std::unique_ptr<IReader> make_reader() { return std::make_unique<WavReader>(); }

}  // namespace wk
