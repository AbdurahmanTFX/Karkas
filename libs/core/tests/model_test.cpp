#include <doctest/doctest.h>

#include "kk/core/model.hpp"

using namespace kk::core;

namespace {
Block block(std::string_view text) {
  auto b = parse_block_text(text, "b.ksl");
  if (!b) FAIL(b.error().str());
  return *b;
}
kk::Error block_error(std::string_view text) {
  auto b = parse_block_text(text, "blocks/x/block.ksl");
  REQUIRE_FALSE(b);
  return b.error();
}
}  // namespace

TEST_CASE("model: leaf card with all directives") {
  auto b = block(R"(@block stream.ring
purpose lock-free single-producer single-consumer ring of audio frames
kind    leaf
thread  any
est     M
in      push Frame[] from=stream.pump:frames link=data sync=lock_free rate=48k/s
out     pop Frame[]
fn      capacity() -> size_t
use     shared.types
inv     push and pop never block
hist    T-0042 drop at wraparound -> mask index with cap-1
)");
  CHECK(b.id == "stream.ring");
  CHECK(b.kind == BlockKind::Leaf);
  CHECK(b.thread.value == "any");
  CHECK(b.est.value == "M");
  REQUIRE(b.ports.size() == 3);
  const Port* push = b.port("push");
  REQUIRE(push != nullptr);
  CHECK(push->dir == PortDir::In);
  CHECK(push->type == "Frame[]");
  REQUIRE(push->from);
  CHECK(push->from->str() == "stream.pump:frames");
  CHECK(push->link == LinkKind::Data);
  CHECK(push->link_written);
  CHECK(push->sync == "lock_free");
  CHECK(push->rate == "48k/s");
  CHECK(b.port("capacity")->signature() == "capacity() -> size_t");
  REQUIRE(b.hist.size() == 1);
  CHECK(b.hist[0].ticket == "T-0042");
  CHECK(b.hist[0].text == "drop at wraparound -> mask index with cap-1");
}

TEST_CASE("model: in port defaults to link=data and accepts event") {
  auto b = block("@block a\nin x Ev from=b:y\nin z Ev from=b:w link=event via=queue\n");
  CHECK(b.ports[0].link == LinkKind::Data);
  CHECK_FALSE(b.ports[0].link_written);
  CHECK(b.ports[1].link == LinkKind::Event);
  CHECK(b.ports[1].via == "queue");
}

TEST_CASE("model: fn signatures") {
  auto b = block("@block a\nfn open(path, AudioFormat) -> Result<Unit>\nfn start()\nfn mix(std::map<K, V> m)\n");
  REQUIRE(b.ports.size() == 3);
  CHECK(b.ports[0].params == std::vector<std::string>{"path", "AudioFormat"});
  CHECK(b.ports[0].ret == "Result<Unit>");
  CHECK(b.ports[1].params.empty());
  CHECK(b.ports[1].ret.empty());
  CHECK(b.ports[1].signature() == "start()");
  CHECK(b.ports[2].params == std::vector<std::string>{"std::map<K, V> m"});
}

TEST_CASE("model: call, use and state") {
  auto b = block("@block a\ncall dsp.gain:process via=direct\nuse shared.types\nstate counter int with=b.c sync=mutex\n");
  REQUIRE(b.calls.size() == 1);
  CHECK(b.calls[0].target.block == "dsp.gain");
  CHECK(b.calls[0].target.port == "process");
  CHECK(b.calls[0].via == "direct");
  CHECK(b.uses[0].target == "shared.types");
  REQUIRE(b.states.size() == 1);
  CHECK(b.states[0].with == "b.c");
  CHECK(b.states[0].sync == "mutex");
}

TEST_CASE("model: unknown directive is S03 with file:line:col") {
  auto e = block_error("@block x\npurpose p\n  colour red\n");
  CHECK(e.str() == "blocks/x/block.ksl:3:3: error S03: unknown directive 'colour' in @block");
}

TEST_CASE("model: repeated single directive is S05") {
  auto e = block_error("@block x\npurpose a\npurpose b\n");
  CHECK(e.code == "S05");
  CHECK(e.loc.line == 3);
  CHECK(e.str() == "blocks/x/block.ksl:3:1: error S05: 'purpose' may appear at most 1 time in @block");
}

TEST_CASE("model: more than three notes is S05") {
  auto e = block_error("@block x\nnote a\nnote b\nnote c\nnote d\n");
  CHECK(e.code == "S05");
  CHECK(e.loc.line == 5);
}

TEST_CASE("model: invalid kind value is S04 at the value column") {
  auto e = block_error("@block x\nkind    module\n");
  CHECK(e.str() == "blocks/x/block.ksl:2:9: error S04: 'kind' must be one of group|leaf|entry|shared, got 'module'");
}

TEST_CASE("model: invalid est is S04") {
  auto e = block_error("@block x\nest XL\n");
  CHECK(e.code == "S04");
  CHECK(e.loc.col == 5);
}

TEST_CASE("model: bad from= reference is S04") {
  auto e = block_error("@block x\nin      a T from=nowhere\n");
  CHECK(e.str() == "blocks/x/block.ksl:2:13: error S04: from= expects <block>:<port>, got 'nowhere'");
}

TEST_CASE("model: link= accepts only data or event") {
  auto e = block_error("@block x\nin a T from=b:c link=call\n");
  CHECK(e.code == "S04");
  CHECK(e.loc.col == 17);
}

TEST_CASE("model: unknown attribute is S04") {
  auto e = block_error("@block x\nout a T speed=fast\n");
  CHECK(e.str() == "blocks/x/block.ksl:2:9: error S04: unknown attribute 'speed' for 'out'");
}

TEST_CASE("model: port needs a name and a type") {
  auto e = block_error("@block x\nin onlyname\n");
  CHECK(e.code == "S04");
  auto e2 = block_error("@block x\nin Bad T\n");
  CHECK(e2.code == "S04");
}

TEST_CASE("model: state needs with=") {
  auto e = block_error("@block x\nstate n int sync=mutex\n");
  CHECK(e.code == "S04");
  CHECK(e.message == "state needs with=<block>");
}

TEST_CASE("model: hist needs a ticket id") {
  auto e = block_error("@block x\nhist fixed something\n");
  CHECK(e.code == "S04");
  CHECK(e.loc.line == 2);
}

TEST_CASE("model: fn without parameter list is S04") {
  auto e = block_error("@block x\nfn capacity -> size_t\n");
  CHECK(e.code == "S04");
  CHECK(e.loc.col == 13);
}

TEST_CASE("model: sections are not allowed in a card") {
  auto e = block_error("@block x\npurpose p\n--- trail\n");
  CHECK(e.code == "S07");
  CHECK(e.loc.line == 3);
}

TEST_CASE("model: project with dirs and rules") {
  auto p = parse_project_text(
      "@project karkas\nidea    schema IDE\nlang    cpp\ndirs    blocks=. api=include impl=src\nrule    r1\nrule    r2\n",
      "project.ksl");
  REQUIRE(p);
  CHECK(p->name == "karkas");
  CHECK(p->dirs.blocks == ".");
  CHECK(p->dirs.api == "include");
  CHECK(p->dirs.impl == "src");
  CHECK(p->dirs.tests == "tests");
  CHECK(p->rules.size() == 2);
  CHECK(write_project(*p) == "@project karkas\nidea    schema IDE\nlang    cpp\ndirs    blocks=. api=include impl=src\nrule    r1\nrule    r2\n");
}

TEST_CASE("model: ticket with trail and result") {
  auto t = parse_ticket_text(R"(@ticket T-0042
kind    bug
origin  tester
status  open
symptom crackle in output after ~10 s of playback, worse with echo enabled
fact    contract stream.ring/no_drop FAIL: dropped=37 of 480000 frames
--- trail
L0 orchestrator at=root   suspect=stream,dsp pick=stream difficulty=M why="frames dropped before dsp; ring runs empty"
fact    probe ok
answer at=stream.pump "rate is 48k/s"
L2 worker       at=stream.ring model=cheap attempt=1 outcome=fixed
--- result
status  fixed
cost    in=21480 out=2930 cache_read=15210
)",
                             "T-0042.ksl");
  REQUIRE(t);
  CHECK(t->kind_enum() == TicketKind::Bug);
  CHECK(t->status_enum() == TicketStatus::Open);
  REQUIRE(t->facts.size() == 1);
  REQUIRE(t->trail.size() == 4);
  CHECK(t->trail[0].type == TrailEntry::Type::Level);
  CHECK(t->trail[0].level == 0);
  CHECK(t->trail[0].role == "orchestrator");
  CHECK(t->trail[0].list("suspect") == std::vector<std::string>{"stream", "dsp"});
  CHECK(t->trail[0].get("why") == "frames dropped before dsp; ring runs empty");
  CHECK(t->trail[1].type == TrailEntry::Type::Fact);
  CHECK(t->trail[2].type == TrailEntry::Type::Answer);
  CHECK(t->trail[2].get("at") == "stream.pump");
  CHECK(t->trail[2].text == "rate is 48k/s");
  CHECK(t->trail[3].get("outcome") == "fixed");
  CHECK(t->result_value("cost") == "in=21480 out=2930 cache_read=15210");
}

TEST_CASE("model: ticket rejects unknown section") {
  auto t = parse_ticket_text("@ticket T-0001\nkind bug\n--- notes\n", "t.ksl");
  REQUIRE_FALSE(t);
  CHECK(t.error().code == "S07");
}

TEST_CASE("model: report with a proposal diff") {
  auto r = parse_report_text(R"(@report T-0042 from=stream.ring role=worker
status  needs_contract_change
cause   push needs a timeout
files   impl/ring.cpp, tests/unit/ring_test.cpp
api     change_requested
--- diff
~ @block stream.ring
+ fn try_push(Frame[], int) -> bool
)",
                             "r.ksl");
  REQUIRE(r);
  CHECK(r->from == "stream.ring");
  CHECK(r->role == "worker");
  CHECK(r->status == "needs_contract_change");
  CHECK(r->files == "impl/ring.cpp, tests/unit/ring_test.cpp");
  REQUIRE(r->proposal.size() == 2);
  CHECK(r->proposal[1] == "+ fn try_push(Frame[], int) -> bool");
  CHECK(parse_report_text(write_report(*r), "r2.ksl").has_value());
}

TEST_CASE("model: report status must be known") {
  auto r = parse_report_text("@report T-0001\nstatus maybe\n", "r.ksl");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "S04");
}

TEST_CASE("model: change with diff") {
  auto c = parse_change_text(R"(@change C-0003
from    orchestrator T-0051
reason  reverb requested; no existing block provides it
status  pending
--- diff
+ @block dsp.reverb
+ purpose apply Schroeder reverb to frames
~ @block dsp.chain
+ call    dsp.reverb:process
)",
                             "C-0003.ksl");
  REQUIRE(c);
  CHECK(c->from.value == "orchestrator T-0051");
  CHECK(c->status.value == "pending");
  REQUIRE(c->diff.size() == 4);
  CHECK(c->diff[2].op == '~');
  CHECK(c->diff[3].text == "call    dsp.reverb:process");
}

TEST_CASE("model: hist keeps the last 10 entries and folds older ones") {
  Block b;
  b.id = "a";
  for (int i = 1; i <= 13; ++i) b.add_hist("T-" + std::to_string(1000 + i), "fix " + std::to_string(i));
  REQUIRE(b.hist.size() == 11);
  CHECK(b.hist[0].ticket == "summary");
  CHECK(b.hist[0].text == "3 earlier entries T-1001..T-1003");
  CHECK(b.hist[1].ticket == "T-1004");
  CHECK(b.hist[10].ticket == "T-1013");
  b.add_hist("T-1014", "fix 14");
  CHECK(b.hist[0].text == "4 earlier entries T-1001..T-1004");
  CHECK(b.hist.size() == 11);
  auto reparsed = parse_block_text(write_block(b), "a.ksl");
  REQUIRE(reparsed);
  CHECK(reparsed->hist.size() == 11);
}

TEST_CASE("model: directive table covers every card directive in canonical order") {
  std::vector<std::string> names;
  for (const auto& d : directive_table()) {
    if (d.record == "block") names.emplace_back(d.name);
  }
  CHECK(names == std::vector<std::string>{"purpose", "kind", "thread", "est", "files", "in", "out", "fn", "call", "use",
                                          "state", "inv", "note", "hist"});
}
