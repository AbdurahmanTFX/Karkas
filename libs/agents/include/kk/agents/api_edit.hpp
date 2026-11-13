#pragma once
// Mechanical edits of api/ headers that need no model (ADR 0026). An approved change writes
//   - its constants: "+ const <name> <C++ type> <value> // note" under "~ @block <id>", after the last
//     constexpr of that block's header, in its style;
//   - the declaration of a new fn of a leaf card, when the header lacks that name, every type is one the
//     header can take as it is (builtin, std:: or declared in that header) and the header's style is
//     plain (free functions in a namespace, or one interface of pure virtual methods).
// Anything else stays with the block's worker, who now finds less to do.
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/error.hpp"
#include "kk/core/model.hpp"
#include "kk/core/schema.hpp"

namespace kk::agents {

struct ConstLine {
  std::string block, name, type, value, note;
  int line = 0;
};

// "const kPaceEvery int 10 // points per step" (the text of a diff line, after its "+ ").
bool is_const_line(std::string_view text);

// The const lines of a diff, each with the block selected before it. A malformed line, a removed
// constant or a constant outside a selected block is an error that says how to write it.
kk::Result<std::vector<ConstLine>> change_constants(const std::vector<core::DiffLine>& diff);

// Checks that every constant can be written: its block has code and an api/ header. Empty when fine.
std::string check_constants(const core::Schema& schema, const std::vector<ConstLine>& consts);

// Writes the constants; a name the header already declares is left alone. One line per constant written.
kk::Result<std::vector<std::string>> write_constants(const core::Schema& schema, const std::vector<ConstLine>& consts);

// Declares the fn lines that `diff` adds to existing leaf cards (see above). One line per declaration.
std::vector<std::string> write_declarations(const core::Schema& schema, const std::vector<core::DiffLine>& diff);

// "vector<Line>" → "std::vector<Line>", "string" → "std::string" (KSL types to C++).
std::string cxx_type(std::string_view ksl);

}  // namespace kk::agents
