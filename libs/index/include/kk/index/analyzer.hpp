#pragma once
// Hook for code analysis (implemented by kk_code with tree-sitter). The index
// calls it only for new or changed files and caches the facts by content hash.
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/schema.hpp"
#include "kk/index/state.hpp"

namespace kk::index {

struct SymbolFact {
  std::string name;   // "push", "IRing", "IRing::push"
  std::string kind;   // class | struct | enum | alias | function | method | var
  int line = 0;
  bool exported = false;  // declared in api/
  int params = -1;        // functions/methods: number of parameters
  std::string scope;      // enclosing class or namespace
  bool pure_virtual = false;
  std::string signature;  // functions/methods: return type + name + params (for mocks)
};

struct RefFact {
  int line = 0;
  std::string kind;    // include | call
  std::string target;  // include path as written, or callee name
  bool system = false; // <...> include
};

struct FileFacts {
  std::vector<SymbolFact> symbols;
  std::vector<RefFact> refs;
};

struct CodeFile {
  std::string path;   // relative to the project root, '/' separators
  std::string block;  // owning block id ("" = outside any block)
  std::string role;   // api | impl | test | mock | other
  std::string hash;   // xxh3 of the content
  int lines = 0;
  bool skeleton = false;  // contains the "karkas:skeleton" marker (generated stub)
};

struct ResolvedRef {
  std::string from_block, from_file;
  int line = 0;
  std::string to_block, to_role, kind;
};

struct IndexFacts {
  std::vector<CodeFile> files;
  std::map<std::string, FileFacts> facts;       // by file path
  std::map<std::string, std::string> api_hash;  // block → hash of its api/ files
  const ProjectState* state = nullptr;
};

struct CheckOutput {
  std::vector<core::Issue> issues;  // D01–D09
  std::vector<ResolvedRef> refs;
};

class ICodeAnalyzer {
 public:
  virtual ~ICodeAnalyzer() = default;
  virtual FileFacts analyze(const CodeFile& file, std::string_view content) = 0;
  virtual CheckOutput check(const core::Schema& schema, const IndexFacts& facts) = 0;
};

}  // namespace kk::index
