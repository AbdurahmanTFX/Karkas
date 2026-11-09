// Panels: cleared rows stay hidden until they change; nothing but cleared.json is written.
#include <doctest/doctest.h>

#include "cleared.hpp"
#include "kk/core/paths.hpp"
#include "kk/test/support.hpp"

using namespace kk;
using kk::gui::Cleared;

TEST_CASE("cleared: a row is hidden until it has newer activity (ADR 0019)") {
  Cleared c;
  index::TicketRow t{"T-0001", "bug", "closed", "human", "", "crackle", 100, 100};
  index::TicketRow u{"T-0002", "feature", "open", "human", "", "volume", 150, 0};
  CHECK_FALSE(c.hides(t));
  c.clear_ticket(t);
  CHECK(c.hides(t));
  CHECK_FALSE(c.hides(u));
  t.created_at = 200;  // the ticket file changed (ticket run): it shows again
  CHECK_FALSE(c.hides(t));
  c.clear_all_tickets(160);
  CHECK(c.hides(u));
  CHECK_FALSE(c.hides(t));
  c.restore_tickets();
  CHECK_FALSE(c.hides(u));

  index::RunRow r1;
  r1.journal = "a.jsonl";
  r1.ts = 10;
  index::RunRow r2;
  r2.journal = "b.jsonl";
  r2.ts = 20;
  c.clear_run(r1);
  CHECK(c.hides(r1));
  CHECK_FALSE(c.hides(r2));
  c.clear_all_runs(15);
  CHECK(c.hides(r1));
  CHECK_FALSE(c.hides(r2));
  c.restore_runs();
  CHECK_FALSE(c.hides(r1));

  index::TestRow tr;
  tr.block = "stream.ring";
  tr.ts = 50;
  c.clear_test(tr);
  CHECK(c.hides(tr));
  tr.ts = 60;  // tests ran again
  CHECK_FALSE(c.hides(tr));
  c.restore_tests();
  CHECK(c.empty());
}

TEST_CASE("cleared: saved per project, only .karkas/cleared.json is touched") {
  test::FixtureCopy p("wavkit");
  auto log = p.path() / ".karkas" / "karkas.log";
  REQUIRE(core::write_file(log, "a log line\n"));
  Cleared c;
  index::TicketRow t{"T-0003", "bug", "closed", "human", "", "x", 42, 42};
  c.clear_ticket(t);
  c.clear_all_runs(1000);
  REQUIRE(gui::save_cleared(p.path(), c));
  auto l = gui::load_cleared(p.path());
  CHECK(l.hides(t));
  CHECK(l.runs_before == 1000);
  CHECK(test::read(log) == "a log line\n");
  // Restoring everything removes the file; a broken file means nothing is hidden.
  REQUIRE(gui::save_cleared(p.path(), Cleared{}));
  CHECK_FALSE(std::filesystem::exists(gui::cleared_path(p.path())));
  REQUIRE(core::write_file(gui::cleared_path(p.path()), "{not json"));
  CHECK(gui::load_cleared(p.path()).empty());
}
