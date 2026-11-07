#include <doctest/doctest.h>

#include <filesystem>

#include "kk/core/model.hpp"
#include "kk/test/support.hpp"

using namespace kk::core;
namespace fs = std::filesystem;

namespace {

std::string rewrite(const std::string& text, const std::string& name) {
  auto recs = parse_records(text, name);
  REQUIRE_MESSAGE(recs, (recs ? "" : recs.error().str()));
  REQUIRE(recs->size() == 1);
  const auto& rec = (*recs)[0];
  if (rec.kind == "block") {
    auto b = parse_block(rec);
    REQUIRE_MESSAGE(b, (b ? "" : b.error().str()));
    return write_block(*b);
  }
  if (rec.kind == "project") {
    auto p = parse_project(rec);
    REQUIRE_MESSAGE(p, (p ? "" : p.error().str()));
    return write_project(*p);
  }
  if (rec.kind == "ticket") {
    auto t = parse_ticket(rec);
    REQUIRE_MESSAGE(t, (t ? "" : t.error().str()));
    return write_ticket(*t);
  }
  if (rec.kind == "change") {
    auto c = parse_change(rec);
    REQUIRE_MESSAGE(c, (c ? "" : c.error().str()));
    return write_change(*c);
  }
  FAIL("unexpected record kind " << rec.kind);
  return {};
}

}  // namespace

TEST_CASE("roundtrip: every KSL file of every fixture is byte-identical after read-write-read") {
  int files = 0;
  for (const auto& e : fs::recursive_directory_iterator(kk::test::source_dir() / "tests" / "fixtures")) {
    if (e.path().extension() != ".ksl") continue;
    const auto name = e.path().string();
    if (name.find("/broken/") != std::string::npos) continue;
    std::string original = kk::test::read(e.path());
    std::string once = rewrite(original, name);
    CHECK_MESSAGE(once == original, name);
    CHECK_MESSAGE(rewrite(once, name) == once, name);
    ++files;
  }
  CHECK(files >= 18);
}

TEST_CASE("roundtrip: non-canonical input becomes canonical and then stable") {
  std::string messy = R"(@block   dsp.gain
  use shared.types
kind leaf
inv gain 1.0 leaves samples unchanged
fn  process(Frame[])   ->   void
purpose   multiply samples by gain
est S
thread main
)";
  std::string canonical = R"(@block dsp.gain
purpose multiply samples by gain
kind    leaf
thread  main
est     S
fn      process(Frame[]) -> void
use     shared.types
inv     gain 1.0 leaves samples unchanged
)";
  CHECK(rewrite(messy, "m.ksl") == canonical);
  CHECK(rewrite(canonical, "c.ksl") == canonical);
}

TEST_CASE("roundtrip: comments survive rewriting") {
  std::string text = R"(# reviewed by human
@block a.b  # header note
# why it exists
purpose p
kind    leaf  # will split later
# trailing remark
)";
  CHECK(rewrite(text, "a.ksl") == R"(# reviewed by human
@block a.b  # header note
# why it exists
purpose p
kind    leaf  # will split later
# trailing remark
)");
}

TEST_CASE("roundtrip: ticket example from the spec") {
  std::string t = R"(@ticket T-0042
kind    bug  # bug | feature | change | question
origin  tester
status  open
symptom crackle in output after ~10 s of playback, worse with echo enabled
fact    contract stream.ring/no_drop FAIL: dropped=37 of 480000 frames
fact    probe stream.pump->stream.ring fill_min=0 at t=9.8s
--- trail
L0 orchestrator at=root suspect=stream,dsp pick=stream difficulty=M why="frames dropped before dsp; ring runs empty"
L1 navigator    at=stream suspect=stream.ring,stream.pump ruled_out=stream.pump pick=stream.ring why="pump rate ok 48k/s; drops at wraparound"
L2 worker       at=stream.ring model=cheap attempt=1 outcome=fixed
answer  at=stream.pump "rate is stable"
--- result
status  fixed
cause   off-by-one at wraparound: write index compared with capacity, not capacity-1
fix     impl/ring.cpp:41 mask index with cap-1; unit test for wraparound added
api     unchanged
verify  stream.ring contract 5/5 pass; dsp.*, io.reader pass
cost    in=21480 out=2930 cache_read=15210
)";
  CHECK(rewrite(t, "T-0042.ksl") == t);
}

TEST_CASE("roundtrip: change example from the spec") {
  std::string c = R"(@change C-0003
from    orchestrator T-0051
reason  reverb requested; no existing block provides it
status  pending
--- diff
+ @block dsp.reverb
+ purpose apply Schroeder reverb to frames
+ kind    leaf
+ fn      process(Frame[]) -> void
~ @block dsp.chain
+ call    dsp.reverb:process
)";
  CHECK(rewrite(c, "C-0003.ksl") == c);
}

TEST_CASE("roundtrip: free text that needs quotes") {
  Block b;
  b.id = "a";
  b.purpose = Scalar{"handles issue #12", {}, 0, 0, true};
  b.notes.push_back(TextItem{"\"quoted\" start", {}, 0, 0});
  auto text = write_block(b);
  CHECK(text == "@block a\npurpose \"handles issue #12\"\nnote    \"\\\"quoted\\\" start\"\n");
  auto back = parse_block_text(text, "a.ksl");
  REQUIRE(back);
  CHECK(back->purpose.value == "handles issue #12");
  CHECK(back->notes[0].text == "\"quoted\" start");
}

TEST_CASE("roundtrip: files directive is canonical and stable") {
  auto b = parse_block_text("@block game.cat\npurpose the cat\nkind leaf\nfn update(float) -> void\nfiles  src/active/cat.cpp   src/active/hpp/cat.hpp\n", "c.ksl");
  REQUIRE(b);
  auto text = write_block(*b);
  CHECK(text == "@block game.cat\npurpose the cat\nkind    leaf\nfiles   src/active/cat.cpp src/active/hpp/cat.hpp\nfn      update(float) -> void\n");
  auto again = parse_block_text(text, "c.ksl");
  REQUIRE(again);
  CHECK(write_block(*again) == text);
}
