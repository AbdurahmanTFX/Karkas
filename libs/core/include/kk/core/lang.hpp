#pragma once
// Programming languages Karkas knows (docs/adr/0051): which files are code, how their comments and
// strings look, and which files are a contract other blocks read. Data only; the analysis of each
// language lives in kk_code.
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kk::core {

struct Project;

struct Language {
  std::string id;    // "cpp", "python", ... (the word of `lang` in project.ksl)
  std::string name;  // "C/C++", "Python"
  std::vector<std::string> exts;        // ".py" (lowercase; ".d.ts" style double extensions allowed)
  std::vector<std::string> interface_exts;  // files that only declare: ".h", ".pyi", ".d.ts", ".mli"
  // true: the interface lives in its own files (C/C++ headers) and the other files are implementation;
  // false: every file is its own interface (Python, Go, Rust, shell ...), its public names are the api.
  bool split_interface = false;
  std::vector<std::string> line_comments;                          // "//", "#", "--"
  std::vector<std::pair<std::string, std::string>> block_comments;  // {"/*", "*/"}
  std::string quotes;              // characters that open a string: "\"'`"
  bool triple_quotes = false;      // """ and ''' strings (Python, Kotlin, Swift, Julia ...)
  bool hash_comment_after_space = false;  // shell: '#' starts a comment only at a word start
  bool package_is_folder = false;  // Go: the files of one folder are one module
  std::vector<std::string> aliases;  // other words for `lang`: "c", "c++" -> cpp
};

// Every language Karkas knows, in a fixed order.
const std::vector<Language>& languages();
// By `lang` word or alias ("cpp", "c", "python", "sh"); nullptr when unknown.
const Language* language_by_id(std::string_view id);
// By the file name (extension, ".d.ts" before ".ts"); nullptr when the file is no known code.
const Language* language_of(const std::filesystem::path& p);
// A file that only declares: a C/C++ header, a Python stub (.pyi), a TypeScript .d.ts, an OCaml .mli.
bool is_interface_file(const std::filesystem::path& p);
// A file other blocks may read as the contract of its block: an interface file, or a file of a
// language that keeps no separate interface (its public names are the api).
bool is_contract_file(const std::filesystem::path& p);

// The languages of the project: the words of `lang` in project.ksl ("lang cpp python lua"), each
// resolved through its aliases. An empty or unknown `lang` means C/C++ (Karkas's first language).
std::vector<const Language*> project_languages(const Project& p);
// A code file of one of the project's languages (orphan check, import, init).
bool is_project_code(const Project& p, const std::filesystem::path& file);

}  // namespace kk::core
