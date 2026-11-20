#pragma once
// C++ code analysis with tree-sitter (8.1) and the deterministic drift checks D01–D09 (8.2).
#include <set>
#include <string>
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
  // The build of this analyzer (ADR 0029): a change of analyze() or check() comes with a new build of
  // analyzer.cpp, so the index analyzes and checks again; karkas and karkas-gui link the same object.
  [[nodiscard]] std::string version() const override;
};

// A method signature decoded from SymbolFact::signature (for mocks).
struct Signature {
  std::string ret;
  std::vector<std::pair<std::string, std::string>> params;  // (type, name); name may be empty
  bool is_const = false;
  bool is_noexcept = false;
};
Signature decode_signature(std::string_view json_text);
// The card form of a C++ type, as the importer writes it into fn lines: no cv or storage words, no
// references, compact pointers and templates ("const std::string &" -> "std::string"). D04 compares
// the return type of a card's fn with the code's in this form (ADR 0029).
std::string card_type(std::string t);

// Call names too common to attribute to one block (D09 skips them; so does import).
const std::set<std::string>& common_call_names();

// Names of blocks whose api/ the block may include (links of the schema, 5.5/8.2).
std::vector<std::string> allowed_api_blocks(const core::Schema& schema, std::string_view block);
// The same over the links computed once by the caller: schema.links() builds them anew on every call,
// and a loop over the blocks of a big imported project spent minutes on that (ADR 0028).
std::vector<std::string> allowed_api_blocks(const core::Schema& schema, std::string_view block,
                                            const std::vector<core::LinkInfo>& links);

}  // namespace kk::code
