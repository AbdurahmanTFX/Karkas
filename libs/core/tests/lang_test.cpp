#include <doctest/doctest.h>

#include "kk/core/lang.hpp"
#include "kk/core/model.hpp"
#include "kk/core/paths.hpp"

using namespace kk::core;

namespace {
std::string lang_id(const char* file) {
  const auto* l = language_of(file);
  return l == nullptr ? std::string("-") : l->id;
}
}  // namespace

TEST_CASE("lang: languages by file name (ADR 0051)") {
  CHECK(lang_id("src/mix.cpp") == "cpp");
  CHECK(lang_id("include/ring.h") == "cpp");
  CHECK(lang_id("app/main.py") == "python");
  CHECK(lang_id("web/App.TSX") == "typescript");
  CHECK(lang_id("types/index.d.ts") == "typescript");
  CHECK(lang_id("cmd/server/main.go") == "go");
  CHECK(lang_id("src/lib.rs") == "rust");
  CHECK(lang_id("MyScripts/ws-navigate.sh") == "bash");
  CHECK(lang_id("hyprland.lua") == "lua");
  CHECK(lang_id("README.md") == "-");
  CHECK(lang_id("hypridle.conf") == "-");
  CHECK(lang_id("Makefile") == "-");
  CHECK(is_code_file("a/b.kt"));
  CHECK_FALSE(is_code_file("a/b.txt"));
}

TEST_CASE("lang: interface and contract files") {
  CHECK(is_interface_file("include/ring.hpp"));
  CHECK_FALSE(is_interface_file("src/ring.cpp"));
  CHECK(is_interface_file("stubs/ring.pyi"));
  CHECK(is_interface_file("types/index.d.ts"));
  CHECK_FALSE(is_interface_file("src/index.ts"));
  CHECK(is_interface_file("lib/ring.mli"));
  // A C++ source is implementation; a file of a language without headers is its own contract.
  CHECK(is_contract_file("include/ring.hpp"));
  CHECK_FALSE(is_contract_file("src/ring.cpp"));
  CHECK(is_contract_file("app/ring.py"));
  CHECK(is_contract_file("pkg/ring/ring.go"));
  CHECK(is_contract_file("scripts/run.sh"));
  CHECK_FALSE(is_contract_file("notes.txt"));
}

TEST_CASE("lang: ids and aliases") {
  REQUIRE(language_by_id("c") != nullptr);
  CHECK(language_by_id("c")->id == "cpp");
  CHECK(language_by_id("sh")->id == "bash");
  CHECK(language_by_id("Python")->id == "python");
  CHECK(language_by_id("cobol") == nullptr);
}

TEST_CASE("lang: a project names several languages, lang without a known word means C/C++") {
  auto p = parse_project_text("@project hypr\nlang    cpp bash lua python\n", "project.ksl");
  REQUIRE(p);
  CHECK(p->lang.value == "cpp bash lua python");
  CHECK(write_project(*p) == "@project hypr\nlang    cpp bash lua python\n");
  auto langs = project_languages(*p);
  REQUIRE(langs.size() == 4);
  CHECK(langs[1]->id == "bash");
  CHECK(is_project_code(*p, "MyScripts/x.sh"));
  CHECK(is_project_code(*p, "MyScripts/x.cpp"));
  CHECK_FALSE(is_project_code(*p, "web/x.ts"));

  auto old = parse_project_text("@project old\nlang    cpp\n", "project.ksl");
  REQUIRE(old);
  CHECK(is_project_code(*old, "src/a.cc"));
  CHECK_FALSE(is_project_code(*old, "tools/gen.py"));
  Project none;
  CHECK(project_languages(none).front()->id == "cpp");

  CHECK_FALSE(parse_project_text("@project x\nlang    cpp Python!\n", "project.ksl"));
}
