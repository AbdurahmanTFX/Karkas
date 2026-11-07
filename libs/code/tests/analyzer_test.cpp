#include <doctest/doctest.h>

#include <algorithm>

#include "kk/code/analyzer.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {
const index::SymbolFact* sym(const index::FileFacts& f, std::string_view name, std::string_view kind) {
  auto it = std::find_if(f.symbols.begin(), f.symbols.end(),
                         [&](const index::SymbolFact& s) { return s.name == name && s.kind == kind; });
  return it == f.symbols.end() ? nullptr : &*it;
}
}  // namespace

TEST_CASE("analyzer: includes, classes, pure virtual methods and free functions in api/") {
  code::CppAnalyzer an;
  auto f = an.analyze({"blocks/stream/ring/api/ring.hpp", "stream.ring", "api", "", 0, false},
                      test::read(test::fixture("wavkit") / "blocks/stream/ring/api/ring.hpp"));
  auto inc = std::find_if(f.refs.begin(), f.refs.end(), [](const index::RefFact& r) { return r.kind == "include" && !r.system; });
  REQUIRE(inc != f.refs.end());
  CHECK(inc->target == "types.hpp");
  CHECK(std::any_of(f.refs.begin(), f.refs.end(), [](const index::RefFact& r) { return r.system && r.target == "span"; }));
  const auto* ring = sym(f, "IRing", "struct");
  REQUIRE(ring != nullptr);
  CHECK(ring->exported);
  CHECK(ring->scope == "wk");
  const auto* push = sym(f, "push", "method");
  REQUIRE(push != nullptr);
  CHECK(push->pure_virtual);
  CHECK(push->params == 1);
  CHECK(push->scope == "wk::IRing");
  auto sig = code::decode_signature(push->signature);
  CHECK(sig.ret == "bool");
  REQUIRE(sig.params.size() == 1);
  CHECK(sig.params[0].first == "std::span<const Frame>");
  CHECK(sig.params[0].second == "frames");
  auto cap = code::decode_signature(sym(f, "capacity", "method")->signature);
  CHECK(cap.is_const);
  CHECK(cap.ret == "std::size_t");
  const auto* make = sym(f, "make_ring", "function");
  REQUIRE(make != nullptr);
  CHECK(make->params == 1);
}

TEST_CASE("analyzer: aliases, enums, function definitions and calls in impl/") {
  code::CppAnalyzer an;
  auto api = an.analyze({"a/api/x.hpp", "a", "api", "", 0, false},
                        "namespace n { using Id = int; typedef float Gain; enum class Mode { A, B };\n"
                        "struct S { int f(int a, int b = 2) const; }; }\n");
  CHECK(sym(api, "Id", "alias") != nullptr);
  CHECK(sym(api, "Gain", "alias") != nullptr);
  CHECK(sym(api, "Mode", "enum") != nullptr);
  CHECK(sym(api, "f", "method")->params == 2);
  CHECK_FALSE(sym(api, "f", "method")->pure_virtual);
  auto impl = an.analyze({"a/impl/x.cpp", "a", "impl", "", 0, false},
                         "#include \"x.hpp\"\nint n::S::f(int a, int b) const { helper(a); obj.run(); ns::go(b); return 0; }\n"
                         "static void helper(int) {}\n");
  const auto* f = sym(impl, "f", "method");
  REQUIRE(f != nullptr);
  CHECK(f->scope == "n::S");
  CHECK_FALSE(f->exported);
  std::vector<std::string> calls;
  for (const auto& r : impl.refs) {
    if (r.kind == "call") calls.push_back(r.target);
  }
  CHECK(calls == std::vector<std::string>{"helper", "run", "go"});
  // Broken code still yields what can be parsed (tree-sitter is tolerant).
  auto broken = an.analyze({"a/impl/y.cpp", "a", "impl", "", 0, false}, "#include \"z.hpp\"\nvoid g( { x(\n");
  CHECK(broken.refs.size() >= 1);
}
