#pragma once
// C++ code analysis with tree-sitter (8.1) and the deterministic drift checks D01–D09 (8.2).
#include <string_view>

#include "kk/index/analyzer.hpp"

namespace kk::code {

class CppAnalyzer final : public index::ICodeAnalyzer {
 public:
  // Extracts #include directives, declarations of api/ files (classes, structs, enums,
  // aliases, functions, methods with parameter counts and signatures), function
  // definitions and called names in other files.
  index::FileFacts analyze(const index::CodeFile& file, std::string_view content) override;
  // D01–D09 over the whole schema plus resolved include/call references.
  index::CheckOutput check(const core::Schema& schema, const index::IndexFacts& facts) override;
};

// A method signature decoded from SymbolFact::signature (for mocks).
struct Signature {
  std::string ret;
  std::vector<std::pair<std::string, std::string>> params;  // (type, name); name may be empty
  bool is_const = false;
  bool is_noexcept = false;
};
Signature decode_signature(std::string_view json_text);

// Names of blocks whose api/ the block may include (links of the schema, 5.5/8.2).
std::vector<std::string> allowed_api_blocks(const core::Schema& schema, std::string_view block);

}  // namespace kk::code
