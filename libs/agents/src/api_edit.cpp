#include "kk/agents/api_edit.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <set>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::agents {

namespace fs = std::filesystem;

namespace {

// Raw lines (core::split trims; indentation matters here).
std::vector<std::string> raw_lines(const std::string& text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    out.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  if (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

std::string join_lines(const std::vector<std::string>& lines) { return core::join(lines, "\n") + "\n"; }

std::vector<fs::path> api_headers(const core::Schema& schema, const std::string& id) {
  std::vector<fs::path> out;
  std::error_code ec;
  auto dir = schema.block_dir(id) / schema.project.dirs.api;
  if (!fs::is_directory(dir, ec)) return out;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    auto ext = e.path().extension().string();
    if (e.is_regular_file(ec) && (ext == ".hpp" || ext == ".h" || ext == ".hh")) out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

const std::regex& constexpr_re() {
  static const std::regex re(R"(^(\s*)((?:(?:inline|static)\s+)*constexpr)\s)");
  return re;
}

int count_constexpr(const std::string& text) {
  int n = 0;
  for (const auto& l : raw_lines(text)) n += std::regex_search(l, constexpr_re()) ? 1 : 0;
  return n;
}

// The header that takes the constants of a block: the one with the most constexpr lines, else the
// one named after the block, else the only one.
std::optional<fs::path> constants_header(const core::Schema& schema, const std::string& id) {
  auto headers = api_headers(schema, id);
  if (headers.empty()) return std::nullopt;
  std::optional<fs::path> best;
  int most = 0;
  for (const auto& h : headers) {
    auto text = core::read_file(h);
    if (!text) continue;
    int n = count_constexpr(*text);
    if (n > most) {
      most = n;
      best = h;
    }
  }
  if (best) return best;
  for (const auto& h : headers) {
    if (h.stem().string() == core::last_segment(id)) return h;
  }
  return headers.front();
}

bool declares(const std::string& text, const std::string& name) {
  std::regex re("(^|[^A-Za-z0-9_])" + name + R"(\s*(=|\{|;))");
  return std::regex_search(text, re);
}

// A tidy place for the declarations of a std:: type.
std::string std_header(const std::string& token) {
  static const std::map<std::string, std::string> m = {
      {"std::vector", "vector"},   {"std::optional", "optional"}, {"std::string", "string"},   {"std::string_view", "string_view"},
      {"std::span", "span"},       {"std::map", "map"},           {"std::array", "array"},     {"std::pair", "utility"},
      {"std::size_t", "cstddef"},  {"std::int8_t", "cstdint"},    {"std::int16_t", "cstdint"}, {"std::int32_t", "cstdint"},
      {"std::int64_t", "cstdint"}, {"std::uint8_t", "cstdint"},   {"std::uint16_t", "cstdint"}, {"std::uint32_t", "cstdint"},
      {"std::uint64_t", "cstdint"}};
  auto it = m.find(token);
  return it == m.end() ? std::string() : it->second;
}

bool builtin(const std::string& t) {
  static const std::set<std::string> b = {"void", "bool", "char", "int", "unsigned", "long", "short", "float", "double",
                                          "signed", "const", "size_t", "auto"};
  return b.contains(t);
}

std::vector<std::string> identifiers(const std::string& s) {
  static const std::regex re(R"([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)");
  std::vector<std::string> out;
  for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) out.push_back(it->str());
  return out;
}

bool declared_type(const std::string& text, const std::string& name) {
  std::regex re("(struct|class|enum\\s+class|enum|using)\\s+" + name + "\\b");
  return std::regex_search(text, re);
}

void add_includes(std::vector<std::string>& lines, const std::set<std::string>& needed) {
  for (const auto& h : needed) {
    std::string inc = "#include <" + h + ">";
    if (std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return core::trim(l) == inc; })) continue;
    std::size_t at = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].starts_with("#include <")) at = i + 1;
    }
    if (at == lines.size()) {
      at = 0;
      for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].starts_with("#pragma once")) {
          at = i + 1;
          break;
        }
      }
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), inc);
  }
}

}  // namespace

bool is_const_line(std::string_view text) { return text.starts_with("const "); }

std::string cxx_type(std::string_view ksl) {
  std::string s(core::trim(ksl));
  static const std::vector<std::pair<std::regex, std::string>> rules = {
      {std::regex(R"((^|[^:A-Za-z0-9_])(vector|optional|span|map|array)<)"), "$1std::$2<"},
      {std::regex(R"((^|[^:A-Za-z0-9_])(string|string_view|size_t)(?![A-Za-z0-9_]))"), "$1std::$2"},
  };
  for (const auto& [re, replacement] : rules) s = std::regex_replace(s, re, replacement);
  return s;
}

kk::Result<std::vector<ConstLine>> change_constants(const std::vector<core::DiffLine>& diff) {
  std::vector<ConstLine> out;
  static const std::regex re(R"(^const\s+([A-Za-z_][A-Za-z0-9_]*)\s+(\S+)\s+(.+?)\s*(?://\s*(.*))?$)");
  std::string current;
  for (const auto& d : diff) {
    if (d.text.starts_with("@block ")) {
      current = std::string(core::trim(std::string_view(d.text).substr(7)));
      continue;
    }
    if (!is_const_line(d.text)) continue;
    if (d.op != '+') {
      return kk::fail("diff", fmt::format("line {}: a change only adds constants ('+ const ...'); a constant is changed or "
                                          "removed by the worker of its block",
                                          d.line));
    }
    if (current.empty()) return kk::fail("diff", fmt::format("line {}: select the block of the constant first ('~ @block <id>')", d.line));
    std::smatch m;
    if (!std::regex_match(d.text, m, re)) {
      return kk::fail("diff", fmt::format("line {}: write a constant as '+ const <name> <C++ type> <value> // note', "
                                          "e.g. '+ const kPaceStep float 0.05f // per 10 points'",
                                          d.line));
    }
    out.push_back({current, m[1], m[2], std::string(core::trim(m[3].str())), m[4].matched ? std::string(core::trim(m[4].str())) : "",
                   d.line});
  }
  return out;
}

std::string check_constants(const core::Schema& schema, const std::vector<ConstLine>& consts) {
  for (const auto& c : consts) {
    const auto* b = schema.find(c.block);
    if (b == nullptr) return fmt::format("const {}: no block {}", c.name, c.block);
    if (!b->has_code() || b->external()) return fmt::format("const {}: {} has no api/ of its own to hold it", c.name, c.block);
    if (!constants_header(schema, c.block)) return fmt::format("const {}: {} has no api/ header yet", c.name, c.block);
  }
  return {};
}

kk::Result<std::vector<std::string>> write_constants(const core::Schema& schema, const std::vector<ConstLine>& consts) {
  std::vector<std::string> out;
  for (const auto& c : consts) {
    auto header = constants_header(schema, c.block);
    if (!header) return kk::fail("const", fmt::format("{} has no api/ header for {}", c.block, c.name));
    auto text = core::read_file(*header);
    if (!text) return std::unexpected(text.error());
    if (declares(*text, c.name)) continue;
    auto lines = raw_lines(*text);
    // After the last constexpr statement, in its indentation and prefix ("inline constexpr").
    std::size_t at = lines.size();
    std::string indent;
    std::string prefix = "inline constexpr";
    bool found = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      std::smatch m;
      if (!std::regex_search(lines[i], m, constexpr_re())) continue;
      indent = m[1];
      prefix = std::regex_replace(m[2].str(), std::regex(R"(\s+)"), " ");
      std::size_t end = i;
      while (end < lines.size() && lines[end].find(';') == std::string::npos) ++end;
      at = std::min(end + 1, lines.size());
      found = true;
    }
    if (!found) {
      for (std::size_t i = lines.size(); i-- > 0;) {
        if (std::regex_search(lines[i], std::regex(R"(^\s*\}\s*//\s*namespace)"))) {
          at = i;
          break;
        }
      }
    }
    std::string line = fmt::format("{}{} {} {} = {};{}", indent, prefix, c.type, c.name, c.value, c.note.empty() ? "" : "  // " + c.note);
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
    if (auto w = core::write_file(*header, join_lines(lines)); !w) return std::unexpected(w.error());
    out.push_back(fmt::format("{}: {} {} = {} written to api/{}", c.block, c.type, c.name, c.value, header->filename().string()));
  }
  return out;
}

std::vector<std::string> write_declarations(const core::Schema& schema, const std::vector<core::DiffLine>& diff) {
  std::vector<std::string> out;
  std::string current;
  bool fresh = false;
  for (const auto& d : diff) {
    if (d.text.starts_with("@block ")) {
      current = std::string(core::trim(std::string_view(d.text).substr(7)));
      fresh = d.op == '+';  // a new block gets its api/ from the architect
      continue;
    }
    if (d.op != '+' || !d.text.starts_with("fn ") || current.empty() || fresh) continue;
    const auto* b = schema.find(current);
    if (b == nullptr || b->kind != core::BlockKind::Leaf || b->external()) continue;
    auto headers = api_headers(schema, current);
    std::optional<fs::path> header;
    for (const auto& h : headers) {
      if (h.stem().string() == core::last_segment(current)) header = h;
    }
    if (!header && headers.size() == 1) header = headers.front();
    if (!header) continue;
    auto parsed = core::parse_block_text("@block " + current + "\n" + d.text + "\n", "diff");
    if (!parsed) continue;
    const core::Port* fn = nullptr;
    for (const auto& p : parsed->ports) {
      if (p.dir == core::PortDir::Fn) fn = &p;
    }
    if (fn == nullptr) continue;
    auto text = core::read_file(*header);
    if (!text) continue;
    if (std::regex_search(*text, std::regex("(^|[^A-Za-z0-9_])" + fn->name + R"(\s*\()"))) continue;  // declared already
    std::string ret = fn->ret.empty() ? "void" : cxx_type(fn->ret);
    std::vector<std::string> params;
    for (const auto& p : fn->params) params.push_back(cxx_type(p));
    // Every type must be one this header can take as it is.
    std::set<std::string> includes;
    bool known = true;
    auto check = [&](const std::string& t, bool param) {
      auto ids = identifiers(t);
      if (param && ids.size() >= 2 && !builtin(ids.back()) && !ids.back().starts_with("std::")) ids.pop_back();  // its name
      for (const auto& id : ids) {
        if (builtin(id)) continue;
        if (id.starts_with("std::")) {
          auto h = std_header(id);
          if (h.empty()) known = false;
          else includes.insert(h);
          continue;
        }
        if (!declared_type(*text, id)) known = false;
      }
    };
    check(ret, false);
    for (const auto& p : params) check(p, true);
    if (!known || ret.find('[') != std::string::npos ||
        std::any_of(params.begin(), params.end(), [](const std::string& p) { return p.find('[') != std::string::npos; })) {
      continue;
    }
    auto lines = raw_lines(*text);
    std::string decl;
    std::size_t at = lines.size();
    static const std::regex pure(R"(=\s*0\s*;)");
    static const std::regex virt(R"(^\s*(\[\[[^\]]*\]\]\s*)?virtual\s)");
    int destructors = 0;
    for (const auto& l : lines) destructors += l.find("virtual ~") != std::string::npos ? 1 : 0;
    std::optional<std::size_t> last_pure;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (std::regex_search(lines[i], pure)) last_pure = i;
    }
    std::string args = core::join(params, ", ");
    if (last_pure) {
      if (destructors > 1) continue;  // several interfaces: which one is not the program's guess
      std::size_t start = *last_pure;
      while (start > 0 && !std::regex_search(lines[start], virt)) --start;
      std::string indent = lines[start].substr(0, lines[start].find_first_not_of(" \t"));
      decl = fmt::format("{}virtual {} {}({}) = 0;", indent, ret, fn->name, args);
      at = *last_pure + 1;
    } else {
      static const std::regex free_fn(
          R"(^(\s*)(?!return\b|using\b|typedef\b|friend\b|static_assert\b|delete\b)[A-Za-z_][A-Za-z0-9_:<>,\*& ]*[\s\*&]([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*(const\s*)?;\s*(//.*)?$)");
      std::optional<std::size_t> last;
      std::size_t min_indent = std::string::npos;
      for (std::size_t i = 0; i < lines.size(); ++i) {
        std::smatch m;
        if (!std::regex_match(lines[i], m, free_fn)) continue;
        auto ind = m[1].length();
        if (static_cast<std::size_t>(ind) < min_indent) {
          min_indent = static_cast<std::size_t>(ind);
          last = i;
        } else if (static_cast<std::size_t>(ind) == min_indent) {
          last = i;
        }
      }
      if (!last) continue;
      decl = fmt::format("{}{} {}({});", lines[*last].substr(0, min_indent), ret, fn->name, args);
      at = *last + 1;
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), decl);
    add_includes(lines, includes);
    if (!core::write_file(*header, join_lines(lines))) continue;
    out.push_back(fmt::format("{}: declared `{}` in api/{}", current, std::string(core::trim(decl)), header->filename().string()));
  }
  return out;
}

}  // namespace kk::agents
