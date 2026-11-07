#include <doctest/doctest.h>

#include "kk/core/text.hpp"
#include "kk/index/index.hpp"
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
      r.code_hash = index::block_code_hash(files, b.id);
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
