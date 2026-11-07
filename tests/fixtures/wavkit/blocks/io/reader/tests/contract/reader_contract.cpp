// Contract tests of io.reader (one per inv of the card).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "reader.hpp"

namespace {

void put16(std::vector<unsigned char>& b, unsigned v) {
  b.push_back(static_cast<unsigned char>(v & 0xff));
  b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
}
void put32(std::vector<unsigned char>& b, unsigned v) {
  put16(b, v & 0xffff);
  put16(b, v >> 16);
}

std::string wav(const std::string& name, unsigned tag, unsigned bits, const std::vector<std::int16_t>& samples) {
  std::vector<unsigned char> b;
  const unsigned data = static_cast<unsigned>(samples.size() * 2);
  for (char c : std::string("RIFF")) b.push_back(static_cast<unsigned char>(c));
  put32(b, 36 + data);
  for (char c : std::string("WAVEfmt ")) b.push_back(static_cast<unsigned char>(c));
  put32(b, 16);
  put16(b, tag);
  put16(b, 2);
  put32(b, 48000);
  put32(b, 48000 * 4);
  put16(b, 4);
  put16(b, bits);
  for (char c : std::string("data")) b.push_back(static_cast<unsigned char>(c));
  put32(b, data);
  for (auto s : samples) put16(b, static_cast<std::uint16_t>(s));
  std::string path = "/tmp/wavkit_reader_" + name + ".wav";
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return path;
}

}  // namespace

TEST_CASE("rejects unsupported formats with an error, never crashes") {
  auto r = wk::make_reader();
  CHECK_FALSE(r->open("/nonexistent/file.wav").ok());
  auto r2 = wk::make_reader();
  CHECK_FALSE(r2->open(wav("pcm8", 1, 8, {1, 2})).ok());
  std::ofstream("/tmp/wavkit_reader_garbage.wav") << "not a wav file at all";
  auto r3 = wk::make_reader();
  CHECK_FALSE(r3->open("/tmp/wavkit_reader_garbage.wav").ok());
}

TEST_CASE("PCM16 samples map into [-1, 1]") {
  std::vector<std::int16_t> s;
  for (int v : {-32768, 32767, 0, 16384, -16384, 1}) s.push_back(static_cast<std::int16_t>(v));
  auto r = wk::make_reader();
  auto fmt = r->open(wav("pcm16", 1, 16, s));
  REQUIRE(fmt.ok());
  CHECK(fmt.value().sample_rate == 48000);
  std::vector<wk::Frame> out(8);
  auto n = r->frames(out);
  CHECK(n == 3);
  for (std::size_t i = 0; i < n; ++i) {
    CHECK(out[i].left >= -1.0f);
    CHECK(out[i].left <= 1.0f);
    CHECK(out[i].right >= -1.0f);
    CHECK(out[i].right <= 1.0f);
  }
  CHECK(out[0].left == doctest::Approx(-1.0f));
  CHECK(r->frames(out) == 0);
}
