#include <doctest/doctest.h>
#include <fmt/format.h>

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
  for (int i = 0; i < 12; ++i) {
    REQUIRE(store.append(t.id, level(2, "worker", {{"at", "stream.ring"}, {"attempt", std::to_string(i + 1)}}),
                         std::vector<std::string>{fmt::format("observed value {} at step {} with a fairly long explanation of the probe", i, i)}));
  }
  auto loaded = store.load(t.id);
  REQUIRE(loaded);
  auto v = path_view(*loaded, "stream.ring", 600);
  CHECK(estimate_tokens(v) <= 600);
  CHECK(contains(v, "facts in ticket file"));
  CHECK(contains(v, "fact    contract stream.ring/no_drop FAIL"));
  CHECK(contains(v, "observed value 11"));  // the latest new fact is kept
  CHECK_FALSE(contains(v, "observed value 0 "));
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
