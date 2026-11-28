#include "kk/core/paths.hpp"

#include "kk/core/lang.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kk::core {

fs::path self_executable() {
#ifdef _WIN32
  std::wstring buf(32768, L'\0');
  auto n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size()) return {};
  buf.resize(n);
  return fs::path(buf);
#endif
  std::error_code ec;
  auto exe = fs::read_symlink("/proc/self/exe", ec).string();
  if (ec || exe.empty()) return {};
  constexpr std::string_view kDeleted = " (deleted)";
  if (exe.ends_with(kDeleted)) {
    auto again = exe.substr(0, exe.size() - kDeleted.size());
    if (fs::exists(again, ec)) return again;
  }
  return exe;
}

fs::path normalize(const fs::path& p) {
  std::error_code ec;
  auto abs = fs::absolute(p, ec);
  if (ec) abs = p;
  auto canon = fs::weakly_canonical(abs, ec);
  if (ec) return abs.lexically_normal();
  return canon.lexically_normal();
}

bool is_within(const fs::path& root, const fs::path& p) {
  auto r = normalize(root);
  auto q = normalize(p);
  auto rit = r.begin();
  auto qit = q.begin();
  for (; rit != r.end(); ++rit, ++qit) {
    if (rit->empty()) continue;  // trailing separator
    if (qit == q.end() || *rit != *qit) return false;
  }
  return true;
}

std::string rel_str(const fs::path& root, const fs::path& p) {
  auto r = p.lexically_relative(root);
  if (r.empty()) return p.generic_string();
  return r.generic_string();
}

Result<std::string> read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return fail("io", fmt::format("cannot read {}", p.string()));
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

VoidResult write_file(const fs::path& p, std::string_view content) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  auto tmp = p;
  tmp += fmt::format(".tmp{:x}", rng());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return fail("io", fmt::format("cannot write {}", p.string()));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) return fail("io", fmt::format("cannot write {}", p.string()));
  }
  // The new file replaces the old one by rename: it takes the old one's mode, or a script an agent
  // edited is no longer executable (bugs.txt 23). Best effort: a mode that cannot be read stays default.
  std::error_code mode_ec;
  if (auto st = fs::status(p, mode_ec); !mode_ec && fs::is_regular_file(st)) {
    fs::permissions(tmp, st.permissions(), fs::perm_options::replace, mode_ec);
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return fail("io", fmt::format("cannot replace {}", p.string()));
  }
  return {};
}

VoidResult append_file(const fs::path& p, std::string_view content) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::app);
  if (!out) return fail("io", fmt::format("cannot append to {}", p.string()));
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return {};
}

std::string target_name(std::string_view block_id) {
  std::string s(block_id);
  std::replace(s.begin(), s.end(), '.', '_');
  return s;
}

bool is_code_file(const fs::path& p) { return language_of(p) != nullptr; }

bool is_build_dir_name(std::string_view name) {
  if (!name.starts_with("build")) return false;
  return name.size() == 5 || !(name[5] >= 'a' && name[5] <= 'z');
}

bool is_build_dir(const fs::path& dir) {
  std::error_code ec;
  return is_build_dir_name(dir.filename().string()) && !fs::exists(dir / "block.ksl", ec);
}

Result<fs::path> find_project_root(const fs::path& start) {
  auto p = normalize(start);
  while (true) {
    if (fs::exists(p / "project.ksl")) return p;
    if (!p.has_parent_path() || p.parent_path() == p) break;
    p = p.parent_path();
  }
  return fail("io", fmt::format("no project.ksl found in {} or its parents", start.string()));
}

}  // namespace kk::core
