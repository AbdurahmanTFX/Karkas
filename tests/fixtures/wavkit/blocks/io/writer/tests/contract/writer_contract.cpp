// Contract tests of io.writer.
#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include "writer.hpp"

namespace {
std::vector<unsigned char> slurp(const char* p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
std::uint32_t le32(const std::vector<unsigned char>& b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
         (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
}  // namespace

TEST_CASE("output is a valid little-endian PCM16 or float32 WAV file") {
  auto w = wk::make_writer();
  REQUIRE(w->open("/tmp/wavkit_writer_a.wav", wk::AudioFormat{48000, 2, 16, false}).ok());
  std::vector<wk::Frame> f = {{0.5f, -0.5f}, {1.0f, -1.0f}};
  REQUIRE(w->write(f).ok());
  REQUIRE(w->close().ok());
  auto b = slurp("/tmp/wavkit_writer_a.wav");
  REQUIRE(b.size() == 44 + 8);
  CHECK(std::string(b.begin(), b.begin() + 4) == "RIFF");
  CHECK(std::string(b.begin() + 8, b.begin() + 12) == "WAVE");
  CHECK(le32(b, 24) == 48000);
  // 0.5 → 16384 (0x4000) stored low byte first.
  CHECK(b[44] == 0x00);
  CHECK(b[45] == 0x40);
  CHECK_FALSE(w->open("/tmp/wavkit_writer_b.wav", wk::AudioFormat{48000, 2, 24, false}).ok());
}

TEST_CASE("header sizes match the number of frames written") {
  auto w = wk::make_writer();
  REQUIRE(w->open("/tmp/wavkit_writer_c.wav", wk::AudioFormat{44100, 2, 32, true}).ok());
  std::vector<wk::Frame> f(1000);
  for (int i = 0; i < 3; ++i) REQUIRE(w->write(f).ok());
  REQUIRE(w->close().ok());
  auto b = slurp("/tmp/wavkit_writer_c.wav");
  CHECK(le32(b, 40) == 3000u * 8u);
  CHECK(le32(b, 4) == 36u + 3000u * 8u);
  CHECK(b.size() == 44u + 3000u * 8u);
}
