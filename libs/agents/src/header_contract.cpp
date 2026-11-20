// The visible part of a C++ header as a contract (ADR 0027): an imported block's headers are its
// api, but only what other code can see is a promise. Textual and deterministic: comments and
// literals are skipped, the text is cut into declarations by ';', '{' and '}' with the scope of each
// (namespace, class and its current access); bodies of functions and initializers are opaque.
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <vector>

#include "kk/agents/sandbox.hpp"
#include "kk/core/text.hpp"

namespace kk::agents {

namespace {

enum class ScopeKind { Namespace, Class };
enum class Access { Public, Protected, Private };

struct Scope {
  ScopeKind kind = ScopeKind::Namespace;
  std::string name;
  Access access = Access::Public;
};

struct Decl {
  std::string scope;  // "Engine::" for a member of class Engine
  std::string text;   // normalized declaration
  bool visible = true;
};

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// Comments removed; the contents of string and character literals replaced by '_' so that their
// braces and semicolons do not count. Line breaks stay (preprocessor lines are line-based).
std::string blank_comments_and_literals(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s.compare(i, 2, "//") == 0) {
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    if (s.compare(i, 2, "/*") == 0) {
      auto e = s.find("*/", i + 2);
      std::size_t end = e == std::string_view::npos ? s.size() : e + 2;
      for (std::size_t k = i; k < end; ++k) {
        if (s[k] == '\n') out += '\n';
      }
      out += ' ';
      i = end;
      continue;
    }
    // A raw string literal: R"delim( ... )delim".
    if (s[i] == 'R' && i + 1 < s.size() && s[i + 1] == '"' && (i == 0 || !ident_char(s[i - 1]) || s[i - 1] == 'u' || s[i - 1] == '8' ||
                                                              s[i - 1] == 'L' || s[i - 1] == 'U')) {
      auto open = s.find('(', i + 2);
      if (open != std::string_view::npos) {
        std::string close = ")" + std::string(s.substr(i + 2, open - i - 2)) + "\"";
        auto e = s.find(close, open + 1);
        std::size_t end = e == std::string_view::npos ? s.size() : e + close.size();
        out += "R\"_\"";
        i = end;
        continue;
      }
    }
    // A quote after a digit is a digit separator (1'000), not a character literal.
    if (s[i] == '"' || (s[i] == '\'' && !(i > 0 && std::isxdigit(static_cast<unsigned char>(s[i - 1])) != 0 && i + 1 < s.size() &&
                                          std::isxdigit(static_cast<unsigned char>(s[i + 1])) != 0))) {
      char q = s[i];
      std::size_t k = i + 1;
      while (k < s.size() && s[k] != q && s[k] != '\n') {
        if (s[k] == '\\') ++k;
        ++k;
      }
      out += q;
      out += '_';
      out += q;
      i = std::min(s.size(), k + 1);
      continue;
    }
    out += s[i++];
  }
  return out;
}

std::string collapse_spaces(std::string_view s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

// "template <...>" prefixes removed (balanced angle brackets).
std::string strip_template_prefix(std::string s) {
  while (true) {
    auto t = std::string(core::trim(s));
    if (!t.starts_with("template")) return t;
    auto lt = t.find('<');
    if (lt == std::string::npos) return t;
    int depth = 0;
    std::size_t k = lt;
    for (; k < t.size(); ++k) {
      if (t[k] == '<') ++depth;
      if (t[k] == '>' && --depth == 0) break;
    }
    if (k >= t.size()) return t;
    s = t.substr(k + 1);
  }
}

std::string strip_attributes(const std::string& s) {
  static const std::regex attr(R"(\[\[[^\]]*\]\]\s*)");
  return std::regex_replace(s, attr, "");
}

// The first '=' of an initializer, outside brackets; npos when none (comparisons, compound
// assignments and operator= are not initializers).
std::size_t initializer_eq(const std::string& t) {
  int depth = 0;
  for (std::size_t i = 0; i < t.size(); ++i) {
    char c = t[i];
    if (c == '(' || c == '[' || c == '{') ++depth;
    if (c == ')' || c == ']' || c == '}') --depth;
    // Template brackets; "<<", "<=" and "->" are operators.
    if (c == '<' && i + 1 < t.size() && t[i + 1] != '<' && t[i + 1] != '=' && (i == 0 || t[i - 1] != '<')) ++depth;
    if (c == '>' && depth > 0 && (i == 0 || t[i - 1] != '-')) --depth;
    if (c != '=' || depth != 0) continue;
    if (i + 1 < t.size() && t[i + 1] == '=') {
      ++i;
      continue;
    }
    if (i > 0 && std::string_view("=!<>+-*/%&|^").find(t[i - 1]) != std::string_view::npos) continue;
    auto before = std::string(core::trim(std::string_view(t).substr(0, i)));
    if (before.ends_with("operator")) continue;
    return i;
  }
  return std::string::npos;
}

// A declaration as it is compared: attributes dropped, a function's body, member initializer list
// and inline-ness left out, a variable's initial value replaced by a placeholder.
std::string normalize_decl(std::string t, bool had_body) {
  t = collapse_spaces(strip_attributes(t));
  if (had_body && t.ends_with("{…}") && t.find('(') != std::string::npos) {
    t = std::string(core::trim(std::string_view(t).substr(0, t.size() - std::string_view("{…}").size())));
    // "Foo() : a_(1), b_{…}" → "Foo()": the initializer list is the implementation.
    static const std::regex init_list(R"(\)\s*:(?!:).*$)");
    t = std::regex_replace(t, init_list, ")");
  }
  if (auto eq = initializer_eq(t); eq != std::string::npos) {
    auto before = std::string(core::trim(std::string_view(t).substr(0, eq)));
    if (before.find('(') == std::string::npos) t = before + " = _";
  }
  if (t.starts_with("inline ")) t = t.substr(7);
  return t;
}

// The definition of something declared elsewhere: "int Foo::Get() const {…}" (a qualified name
// before the parameter list). Its body is implementation, like a source file's.
bool qualified_definition(const std::string& t, bool had_body) {
  if (!had_body) return false;
  auto paren = t.find('(');
  if (paren == std::string::npos) return false;
  auto head = std::string(core::trim(std::string_view(t).substr(0, paren)));
  auto sp = head.find_last_of(" *&");
  auto name = sp == std::string::npos ? head : head.substr(sp + 1);
  return name.find("::") != std::string::npos;
}

bool is_constant_decl(const std::string& t) {
  static const std::regex bad_start(R"(^(using|typedef|friend|class|struct|union|enum|namespace|template|#)\b)");
  if (std::regex_search(t, bad_start)) return false;
  auto eq = initializer_eq(t);
  auto head = eq == std::string::npos ? t : t.substr(0, eq);
  if (head.find('(') != std::string::npos) return false;
  static const std::regex constant_word(R"(\b(const|constexpr|constinit)\b)");
  return std::regex_search(head, constant_word);
}

class Scanner {
 public:
  explicit Scanner(std::string_view text) : s_(blank_comments_and_literals(text)) { scopes_.push_back({}); }

  std::vector<Decl> run() {
    bool line_start = true;
    while (i_ < s_.size()) {
      char c = s_[i_];
      if (line_start && c == '#') {
        directive();
        continue;
      }
      if (c == '\n') {
        line_start = true;
        cur_ += ' ';
        ++i_;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(c)) == 0) line_start = false;
      if (c == ';') {
        emit();
        ++i_;
        continue;
      }
      if (c == '{') {
        open_brace();
        continue;
      }
      if (c == '}') {
        emit();
        if (scopes_.size() > 1) scopes_.pop_back();
        ++i_;
        continue;
      }
      cur_ += c;
      ++i_;
    }
    emit();
    return std::move(out_);
  }

 private:
  std::string scope_name() const {
    std::string out;
    for (const auto& sc : scopes_) {
      if (!sc.name.empty()) out += sc.name + "::";
    }
    return out;
  }

  bool visible() const {
    return std::all_of(scopes_.begin(), scopes_.end(),
                       [](const Scope& sc) { return sc.kind != ScopeKind::Class || sc.access != Access::Private; });
  }

  // "public:" / "private:" / "protected:" at the start of the current text set the access of the
  // class scope and are removed.
  void take_labels() {
    static const std::regex label(R"(^\s*(public|private|protected)\s*:(?!:))");
    std::smatch m;
    while (std::regex_search(cur_, m, label)) {
      if (scopes_.back().kind == ScopeKind::Class) {
        auto w = m[1].str();
        scopes_.back().access = w == "public" ? Access::Public : w == "protected" ? Access::Protected : Access::Private;
      }
      cur_ = m.suffix();
    }
  }

  void push_decl(std::string text, bool had_body = false) {
    auto t = std::string(core::trim(text));
    if (t.empty()) return;
    bool vis = visible() && !qualified_definition(t, had_body);
    out_.push_back({scope_name(), normalize_decl(t, had_body), vis});
  }

  void emit() {
    take_labels();
    push_decl(cur_, body_);
    cur_.clear();
    body_ = false;
  }

  void directive() {
    std::string line;
    while (i_ < s_.size()) {
      auto e = s_.find('\n', i_);
      if (e == std::string::npos) e = s_.size();
      std::string part = s_.substr(i_, e - i_);
      i_ = std::min(s_.size(), e + 1);
      bool cont = !core::trim(part).empty() && core::trim(part).back() == '\\';
      if (cont) part = std::string(core::trim(part)).substr(0, std::string(core::trim(part)).size() - 1);
      line += part + " ";
      if (!cont) break;
    }
    take_labels();
    out_.push_back({scope_name(), collapse_spaces(line), visible()});
  }

  // Skips a brace block from the '{' at i_ to its '}' and returns what was inside.
  std::string skip_block() {
    int depth = 0;
    std::size_t start = i_;
    for (; i_ < s_.size(); ++i_) {
      if (s_[i_] == '{') ++depth;
      if (s_[i_] == '}' && --depth == 0) break;
    }
    std::string inside = i_ > start ? s_.substr(start + 1, i_ - start - 1) : std::string();
    if (i_ < s_.size()) ++i_;
    return inside;
  }

  void open_brace() {
    take_labels();
    static const std::regex alignas_spec(R"(\balignas\s*\([^)]*\))");
    auto head = collapse_spaces(std::regex_replace(strip_attributes(strip_template_prefix(cur_)), alignas_spec, ""));
    static const std::regex ns(R"(^(?:inline\s+)?namespace\b\s*([\w:]*))");
    static const std::regex extern_c(R"(^extern\s+"_")");
    static const std::regex cls(R"(^(?:typedef\s+)?(class|struct|union)\b(?:\s+([A-Za-z_]\w*))?)");
    static const std::regex en(R"(^enum\b)");
    std::smatch m;
    if (std::regex_search(head, m, ns) && head.find('=') == std::string::npos) {
      scopes_.push_back({ScopeKind::Namespace, m[1].str(), Access::Public});
      cur_.clear();
      ++i_;
      return;
    }
    if (std::regex_search(head, extern_c)) {
      scopes_.push_back({ScopeKind::Namespace, "", Access::Public});
      cur_.clear();
      ++i_;
      return;
    }
    if (std::regex_search(head, m, cls) && head.find('(') == std::string::npos && head.find('=') == std::string::npos) {
      push_decl(cur_);
      cur_.clear();
      body_ = false;
      scopes_.push_back({ScopeKind::Class, m[2].str(), m[1].str() == "class" ? Access::Private : Access::Public});
      ++i_;
      return;
    }
    if (std::regex_search(head, en)) {
      // Enumerators are part of the declaration (a new value is a contract change).
      cur_ += "{" + collapse_spaces(skip_block()) + "}";
      return;
    }
    // A function body, a lambda or an initializer: opaque.
    (void)skip_block();
    cur_ += " {…}";
    body_ = true;
    // After a function body the declaration ends; after an initializer it goes on to its ';'.
    std::size_t k = i_;
    while (k < s_.size() && std::isspace(static_cast<unsigned char>(s_[k])) != 0) ++k;
    if (k < s_.size() && std::string_view(";,){=.").find(s_[k]) != std::string_view::npos) return;
    emit();
  }

  std::string s_;
  std::size_t i_ = 0;
  std::string cur_;
  bool body_ = false;
  std::vector<Scope> scopes_;
  std::vector<Decl> out_;
};

std::string where(const std::string& scope) {
  if (scope.empty()) return "";
  return " (in " + scope.substr(0, scope.size() - 2) + ")";
}

}  // namespace

bool header_contract_kept(std::string_view before, std::string_view after, std::string* why) {
  auto old_decls = Scanner(before).run();
  auto new_decls = Scanner(after).run();
  std::multiset<std::pair<std::string, std::string>> fresh;
  for (const auto& d : new_decls) {
    if (d.visible) fresh.insert({d.scope, d.text});
  }
  for (const auto& d : old_decls) {
    if (!d.visible) continue;
    auto it = fresh.find({d.scope, d.text});
    if (it == fresh.end()) {
      if (why != nullptr) *why = fmt::format("the public declaration `{}`{} was removed or changed", d.text, where(d.scope));
      return false;
    }
    fresh.erase(it);
  }
  for (const auto& [scope, text] : fresh) {
    if (text.starts_with("#include")) continue;
    if (is_constant_decl(text)) continue;
    if (why != nullptr) *why = fmt::format("`{}`{} is a new public declaration", text, where(scope));
    return false;
  }
  return true;
}

}  // namespace kk::agents
