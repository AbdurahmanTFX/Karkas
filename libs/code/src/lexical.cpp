#include "kk/code/lexical.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <regex>
#include <set>

#include "kk/core/text.hpp"

namespace kk::code {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t npos = std::string::npos;

bool at(std::string_view s, std::size_t i, std::string_view tok) { return s.substr(i, tok.size()) == tok; }
bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// --- masking -----------------------------------------------------------------------------------

// Lua long brackets: "[[", "[=[", "[==[" ... at i; returns the closing bracket or "".
std::string lua_long_close(std::string_view s, std::size_t i) {
  if (i >= s.size() || s[i] != '[') return {};
  std::size_t j = i + 1;
  while (j < s.size() && s[j] == '=') ++j;
  if (j >= s.size() || s[j] != '[') return {};
  return "]" + std::string(j - i - 1, '=') + "]";
}

}  // namespace

std::string mask_code(const core::Language& lang, std::string_view text, bool strings) {
  std::string out(text);
  const std::size_t n = text.size();
  auto blank = [&](std::size_t b, std::size_t e) {
    for (std::size_t k = b; k < e && k < n; ++k) {
      if (out[k] != '\n') out[k] = ' ';
    }
  };
  const bool shell = lang.hash_comment_after_space && lang.id == "bash";
  const bool lua = lang.id == "lua";
  std::vector<std::pair<std::string, bool>> heredocs;  // shell: delimiters whose bodies start on the next line
  std::size_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n') {
      ++i;
      for (const auto& [delim, tabs] : heredocs) {
        while (i < n) {
          auto e = text.find('\n', i);
          if (e == npos) e = n;
          auto line = text.substr(i, e - i);
          if (tabs) {
            while (!line.empty() && line.front() == '\t') line.remove_prefix(1);
          }
          if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
          const bool end = line == delim;
          if (!end && strings) blank(i, e);
          i = e < n ? e + 1 : n;
          if (end) break;
        }
      }
      heredocs.clear();
      continue;
    }
    // Block comments. A "=begin" / "=pod" comment starts a line and ends at a line "=end" / "=cut".
    bool done = false;
    if (lua && at(text, i, "--")) {
      auto close = lua_long_close(text, i + 2);
      if (!close.empty()) {
        auto e = text.find(close, i + 2);
        e = e == npos ? n : e + close.size();
        blank(i, e);
        i = e;
        continue;
      }
    }
    for (const auto& [open, close] : lang.block_comments) {
      if (lua || !at(text, i, open)) continue;
      const bool line_start = open.front() == '=';
      if (line_start && i > 0 && text[i - 1] != '\n') continue;
      std::size_t e = line_start ? text.find("\n" + close, i + open.size()) : text.find(close, i + open.size());
      if (e == npos) {
        e = n;
      } else {
        e += (line_start ? 1 : 0) + close.size();
        if (line_start) {
          auto eol = text.find('\n', e);
          e = eol == npos ? n : eol;
        }
      }
      blank(i, e);
      i = e;
      done = true;
      break;
    }
    if (done) continue;
    for (const auto& lc : lang.line_comments) {
      if (!at(text, i, lc)) continue;
      if (lang.hash_comment_after_space && lc == "#" && i > 0) {
        const char p = text[i - 1];
        if (!(p == ' ' || p == '\t' || p == '\n' || p == ';' || p == '|' || p == '&' || p == '(')) continue;
      }
      auto e = text.find('\n', i);
      if (e == npos) e = n;
      blank(i, e);
      i = e;
      done = true;
      break;
    }
    if (done) continue;
    if (lua && c == '[') {
      auto close = lua_long_close(text, i);
      if (!close.empty()) {
        auto e = text.find(close, i);
        e = e == npos ? n : e + close.size();
        if (strings) blank(i + 1, e - 1);
        i = e;
        continue;
      }
    }
    if (lang.quotes.find(c) != npos) {
      // A quote inside a word is no string in the shell (don't), in Lisp ('sym) or after an identifier.
      if (c == '\'' && i > 0 && word_char(text[i - 1]) && lang.id != "python") {
        ++i;
        continue;
      }
      if (lang.triple_quotes && (c == '"' || c == '\'') && at(text, i, std::string(3, c))) {
        auto e = text.find(std::string(3, c), i + 3);
        e = e == npos ? n : e + 3;
        if (strings) blank(i + 3, e >= i + 6 ? e - 3 : i + 3);
        i = e;
        continue;
      }
      const bool escapes = !(shell && c == '\'');
      const bool multiline = shell || c == '`' || lang.id == "ruby" || lang.id == "perl" || lang.id == "php" ||
                             lang.id == "r" || lang.id == "julia" || lang.id == "elixir" || lang.id == "nim" ||
                             lang.id == "clojure" || lang.id == "haskell" || lang.id == "ocaml";
      std::size_t j = i + 1;
      while (j < n) {
        const char d = text[j];
        if (escapes && d == '\\') {
          j += 2;
          continue;
        }
        if (d == c || (d == '\n' && !multiline)) break;
        ++j;
      }
      const std::size_t e = std::min(j, n);
      if (strings) blank(i + 1, e);
      i = (j < n && text[j] == c) ? j + 1 : e;
      continue;
    }
    if (shell && at(text, i, "<<") && !at(text, i, "<<<")) {
      std::size_t j = i + 2;
      bool tabs = false;
      if (j < n && text[j] == '-') {
        tabs = true;
        ++j;
      }
      while (j < n && (text[j] == ' ' || text[j] == '\t')) ++j;
      const char q = j < n && (text[j] == '\'' || text[j] == '"') ? text[j] : 0;
      if (q != 0) ++j;
      const std::size_t s = j;
      while (j < n && word_char(text[j])) ++j;
      if (j > s) {
        heredocs.emplace_back(std::string(text.substr(s, j - s)), tabs);
        if (q != 0 && j < n && text[j] == q) ++j;
        i = j;
        continue;
      }
    }
    ++i;
  }
  return out;
}

namespace {

// --- rules per language -----------------------------------------------------------------------------

enum class Scoping { Braces, Indent };
enum class Calls { Parens, Shell, Lisp, None };

struct Decl {
  std::regex re;
  std::string kind;  // function | class | struct | enum | alias | namespace | impl | kw (by the keyword group)
  std::size_t name = 1;   // group of the name
  std::size_t mods = 0;   // group of the modifiers that decide visibility
  std::size_t owner = 0;  // group of the owner type (a Go receiver, a Rust impl target)
  std::size_t kw = 0;     // group of the keyword when kind == "kw"
  bool member = false;  // only directly inside a class-like scope
  bool top = false;     // only outside every class and function
  bool push = true;     // opens a scope
};

struct Rules {
  Scoping scoping = Scoping::Braces;
  Calls calls = Calls::Parens;
  std::vector<Decl> decls;
  std::vector<std::pair<std::regex, std::size_t>> imports;  // on the text without comments; group of the target
  std::vector<std::regex> entries;                  // a program's main
  std::set<std::string> receivers;                  // first parameters that are the object itself
  bool generics = false;                            // <...> groups commas in parameter lists
};

std::regex rx(const std::string& s) { return std::regex(s, std::regex::ECMAScript | std::regex::optimize); }

Decl decl(const std::string& re, std::string kind, std::size_t name = 1, std::size_t mods = 0) {
  Decl d;
  d.re = rx(re);
  d.kind = std::move(kind);
  d.name = name;
  d.mods = mods;
  return d;
}

Decl member(Decl d) {
  d.member = true;
  return d;
}

Decl kw(const std::string& re, std::size_t kw_group, std::size_t name, std::size_t mods = 0) {
  auto d = decl(re, "kw", name, mods);
  d.kw = kw_group;
  return d;
}

void add_imports(Rules& r, std::initializer_list<std::pair<const char*, std::size_t>> list) {
  for (const auto& [re, g] : list) r.imports.emplace_back(rx(re), g);
}

#define KK_ID "[A-Za-z_$][\\w$]*"  // an identifier of the C-like languages

Rules python() {
  Rules r;
  r.scoping = Scoping::Indent;
  r.decls.push_back(decl("^\\s*class\\s+([A-Za-z_]\\w*)", "class"));
  r.decls.push_back(decl("^\\s*(?:async\\s+)?def\\s+([A-Za-z_]\\w*)\\s*\\(", "function"));
  add_imports(r, {{"^\\s*import\\s+([\\w.]+)", 1}, {"^\\s*from\\s+(\\.*[\\w.]*)\\s+import\\b", 1}});
  r.entries.push_back(rx("__name__\\s*==\\s*['\"]__main__['\"]"));
  r.receivers = {"self", "cls"};
  return r;
}

Rules javascript() {
  Rules r;
  r.generics = true;
  r.decls.push_back(decl("^\\s*(export\\s+)?(?:default\\s+)?(?:declare\\s+)?(?:abstract\\s+)?class\\s+(" KK_ID ")", "class", 2, 1));
  r.decls.push_back(kw("^\\s*(export\\s+)?(?:declare\\s+)?(?:const\\s+)?(interface|enum)\\s+(" KK_ID ")", 2, 3, 1));
  auto alias = decl("^\\s*(export\\s+)?(?:declare\\s+)?type\\s+(" KK_ID ")\\s*(?:<[^=]*>)?\\s*=", "alias", 2, 1);
  alias.push = false;
  r.decls.push_back(alias);
  r.decls.push_back(decl("^\\s*(export\\s+)?(?:default\\s+)?(?:declare\\s+)?(?:async\\s+)?function\\s*\\*?\\s*(" KK_ID ")\\s*[<(]",
                         "function", 2, 1));
  r.decls.push_back(decl("^\\s*(export\\s+)?(?:const|let|var)\\s+(" KK_ID ")\\s*(?::[^=]+)?=\\s*(?:async\\s+)?(?:function\\b|\\([^)]*\\)\\s*(?::[^=]+)?=>|" KK_ID "\\s*=>)",
                         "function", 2, 1));
  r.decls.push_back(member(decl("^\\s*((?:(?:public|private|protected|static|readonly|abstract|override|async|get|set|declare)\\s+)*)(#?" KK_ID ")\\s*\\?" "?\\s*(?:<[^>()]*>)?\\s*\\(",
                                "function", 2, 1)));
  add_imports(r, {{"\\bfrom\\s+['\"]([^'\"]+)['\"]", 1},
                  {"^\\s*import\\s+['\"]([^'\"]+)['\"]", 1},
                  {"\\brequire\\s*\\(\\s*['\"]([^'\"]+)['\"]\\s*\\)", 1},
                  {"\\bimport\\s*\\(\\s*['\"]([^'\"]+)['\"]\\s*\\)", 1}});
  return r;
}

Rules go() {
  Rules r;
  r.generics = false;
  auto method = decl("^func\\s*\\(\\s*(?:\\w+\\s+)?\\*?\\s*([A-Za-z_]\\w*)(?:\\[[^\\]]*\\])?\\s*\\)\\s*([A-Za-z_]\\w*)\\s*[\\[(]", "function", 2);
  method.owner = 1;
  r.decls.push_back(method);
  r.decls.push_back(decl("^func\\s+([A-Za-z_]\\w*)\\s*[\\[(]", "function"));
  r.decls.push_back(kw("^type\\s+([A-Za-z_]\\w*)(?:\\[[^\\]]*\\])?\\s+(struct|interface)\\b", 2, 1));
  auto alias = decl("^type\\s+([A-Za-z_]\\w*)(?:\\[[^\\]]*\\])?\\s+[^\\s{(]", "alias");
  alias.push = false;
  r.decls.push_back(alias);
  r.decls.push_back(member(decl("^\\s+([A-Za-z_]\\w*)\\s*\\(", "function")));
  add_imports(r, {{"^import\\s+(?:[\\w.]+\\s+)?\"([^\"]+)\"", 1}, {"^\\s+(?:[\\w.]+\\s+)?\"([^\"]+)\"\\s*$", 1}});
  return r;
}

Rules rust() {
  Rules r;
  r.generics = true;
  const std::string pub = "(pub(?:\\s*\\([^)]*\\))?\\s+)?";
  r.decls.push_back(decl("^\\s*" + pub + "(?:default\\s+)?(?:const\\s+)?(?:async\\s+)?(?:unsafe\\s+)?(?:extern\\s+(?:\"[^\"]*\"\\s+)?)?fn\\s+([A-Za-z_]\\w*)",
                         "function", 2, 1));
  r.decls.push_back(kw("^\\s*" + pub + "(struct|enum|trait|union)\\s+([A-Za-z_]\\w*)", 2, 3, 1));
  auto alias = decl("^\\s*" + pub + "type\\s+([A-Za-z_]\\w*)", "alias", 2, 1);
  alias.push = false;
  r.decls.push_back(alias);
  auto impl = decl("^\\s*(?:unsafe\\s+)?impl\\b(?:\\s*<[^{]*?>)?\\s+(?:([\\w:]+)(?:<[^{]*?>)?\\s+for\\s+)?&?(?:mut\\s+)?(?:[\\w]+::)*([A-Za-z_]\\w*)",
                   "impl", 2, 1);
  r.decls.push_back(impl);
  r.decls.push_back(decl("^\\s*" + pub + "mod\\s+([A-Za-z_]\\w*)\\s*\\{", "namespace", 2, 1));
  add_imports(r, {{"^\\s*(?:pub(?:\\([^)]*\\))?\\s+)?use\\s+([\\w:]+)", 1}, {"^\\s*(?:pub(?:\\([^)]*\\))?\\s+)?(mod\\s+[A-Za-z_]\\w*)\\s*;", 1}});
  r.receivers = {"self", "&self", "&mut self", "mut self"};
  return r;
}

Rules jvm(const std::string& lang) {
  Rules r;
  r.generics = true;
  if (lang == "java") {
    r.decls.push_back(kw("^\\s*(?:@\\w+(?:\\([^)]*\\))?\\s+)*((?:(?:public|protected|private|abstract|final|static|sealed|non-sealed|strictfp)\\s+)*)(class|interface|enum|record)\\s+([A-Za-z_]\\w*)",
                         2, 3, 1));
    r.decls.push_back(member(decl("^\\s*(?:@\\w+(?:\\([^)]*\\))?\\s+)*((?:(?:public|protected|private|static|final|abstract|synchronized|native|default|strictfp)\\s+)*)(?:<[^>]*>\\s+)?([\\w$.]+(?:<[^()]*>)?(?:\\[\\])*)\\s+([a-zA-Z_$][\\w$]*)\\s*\\(",
                                  "function", 3, 1)));
    add_imports(r, {{"^\\s*import\\s+(?:static\\s+)?([\\w.]+(?:\\.\\*)?)\\s*;", 1}});
    r.entries.push_back(rx("\\bstatic\\s+(?:final\\s+)?(?:public\\s+)?void\\s+main\\s*\\(|\\bpublic\\s+static\\s+void\\s+main\\s*\\("));
  } else if (lang == "kotlin") {
    r.decls.push_back(kw("^\\s*((?:(?:public|private|protected|internal|open|abstract|sealed|data|enum|inner|annotation|value|final|expect|actual)\\s+)*)(class|interface|object)\\s+([A-Za-z_]\\w*)",
                         2, 3, 1));
    r.decls.push_back(decl("^\\s*((?:(?:public|private|protected|internal|open|override|abstract|final|suspend|inline|operator|infix|tailrec|external|actual|expect)\\s+)*)fun\\s+(?:<[^>]*>\\s*)?(?:[\\w.<>?]+\\.)?([A-Za-z_]\\w*)\\s*\\(",
                           "function", 2, 1));
    add_imports(r, {{"^\\s*import\\s+([\\w.]+(?:\\.\\*)?)", 1}});
  } else if (lang == "scala") {
    r.decls.push_back(kw("^\\s*((?:(?:private|protected|final|sealed|abstract|implicit|case|lazy)(?:\\[[^\\]]*\\])?\\s+)*)(class|object|trait|enum)\\s+([A-Za-z_]\\w*)",
                         2, 3, 1));
    r.decls.push_back(decl("^\\s*((?:(?:private|protected|override|final|implicit|inline)(?:\\[[^\\]]*\\])?\\s+)*)def\\s+([A-Za-z_]\\w*)", "function", 2, 1));
    add_imports(r, {{"^\\s*import\\s+([\\w.]+)", 1}});
    r.entries.push_back(rx("\\bdef\\s+main\\s*\\(\\s*args"));
  }
  return r;
}

Rules csharp() {
  Rules r;
  r.generics = true;
  r.decls.push_back(decl("^\\s*namespace\\s+([\\w.]+)", "namespace"));
  r.decls.push_back(kw("^\\s*(?:\\[[^\\]]*\\]\\s*)*((?:(?:public|private|protected|internal|static|abstract|sealed|partial|readonly|ref|unsafe|file|new)\\s+)*)(class|struct|interface|enum|record)\\s+([A-Za-z_]\\w*)",
                       2, 3, 1));
  r.decls.push_back(member(decl("^\\s*(?:\\[[^\\]]*\\]\\s*)*((?:(?:public|private|protected|internal|static|virtual|override|abstract|sealed|async|extern|unsafe|new|partial|readonly)\\s+)*)([\\w.]+(?:<[^()]*>)?(?:\\[\\])*\\?" "?)\\s+([A-Za-z_]\\w*)\\s*(?:<[^()]*>)?\\s*\\(",
                                "function", 3, 1)));
  r.entries.push_back(rx("\\bstatic\\s+(?:async\\s+)?(?:void|int|Task(?:<int>)?)\\s+Main\\s*\\("));
  return r;
}

Rules swift() {
  Rules r;
  r.generics = true;
  r.decls.push_back(kw("^\\s*((?:(?:public|open|internal|private|fileprivate|final|indirect)\\s+)*)(class|struct|enum|protocol|extension|actor)\\s+([A-Za-z_]\\w*)", 2,
                       3, 1));
  r.decls.push_back(decl("^\\s*((?:(?:public|open|internal|private|fileprivate|static|class|final|override|mutating|nonmutating|@\\w+)\\s+)*)func\\s+([A-Za-z_]\\w*)",
                         "function", 2, 1));
  r.entries.push_back(rx("^\\s*@main\\b"));
  return r;
}

Rules dart() {
  Rules r;
  r.generics = true;
  r.decls.push_back(kw("^\\s*((?:(?:abstract|sealed|base|final|interface)\\s+)*)(class|mixin|enum|extension)\\s+([A-Za-z_]\\w*)", 2, 3, 1));
  r.decls.push_back(decl("^\\s*((?:(?:static|external)\\s+)*)(?:void|int|double|bool|String|num|dynamic|Object|Future(?:<[^>]*>)?|Stream(?:<[^>]*>)?|List(?:<[^>]*>)?|Map(?:<[^>]*>)?|Set(?:<[^>]*>)?|Iterable(?:<[^>]*>)?|[A-Z]\\w*(?:<[^>]*>)?)\\?" "?\\s+([a-zA-Z_]\\w*)\\s*\\(",
                         "function", 2, 1));
  add_imports(r, {{"^\\s*(?:import|export|part)\\s+'([^']+)'", 1}, {"^\\s*(?:import|export|part)\\s+\"([^\"]+)\"", 1}});
  return r;
}

Rules php() {
  Rules r;
  r.decls.push_back(decl("^\\s*namespace\\s+([\\w\\\\]+)", "namespace"));
  r.decls.push_back(kw("^\\s*((?:(?:abstract|final|readonly)\\s+)*)(class|interface|trait|enum)\\s+([A-Za-z_]\\w*)", 2, 3, 1));
  r.decls.push_back(decl("^\\s*((?:(?:public|private|protected|static|abstract|final)\\s+)*)function\\s+&?([A-Za-z_]\\w*)\\s*\\(", "function", 2, 1));
  add_imports(r, {{"\\b(?:require|include)(?:_once)?\\s*\\(?\\s*(?:__DIR__\\s*\\.\\s*)?['\"]([^'\"]+)['\"]", 1}, {"^\\s*use\\s+([\\w\\\\]+)\\s*;", 1}});
  r.receivers = {};
  return r;
}

Rules ruby() {
  Rules r;
  r.scoping = Scoping::Indent;
  r.decls.push_back(decl("^\\s*(?:class|module)\\s+([A-Z][\\w:]*)", "class"));
  r.decls.push_back(decl("^\\s*def\\s+(?:self\\.)?([A-Za-z_]\\w*[?!=]?)", "function"));
  add_imports(r, {{"^\\s*(require_relative\\s*\\(?\\s*['\"][^'\"]+)['\"]", 1}, {"^\\s*require\\s*\\(?\\s*['\"]([^'\"]+)['\"]", 1}});
  return r;
}

Rules lua() {
  Rules r;
  r.scoping = Scoping::Indent;
  r.decls.push_back(decl("^\\s*(local\\s+)?function\\s+([A-Za-z_][\\w.:]*)\\s*\\(", "function", 2, 1));
  r.decls.push_back(decl("^\\s*(local\\s+)?([A-Za-z_][\\w.]*)\\s*=\\s*function\\s*\\(", "function", 2, 1));
  add_imports(r, {{"\\brequire\\s*\\(?\\s*['\"]([^'\"]+)['\"]", 1}});
  return r;
}

Rules bash() {
  Rules r;
  r.calls = Calls::Shell;
  r.decls.push_back(decl("^\\s*(?:function\\s+)?([A-Za-z_][\\w.:-]*)\\s*\\(\\s*\\)", "function"));
  r.decls.push_back(decl("^\\s*function\\s+([A-Za-z_][\\w.:-]*)\\s*(?:\\{|$)", "function"));
  add_imports(r, {{"(?:^|[;&|]\\s*)\\s*(?:source|\\.)\\s+([\"']?)([^\\s\"';|&]+)\\1", 2}});
  return r;
}

Rules perl() {
  Rules r;
  r.decls.push_back(decl("^\\s*sub\\s+([A-Za-z_]\\w*)", "function"));
  add_imports(r, {{"^\\s*(?:use|require)\\s+([A-Z][\\w:]*)", 1}, {"^\\s*require\\s+['\"]([^'\"]+)['\"]", 1}});
  return r;
}

Rules small(const std::string& lang) {
  Rules r;
  if (lang == "r") {
    r.decls.push_back(decl("^\\s*([A-Za-z_.][\\w.]*)\\s*(?:<-|=)\\s*function\\s*\\(", "function"));
    add_imports(r, {{"\\bsource\\s*\\(\\s*['\"]([^'\"]+)['\"]", 1}});
  } else if (lang == "julia") {
    r.scoping = Scoping::Indent;
    r.decls.push_back(decl("^\\s*module\\s+([A-Za-z_]\\w*)", "namespace"));
    r.decls.push_back(decl("^\\s*(?:mutable\\s+)?struct\\s+([A-Za-z_]\\w*)", "struct"));
    r.decls.push_back(decl("^\\s*function\\s+([A-Za-z_][\\w.!]*)\\s*[({]", "function"));
    r.decls.push_back(decl("^([A-Za-z_][\\w!]*)\\s*\\([^)]*\\)\\s*=[^=]", "function"));
    add_imports(r, {{"\\binclude\\s*\\(\\s*\"([^\"]+)\"", 1}});
  } else if (lang == "elixir") {
    r.scoping = Scoping::Indent;
    r.decls.push_back(decl("^\\s*defmodule\\s+([\\w.]+)", "class"));
    r.decls.push_back(decl("^\\s*(def|defp|defmacro|defmacrop)\\s+([a-z_]\\w*[?!]?)", "function", 2, 1));
  } else if (lang == "erlang") {
    r.scoping = Scoping::Indent;
    r.decls.push_back(decl("^([a-z]\\w*)\\s*\\(", "function"));
    add_imports(r, {{"^-include(?:_lib)?\\(\\s*\"([^\"]+)\"", 1}});
  } else if (lang == "haskell") {
    r.scoping = Scoping::Indent;
    r.calls = Calls::None;
    auto sig = decl("^([a-z_][\\w']*)\\s*::", "function");
    sig.push = false;
    r.decls.push_back(sig);
    r.decls.push_back(kw("^(data|newtype|type|class)\\s+([A-Z]\\w*)", 1, 2));
    add_imports(r, {{"^import\\s+(?:qualified\\s+)?([\\w.]+)", 1}});
  } else if (lang == "ocaml" || lang == "fsharp") {
    r.scoping = Scoping::Indent;
    r.calls = Calls::None;
    auto let = decl("^\\s*let\\s+((?:rec\\s+|inline\\s+|private\\s+|public\\s+|mutable\\s+)*)([a-zA-Z_][\\w']*)", "function", 2, 1);
    let.top = true;
    r.decls.push_back(let);
    auto val = decl("^\\s*val\\s+([a-z_][\\w']*)\\s*:", "function");
    val.push = false;
    r.decls.push_back(val);
    auto type = decl("^\\s*type\\s+((?:private\\s+)?)([A-Za-z_][\\w']*)", "class", 2, 1);
    r.decls.push_back(type);
    r.decls.push_back(member(decl("^\\s*(?:static\\s+)?(?:member|override|abstract)\\s+(?:\\w+\\.)?([A-Za-z_]\\w*)", "function")));
  } else if (lang == "clojure") {
    r.calls = Calls::Lisp;
    r.decls.push_back(decl("^\\s*\\((defn-?|defmacro|defmulti)\\s+([^\\s\\[\\]()]+)", "function", 2, 1));
  } else if (lang == "zig") {
    r.decls.push_back(decl("^\\s*(pub\\s+)?(?:export\\s+|inline\\s+|extern\\s+)*fn\\s+([A-Za-z_]\\w*)\\s*\\(", "function", 2, 1));
    r.decls.push_back(kw("^\\s*(pub\\s+)?const\\s+([A-Za-z_]\\w*)\\s*=\\s*(?:extern\\s+|packed\\s+)?(struct|enum|union)\\b", 3, 2, 1));
    add_imports(r, {{"@import\\(\\s*\"([^\"]+)\"\\s*\\)", 1}});
  } else if (lang == "nim") {
    r.scoping = Scoping::Indent;
    r.decls.push_back(decl("^\\s*(?:proc|func|method|iterator|template|macro|converter)\\s+([A-Za-z_]\\w*)(\\*)?", "function", 1, 2));
    auto type = decl("^\\s+([A-Z]\\w*)(\\*)?\\s*(?:\\[[^\\]]*\\])?\\s*=\\s*(?:ref\\s+|ptr\\s+)?(?:object|enum|tuple|distinct|concept)\\b", "struct", 1, 2);
    type.push = false;
    r.decls.push_back(type);
    add_imports(r, {{"^\\s*import\\s+([\\w/.]+)", 1}, {"^\\s*from\\s+([\\w/.]+)\\s+import\\b", 1}, {"^\\s*include\\s+([\\w/.]+)", 1}});
    r.entries.push_back(rx("\\bwhen\\s+isMainModule\\b"));
  } else if (lang == "objc") {
    r.decls.push_back(decl("^\\s*@(?:interface|implementation|protocol)\\s+([A-Za-z_]\\w*)", "class"));
    r.decls.push_back(member(decl("^\\s*[-+]\\s*\\([^)]*\\)\\s*([A-Za-z_]\\w*)", "function")));
    add_imports(r, {{"^\\s*#\\s*(?:import|include)\\s*\"([^\"]+)\"", 1}});
  }
  return r;
}

const Rules& rules_of(const std::string& id) {
  static const std::map<std::string, Rules> all = [] {
    std::map<std::string, Rules> m;
    m["python"] = python();
    m["javascript"] = javascript();
    m["typescript"] = javascript();
    m["go"] = go();
    m["rust"] = rust();
    for (const char* l : {"java", "kotlin", "scala"}) m[l] = jvm(l);
    m["csharp"] = csharp();
    m["swift"] = swift();
    m["dart"] = dart();
    m["php"] = php();
    m["ruby"] = ruby();
    m["lua"] = lua();
    m["bash"] = bash();
    m["perl"] = perl();
    for (const char* l : {"r", "julia", "elixir", "erlang", "haskell", "ocaml", "fsharp", "clojure", "zig", "nim", "objc"}) m[l] = small(l);
    return m;
  }();
  static const Rules none;
  auto it = all.find(id);
  return it == all.end() ? none : it->second;
}

// Words that look like calls but are the language: "if (", "for (", "return (".
const std::set<std::string>& keywords() {
  static const std::set<std::string> k = {
      "if",     "for",     "while",   "switch",  "catch",   "return",   "sizeof",   "function", "def",     "elif",
      "else",   "when",    "match",   "with",    "fn",      "func",     "fun",      "typeof",   "await",
      "async",  "yield",   "lambda",  "and",     "or",      "not",      "in",       "is",       "assert",  "del",
      "raise",  "except",  "try",     "finally", "import",  "from",     "export",   "class",    "struct",  "interface",
      "enum",   "type",    "using",   "namespace", "package", "local",  "then",     "end",      "do",      "done",
      "case",   "esac",    "unless",  "until",   "loop",    "select",   "foreach",  "elseif",   "require", "include", "go",
      "super",  "this",    "self",    "throw",   "throws",  "instanceof", "delete", "void",     "defer",
      "unsafe", "impl",    "where",   "let",     "var",     "const",    "static",   "public",   "private", "protected",
      "elsif",  "rescue",  "ensure",  "begin",   "module",  "require_relative", "fi", "guard",  "repeat",  "constructor"};
  return k;
}

const std::set<std::string>& shell_words() {
  static const std::set<std::string> k = {
      "if",     "then",   "else",    "elif",    "fi",      "for",    "while",  "until",  "do",       "done",
      "case",   "esac",   "in",      "function", "return", "local",  "export", "declare", "readonly", "typeset",
      "set",    "unset",  "shift",   "break",   "continue", "echo",  "printf", "test",   "true",     "false",
      "exit",   "source", "eval",    "exec",    "read",    "cd",     "trap",   "wait",   "let",      "select",
      "time",   "command", "builtin", "type",   "[",       "[[",     "]]",     "]",      "!",        "{",
      "}",      "local",  "sleep",   "kill",    "pushd",   "popd",   "mapfile", "shopt", "getopts",  "alias"};
  return k;
}

struct Scope {
  std::size_t opens = 0;  // Braces: the line of its '{' (it is not closed before that line)
  std::string name;
  std::string kind;   // class | function | namespace | impl
  int level = 0;      // Braces: depth of the body; Indent: indentation of the declaring line
  bool exported = true;
  bool interface = false;  // members are public (interface, trait, protocol, trait impl)
  bool private_now = false;  // Ruby: after a bare `private`
};

int indent_of(std::string_view line) {
  int n = 0;
  for (char c : line) {
    if (c == ' ') {
      ++n;
    } else if (c == '\t') {
      n += 4;
    } else {
      break;
    }
  }
  return n;
}

bool blank_line(std::string_view line) {
  return std::all_of(line.begin(), line.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
}

std::string kind_of_keyword(const std::string& w) {
  if (w == "struct" || w == "record" || w == "union" || w == "data" || w == "newtype") return "struct";
  if (w == "enum") return "enum";
  if (w == "type") return "alias";
  return "class";  // class, interface, trait, protocol, object, mixin, module, extension, actor
}

// Parameters of the list that opens at text[open] ('('): count of top-level commas + 1, 0 when empty,
// -1 when no list. `first` gets the first parameter.
int count_params(std::string_view text, std::size_t open, bool generics, std::string* first) {
  if (open >= text.size() || text[open] != '(') return -1;
  int depth = 0;
  int angle = 0;
  int count = 0;
  bool any = false;
  std::string cur;
  for (std::size_t i = open; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
      if (depth == 1) continue;
    } else if (c == ')' || c == ']' || c == '}') {
      --depth;
      if (depth == 0) {
        if (any) ++count;
        if (first != nullptr && count <= 1) *first = std::string(core::trim(cur));
        return count;
      }
    } else if (generics && c == '<' && i > 0 && (word_char(text[i - 1]) || text[i - 1] == ' ')) {
      ++angle;
    } else if (generics && c == '>' && angle > 0 && text[i - 1] != '=') {
      --angle;
    } else if (c == ',' && depth == 1 && angle == 0) {
      ++count;
      if (first != nullptr && count == 1) *first = std::string(core::trim(cur));
      cur.clear();
      continue;
    }
    if (depth >= 1 && c != '\n') {
      if (!std::isspace(static_cast<unsigned char>(c))) any = true;
      if (count == 0) cur += c;
    }
  }
  return -1;
}

bool has_word(const std::string& text, std::string_view w) {
  for (std::size_t p = text.find(w); p != npos; p = text.find(w, p + 1)) {
    bool left = p == 0 || !word_char(text[p - 1]);
    bool right = p + w.size() >= text.size() || !word_char(text[p + w.size()]);
    if (left && right) return true;
  }
  return false;
}

// Does the language show a declaration to other files?
bool visible(const std::string& lang, const std::string& name, const std::string& mods, const Scope* in_class, bool interface_file) {
  if (interface_file) return true;
  if (in_class != nullptr && !in_class->exported) return false;
  if (in_class != nullptr && in_class->interface) return true;
  const bool priv = has_word(mods, "private") || has_word(mods, "protected") || has_word(mods, "fileprivate");
  if (lang == "python" || lang == "dart" || lang == "bash" || lang == "perl") return !name.starts_with('_');
  if (lang == "javascript" || lang == "typescript") {
    if (in_class != nullptr) return !priv && !name.starts_with('#');
    return has_word(mods, "export");
  }
  if (lang == "go") return !name.empty() && std::isupper(static_cast<unsigned char>(name[0])) != 0;
  if (lang == "rust" || lang == "zig") return has_word(mods, "pub");
  if (lang == "java" || lang == "csharp") return has_word(mods, "public");
  if (lang == "lua") return !has_word(mods, "local");
  if (lang == "elixir") return mods == "def" || mods == "defmacro";
  if (lang == "clojure") return mods != "defn-";
  if (lang == "nim") return mods == "*";
  if (lang == "ruby") return in_class == nullptr || !in_class->private_now;
  return !priv;  // kotlin, scala, swift, php, fsharp, ocaml, haskell, julia, r, erlang, objc
}

// Braces: the line where the body of a declaration ending at `pos` opens, or -1 when it has none
// ("void f();", "fun f() = x", "type X = Y"). The '{' may follow on a later line (Allman style,
// parameters over several lines).
long body_line(std::string_view code, std::size_t pos, const std::vector<std::size_t>& starts) {
  int parens = 0;
  std::size_t lines = 0;
  for (std::size_t i = pos; i < code.size(); ++i) {
    const char c = code[i];
    if (c == '\n' && ++lines > 6) return -1;
    if (c == '(' || c == '[') ++parens;
    if (c == ')' || c == ']') --parens;
    if (parens > 0) continue;
    if (c == '{') {
      auto it = std::upper_bound(starts.begin(), starts.end(), i);
      return static_cast<long>(it - starts.begin()) - 1;
    }
    if (c == ';' || c == '}') return -1;
    if (c == '=' && i + 1 < code.size() && code[i + 1] != '>') {
      std::size_t j = i + 1;
      while (j < code.size() && (code[j] == ' ' || code[j] == '\t')) ++j;
      if (j >= code.size() || code[j] != '{') return -1;
    }
  }
  return -1;
}

}  // namespace

std::string lexical_version() { return "lex " __DATE__ " " __TIME__; }

index::FileFacts analyze_lexical(const core::Language& lang, std::string_view text, bool interface_file, std::string_view file) {
  index::FileFacts facts;
  static const std::regex kRx0 = rx("(" KK_ID ")\\s*\\(");
  static const std::regex kRx1 = rx("\\(\\s*([A-Za-z_][\\w\\-?!*<>.]*)");
  static const std::regex kRx2 = rx("^import\\s*\\(\\s*$");
  static const std::regex kRx3 = rx("^type\\s*\\(\\s*$");
  static const std::regex kRx4 = rx("^\\)");
  static const std::regex kRx5 = rx("^\\s+([A-Za-z_]\\w*)(?:\\[[^\\]]*\\])?\\s+(struct|interface)?");
  static const std::regex kRx6 = rx("^-export\\(\\s*\\[([^\\]]*)\\]");
  static const std::regex kRx7 = rx("^-compile\\(\\s*export_all");
  static const std::regex kRx8 = rx("^module\\s+[\\w.]+\\s*\\(([^)]*)\\)?");
  static const std::regex kRx9 = rx("^\\s*export\\s*\\{([^}]*)\\}");
  static const std::regex kRx10 = rx("module\\.exports\\s*=\\s*\\{([^}]*)\\}");
  static const std::regex kRx11 = rx("^\\s*(?:module\\.)?exports\\.(" KK_ID ")\\s*=");
  static const std::regex kRx12 = rx("^\\s*(private|protected)\\s*$");
  static const std::regex kRx13 = rx("^\\s*public\\s*$");
  static const std::regex kRx14 = rx("^\\s*(?:local\\s+)?([A-Za-z_]\\w*)=[\"']?\\$\\{?1(?:[}:\"'\\s]|$)");
  static const std::regex kRx15 = rx("(^|[\\s;&|(])(case|esac)\\b");
  static const std::regex kRx16 = rx("\\bcase\\s+\"?\\$\\{?([A-Za-z_]\\w*|1)[^\\s]*\\s+in\\b");
  static const std::regex kRx17 = rx("^\\s*\\(?\\s*([A-Za-z0-9_\"'|.-][^()]*?)\\)");
  static const std::regex kRx18 = rx("^\\s*import\\s+([\\w.]+(?:\\s*,\\s*[\\w.]+)+)");
  static const std::regex kRx19 = rx("(?:^|[;&|`({]|\\)\\s|\\$\\(|\\bthen\\b|\\bdo\\b|\\belse\\b)\\s*([A-Za-z_][\\w.-]*)(?=\\s|$|;|\\))");
  const auto& rules = rules_of(lang.id);
  const std::string code = mask_code(lang, text, true);      // no comments, no string contents
  const std::string plain = mask_code(lang, text, false);    // no comments, strings kept
  // Line starts.
  std::vector<std::size_t> starts = {0};
  for (std::size_t i = 0; i < code.size(); ++i) {
    if (code[i] == '\n') starts.push_back(i + 1);
  }
  auto line_text = [&](const std::string& s, std::size_t ln) {
    auto b = starts[ln];
    auto e = ln + 1 < starts.size() ? starts[ln + 1] - 1 : s.size();
    return std::string_view(s).substr(b, e - b);
  };
  const auto& kw = keywords();
  std::vector<Scope> scopes;
  int depth = 0;
  bool go_import_block = false;
  bool go_type_block = false;
  bool erlang_exports_all = false;
  std::set<std::string> erlang_exports;
  std::set<std::string> js_exports;  // export { a, b } and module.exports names
  std::set<std::string> haskell_exports;
  bool haskell_list = false;
  // Shell: subcommands of the top-level `case "$1" in`.
  std::set<std::string> arg_vars = {"1"};
  int case_depth = 0;
  int command_case = -1;  // case_depth of the command case while inside it
  std::set<std::string> seen_symbols;  // Erlang clauses: one symbol per name
  bool entry = false;

  const std::regex call_re = kRx0;
  const std::regex lisp_call = kRx1;

  for (std::size_t ln = 0; ln < starts.size(); ++ln) {
    const int line_no = static_cast<int>(ln) + 1;
    const std::string line(line_text(code, ln));
    const std::string raw(line_text(plain, ln));
    // Scopes the line is no longer in.
    if (rules.scoping == Scoping::Indent) {
      if (!blank_line(line)) {
        const int ind = indent_of(line);
        while (!scopes.empty() && scopes.back().level >= ind) scopes.pop_back();
      }
    } else {
      while (!scopes.empty() && scopes.back().level > depth && scopes.back().opens < ln) scopes.pop_back();
    }
    // The innermost class-like scope and whether the line is directly in it (not in a function).
    const Scope* cls = nullptr;
    bool in_function = false;
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      if (it->kind == "function") {
        in_function = true;
        break;
      }
      if (it->kind == "class" || it->kind == "impl") {
        cls = &*it;
        break;
      }
    }
    const bool body_level = rules.scoping == Scoping::Indent || cls == nullptr || depth == cls->level;
    const bool top = cls == nullptr && !in_function;

    // Language specifics before the declarations.
    if (lang.id == "go") {
      if (std::regex_search(raw, kRx2)) go_import_block = true;
      if (std::regex_search(raw, kRx3)) go_type_block = true;
      if (go_import_block || go_type_block) {
        if (std::regex_search(raw, kRx4)) {
          go_import_block = go_type_block = false;
        } else if (go_type_block) {
          std::smatch m;
          if (std::regex_search(line, m, kRx5)) {
            std::string kind = m[2].matched ? kind_of_keyword(m[2].str()) : "alias";
            facts.symbols.push_back({m[1].str(), kind, line_no, visible("go", m[1].str(), "", nullptr, interface_file), -1, "", false, "", ""});
          }
        }
      }
    }
    if (lang.id == "erlang") {
      std::smatch m;
      if (std::regex_search(raw, m, kRx6)) {
        for (const auto& item : core::split(m[1].str(), ',')) {
          auto name = std::string(core::trim(item.substr(0, item.find('/'))));
          if (!name.empty()) erlang_exports.insert(name);
        }
      }
      if (std::regex_search(raw, kRx7)) erlang_exports_all = true;
    }
    if (lang.id == "haskell") {
      std::smatch m;
      if (std::regex_search(line, m, kRx8)) {
        haskell_list = true;
        for (const auto& item : core::split(m[1].str(), ',')) {
          auto name = std::string(core::trim(item));
          if (auto p = name.find('('); p != npos) name = std::string(core::trim(name.substr(0, p)));
          if (!name.empty()) haskell_exports.insert(name);
        }
      }
    }
    if (lang.id == "javascript" || lang.id == "typescript") {
      std::smatch m;
      if (std::regex_search(line, m, kRx9) || std::regex_search(line, m, kRx10)) {
        for (const auto& item : core::split(m[1].str(), ',')) {
          auto name = std::string(core::trim(item));
          if (auto p = name.find_first_of(" :"); p != npos) name = name.substr(0, p);
          if (!name.empty()) js_exports.insert(name);
        }
      }
      if (std::regex_search(line, m, kRx11)) js_exports.insert(m[1].str());
    }
    if (lang.id == "ruby" && !scopes.empty()) {
      if (std::regex_search(line, kRx12)) scopes.back().private_now = true;
      if (std::regex_search(line, kRx13)) scopes.back().private_now = false;
    }

    // Declarations: the first rule that matches the line.
    std::string decl_name;
    for (const auto& d : rules.decls) {
      if (in_function) break;  // a local function or class is nobody else's
      if (d.member && (cls == nullptr || !body_level)) continue;
      if (d.top && !top) continue;
      std::smatch m;
      if (!std::regex_search(line, m, d.re)) continue;
      std::string name = m[d.name].str();
      // A member rule has no keyword of its own: "if (x)" or "return f(x)" in a class body is no method.
      if (name.empty() || (d.member && kw.contains(name))) continue;
      std::string mods = d.mods > 0 && m[d.mods].matched ? m[d.mods].str() : "";
      std::string kind = d.kind == "kw" ? kind_of_keyword(m[d.kw].str()) : d.kind;
      std::string keyword = d.kw > 0 ? m[d.kw].str() : "";
      // "return foo(x)" or "new Foo(x)" in a Java or C# class body is no declaration.
      static const std::set<std::string> kStatement = {"return", "new", "throw", "else", "case", "yield", "await", "goto"};
      if (d.member && (lang.id == "java" || lang.id == "csharp") && kStatement.contains(m[2].str())) continue;
      std::string owner = d.owner > 0 && m[d.owner].matched ? m[d.owner].str() : "";
      // Lua "M.f" / "T:f", Julia "Base.show": the part before the last dot or colon is the owner.
      if (lang.id == "lua" || lang.id == "julia") {
        auto p = name.find_last_of(".:");
        if (p != npos) {
          owner = name.substr(0, p);
          name = name.substr(p + 1);
        }
      }
      const std::size_t match_end = starts[ln] + static_cast<std::size_t>(m.position(0) + m.length(0));
      auto place = [&](Scope& s) {
        if (rules.scoping == Scoping::Indent) {
          s.level = indent_of(line);
          return true;
        }
        auto open = body_line(code, match_end, starts);
        if (open < 0) return false;
        s.level = depth + 1;
        s.opens = static_cast<std::size_t>(open);
        return true;
      };
      if (d.kind == "impl") {
        Scope s;
        s.name = name;
        s.kind = "impl";
        s.interface = m[1].matched;  // impl Trait for Type: the trait's methods are public
        if (place(s)) scopes.push_back(s);
        decl_name = name;
        break;
      }
      if (kind == "namespace") {
        Scope s;
        s.name = name;
        s.kind = "namespace";
        if (place(s)) scopes.push_back(s);  // a file-scoped namespace ("namespace A;") opens none
        break;
      }
      const bool is_type = kind == "class" || kind == "struct" || kind == "enum" || kind == "alias";
      const Scope* in_class = owner.empty() ? cls : nullptr;
      bool vis = visible(lang.id, name, mods, is_type ? nullptr : in_class, interface_file);
      if (lang.id == "javascript" || lang.id == "typescript") {
        if (is_type || in_class == nullptr) vis = vis || js_exports.contains(name);
      }
      if (lang.id == "kotlin" || lang.id == "scala" || lang.id == "swift") vis = vis && !has_word(mods, "private");
      if (lang.id == "erlang") vis = erlang_exports_all || erlang_exports.contains(name);
      if (lang.id == "haskell") vis = !haskell_list || haskell_exports.contains(name);
      index::SymbolFact s;
      s.name = name;
      s.kind = is_type ? kind : (in_class != nullptr || !owner.empty() ? "method" : "function");
      s.line = line_no;
      s.exported = vis;
      s.scope = !owner.empty() ? owner : (in_class != nullptr ? in_class->name : "");
      // Parameters: the list after the name.
      if (!is_type && lang.id != "bash") {  // a shell function takes "$@": its parameters are unknown (-1)
        auto name_end = static_cast<std::size_t>(m.position(d.name) + m.length(d.name));
        auto open = line.find('(', name_end);
        if (open != npos) {
          std::string first;
          s.params = count_params(code, starts[ln] + open, rules.generics, &first);
          if (s.params > 0 && rules.receivers.contains(first.substr(0, first.find(':')))) --s.params;
        }
      }
      if (lang.id == "erlang" && !seen_symbols.insert(name).second) break;
      if (s.name == "main" && s.scope.empty() && s.kind == "function") entry = true;
      facts.symbols.push_back(s);
      decl_name = name;
      if (d.push && (is_type ? kind != "alias" : true)) {
        Scope sc;
        sc.name = name;
        sc.kind = is_type ? "class" : "function";
        sc.exported = !is_type || vis;
        sc.interface = keyword == "interface" || keyword == "trait" || keyword == "protocol";
        if (lang.id == "erlang" || lang.id == "haskell") sc.kind = "function";
        if (place(sc)) scopes.push_back(sc);
      }
      break;
    }

    // Shell subcommands.
    if (lang.id == "bash") {
      std::smatch m;
      if (!in_function && case_depth == 0 && std::regex_search(raw, m, kRx14)) {
        arg_vars.insert(m[1].str());
      }
      for (std::sregex_iterator it(line.begin(), line.end(), kRx15), end; it != end; ++it) {
        if ((*it)[2].str() == "case") {
          ++case_depth;
          std::smatch cm;
          if (!in_function && case_depth == 1 && command_case < 0 &&
              std::regex_search(raw, cm, kRx16) && arg_vars.contains(cm[1].str())) {
            command_case = 1;
          }
        } else {
          if (command_case == case_depth) command_case = -2;  // done: only the first command case counts
          case_depth = std::max(0, case_depth - 1);
        }
      }
      if (command_case == case_depth && case_depth > 0) {
        std::smatch lm;
        if (std::regex_search(raw, lm, kRx17)) {
          for (const auto& alt : core::split(lm[1].str(), '|')) {
            std::string label(core::trim(alt));
            if (label.size() >= 2 && (label.front() == '"' || label.front() == '\'') && label.back() == label.front()) {
              label = label.substr(1, label.size() - 2);
            }
            if (label.empty() || label.starts_with('-') || label.find_first_of("*?[$ ") != npos) continue;
            if (!seen_symbols.insert("cmd:" + label).second) continue;
            facts.symbols.push_back({label, "command", line_no, true, -1, "", false, "", ""});
          }
        }
      }
    }

    // Braces after the declarations of the line (the body of a class opens on its line or later).
    if (rules.scoping == Scoping::Braces) {
      for (char c : line) {
        if (c == '{') ++depth;
        if (c == '}') depth = std::max(0, depth - 1);
      }
    }

    // Imports, on the line with its strings.
    for (std::size_t k = 0; k < rules.imports.size(); ++k) {
      // Go: "import "x"" on its own line, the bare strings only inside "import ( ... )".
      if (lang.id == "go" && (k == 1) != go_import_block) continue;
      const auto& [re, g] = rules.imports[k];
      for (std::sregex_iterator it(raw.begin(), raw.end(), re), end; it != end; ++it) {
        auto target = (*it)[g].str();
        if (!target.empty()) facts.refs.push_back({line_no, "import", target, false});
      }
    }
    if (lang.id == "python") {
      std::smatch m;
      if (std::regex_search(raw, m, kRx18)) {
        auto items = core::split(m[1].str(), ',');
        for (std::size_t k = 1; k < items.size(); ++k) facts.refs.push_back({line_no, "import", std::string(core::trim(items[k])), false});
      }
    }
    for (const auto& e : rules.entries) {
      if (!entry && std::regex_search(raw, e)) {
        entry = true;
        facts.symbols.push_back({"main", "function", line_no, false, 0, "", false, "", ""});
      }
    }

    // Calls.
    if (rules.calls == Calls::Parens) {
      bool skipped_decl = false;
      for (std::sregex_iterator it(line.begin(), line.end(), call_re), end; it != end; ++it) {
        auto name = (*it)[1].str();
        if (kw.contains(name)) continue;
        if (!skipped_decl && name == decl_name) {
          skipped_decl = true;
          continue;
        }
        facts.refs.push_back({line_no, "call", name, false});
      }
    } else if (rules.calls == Calls::Lisp) {
      for (std::sregex_iterator it(line.begin(), line.end(), lisp_call), end; it != end; ++it) {
        auto name = (*it)[1].str();
        if (name.starts_with("def") || name == "ns" || name == "let" || name == "fn" || name == "if" || name == "do") continue;
        facts.refs.push_back({line_no, "call", name, false});
      }
    } else if (rules.calls == Calls::Shell) {
      // The first word of each simple command: at the line start, after ; && || | $( ` and after
      // the words that open a command list (then, do, else, ...).
      static const std::regex cmd_re = kRx19;
      for (std::sregex_iterator it(line.begin(), line.end(), cmd_re), end; it != end; ++it) {
        auto name = (*it)[1].str();
        auto after = static_cast<std::size_t>(it->position(1) + it->length(1));
        if (after < line.size() && line[after] == '=') continue;  // an assignment
        auto before = static_cast<std::size_t>(it->position(1));
        while (before > 0 && (line[before - 1] == ' ' || line[before - 1] == '\t')) --before;
        if (before >= 2 && line[before - 1] == '(' && line[before - 2] == '(') continue;  // (( arithmetic ))
        if (shell_words().contains(name) || name == decl_name) continue;
        facts.refs.push_back({line_no, "call", name, false});
      }
    }
  }
  // An executable script without subcommands is one command: its file name.
  // "#!" among the first lines: a banner comment above it still says "this file is run, not sourced".
  auto shebang = [&] {
    std::size_t pos = 0;
    for (int k = 0; k < 3 && pos < text.size(); ++k) {
      if (text.substr(pos, 2) == "#!") return true;
      auto nl = text.find('\n', pos);
      if (nl == npos) break;
      pos = nl + 1;
    }
    return false;
  };
  if (lang.id == "bash" && shebang() && !file.empty() &&
      std::none_of(facts.symbols.begin(), facts.symbols.end(), [](const index::SymbolFact& s) { return s.kind == "command"; })) {
    auto stem = fs::path(std::string(file)).stem().string();
    if (!stem.empty()) facts.symbols.push_back({stem, "command", 1, true, -1, "", false, "", ""});
  }
  // JavaScript: names exported by a list further down ("export { a }", "module.exports = { a }").
  for (auto& s : facts.symbols) {
    if (s.scope.empty() && js_exports.contains(s.name)) s.exported = true;
  }
  return facts;
}

// --- imports --------------------------------------------------------------------------------------

PathIndex::PathIndex(const std::vector<std::string>& paths) : paths_(paths) {
  std::sort(paths_.begin(), paths_.end());
  paths_.erase(std::unique(paths_.begin(), paths_.end()), paths_.end());
  for (std::size_t i = 0; i < paths_.size(); ++i) {
    fs::path p(paths_[i]);
    by_name_[p.filename().string()].push_back(i);
    by_folder_[p.parent_path().filename().string()].push_back(i);
  }
}

bool PathIndex::has(const std::string& path) const { return std::binary_search(paths_.begin(), paths_.end(), path); }

std::vector<std::string> PathIndex::with_suffix(const std::string& suffix) const {
  std::vector<std::string> out;
  auto it = by_name_.find(fs::path(suffix).filename().string());
  if (it == by_name_.end()) return out;
  for (auto i : it->second) {
    const auto& p = paths_[i];
    if (p == suffix || p.ends_with("/" + suffix)) out.push_back(p);
  }
  return out;
}

std::vector<std::string> PathIndex::in_folder(const std::string& dir_in) const {
  std::vector<std::string> out;
  std::string dir = dir_in;
  while (dir.ends_with('/')) dir.pop_back();
  auto it = by_folder_.find(fs::path(dir).filename().string());
  if (it == by_folder_.end()) return out;
  for (auto i : it->second) {
    auto parent = fs::path(paths_[i]).parent_path().generic_string();
    if (parent == dir || parent.ends_with("/" + dir)) out.push_back(paths_[i]);
  }
  return out;
}

namespace {

std::string norm(const fs::path& p) { return p.lexically_normal().generic_string(); }

// A path written in a script: "~/.config/hypr/MyScripts/config.sh", "$HOME/x.sh", "${DIR}/x.sh".
// The part after the last variable or ~ is the project-relative tail.
std::string script_tail(std::string t, bool* anchored) {
  *anchored = false;
  std::size_t cut = 0;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '$') {
      std::size_t j = i + 1;
      if (j < t.size() && t[j] == '{') {
        auto e = t.find('}', j);
        j = e == npos ? t.size() : e + 1;
      } else if (j < t.size() && t[j] == '(') {
        auto e = t.find(')', j);
        j = e == npos ? t.size() : e + 1;
      } else {
        while (j < t.size() && word_char(t[j])) ++j;
      }
      cut = j;
      *anchored = true;
      i = j - 1;
    }
  }
  if (t.starts_with("~")) {
    cut = std::max<std::size_t>(cut, 1);
    *anchored = true;
  }
  t = t.substr(cut);
  while (t.starts_with('/')) t.erase(0, 1);
  return t;
}

}  // namespace

std::vector<std::string> resolve_import(const core::Language& lang, const std::string& from, const std::string& target_in,
                                        const PathIndex& files) {
  const fs::path dir = fs::path(from).parent_path();
  std::string target(core::trim(target_in));
  // Candidates in the order to try: relative to the file (exact), then a suffix anywhere.
  std::vector<std::string> rel, suffix;
  std::vector<std::string> rel_dirs, suffix_dirs;
  auto with_exts = [](const std::string& base, std::initializer_list<const char*> exts) {
    std::vector<std::string> v;
    for (const char* e : exts) v.push_back(base + e);
    return v;
  };
  auto add = [](std::vector<std::string>& to, const std::vector<std::string>& v) { to.insert(to.end(), v.begin(), v.end()); };
  const std::string& id = lang.id;
  if (id == "python") {
    std::size_t dots = 0;
    while (dots < target.size() && target[dots] == '.') ++dots;
    auto mod = core::replace_all(target.substr(dots), ".", "/");
    auto forms = [&](const std::string& base) {
      return base.empty() ? std::vector<std::string>{"__init__.py"} : with_exts(base, {".py", ".pyi", "/__init__.py"});
    };
    if (dots > 0) {
      fs::path base = dir;
      for (std::size_t k = 1; k < dots; ++k) base = base.parent_path();
      for (const auto& f : forms(mod)) rel.push_back(norm(base / f));
    } else {
      add(suffix, forms(mod));
    }
  } else if (id == "javascript" || id == "typescript") {
    if (!target.starts_with(".")) return {};
    auto base = norm(dir / target);
    rel.push_back(base);
    add(rel, with_exts(base, {".ts", ".tsx", ".d.ts", ".js", ".jsx", ".mjs", ".cjs", "/index.ts", "/index.tsx", "/index.js", "/index.jsx"}));
    if (base.ends_with(".js")) add(rel, with_exts(base.substr(0, base.size() - 3), {".ts", ".tsx"}));
  } else if (id == "go") {
    auto parts = core::split(target, '/');
    if (parts.empty() || parts.front().find('.') == npos) return {};  // the standard library
    for (std::size_t k = 0; k < parts.size(); ++k) {
      std::vector<std::string> tail(parts.begin() + static_cast<std::ptrdiff_t>(k), parts.end());
      suffix_dirs.push_back(core::join(tail, "/"));
    }
  } else if (id == "rust") {
    if (target.starts_with("mod ")) {
      auto name = std::string(core::trim(target.substr(4)));
      auto stem = fs::path(from).stem().string();
      fs::path base = (stem == "mod" || stem == "lib" || stem == "main") ? dir : dir / stem;
      rel.push_back(norm(base / (name + ".rs")));
      rel.push_back(norm(base / name / "mod.rs"));
    } else {
      auto parts = core::split(target, ':');
      std::erase_if(parts, [](const std::string& s) { return s.empty(); });
      if (parts.empty()) return {};
      const auto first = parts.front();
      if (first != "crate" && first != "self" && first != "super") return {};  // another crate
      parts.erase(parts.begin());
      // use crate::a::b::Item: the module is a/b (or a, when b is an item).
      for (std::size_t len = parts.size(); len >= 1; --len) {
        auto mod = core::join(std::vector<std::string>(parts.begin(), parts.begin() + static_cast<std::ptrdiff_t>(len)), "/");
        if (first == "crate") {
          add(suffix, {"src/" + mod + ".rs", "src/" + mod + "/mod.rs"});
        } else {
          fs::path base = first == "super" ? dir.parent_path() : dir;
          rel.push_back(norm(base / (mod + ".rs")));
          rel.push_back(norm(base / mod / "mod.rs"));
        }
      }
    }
  } else if (id == "java" || id == "kotlin" || id == "scala") {
    auto path = core::replace_all(target, ".", "/");
    if (path.ends_with("/*")) {
      suffix_dirs.push_back(path.substr(0, path.size() - 2));
    } else {
      add(suffix, with_exts(path, {".java", ".kt", ".scala"}));
      auto p = path.rfind('/');
      if (p != npos) add(suffix, with_exts(path.substr(0, p), {".java", ".kt", ".scala"}));  // static import of a member
    }
  } else if (id == "php") {
    if (target.find('\\') != npos) {
      add(suffix, {core::replace_all(target, "\\", "/") + ".php"});
    } else {
      rel.push_back(norm(dir / target));
      suffix.push_back(target);
    }
  } else if (id == "ruby") {
    if (target.starts_with("require_relative")) {
      auto q = target.find_first_of("'\"");
      auto t = q == npos ? std::string() : target.substr(q + 1);
      rel.push_back(norm(dir / (t.ends_with(".rb") ? t : t + ".rb")));
    } else {
      auto t = target.ends_with(".rb") ? target : target + ".rb";
      add(suffix, {"lib/" + t, t});
    }
  } else if (id == "lua") {
    auto mod = core::replace_all(target, ".", "/");
    add(suffix, {mod + ".lua", mod + "/init.lua"});
  } else if (id == "perl") {
    if (target.find("::") != npos || (std::isupper(static_cast<unsigned char>(target[0])) != 0 && target.find('.') == npos)) {
      suffix.push_back(core::replace_all(target, "::", "/") + ".pm");
    } else {
      rel.push_back(norm(dir / target));
      suffix.push_back(target);
    }
  } else if (id == "haskell") {
    suffix.push_back(core::replace_all(target, ".", "/") + ".hs");
  } else if (id == "nim") {
    auto t = target.ends_with(".nim") ? target : target + ".nim";
    rel.push_back(norm(dir / t));
    suffix.push_back(t);
  } else if (id == "dart") {
    if (target.starts_with("dart:")) return {};
    if (target.starts_with("package:")) {
      auto slash = target.find('/');
      if (slash == npos) return {};
      suffix.push_back("lib/" + target.substr(slash + 1));
    } else {
      rel.push_back(norm(dir / target));
    }
  } else if (id == "bash") {
    bool anchored = false;
    auto tail = script_tail(target, &anchored);
    if (tail.empty()) return {};
    if (!anchored) rel.push_back(norm(dir / tail));
    // "~/.config/hypr/MyScripts/config.sh" in a project at ~/.config/hypr: the longest tail that names one file.
    auto parts = core::split(tail, '/');
    for (std::size_t k = 0; k < parts.size(); ++k) {
      suffix.push_back(core::join(std::vector<std::string>(parts.begin() + static_cast<std::ptrdiff_t>(k), parts.end()), "/"));
    }
  } else {
    // A path to a file (Zig, R, Julia, Erlang, Objective-C, ...).
    if (target.find('/') == npos && target.find('.') == npos) return {};
    rel.push_back(norm(dir / target));
    suffix.push_back(target);
  }
  for (const auto& r : rel) {
    if (files.has(r)) return {r};
  }
  for (const auto& d : rel_dirs) {
    auto in = files.in_folder(d);
    if (!in.empty()) return in;
  }
  for (const auto& s : suffix) {
    auto hits = files.with_suffix(s);
    if (hits.size() == 1) return hits;
    if (hits.size() > 1) return {};  // ambiguous
  }
  for (const auto& d : suffix_dirs) {
    auto in = files.in_folder(d);
    if (!in.empty()) return in;
  }
  return {};
}

}  // namespace kk::code
