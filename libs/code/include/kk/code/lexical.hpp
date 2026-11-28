#pragma once
// Analysis of code in languages other than C/C++ (docs/adr/0051). Deterministic and textual: comments
// and strings are masked by the language's own syntax (core::Language), then rules per language find
// the declarations (with their scope, parameter count and whether other files see them), the imports
// and the called names. The result has the shape of the tree-sitter facts of C/C++ (index::FileFacts),
// so the index, the drift checks, import and kk_locate treat every language alike.
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kk/core/lang.hpp"
#include "kk/index/analyzer.hpp"

namespace kk::code {

// Facts of one file. `interface_file`: a file that only declares (.pyi, .d.ts, .mli): all its
// declarations are public. Ref kinds: "import" (target as written) and "call" (a name).
// A shell script's subcommands (the labels of its top-level `case "$1" in`) are public symbols of
// kind "command"; an executable script (#!) without subcommands is one command named after `file`
// ("show-desktop.sh" -> show-desktop). A program's entry gives a function symbol "main" without scope.
index::FileFacts analyze_lexical(const core::Language& lang, std::string_view text, bool interface_file = false,
                                 std::string_view file = {});

// The text with comments (and with `strings`, the insides of string literals) replaced by spaces;
// lines stay where they are.
std::string mask_code(const core::Language& lang, std::string_view text, bool strings);

// The build of the lexical analysis (part of CppAnalyzer::version()).
std::string lexical_version();

// Project files by path, for resolving imports.
class PathIndex {
 public:
  explicit PathIndex(const std::vector<std::string>& paths);
  [[nodiscard]] bool has(const std::string& path) const;
  // Files whose path is `suffix` or ends with "/" + suffix.
  [[nodiscard]] std::vector<std::string> with_suffix(const std::string& suffix) const;
  // Files right inside a folder whose path is `dir` or ends with "/" + dir.
  [[nodiscard]] std::vector<std::string> in_folder(const std::string& dir) const;

 private:
  std::vector<std::string> paths_;
  std::unordered_map<std::string, std::vector<std::size_t>> by_name_;    // file name -> paths
  std::unordered_map<std::string, std::vector<std::size_t>> by_folder_;  // last folder name -> paths
};

// The project files an import names (`target` as the file `from` writes it), or none when it names a
// package of the language or a system module. One file, or the files of a folder (a Go package, a
// Java wildcard import). A suffix that fits several files resolves to none.
std::vector<std::string> resolve_import(const core::Language& lang, const std::string& from, const std::string& target,
                                        const PathIndex& files);

}  // namespace kk::code
