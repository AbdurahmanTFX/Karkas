#include <doctest/doctest.h>

#include <algorithm>
#include <map>

#include "kk/core/validate.hpp"
#include "kk/test/support.hpp"

using namespace kk::core;

namespace {

// Builds a small project in a temp dir. Keys are block ids, values are card bodies
// without the "@block" header; the entry block "app" is added unless present.
struct Proj {
  kk::test::TempDir dir{"val"};
  explicit Proj(std::map<std::string, std::string> cards, bool add_entry = true) {
    kk::test::write(dir / "project.ksl", "@project t\nidea    test\n");
    if (add_entry && !cards.contains("app")) cards["app"] = "purpose entry point\nkind entry\nthread main\n";
    for (const auto& [id, body] : cards) {
      auto rel = id_to_rel_path(id);
      kk::test::write(dir.path() / "blocks" / rel / "block.ksl", "@block " + id + "\n" + body);
    }
  }
  Schema schema() {
    auto s = load_schema(dir.path());
    REQUIRE_MESSAGE(s, (s ? "" : s.error().str()));
    return *s;
  }
  std::vector<Issue> issues() { return check_schema(schema()); }
};

std::vector<Issue> with_code(const std::vector<Issue>& all, std::string_view code) {
  std::vector<Issue> out;
  std::copy_if(all.begin(), all.end(), std::back_inserter(out), [&](const Issue& i) { return i.code == code; });
  return out;
}

const char* kLeaf = "purpose p\nkind leaf\nthread main\ninv ok\n";

}  // namespace

TEST_CASE("validate: wavkit fixture has no errors and no warnings") {
  auto s = load_schema(kk::test::fixture("wavkit"));
  REQUIRE(s);
  auto issues = check_schema(*s);
  for (const auto& i : issues) MESSAGE(i.str());
  CHECK(issues.empty());
  CHECK(s->blocks.size() == 17);
  CHECK(s->find("stream.ring")->parent == "stream");
  CHECK(s->find("stream.ring")->depth == 2);
  CHECK(s->find("stream")->children == std::vector<std::string>{"stream.pump", "stream.ring"});
}

TEST_CASE("validate: E01 id does not match the folder") {
  kk::test::TempDir d{"e01"};
  kk::test::write(d / "project.ksl", "@project t\n");
  kk::test::write(d / "blocks/app/block.ksl", "@block app\npurpose p\nkind entry\n");
  kk::test::write(d / "blocks/io/block.ksl", "@block input\npurpose p\nkind group\n");
  kk::test::write(d / "blocks/io/r/block.ksl", "@block io.r\npurpose p\nkind leaf\ninv x\n");
  auto s = load_schema(d.path());
  REQUIRE(s);
  auto e = with_code(check_schema(*s), "E01");
  REQUIRE(e.size() == 1);
  CHECK(e[0].str() == "blocks/io/block.ksl:1:8: E E01 io: id 'input' does not match folder path, expected 'io'");
}

TEST_CASE("validate: E01 folder with blocks but without block.ksl") {
  kk::test::TempDir d{"e01b"};
  kk::test::write(d / "project.ksl", "@project t\n");
  kk::test::write(d / "blocks/app/block.ksl", "@block app\npurpose p\nkind entry\n");
  kk::test::write(d / "blocks/dsp/gain/block.ksl", "@block dsp.gain\npurpose p\nkind leaf\ninv x\n");
  auto s = load_schema(d.path());
  REQUIRE(s);
  auto e = with_code(check_schema(*s), "E01");
  REQUIRE(e.size() == 1);
  CHECK(e[0].message == "folder 'blocks/dsp' holds blocks but has no block.ksl");
}

TEST_CASE("validate: E02 missing purpose or kind") {
  Proj p({{"a", "kind leaf\ninv x\n"}, {"b", "purpose p\n"}});
  auto e = with_code(p.issues(), "E02");
  REQUIRE(e.size() == 2);
  CHECK(e[0].block == "a");
  CHECK(e[0].message == "missing purpose");
  CHECK(e[1].block == "b");
  CHECK(e[1].message == "missing kind");
}

TEST_CASE("validate: E03 unknown block, unknown port, wrong use target") {
  Proj p({{"a", std::string(kLeaf) + "in x T from=nope:y\nin z T from=b:missing\ncall b:nofn\nuse b\n"},
          {"b", std::string(kLeaf) + "out y T\n"}});
  auto e = with_code(p.issues(), "E03");
  REQUIRE(e.size() == 4);
  CHECK(e[0].str() == "blocks/a/block.ksl:6:1: E E03 a: from= refers to unknown block 'nope'");
  CHECK(e[1].message == "block 'b' has no port 'missing'");
  CHECK(e[2].message == "block 'b' has no port 'nofn'");
  CHECK(e[3].message == "use target 'b' is a leaf block, not shared");
}

TEST_CASE("validate: E04 data link must come from an out port") {
  Proj p({{"a", std::string(kLeaf) + "in x T from=b:y\n"}, {"b", std::string(kLeaf) + "in y T\n"}});
  auto e = with_code(p.issues(), "E04");
  REQUIRE(e.size() == 1);
  CHECK(e[0].message == "data link needs an out port, but b:y is 'in'");
  CHECK(e[0].loc.line == 6);
}

TEST_CASE("validate: E05 call must target a fn") {
  Proj p({{"a", std::string(kLeaf) + "call b:y\n"}, {"b", std::string(kLeaf) + "out y T\n"}});
  auto e = with_code(p.issues(), "E05");
  REQUIRE(e.size() == 1);
  CHECK(e[0].message == "call target b:y is 'out', not fn");
}

TEST_CASE("validate: E06 tree shape and exactly one entry") {
  Proj p({{"g", "purpose p\nkind group\n"},
          {"l", std::string(kLeaf)},
          {"l.child", std::string(kLeaf)},
          {"app2", "purpose second entry\nkind entry\n"}});
  auto e = with_code(p.issues(), "E06");
  REQUIRE(e.size() == 3);
  std::vector<std::string> msgs;
  for (const auto& i : e) msgs.push_back(i.message);
  CHECK(std::find(msgs.begin(), msgs.end(), "group has no children") != msgs.end());
  CHECK(std::find(msgs.begin(), msgs.end(), "leaf block must not have children (found l.child)") != msgs.end());
  CHECK(std::find(msgs.begin(), msgs.end(), "project must have exactly one entry block, found 2") != msgs.end());
}

TEST_CASE("validate: E07 duplicate port names") {
  Proj p({{"a", std::string(kLeaf) + "out x T\nfn x() -> void\n"}});
  auto e = with_code(p.issues(), "E07");
  REQUIRE(e.size() == 1);
  CHECK(e[0].str() == "blocks/a/block.ksl:7:1: E E07 a: duplicate port name 'x'");
}

TEST_CASE("validate: E08 directive not allowed for the block kind") {
  Proj p({{"g", "purpose p\nkind group\nthread main\n"},
          {"g.l", std::string(kLeaf)},
          {"app", "purpose p\nkind entry\nfn run() -> void\ninv x\n"}});
  auto e = with_code(p.issues(), "E08");
  REQUIRE(e.size() == 3);
  CHECK(e[0].str() == "blocks/app/block.ksl:4:1: E E08 app: 'fn' is not allowed in a entry block");
  CHECK(e[1].message == "'inv' is not allowed in a entry block");
  CHECK(e[2].message == "'thread' is not allowed in a group block");
}

TEST_CASE("validate: W01 thread crossing without via=") {
  Proj p({{"a", std::string(kLeaf) + "in x T from=b:y\nin z T from=b:w via=queue\n"},
          {"b", "purpose p\nkind leaf\nthread reader\ninv ok\nout y T\nout w T\n"},
          {"c", "purpose p\nkind leaf\nthread any\ninv ok\nin q T from=b:y\n"}});
  auto w = with_code(p.issues(), "W01");
  REQUIRE(w.size() == 1);
  CHECK(w[0].severity == 'W');
  CHECK(w[0].message == "data link b -> a crosses threads reader -> main without via=");
}

TEST_CASE("validate: W02 shared mutable state") {
  Proj p({{"a", std::string(kLeaf) + "state counter int with=b via=atomic\n"}, {"b", std::string(kLeaf)}});
  auto w = with_code(p.issues(), "W02");
  REQUIRE(w.size() == 1);
  CHECK(w[0].message == "shared mutable state 'counter' with b (risk)");
}

TEST_CASE("validate: W03 too many children and too deep") {
  std::map<std::string, std::string> cards{{"g", "purpose p\nkind group\n"}};
  for (int i = 0; i < 10; ++i) cards["g.c" + std::to_string(i)] = kLeaf;
  cards["d1"] = "purpose p\nkind group\n";
  cards["d1.d2"] = "purpose p\nkind group\n";
  cards["d1.d2.d3"] = "purpose p\nkind group\n";
  cards["d1.d2.d3.d4"] = "purpose p\nkind group\n";
  cards["d1.d2.d3.d4.d5"] = "purpose p\nkind group\n";
  cards["d1.d2.d3.d4.d5.d6"] = kLeaf;
  Proj p(cards);
  auto w = with_code(p.issues(), "W03");
  REQUIRE(w.size() == 2);
  CHECK(w[0].message == "depth 6 is more than 5");
  CHECK(w[1].message == "group has 10 children (more than 9)");
}

TEST_CASE("validate: W04 long purpose or 'and'") {
  Proj p({{"a", "purpose read and write files\nkind leaf\nthread main\ninv x\n"},
          {"b", "purpose " + std::string(101, 'x') + "\nkind leaf\nthread main\ninv x\n"}});
  auto w = with_code(p.issues(), "W04");
  REQUIRE(w.size() == 2);
  CHECK(w[0].block == "a");
  CHECK(w[1].message == "purpose is 101 characters long (more than 100)");
}

TEST_CASE("validate: W05 call cycle between leaves") {
  Proj p({{"a", std::string(kLeaf) + "fn f() -> void\ncall b:g\n"},
          {"b", std::string(kLeaf) + "fn g() -> void\ncall a:f\n"}});
  auto w = with_code(p.issues(), "W05");
  REQUIRE(w.size() == 1);
  CHECK(w[0].message == "call cycle between a, b");
}

TEST_CASE("validate: W06 leaf without inv") {
  Proj p({{"a", "purpose p\nkind leaf\nthread main\n"}});
  auto w = with_code(p.issues(), "W06");
  REQUIRE(w.size() == 1);
  CHECK(w[0].str() == "blocks/a/block.ksl:1:1: W W06 a: leaf has no inv");
}

TEST_CASE("validate: syntax errors in a card become issues and other cards still load") {
  Proj p({{"a", "purpose p\nkind leaf\ninv x\ncolour red\n"}, {"b", std::string(kLeaf)}});
  auto s = p.schema();
  CHECK(s.find("a") == nullptr);
  CHECK(s.find("b") != nullptr);
  auto e = with_code(check_schema(s), "S03");
  REQUIRE(e.size() == 1);
  CHECK(e[0].str() == "blocks/a/block.ksl:5:1: E S03 a: unknown directive 'colour' in @block");
}

TEST_CASE("validate: links and data-flow order of the wavkit schema") {
  auto s = load_schema(kk::test::fixture("wavkit"));
  REQUIRE(s);
  auto links = s->links();
  auto it = std::find_if(links.begin(), links.end(), [](const LinkInfo& l) {
    return l.src_block == "stream.ring" && l.dst_block == "dsp.chain";
  });
  REQUIRE(it != links.end());
  CHECK(it->kind == LinkKind::Data);
  CHECK(it->thread_src == "any");
  CHECK(it->thread_dst == "main");
  CHECK_FALSE(it->crosses_threads());
  std::vector<std::string> top;
  for (const auto* b : s->ordered_children("")) top.push_back(b->id);
  CHECK(top == std::vector<std::string>{"app", "io", "stream", "dsp", "mix", "shared"});
  std::vector<std::string> dsp;
  for (const auto* b : s->ordered_children("dsp")) dsp.push_back(b->id);
  CHECK(dsp == std::vector<std::string>{"dsp.chain", "dsp.echo", "dsp.gain", "dsp.lowpass"});
}
