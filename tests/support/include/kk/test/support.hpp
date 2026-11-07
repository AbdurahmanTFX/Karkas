#pragma once
// Helpers for Karkas tests: fixtures, golden files, temporary directories.
#include <filesystem>
#include <string>
#include <string_view>

namespace kk::test {

namespace fs = std::filesystem;

fs::path source_dir();
fs::path fixture(std::string_view name);  // tests/fixtures/<name>
fs::path golden(std::string_view name);   // tests/golden/<name>
std::string read(const fs::path& p);
void write(const fs::path& p, std::string_view content);
void copy_tree(const fs::path& from, const fs::path& to);

// Compares `actual` with a golden file. With KK_UPDATE_GOLDEN=1 in the
// environment the golden file is (re)written instead and the check passes.
bool matches_golden(std::string_view name, std::string_view actual);

// A unique empty directory under the build tree, removed on destruction
// (kept when KK_KEEP_TMP=1).
class TempDir {
 public:
  explicit TempDir(std::string_view tag = "t");
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] const fs::path& path() const { return path_; }
  fs::path operator/(std::string_view rel) const { return path_ / rel; }

 private:
  fs::path path_;
};

// Copies a fixture into a fresh temp dir and returns it.
class FixtureCopy : public TempDir {
 public:
  explicit FixtureCopy(std::string_view fixture_name);
};

}  // namespace kk::test
