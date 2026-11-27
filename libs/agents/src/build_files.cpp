#include "kk/agents/build_files.hpp"
#include "kk/core/lang.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::agents {

namespace fs = std::filesystem;

namespace {

bool is_header(const fs::path& p) {
  auto e = p.extension().string();
  return e == ".h" || e == ".hh" || e == ".hpp" || e == ".hxx";
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// The CMakeLists.txt files of the project, deepest first; build trees, dependencies and hidden folders skipped.
std::vector<fs::path> cmake_lists(const fs::path& root) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end;
       it.increment(ec)) {
    if (ec) break;
    const auto name = it->path().filename().string();
    if (it->is_directory(ec)) {
      if (name.starts_with(".") || name == "build" || name.starts_with("build-") || name == "_deps" ||
          name == "third_party" || name == "external" || name == "vcpkg_installed" || name == "node_modules") {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (name == "CMakeLists.txt") out.push_back(it->path());
  }
  std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
    auto da = std::distance(a.begin(), a.end()), db = std::distance(b.begin(), b.end());
    return da != db ? da > db : a < b;
  });
  return out;
}

// One token of a CMakeLists.txt line: a word between whitespace, parentheses and quotes.
struct Token {
  std::size_t line;
  std::size_t begin;
  std::size_t end;
  std::string text;
};

std::vector<Token> tokens_of(const std::vector<std::string>& lines) {
  std::vector<Token> out;
  for (std::size_t n = 0; n < lines.size(); ++n) {
    const auto& l = lines[n];
    std::size_t i = 0;
    while (i < l.size()) {
      if (l[i] == '#') break;  // a comment
      if (std::isspace(static_cast<unsigned char>(l[i])) || l[i] == '(' || l[i] == ')' || l[i] == '"') {
        ++i;
        continue;
      }
      std::size_t j = i;
      while (j < l.size() && !std::isspace(static_cast<unsigned char>(l[j])) && l[j] != '(' && l[j] != ')' && l[j] != '"') ++j;
      out.push_back({n, i, j, l.substr(i, j - i)});
      i = j;
    }
  }
  return out;
}

// The file a token names, relative to the project root, or nothing (a variable, a keyword, not a code file).
std::optional<fs::path> named_file(const fs::path& root, const fs::path& list_dir, std::string token) {
  for (const char* prefix : {"${CMAKE_CURRENT_SOURCE_DIR}/", "${CMAKE_CURRENT_LIST_DIR}/"}) {
    if (token.starts_with(prefix)) token = token.substr(std::string(prefix).size());
  }
  if (token.empty() || core::contains(token, "${") || core::contains(token, "*")) return std::nullopt;
  fs::path p(token);
  const auto* lang = core::language_of(p);
  if (lang == nullptr || lang->id != "cpp") return std::nullopt;  // a CMakeLists.txt names C and C++ files
  return fs::relative((list_dir / p).lexically_normal(), root).lexically_normal();
}

// The lines of a text as they are (indentation and empty lines kept); `newline_at_end` remembers the last '\n'.
std::vector<std::string> lines_of(const std::string& text, bool& newline_at_end) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    auto pos = text.find('\n', start);
    if (pos == std::string::npos) pos = text.size();
    out.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  newline_at_end = !text.empty() && text.back() == '\n';
  return out;
}

bool alone_on_line(const std::vector<std::string>& lines, const Token& t) {
  const auto& l = lines[t.line];
  return core::trim(std::string_view(l).substr(0, t.begin)).empty() && core::trim(std::string_view(l).substr(t.end)).empty();
}

}  // namespace

std::vector<std::string> add_sources_to_build(const fs::path& root, const std::vector<std::string>& files) {
  std::vector<std::string> report;
  const auto lists = cmake_lists(root);
  if (lists.empty()) return report;
  std::vector<std::pair<fs::path, std::string>> added;  // build file, entry written
  for (const auto& f : files) {
    fs::path file = fs::path(std::string(core::trim(f))).lexically_normal();
    std::error_code ec;
    // A CMakeLists.txt builds C and C++; a new Python or shell file of a block needs no build entry (ADR 0051).
    const auto* lang = core::language_of(file);
    if (file.empty() || file.is_absolute() || lang == nullptr || lang->id != "cpp" || !fs::is_regular_file(root / file, ec)) continue;
    const bool header = is_header(file);
    // Already in a build file?
    bool named = false;
    struct Hit {
      fs::path list;
      std::vector<std::string> lines;
      std::vector<Token> anchors;  // the code files of the same folder this list names
      bool same_kind = false;
      bool newline_at_end = false;
    };
    std::optional<Hit> hit;
    for (const auto& list : lists) {
      auto text = core::read_file(list);
      if (!text) continue;
      bool nl = false;
      auto lines = lines_of(*text, nl);
      Hit h{list, lines, {}, false, nl};
      for (const auto& t : tokens_of(lines)) {
        auto nf = named_file(root, list.parent_path(), t.text);
        if (!nf) continue;
        if (*nf == file) named = true;
        if (nf->parent_path() == file.parent_path()) {
          h.anchors.push_back(t);
          if (is_header(*nf) == header) h.same_kind = true;
        }
      }
      if (named) break;
      if (!hit && h.same_kind) hit = std::move(h);
    }
    if (named || !hit) continue;
    auto& h = *hit;
    const auto entry = fs::relative(root / file, h.list.parent_path()).lexically_normal().generic_string();
    // Before the first file of the folder that sorts after the new one; after the last one otherwise.
    const Token* before = nullptr;
    for (const auto& a : h.anchors) {
      if (lower(a.text) > lower(entry)) {
        before = &a;
        break;
      }
    }
    if (before != nullptr && alone_on_line(h.lines, *before)) {
      const auto& l = h.lines[before->line];
      h.lines.insert(h.lines.begin() + static_cast<std::ptrdiff_t>(before->line), l.substr(0, before->begin) + entry);
    } else if (before != nullptr) {
      h.lines[before->line].insert(before->begin, entry + " ");
    } else {
      const auto& last = h.anchors.back();
      if (alone_on_line(h.lines, last)) {
        const auto& l = h.lines[last.line];
        h.lines.insert(h.lines.begin() + static_cast<std::ptrdiff_t>(last.line) + 1, l.substr(0, last.begin) + entry);
      } else {
        h.lines[last.line].insert(last.end, " " + entry);
      }
    }
    if (!core::write_file(h.list, core::join(h.lines, "\n") + (h.newline_at_end ? "\n" : ""))) continue;
    added.emplace_back(h.list, entry);
  }
  // One line per build file, in the order of the additions.
  std::vector<fs::path> order;
  for (const auto& [list, entry] : added) {
    if (std::find(order.begin(), order.end(), list) == order.end()) order.push_back(list);
  }
  for (const auto& list : order) {
    std::vector<std::string> entries;
    for (const auto& [l, e] : added) {
      if (l == list) entries.push_back(e);
    }
    report.push_back(fs::relative(list, root).generic_string() + ": " + core::join(entries, ", "));
  }
  return report;
}

std::vector<std::string> link_error_names(const std::string& line) {
  std::string symbol;
  for (const char* marker : {"undefined reference to `", "multiple definition of `"}) {
    if (auto at = line.find(marker); at != std::string::npos) {
      auto from = at + std::string(marker).size();
      auto to = line.find('\'', from);
      symbol = line.substr(from, to == std::string::npos ? std::string::npos : to - from);
      break;
    }
  }
  if (symbol.empty()) {
    if (auto at = line.find("undefined symbol: "); at != std::string::npos) symbol = line.substr(at + 18);
  }
  if (symbol.empty()) return {};
  for (const char* prefix : {"vtable for ", "typeinfo for ", "typeinfo name for ", "non-virtual thunk to ", "VTT for "}) {
    if (symbol.starts_with(prefix)) symbol = symbol.substr(std::string(prefix).size());
  }
  if (auto paren = symbol.find('('); paren != std::string::npos) symbol = symbol.substr(0, paren);
  if (auto angle = symbol.find('<'); angle != std::string::npos) symbol = symbol.substr(0, angle);
  std::vector<std::string> names;
  std::size_t i = 0;
  while (i <= symbol.size()) {
    auto j = symbol.find("::", i);
    auto part = std::string(core::trim(symbol.substr(i, j == std::string::npos ? std::string::npos : j - i)));
    if (!part.empty() && part.find(' ') == std::string::npos) names.push_back(part);
    if (j == std::string::npos) break;
    i = j + 2;
  }
  return names;
}

}  // namespace kk::agents
