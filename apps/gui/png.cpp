#include "png.hpp"

#include <array>
#include <fstream>

namespace kk::gui {

namespace {

std::uint32_t crc32(const std::uint8_t* d, std::size_t n, std::uint32_t c = 0xffffffffu) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1u) != 0 ? 0xedb88320u ^ (v >> 1) : v >> 1;
      t[i] = v;
    }
    return t;
  }();
  for (std::size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xffu] ^ (c >> 8);
  return c;
}

void be32(std::vector<std::uint8_t>& o, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<std::uint8_t>(v >> s));
}

void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
  be32(out, static_cast<std::uint32_t>(data.size()));
  std::vector<std::uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  be32(out, crc32(td.data(), td.size()) ^ 0xffffffffu);
}

}  // namespace

bool write_png(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgba) {
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>((w * 4 + 1) * h));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    auto row = rgba.begin() + static_cast<long>(y) * w * 4;
    raw.insert(raw.end(), row, row + w * 4);
  }
  std::vector<std::uint8_t> z = {0x78, 0x01};
  std::uint32_t a = 1, b = 0;
  for (auto v : raw) {
    a = (a + v) % 65521u;
    b = (b + a) % 65521u;
  }
  for (std::size_t off = 0; off < raw.size() || off == 0; off += 65535) {
    std::size_t len = std::min<std::size_t>(65535, raw.size() - off);
    z.push_back(off + len >= raw.size() ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(len & 0xff));
    z.push_back(static_cast<std::uint8_t>(len >> 8));
    z.push_back(static_cast<std::uint8_t>(~len & 0xff));
    z.push_back(static_cast<std::uint8_t>((~len >> 8) & 0xff));
    z.insert(z.end(), raw.begin() + static_cast<long>(off), raw.begin() + static_cast<long>(off + len));
    if (raw.empty()) break;
  }
  be32(z, (b << 16) | a);
  std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<std::uint8_t> ihdr;
  be32(ihdr, static_cast<std::uint32_t>(w));
  be32(ihdr, static_cast<std::uint32_t>(h));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  return static_cast<bool>(f);
}

}  // namespace kk::gui
