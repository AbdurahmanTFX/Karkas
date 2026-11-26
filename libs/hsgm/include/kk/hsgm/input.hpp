#pragma once
// Input of the HSGM measurements (ADR 0049): everything they read, taken once from an updated index.
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/error.hpp"
#include "kk/index/index.hpp"

namespace kk::hsgm {

struct Input {
  index::Snapshot snap;
  std::vector<index::CodeFile> files;
  std::vector<index::RunRow> runs;
  std::map<std::string, std::vector<std::string>> test_cases;  // block → TEST_CASE names of its contract test files
};

// Fails ("hsgm", "index not updated") when the index has loaded no schema yet.
kk::Result<Input> gather(index::Index& idx);

// TEST_CASE("…") names of a doctest source, in order; \" and \\ unescaped.
std::vector<std::string> test_case_names(std::string_view source);

bool is_code_block(const index::BlockRow& b);  // kind leaf, entry or shared
bool is_code_project(const Input& in);         // at least one code block

// "data io.reader:frames -> stream.pump:src"; empty ports are left out.
std::string link_name(const index::LinkRow& l);

// The ancestor of `id` on `level` (0 = the top level): its first level+1 segments, or `id` when shorter.
std::string ancestor_on(std::string_view id, int level);

}  // namespace kk::hsgm
