#include "kk/core/lang.hpp"

#include <algorithm>

#include "kk/core/model.hpp"
#include "kk/core/text.hpp"

namespace kk::core {

namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

std::vector<Language> make_languages() {
  const Pairs c_block = {{"/*", "*/"}};
  std::vector<Language> v;
  auto add = [&](Language l) { v.push_back(std::move(l)); };
  // C and C++ (tree-sitter, kk_code CppAnalyzer). Objective-C has its own entry: tree-sitter-cpp
  // does not parse it.
  add({"cpp", "C/C++", {".cpp", ".cc", ".cxx", ".c", ".hpp", ".hh", ".h", ".hxx", ".ipp", ".inl"},
       {".hpp", ".hh", ".h", ".hxx", ".ipp", ".inl"}, true, {"//"}, c_block, "\"'", false, false, false,
       {"c", "c++", "cxx"}});
  add({"objc", "Objective-C", {".m", ".mm"}, {}, false, {"//"}, c_block, "\"'", false, false, false, {"objective-c"}});
  add({"python", "Python", {".py", ".pyi", ".pyw"}, {".pyi"}, false, {"#"}, {}, "\"'", true, false, false, {"py"}});
  add({"typescript", "TypeScript", {".ts", ".tsx", ".mts", ".cts", ".d.ts"}, {".d.ts"}, false, {"//"}, c_block, "\"'`",
       false, false, false, {"ts"}});
  add({"javascript", "JavaScript", {".js", ".jsx", ".mjs", ".cjs"}, {}, false, {"//"}, c_block, "\"'`", false, false,
       false, {"js", "node"}});
  add({"go", "Go", {".go"}, {}, false, {"//"}, c_block, "\"`", false, false, true, {"golang"}});
  add({"rust", "Rust", {".rs"}, {}, false, {"//"}, c_block, "\"", false, false, false, {"rs"}});
  add({"java", "Java", {".java"}, {}, false, {"//"}, c_block, "\"'", true, false, false, {}});
  add({"kotlin", "Kotlin", {".kt", ".kts"}, {}, false, {"//"}, c_block, "\"'", true, false, false, {"kt"}});
  add({"scala", "Scala", {".scala", ".sc"}, {}, false, {"//"}, c_block, "\"'", true, false, false, {}});
  add({"csharp", "C#", {".cs"}, {}, false, {"//"}, c_block, "\"'", true, false, false, {"cs", "c#"}});
  add({"fsharp", "F#", {".fs", ".fsx", ".fsi"}, {".fsi"}, false, {"//"}, {{"(*", "*)"}}, "\"", true, false, false,
       {"fs", "f#"}});
  add({"swift", "Swift", {".swift"}, {}, false, {"//"}, c_block, "\"", true, false, false, {}});
  add({"dart", "Dart", {".dart"}, {}, false, {"//"}, c_block, "\"'", true, false, false, {}});
  add({"php", "PHP", {".php"}, {}, false, {"//", "#"}, c_block, "\"'", false, false, false, {}});
  add({"ruby", "Ruby", {".rb", ".rake"}, {}, false, {"#"}, {{"\n=begin", "\n=end"}}, "\"'", false, false, false, {"rb"}});
  add({"lua", "Lua", {".lua"}, {}, false, {"--"}, {{"--[[", "]]"}}, "\"'", false, false, false, {}});
  add({"bash", "Shell", {".sh", ".bash", ".zsh", ".ksh"}, {}, false, {"#"}, {}, "\"'", false, true, false,
       {"sh", "shell", "zsh"}});
  add({"perl", "Perl", {".pl", ".pm"}, {}, false, {"#"}, {{"\n=pod", "\n=cut"}}, "\"'", false, true, false, {}});
  add({"r", "R", {".r"}, {}, false, {"#"}, {}, "\"'", false, false, false, {}});
  add({"julia", "Julia", {".jl"}, {}, false, {"#"}, {{"#=", "=#"}}, "\"", true, false, false, {}});
  add({"elixir", "Elixir", {".ex", ".exs"}, {}, false, {"#"}, {}, "\"'", true, false, false, {}});
  add({"erlang", "Erlang", {".erl", ".hrl"}, {".hrl"}, false, {"%"}, {}, "\"", false, false, false, {}});
  add({"haskell", "Haskell", {".hs"}, {}, false, {"--"}, {{"{-", "-}"}}, "\"", false, false, false, {"hs"}});
  add({"ocaml", "OCaml", {".ml", ".mli"}, {".mli"}, false, {}, {{"(*", "*)"}}, "\"", false, false, false, {"ml"}});
  add({"clojure", "Clojure", {".clj", ".cljs", ".cljc"}, {}, false, {";"}, {}, "\"", false, false, false, {}});
  add({"zig", "Zig", {".zig"}, {}, false, {"//"}, {}, "\"", false, false, false, {}});
  add({"nim", "Nim", {".nim"}, {}, false, {"#"}, {{"#[", "]#"}}, "\"", true, false, false, {}});
  return v;
}

std::string lower_name(const std::filesystem::path& p) { return to_lower(p.filename().string()); }

bool ends_with_ext(const std::string& name, const std::string& ext) {
  return name.size() > ext.size() && name.ends_with(ext);
}

}  // namespace

const std::vector<Language>& languages() {
  static const std::vector<Language> v = make_languages();
  return v;
}

const Language* language_by_id(std::string_view id_in) {
  const auto id = to_lower(id_in);
  for (const auto& l : languages()) {
    if (l.id == id || std::find(l.aliases.begin(), l.aliases.end(), id) != l.aliases.end()) return &l;
  }
  return nullptr;
}

const Language* language_of(const std::filesystem::path& p) {
  auto name = lower_name(p);
  const Language* best = nullptr;
  std::size_t best_len = 0;
  // The longest matching extension wins: "x.d.ts" is TypeScript's interface, not a ".ts" file.
  for (const auto& l : languages()) {
    for (const auto& e : l.exts) {
      if (e.size() > best_len && ends_with_ext(name, e)) {
        best = &l;
        best_len = e.size();
      }
    }
  }
  return best;
}

bool is_interface_file(const std::filesystem::path& p) {
  const auto* l = language_of(p);
  if (l == nullptr) return false;
  auto name = lower_name(p);
  return std::any_of(l->interface_exts.begin(), l->interface_exts.end(), [&](const std::string& e) { return ends_with_ext(name, e); });
}

bool is_contract_file(const std::filesystem::path& p) {
  const auto* l = language_of(p);
  if (l == nullptr) return false;
  return !l->split_interface || is_interface_file(p);
}

std::vector<const Language*> project_languages(const Project& p) {
  std::vector<const Language*> out;
  for (const auto& w : split(p.lang.value, ' ')) {
    auto word = std::string(trim(w));
    if (word.empty()) continue;
    const auto* l = language_by_id(word);
    if (l != nullptr && std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
  }
  if (out.empty()) out.push_back(language_by_id("cpp"));
  return out;
}

bool is_project_code(const Project& p, const std::filesystem::path& file) {
  const auto* l = language_of(file);
  if (l == nullptr) return false;
  auto langs = project_languages(p);
  return std::find(langs.begin(), langs.end(), l) != langs.end();
}

}  // namespace kk::core
