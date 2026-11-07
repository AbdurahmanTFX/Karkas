#include "kk/test/support.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace kk::test {

fs::path source_dir() { return fs::path(KK_SOURCE_DIR); }
fs::path fixture(std::string_view name) { return source_dir() / "tests" / "fixtures" / name; }
fs::path golden(std::string_view name) { return source_dir() / "tests" / "golden" / name; }

std::string read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write(const fs::path& p, std::string_view content) {
  if (p.has_parent_path()) fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void copy_tree(const fs::path& from, const fs::path& to) {
  fs::create_directories(to);
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                         fs::copy_options::copy_symlinks);
}

bool matches_golden(std::string_view name, std::string_view actual) {
  auto path = golden(name);
  const char* upd = std::getenv("KK_UPDATE_GOLDEN");
  if (upd != nullptr && std::string_view(upd) == "1") {
    write(path, actual);
    return true;
  }
  if (!fs::exists(path)) return false;
  return read(path) == actual;
}

TempDir::TempDir(std::string_view tag) {
  static std::atomic<int> counter{0};
  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  path_ = fs::path(KK_TEST_TMP) / (std::string(tag) + "-" + std::to_string(::getpid()) + "-" +
                                   std::to_string(stamp) + "-" + std::to_string(counter++));
  fs::create_directories(path_);
}

TempDir::~TempDir() {
  const char* keep = std::getenv("KK_KEEP_TMP");
  if (keep != nullptr && std::string_view(keep) == "1") return;
  std::error_code ec;
  fs::remove_all(path_, ec);
}

FixtureCopy::FixtureCopy(std::string_view fixture_name) : TempDir(fixture_name) {
  copy_tree(fixture(fixture_name), path());
}

}  // namespace kk::test
