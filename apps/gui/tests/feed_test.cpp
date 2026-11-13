#include <doctest/doctest.h>

#include <fstream>

#include "feed.hpp"
#include "kk/test/support.hpp"

using namespace kk;

TEST_CASE("feed: a journal becomes readable steps, and only what is appended is read again") {
  test::TempDir d{"feed"};
  auto path = d.path() / "20261112-080000-000-worker-rules.round.jsonl";
  {
    std::ofstream f(path);
    f << R"({"type":"run_start","ticket":"T-0007","role":"worker","block":"rules.round","model":"deepseek-v4-pro","tier":"deepseek","attempt":1,"ts":100})" "\n";
    f << R"({"type":"step","kind":"start","n":1,"ts":100,"data":{"role":"worker","task":"wire the slow pickup"}})" "\n";
    f << R"({"type":"step","kind":"response","n":2,"ts":101,"data":{"usage":{"output":50,"reasoning":40},"content":[{"type":"reasoning","text":"round must call start_slow"},{"type":"tool_use","name":"read_file","input":{"path":"impl/round.cpp"}}]}})" "\n";
    f << R"({"type":"step","kind":"tool","n":3,"ts":101,"data":{"name":"read_file","is_error":false,"result":"#include \"round.hpp\""}})" "\n";
    f << R"({"type":"step","kind":"response","n":4,"ts":102,"data":{"content":[{"type":"tool_use","name":"edit_file","input":{"path":"impl/round.cpp","old":"a","new":"b"}}]}})";  // no newline yet
  }
  gui::JournalFeed feed(d.path());
  const auto& r = feed.load(path.filename().string());
  CHECK(r.ticket == "T-0007");
  CHECK(r.role == "worker");
  CHECK(r.block == "rules.round");
  CHECK(r.start == 100);
  CHECK(r.end == 0);
  REQUIRE(r.steps.size() == 4);  // start, think, call, result; the unfinished line waits
  CHECK(r.steps[0].kind == gui::FeedStep::Kind::Start);
  CHECK(r.steps[0].text == "wire the slow pickup");
  CHECK(r.steps[1].kind == gui::FeedStep::Kind::Think);
  CHECK(r.steps[1].reasoning_tokens == 40);
  CHECK(r.steps[2].kind == gui::FeedStep::Kind::Call);
  CHECK(r.steps[2].title == "read_file");
  CHECK(r.steps[2].text == "impl/round.cpp");
  CHECK(r.steps[3].kind == gui::FeedStep::Kind::Result);
  {
    std::ofstream f(path, std::ios::app);
    f << "\n" R"({"type":"step","kind":"end","n":5,"ts":103,"data":{"outcome":"terminal","terminal":"report"}})" "\n";
    f << R"({"type":"run_end","ts":103,"outcome":"done","cost_usd":0.0417,"input_tokens":10,"output_tokens":20,"cache_read_tokens":30})" "\n";
  }
  const auto& r2 = feed.load(path.filename().string());
  REQUIRE(r2.steps.size() == 6);
  CHECK(r2.steps[4].kind == gui::FeedStep::Kind::Call);
  CHECK(r2.steps[4].text == "impl/round.cpp\n- a\n+ b");
  CHECK(r2.steps[5].kind == gui::FeedStep::Kind::End);
  CHECK(r2.outcome == "done");
  CHECK(r2.end == 103);
  CHECK(r2.cost == doctest::Approx(0.0417));
}
