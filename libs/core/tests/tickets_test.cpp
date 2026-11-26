#include <doctest/doctest.h>
#include <fmt/format.h>

#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/core/tickets.hpp"
#include "kk/test/support.hpp"

using namespace kk::core;

namespace {

TrailEntry level(int n, std::string role, std::vector<Attr> attrs) {
  TrailEntry e;
  e.type = TrailEntry::Type::Level;
  e.level = n;
  e.role = std::move(role);
  e.attrs = std::move(attrs);
  return e;
}

Ticket spec_ticket(TicketStore& store) {
  auto t = store.create(TicketKind::Bug, "tester", "crackle in output after ~10 s of playback, worse with echo enabled",
                        {"contract stream.ring/no_drop FAIL: dropped=37 of 480000 frames",
                         "probe stream.pump->stream.ring fill_min=0 at t=9.8s"});
  REQUIRE(t);
  REQUIRE(store.append(t->id, level(0, "orchestrator",
                                    {{"at", "root"}, {"suspect", "stream,dsp"}, {"pick", "stream"}, {"difficulty", "M"},
                                     {"why", "frames dropped before dsp; ring runs empty"}})));
  REQUIRE(store.append(t->id, level(1, "navigator",
                                    {{"at", "stream"}, {"suspect", "stream.ring,stream.pump"}, {"ruled_out", "stream.pump"},
                                     {"pick", "stream.ring"}, {"why", "pump rate ok 48k/s; drops at wraparound"}})));
  auto loaded = store.load(t->id);
  REQUIRE(loaded);
  return *loaded;
}

}  // namespace

TEST_CASE("tickets: create assigns sequential ids and writes canonical KSL") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto a = store.create(TicketKind::Bug, "human", "sound crackles", {"fact one"});
  auto b = store.create(TicketKind::Feature, "human", "add reverb", {});
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->id == "T-0001");
  CHECK(b->id == "T-0002");
  CHECK(kk::test::read(store.file("T-0001")) ==
        "@ticket T-0001\nkind    bug\norigin  human\nstatus  open\nsymptom sound crackles\nfact    fact one\n");
  CHECK(kk::test::read(store.file("T-0002")).find("task    add reverb") != std::string::npos);
}

TEST_CASE("tickets: path views of appendix D") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto t = spec_ticket(store);
  const std::string base =
      "kind    bug\n"
      "symptom crackle in output after ~10 s of playback, worse with echo enabled\n"
      "fact    contract stream.ring/no_drop FAIL: dropped=37 of 480000 frames\n"
      "fact    probe stream.pump->stream.ring fill_min=0 at t=9.8s\n";
  auto before_l1 = t;  // the navigator's view is built before it writes its own entry
  before_l1.trail.pop_back();
  CHECK(path_view(before_l1, "stream") ==
        "@ticket " + t.id + " view=path at=stream\n" + base +
            "--- trail\n"
            "L0 orchestrator at=root pick=stream difficulty=M why=\"frames dropped before dsp; ring runs empty\"\n");
  CHECK(path_view(t, "stream.ring") ==
        "@ticket " + t.id + " view=path at=stream.ring\n" + base +
            "--- trail\n"
            "L0 orchestrator at=root pick=stream difficulty=M why=\"frames dropped before dsp; ring runs empty\"\n"
            "L1 navigator    at=stream ruled_out=stream.pump pick=stream.ring why=\"pump rate ok 48k/s; drops at wraparound\"\n");
  // A view for another branch does not see the stream navigator's entry.
  auto other = path_view(t, "dsp.chain");
  CHECK_FALSE(contains(other, "L1 navigator"));
  CHECK(estimate_tokens(path_view(t, "stream.ring")) <= 600);
}

TEST_CASE("tickets: path view keeps only the latest decision of a level") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto t = spec_ticket(store);
  REQUIRE(store.append(t.id, level(2, "worker", {{"at", "stream.ring"}, {"model", "cheap"}, {"attempt", "1"}, {"outcome", "not_here"}})));
  REQUIRE(store.append(t.id, level(1, "navigator", {{"at", "stream"}, {"ruled_out", "stream.ring"}, {"pick", "stream.pump"}, {"why", "ring ok"}})));
  auto loaded = store.load(t.id);
  REQUIRE(loaded);
  auto v = path_view(*loaded, "stream.pump");
  CHECK(contains(v, "pick=stream.pump"));
  CHECK_FALSE(contains(v, "pick=stream.ring"));
  CHECK_FALSE(contains(v, "outcome=not_here"));
}

TEST_CASE("tickets: new facts fold under the budget, original facts stay") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto t = spec_ticket(store);
  // Repeated attempts fold into one trail line (repeats=N), so it takes many observations to
  // reach the budget.
  for (int i = 0; i < 30; ++i) {
    REQUIRE(store.append(t.id, level(2, "worker", {{"at", "stream.ring"}, {"attempt", std::to_string(i + 1)}}),
                         std::vector<std::string>{fmt::format("observed value {} at step {} with a fairly long explanation of the probe", i, i)}));
  }
  auto loaded = store.load(t.id);
  REQUIRE(loaded);
  auto v = path_view(*loaded, "stream.ring", 600);
  CHECK(estimate_tokens(v) <= 600);
  CHECK(contains(v, "facts in ticket file"));
  CHECK(contains(v, "fact    contract stream.ring/no_drop FAIL"));
  CHECK(contains(v, "observed value 29"));  // the latest new fact is kept
  CHECK_FALSE(contains(v, "observed value 0 "));
  CHECK(contains(v, "repeats=30"));
}

TEST_CASE("tickets: the root view tells the orchestrator how its earlier descents ended") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto t = store.create(TicketKind::Feature, "human", "add a slow pickup", {});
  REQUIRE(t);
  // Three runs: each dispatched to the same leaf, which asked for a contract change.
  for (int run = 0; run < 3; ++run) {
    REQUIRE(store.append(t->id, level(0, "orchestrator",
                                      {{"at", "root"}, {"suspect", "dsp.echo,stream.ring"}, {"pick", "dsp.echo"},
                                       {"difficulty", "S"}, {"why", fmt::format("try {}", run)}}),
                         std::vector<std::string>{"status dsp.echo drift D04"}));
    REQUIRE(store.append(t->id, level(2, "worker", {{"at", "dsp.echo"}, {"model", "cheap"}, {"attempt", "1"},
                                                    {"outcome", "needs_contract_change"}})));
    REQUIRE(store.add_result(t->id, {{"status", "needs_contract_change", {}, 0},
                                     {"cause", fmt::format("api/echo.hpp lacks set_mix (run {})", run), {}, 0},
                                     {"cost", "in=1 out=1", {}, 0}}));
  }
  auto loaded = store.load(t->id);
  REQUIRE(loaded);
  auto v = path_view(*loaded, "root");
  // The own decision is shown once, with its suspects and the number of repeats.
  CHECK(contains(v, "suspect=dsp.echo,stream.ring pick=dsp.echo"));
  CHECK(contains(v, "repeats=3"));
  CHECK(contains(v, "why=\"try 2\""));
  CHECK_FALSE(contains(v, "why=\"try 0\""));
  // A repeated observation appears once.
  CHECK(v.find("status dsp.echo drift D04") == v.rfind("status dsp.echo drift D04"));
  // How the descents ended, folded.
  CHECK(contains(v, "--- earlier descents from here (oldest first)"));
  CHECK(contains(v, "tried   dsp.echo -> worker at dsp.echo: needs_contract_change (x3)"));
  // The latest result, without its cost line.
  CHECK(contains(v, "--- last result\nstatus  needs_contract_change\ncause   api/echo.hpp lacks set_mix (run 2)\n"));
  CHECK_FALSE(contains(v, "in=1 out=1"));
  // The leaf where the descents ended sees the latest result too; another branch does not.
  auto leaf = path_view(*loaded, "dsp.echo");
  CHECK(contains(leaf, "cause   api/echo.hpp lacks set_mix (run 2)"));
  CHECK(contains(leaf, "outcome=needs_contract_change repeats=3"));
  CHECK_FALSE(contains(leaf, "--- earlier descents"));
  CHECK_FALSE(contains(path_view(*loaded, "stream.ring"), "lacks set_mix"));
}

TEST_CASE("tickets: two proposed changes stay two lines of the path view, not one with repeats=2") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto t = store.create(TicketKind::Feature, "human", "breathing columns", {});
  REQUIRE(t);
  for (const char* c : {"C-0006", "C-0007"}) {
    REQUIRE(store.append(t->id, level(0, "orchestrator", {{"at", "root"}, {"outcome", "change"}, {"why", std::string("proposed ") + c}})));
  }
  auto loaded = store.load(t->id);
  REQUIRE(loaded);
  auto v = path_view(*loaded, "root");
  CHECK(contains(v, "why=\"proposed C-0006\""));
  CHECK(contains(v, "why=\"proposed C-0007\""));
  CHECK_FALSE(contains(v, "repeats=2"));
}

TEST_CASE("tickets: below the root the last result of a feature shows only the lines of that block") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto t = store.create(TicketKind::Feature, "human", "pace", {});
  REQUIRE(t);
  REQUIRE(store.append(t->id, level(0, "orchestrator", {{"at", "root"}, {"pick", "view.hud"}})));
  REQUIRE(store.append(t->id, level(2, "worker", {{"at", "view.hud"}, {"outcome", "needs_contract_change"}})));
  REQUIRE(store.add_result(t->id, {{"status", "done", {}, 0},
                                   {"fix", "config: constants added", {}, 0},
                                   {"files", "config: api/config.hpp", {}, 0},
                                   {"fix", "view.hud: shows the pace", {}, 0}}));
  auto loaded = store.load(t->id);
  REQUIRE(loaded);
  auto hud = path_view(*loaded, "view.hud");
  CHECK(contains(hud, "fix     view.hud: shows the pace"));
  CHECK_FALSE(contains(hud, "config: constants added"));
  CHECK(contains(path_view(*loaded, "root"), "config: constants added"));
}

TEST_CASE("tickets: append-only is enforced") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto t = spec_ticket(store);
  auto changed = t;
  changed.symptom.value = "something else";
  CHECK_FALSE(store.save(changed));
  auto dropped = t;
  dropped.trail.pop_back();
  CHECK_FALSE(store.save(dropped));
  auto rewritten = t;
  rewritten.facts[0].text = "edited";
  CHECK_FALSE(store.save(rewritten));
  REQUIRE(store.set_status(t.id, TicketStatus::Fixing));
  CHECK(store.load(t.id)->status.value == "fixing");
  REQUIRE(store.append_line(t.id, "answer  at=stream.pump \"rate is 48k/s\""));
  CHECK(store.load(t.id)->trail.back().text == "rate is 48k/s");
  CHECK_FALSE(store.append_line(t.id, "@block nope"));
  CHECK_FALSE(store.append(t.id, level(2, "worker", {{"at", "x"}}), {"a", "b", "c"}));
}

TEST_CASE("tickets: one agent per ticket (lock file)") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto t = spec_ticket(store);
  {
    auto l1 = store.lock(t.id, "worker stream.ring");
    REQUIRE(l1);
    auto l2 = store.lock(t.id, "navigator");
    CHECK_FALSE(l2);
  }
  auto l3 = store.lock(t.id, "again");
  CHECK(l3);
}

TEST_CASE("tickets: a human edits the whole ticket, the id stays, a locked ticket is refused (ADR 0033)") {
  kk::test::TempDir d{"tk"};
  TicketStore store(d.path());
  auto t = spec_ticket(store);
  auto text = kk::test::read(store.file(t.id));
  // The symptom and an original fact are changed, a fact added: refused for agents, allowed here.
  auto edited = replace_all(text, "worse with echo enabled", "only with echo enabled");
  edited = replace_all(edited, "fact    probe stream.pump->stream.ring fill_min=0 at t=9.8s\n",
                       "fact    probe stream.pump->stream.ring fill_min=0 at t=9.8s\nfact    happens on 44.1 kHz files too\n");
  Ticket changed = t;
  changed.symptom.value = "only with echo";
  CHECK_FALSE(store.save(changed));  // the agents' path stays append-only
  auto r = store.edit_by_human(t.id, edited);
  REQUIRE(r);
  CHECK(r->symptom.value.find("only with echo enabled") != std::string::npos);
  CHECK(r->facts.size() == 3);
  CHECK(r->trail.size() == 2);
  CHECK_FALSE(std::filesystem::exists(d.path() / ".karkas" / "tickets" / (t.id + ".lock")));
  // Broken text, another id, an unknown status: nothing written.
  auto before = kk::test::read(store.file(t.id));
  CHECK_FALSE(store.edit_by_human(t.id, "@ticket T-0001\nkind bug\nstatus  open\n  bad indent\n"));
  auto other = store.edit_by_human(t.id, replace_all(before, "@ticket T-0001", "@ticket T-0099"));
  REQUIRE_FALSE(other);
  CHECK(other.error().message.find("cannot change") != std::string::npos);
  CHECK_FALSE(store.edit_by_human(t.id, replace_all(before, "status  open", "status  sleeping")));
  CHECK(kk::test::read(store.file(t.id)) == before);
  // An agent holds the ticket: refused.
  auto lock = store.lock(t.id, "karkas run_ticket");
  REQUIRE(lock);
  auto busy = store.edit_by_human(t.id, before);
  REQUIRE_FALSE(busy);
  CHECK(busy.error().message.find("being worked on") != std::string::npos);
  CHECK_FALSE(store.edit_by_human("T-0042", before));
}

// --- known failures (ADR 0050) -------------------------------------------------------------

TEST_CASE("tickets: a path names the deepest block, not an id inside the path") {
  auto s = load_schema(kk::test::fixture("wavkit"));
  REQUIRE(s);
  CHECK(block_named_in("blocks/dsp/echo/impl/echo.cpp:24", *s) == "dsp.echo");
  CHECK(block_named_in("io.writer, blocks/io/writer/impl (WavWriter::write)", *s) == "io.writer");
  CHECK(block_named_in("Ring buffer corrupts frames in stream.ring at wraparound", *s) == "stream.ring");
  CHECK(block_named_in("contract stream.ring/no_drop FAIL", *s) == "stream.ring");
  CHECK(block_named_in("the output sounds wrong", *s).empty());
}

TEST_CASE("tickets: open bugs by block leave out closed tickets and other kinds") {
  kk::test::TempDir d{"tk"};
  auto s = load_schema(kk::test::fixture("wavkit"));
  REQUIRE(s);
  TicketStore store(d.path());
  REQUIRE(store.create(TicketKind::Bug, "agent", "bytes swapped", {"found at blocks/io/writer/impl/writer.cpp:40"}));
  REQUIRE(store.create(TicketKind::Bug, "agent", "ring drops frames", {"block stream.ring"}));
  REQUIRE(store.create(TicketKind::Feature, "human", "add reverb to dsp.chain", {}));
  auto closed = store.create(TicketKind::Bug, "agent", "mix not clipped", {"found at mix"});
  REQUIRE(closed);
  closed->status.value = "closed";
  REQUIRE(store.save(*closed));
  auto bugs = open_bugs(store, *s);
  REQUIRE(bugs);
  CHECK(bugs->size() == 2);
  REQUIRE(bugs->contains("io.writer"));
  CHECK(bugs->at("io.writer")[0].symptom.value == "bytes swapped");
  CHECK(bugs->contains("stream.ring"));
  CHECK_FALSE(bugs->contains("mix"));
}

TEST_CASE("tickets: known failures name their tickets and the rest stay unexplained") {
  kk::test::TempDir d{"tk"};
  auto s = load_schema(kk::test::fixture("wavkit"));
  REQUIRE(s);
  TicketStore store(d.path());
  REQUIRE(store.create(TicketKind::Bug, "agent", "bytes swapped", {"block io.writer"}));
  REQUIRE(store.create(TicketKind::Bug, "agent", "gain applied twice", {"block dsp.chain"}));
  auto bugs = open_bugs(store, *s);
  REQUIRE(bugs);
  auto text = known_failures_text({"io.writer", "dsp", "dsp.echo"}, *bugs, *s);
  CHECK(text.find("--- known: these failures already have an open ticket\n") != std::string::npos);
  CHECK(text.find("io.writer  T-0001  bytes swapped\n") != std::string::npos);
  CHECK(text.find("dsp  T-0002  gain applied twice\n") != std::string::npos);  // a group: the tickets under it
  CHECK(text.find("--- no ticket: investigate them only if they are your task\ndsp.echo\n") != std::string::npos);
  CHECK(text.find("If one of the known failures is the bug of your task, fix it; otherwise leave it.") != std::string::npos);
  CHECK(known_failures_text({}, *bugs, *s).empty());
}
