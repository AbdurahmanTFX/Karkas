#include <doctest/doctest.h>

#include <algorithm>

#include "kk/core/text.hpp"
#include "kk/index/analyzer.hpp"
#include "kk/index/index.hpp"
#include "kk/index/state.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

// wavkit with recorded state: every leaf passing except stream.ring (1 contract test
// failing), plus a probe run on the pump -> ring link (as in 4.7).
struct ViewFixture {
  test::FixtureCopy p{"wavkit"};
  std::unique_ptr<index::Index> idx;
  ViewFixture() {
    test::write(p / ".karkas/probes/run1.jsonl",
                "{\"link\":\"stream.pump->stream.ring\",\"metric\":\"fill\",\"v\":0}\n"
                "{\"link\":\"stream.pump->stream.ring\",\"metric\":\"fill\",\"v\":8192}\n"
                "{\"link\":\"stream.pump->stream.ring\",\"metric\":\"fill\",\"v\":3238}\n");
    auto schema = core::load_schema(p.path());
    REQUIRE(schema);
    auto files = index::collect_code_files(*schema);
    std::vector<index::TestResult> results;
    for (const auto& b : schema->blocks) {
      if (!b.has_code()) continue;
      index::TestResult r;
      r.block = b.id;
      r.code_hash = index::block_test_hash(*schema, files, b.id);
      r.contract = {static_cast<int>(b.invs.size()), static_cast<int>(b.invs.size())};
      if (b.id == "stream.ring") {
        r.contract = {2, 3};
        r.failures = {"contract: never loses or duplicates frames, including at wraparound"};
      }
      if (r.contract.total == 0) r.unit = {1, 1};
      results.push_back(r);
    }
    REQUIRE(index::StateStore(p.path()).record_tests(results));
    auto i = index::Index::open(p.path());
    REQUIRE(i);
    idx = std::move(*i);
    REQUIRE(idx->update());
  }
};

void check_golden(const std::string& name, const std::string& text) {
  CHECK_MESSAGE(test::matches_golden(name, text), "golden mismatch for " << name << ":\n" << text);
}

}  // namespace

TEST_CASE("views: L0 overview matches golden and stays within 800 tokens") {
  ViewFixture f;
  auto v = f.idx->overview();
  check_golden("views/wavkit_L0.txt", v);
  CHECK(core::estimate_tokens(v) <= 800);
  CHECK(core::contains(v, "stream  group  move frames from reader thread to processing (2)"));
  CHECK(core::contains(v, "FAIL 1"));
  CHECK(core::contains(v, "-data->"));
  CHECK_FALSE(core::contains(v, "shared"));  // use links and shared blocks hidden (3.7)
}

TEST_CASE("views: L0 with use links shows the shared group") {
  ViewFixture f;
  auto v = f.idx->overview({true});
  CHECK(core::contains(v, "shared"));
  CHECK(core::contains(v, "-use->"));
}

TEST_CASE("views: L1 of groups match golden and stay within 600 tokens") {
  ViewFixture f;
  for (const char* id : {"stream", "dsp", "io"}) {
    auto v = f.idx->view(id, 1);
    REQUIRE(v);
    check_golden(std::string("views/wavkit_L1_") + id + ".txt", *v);
    CHECK(core::estimate_tokens(*v) <= 600);
  }
  auto s = f.idx->view("stream", 1);
  CHECK(core::contains(*s, "--- external"));
  CHECK(core::contains(*s, "--- probes"));
  CHECK(core::contains(*s, "stream.pump->stream.ring fill min=0 avg=3810 max=8192 anomaly=min0"));
}

TEST_CASE("views: L2 of leaves match golden and stay within 1200 tokens") {
  ViewFixture f;
  for (const char* id : {"stream.ring", "dsp.chain", "io.reader"}) {
    auto v = f.idx->view(id, 2);
    REQUIRE(v);
    check_golden(std::string("views/wavkit_L2_") + id + ".txt", *v);
    CHECK(core::estimate_tokens(*v) <= 1200);
  }
  auto ring = f.idx->view("stream.ring", 2);
  CHECK(core::contains(*ring, "--- neighbors"));
  CHECK(core::contains(*ring, "stream.pump"));
  CHECK(core::contains(*ring, "dsp.chain"));
  CHECK(core::contains(*ring, "status=failing"));
}

TEST_CASE("views: status, children and group of a leaf") {
  ViewFixture f;
  auto st = f.idx->status();
  REQUIRE(st);
  CHECK(core::contains(*st, "stream.ring  FAIL 1"));
  auto one = f.idx->status("stream.ring");
  REQUIRE(one);
  CHECK(core::contains(*one, "tests stream.ring contract 2/3"));
  CHECK(core::contains(*one, "fail contract: never loses"));
  auto kids = f.idx->children("dsp");
  REQUIRE(kids);
  CHECK(kids->starts_with("dsp.chain"));
}

TEST_CASE("views: locate ranks the blocks of a symptom by ids, purposes and file names") {
  ViewFixture f;
  auto text = f.idx->locate("crackle after a few seconds: the ring buffer wraps around badly");
  INFO(text);
  CHECK(text.starts_with("@locate "));
  auto first = text.find("1. ");
  REQUIRE(first != std::string::npos);
  CHECK(text.substr(first, 16) == "1. stream.ring  ");
  CHECK(core::contains(text, "files "));
  CHECK(core::contains(f.idx->locate("the the and"), "(no words to look for)"));
  CHECK(core::contains(f.idx->locate("zzzz qqqq"), "(no block matches"));
  CHECK(std::count(text.begin(), text.end(), '\n') <= 1 + 5 * 4);  // compact: at most 5 blocks
}

TEST_CASE("views: status marks stale results and old failures, and tells the state of the api (ADR 0020)") {
  ViewFixture f;
  // A linked block's api changes after the recorded run: the ring's results go stale.
  auto api = f.p / "blocks/stream/pump/api/pump.hpp";
  test::write(api, test::read(api) + "\n// changed after the test run\n");
  REQUIRE(f.idx->update());
  auto one = f.idx->status("stream.ring");
  REQUIRE(one);
  CHECK(core::contains(*one, "tests stream.ring stale (the code or a linked api/ changed since this run"));
  CHECK(core::contains(*one, "old fail contract: never loses"));
  CHECK_FALSE(core::contains(*one, "\nfail contract"));
  // Nothing frozen in this fixture: no api line; after a freeze of another block the ring's
  // api reads as unfrozen, its own freeze as frozen.
  CHECK_FALSE(core::contains(*one, "api frozen"));
  REQUIRE(index::StateStore(f.p.path()).freeze_api("stream.pump", "x"));
  REQUIRE(f.idx->update());
  CHECK(core::contains(*f.idx->status("stream.ring"), "api unfrozen by an approved change, already in line with its card"));
  CHECK(core::contains(*f.idx->status("stream.pump"), "api frozen (declarations are the contract; its worker may tune values of constants and add new constants"));
  // A card that names a fn the api does not declare yet (D04): the api is behind its card.
  struct D04Analyzer final : index::ICodeAnalyzer {
    index::FileFacts analyze(const index::CodeFile&, std::string_view) override { return {}; }
    index::CheckOutput check(const core::Schema&, const index::IndexFacts&) override {
      index::CheckOutput o;
      o.issues.push_back(core::Issue{"D04", 'E', "stream.ring", "fn reset() of the card is not declared in api/", {}});
      return o;
    }
  } d04;
  REQUIRE(f.idx->update({false, &d04}));
  CHECK(core::contains(*f.idx->status("stream.ring"), "api unfrozen by an approved change and behind its card"));
  // Other parameter counts (D04 W): api/ may be the right side (a defaulted parameter), so it is
  // not called behind (T-0013 of flappy-karkas).
  struct D04Params final : index::ICodeAnalyzer {
    index::FileFacts analyze(const index::CodeFile&, std::string_view) override { return {}; }
    index::CheckOutput check(const core::Schema&, const index::IndexFacts&) override {
      index::CheckOutput o;
      o.issues.push_back(core::Issue{"D04", 'W', "stream.ring", "fn push takes 1 parameter(s) in the card but 2 in api/", {}});
      return o;
    }
  } params;
  REQUIRE(f.idx->update({true, &params}));  // full: nothing changed on disk, the analyzer did
  auto st = *f.idx->status("stream.ring");
  CHECK_FALSE(core::contains(st, "behind its card"));
  CHECK(core::contains(st, "api/ and its card differ (fn push takes 1 parameter(s) in the card but 2 in api/)"));
}
