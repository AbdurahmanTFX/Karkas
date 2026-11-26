#include "kk/hsgm/input.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>

#include "kk/core/paths.hpp"

namespace kk::hsgm {

namespace {

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool space_char(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

}  // namespace

kk::Result<Input> gather(index::Index& idx) {
  const auto* schema = idx.schema();
  if (schema == nullptr) return kk::fail("hsgm", "index not updated");
  Input in;
  in.snap = idx.snapshot();
  in.files = index::collect_code_files(*schema);
  in.runs = idx.runs();
  for (const auto& f : in.files) {
    if (f.role != "test" || f.path.starts_with("tests/integration/") || f.path.find("/contract/") == std::string::npos) {
      continue;
    }
    auto text = core::read_file(idx.root() / f.path);
    if (!text) continue;
    auto names = test_case_names(*text);
    auto& out = in.test_cases[f.block];
    out.insert(out.end(), names.begin(), names.end());
  }
  return in;
}

std::vector<std::string> test_case_names(std::string_view s) {
  constexpr std::string_view kKey = "TEST_CASE";
  std::vector<std::string> out;
  std::size_t pos = 0;
  while ((pos = s.find(kKey, pos)) != std::string_view::npos) {
    const std::size_t start = pos;
    std::size_t i = pos + kKey.size();
    pos = i;
    if (start > 0 && ident_char(s[start - 1])) continue;  // MY_TEST_CASE
    while (i < s.size() && space_char(s[i])) ++i;
    if (i >= s.size() || s[i] != '(') continue;  // TEST_CASE_FIXTURE, a mention in a comment
    ++i;
    while (i < s.size() && space_char(s[i])) ++i;
    if (i >= s.size() || s[i] != '"') continue;
    ++i;
    std::string name;
    bool closed = false;
    while (i < s.size() && s[i] != '\n') {
      if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\')) {
        name += s[i + 1];
        i += 2;
        continue;
      }
      if (s[i] == '"') {
        closed = true;
        break;
      }
      name += s[i++];
    }
    if (closed) out.push_back(std::move(name));
    pos = i;
  }
  return out;
}

bool is_code_block(const index::BlockRow& b) { return b.kind == "leaf" || b.kind == "entry" || b.kind == "shared"; }

bool is_code_project(const Input& in) {
  return std::any_of(in.snap.blocks.begin(), in.snap.blocks.end(), [](const auto& b) { return is_code_block(b); });
}

std::string link_name(const index::LinkRow& l) {
  auto end = [](const std::string& block, const std::string& port) { return port.empty() ? block : block + ":" + port; };
  return fmt::format("{} {} -> {}", l.kind, end(l.src_block, l.src_port), end(l.dst_block, l.dst_port));
}

std::string ancestor_on(std::string_view id, int level) {
  std::size_t pos = 0;
  for (int k = 0; k <= level; ++k) {
    pos = id.find('.', pos);
    if (pos == std::string_view::npos) return std::string(id);
    if (k == level) return std::string(id.substr(0, pos));
    ++pos;
  }
  return std::string(id);
}

}  // namespace kk::hsgm
