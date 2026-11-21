#pragma once
// karkas import (stage 11, docs/adr/0009): turns existing C/C++ code into blocks without
// moving it. Deterministic, no model calls:
//   folders                  → groups (a leading src/ is dropped, one-child chains fold)
//   x.cpp + x.hpp (any area) → one leaf whose card lists the files ("files")
//   public functions/methods of the headers → fn ports (names as KSL identifiers)
//   calls of those functions → call links; includes without calls → use links
//   main()                   → the entry block; header-only modules without calls → shared
//   first comment of the header → purpose (otherwise a placeholder to describe later)
//   test folders and test programs (foo_test.cc next to foo.cc) stay out: the project's own suite
// The result is a list of new cards; the CLI turns it into a @change for approval.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/error.hpp"
#include "kk/core/model.hpp"

namespace kk::code {

struct ImportOptions {
  std::vector<std::string> exclude;  // extra folders to skip, project-relative
  std::size_t max_ports = 24;        // fn ports per block; the rest are named in a note
};

struct ImportPlan {
  std::vector<core::Block> blocks;   // new cards, parents before children
  std::vector<core::DiffLine> diff;  // "+ @block ..." lines of all cards
  std::vector<std::string> files;    // code files that became part of blocks
  std::vector<std::string> skipped;  // folders skipped on purpose (tests, third_party, ...)
  std::vector<std::string> tests;    // test programs left out (foo_test.cc ...), docs/adr/0048
  std::vector<std::string> warnings;
  int groups = 0;
  int leaves = 0;  // leaf + entry + shared
  int links = 0;   // call + use lines
};

// What import leaves out by name (docs/adr/0009, 0048); the drift check of an imported project does not
// call it an orphan. Folders of other code and of tests: third_party, vendor, external, tests, testing, ...
bool import_skips_dir(std::string_view name);
// A test program by its name: foo_test.cc, foo_unittest.cc, foo-test.cpp, foo.test.cpp, test_foo.cpp,
// tst_foo.cpp, FooTest.cpp, test.cpp. Never a header. Import keeps one that has a header of the same stem
// (then it has an api of its own).
bool is_test_source(const std::filesystem::path& p);

kk::Result<ImportPlan> plan_import(const std::filesystem::path& root, const ImportOptions& opts = {});

// Text of the plan for the terminal: tree of blocks with their files and links.
std::string describe_plan(const ImportPlan& plan);

}  // namespace kk::code
