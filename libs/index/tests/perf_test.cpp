#include <doctest/doctest.h>

#include <chrono>
#include <fmt/format.h>

#include "kk/index/index.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

// Synthetic reference project: 8 groups x 4 leaves + entry + shared group with
// 2 shared blocks = 44 blocks, ~20k lines of code (7.2, stage 2 acceptance).
void make_big_project(const std::filesystem::path& root) {
  test::write(root / "project.ksl", "@project big\nidea    synthetic project for index timing\n");
  test::write(root / "blocks/app/block.ksl", "@block app\npurpose entry\nkind    entry\nthread  main\nuse     shared.types\n");
  test::write(root / "blocks/shared/block.ksl", "@block shared\npurpose common code\nkind    group\n");
  for (const char* s : {"types", "log"}) {
    test::write(root / "blocks/shared" / s / "block.ksl",
                fmt::format("@block shared.{}\npurpose shared {}\nkind    shared\nthread  any\ninv     stable\n", s, s));
    test::write(root / "blocks/shared" / s / "api" / (std::string(s) + ".hpp"), "#pragma once\nstruct Frame { float l, r; };\n");
  }
  std::string prev;
  for (int g = 0; g < 8; ++g) {
    auto gid = fmt::format("g{}", g);
    test::write(root / "blocks" / gid / "block.ksl", fmt::format("@block {}\npurpose group {}\nkind    group\n", gid, g));
    for (int l = 0; l < 4; ++l) {
      auto lid = fmt::format("{}.l{}", gid, l);
      std::string card = fmt::format("@block {}\npurpose leaf {}\nkind    leaf\nthread  main\nest     M\n", lid, l);
      if (!prev.empty()) card += fmt::format("in      src Frame[] from={}:out\n", prev);
      card += "out     out Frame[]\nfn      run() -> void\nuse     shared.types\ninv     keeps order\n";
      auto dir = root / "blocks" / gid / fmt::format("l{}", l);
      test::write(dir / "block.ksl", card);
      std::string api = "#pragma once\n#include \"types.hpp\"\nstruct ILeaf {\n  virtual ~ILeaf() = default;\n";
      for (int i = 0; i < 40; ++i) api += fmt::format("  virtual int f{}(int a, int b) = 0;\n", i);
      api += "};\n";
      test::write(dir / "api" / "leaf.hpp", api);
      std::string impl = "#include \"leaf.hpp\"\n";
      for (int i = 0; i < 100; ++i) impl += fmt::format("int helper{}(int a, int b) {{\n  int s = a + b;\n  return s * {};\n}}\n", i, i);
      test::write(dir / "impl" / "leaf.cpp", impl);
      test::write(dir / "tests/contract/leaf_contract.cpp", "// contract\nint t() { return 0; }\n");
      prev = lid;
    }
  }
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST_CASE("perf: 40+ blocks, ~20k lines: full index < 2 s, unchanged update < 100 ms") {
  test::TempDir d{"big"};
  make_big_project(d.path());
  auto idx = index::Index::open(d.path());
  REQUIRE(idx);
  auto t0 = std::chrono::steady_clock::now();
  auto full = (*idx)->update({true, nullptr});
  double full_ms = ms_since(t0);
  REQUIRE(full);
  CHECK(full->blocks == 44);
  CHECK(full->files >= 96);
  MESSAGE("full index: " << full_ms << " ms");
  CHECK(full_ms < 2000.0);

  t0 = std::chrono::steady_clock::now();
  auto again = (*idx)->update();
  double inc_ms = ms_since(t0);
  REQUIRE(again);
  CHECK_FALSE(again->changed);
  MESSAGE("unchanged update: " << inc_ms << " ms");
  CHECK(inc_ms < 100.0);

  test::write(d / "blocks/g3/l2/impl/leaf.cpp", "#include \"leaf.hpp\"\nint changed() { return 1; }\n");
  t0 = std::chrono::steady_clock::now();
  auto one = (*idx)->update();
  double one_ms = ms_since(t0);
  REQUIRE(one);
  CHECK(one->changed);
  MESSAGE("one file changed: " << one_ms << " ms");
  CHECK(one_ms < 500.0);
}
