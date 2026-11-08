#include "kk/code/init.hpp"

#include <fmt/format.h>

#include <regex>

#include "kk/code/gen.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/index/state.hpp"

namespace fs = std::filesystem;

namespace kk::code {

std::string project_id_from(std::string_view folder) {
  auto name = core::to_lower(std::string(folder));
  for (auto& c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
  }
  if (name.empty() || !(name[0] >= 'a' && name[0] <= 'z')) return "";
  return name;
}

bool has_code(const fs::path& dir) {
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    auto n = it->path().filename().string();
    if (it->is_directory(ec) && (n.starts_with(".") || n.starts_with("build") || n == "third_party" || n == "_deps")) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec) && core::is_code_file(it->path())) return true;
  }
  return false;
}

kk::Result<InitResult> init_project(const fs::path& root_in, std::string_view idea) {
  fs::path root = core::normalize(root_in);
  InitResult out;
  out.name = project_id_from(root.filename().string());
  if (out.name.empty()) return kk::fail("init", "project folder name must start with a letter");
  std::error_code ec;
  if (fs::exists(root / "project.ksl", ec)) return kk::fail("init", root.string() + " already holds a project.ksl");
  fs::create_directories(root, ec);
  if (ec) return kk::fail("init", fmt::format("cannot create {}: {}", root.string(), ec.message()));
  // An existing project: keep its C++ standard and do not invent rules for its code.
  out.existing_code = has_code(root);
  std::string cxx = "20";
  if (auto cm = core::read_file(root / "CMakeLists.txt")) {
    std::smatch m;
    if (std::regex_search(*cm, m, std::regex(R"(CMAKE_CXX_STANDARD\s+(\d+))")) ||
        std::regex_search(*cm, m, std::regex(R"(cxx_std_(\d+))"))) {
      cxx = m[1].str();
    }
  }
  std::string project = fmt::format("@project {}\n", out.name);
  if (!idea.empty()) project += core::format_line("idea", core::format_free_text(idea)) + "\n";
  project += fmt::format("lang    cpp\nbuild   cmake-ninja\nstd     c++{}\ntest    doctest\n", cxx);
  if (!out.existing_code) {
    project += "rule    blocks talk only through api/ headers\n"
               "rule    no exceptions across block boundaries, use Result<T>\n";
  }
  auto write = [&](const fs::path& p, std::string_view text) -> kk::VoidResult {
    if (fs::exists(p, ec)) return {};
    if (auto r = core::write_file(p, text); !r) return r;
    out.written.push_back(core::rel_str(root, p));
    return {};
  };
  auto toml = target_template("karkas.toml");
  if (!toml) return std::unexpected(toml.error());
  for (const auto& [path, text] : std::vector<std::pair<fs::path, std::string>>{
           {root / "project.ksl", project},
           {root / "karkas.toml", std::string(*toml)},
           {root / "cmake" / "user.cmake", std::string(target_template("user.cmake").value_or(""))},
           {root / ".gitignore", std::string(target_template("gitignore").value_or(""))}}) {
    if (auto r = write(path, text); !r) return std::unexpected(r.error());
  }
  fs::create_directories(root / "blocks", ec);
  fs::create_directories(root / "tests" / "integration", ec);
  if (auto r = index::ensure_karkas_dir(root); !r) return std::unexpected(r.error());
  return out;
}

}  // namespace kk::code
