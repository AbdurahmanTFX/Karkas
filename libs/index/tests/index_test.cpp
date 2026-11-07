#include <doctest/doctest.h>

#include <filesystem>

#include "kk/core/text.hpp"
#include "kk/index/hash.hpp"
#include "kk/index/index.hpp"
#include "kk/test/support.hpp"

using namespace kk;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<index::Index> open_index(const fs::path& root) {
  auto idx = index::Index::open(root);
  REQUIRE_MESSAGE(idx, (idx ? "" : idx.error().str()));
  return std::move(*idx);
}

index::UpdateStats update(index::Index& idx, bool full = false) {
  auto r = idx.update({full, nullptr});
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  return *r;
}

}  // namespace

TEST_CASE("index: full build, no-change fast path and rebuild after deleting index.db") {
  test::FixtureCopy p("wavkit");
  auto idx = open_index(p.path());
  auto first = update(*idx);
  CHECK(first.changed);
  CHECK(first.blocks == 17);
  auto second = update(*idx);
  CHECK_FALSE(second.changed);
  auto snap = idx->snapshot();
  CHECK(snap.blocks.size() == 17);
  CHECK(snap.project == "wavkit");
  CHECK(snap.top_level == std::vector<std::string>{"app", "io", "stream", "dsp", "mix", "shared"});
  auto overview = idx->overview();
  idx.reset();
  fs::remove(p / ".karkas/index.db");
  auto idx2 = open_index(p.path());
  CHECK(update(*idx2).changed);
  CHECK(idx2->overview() == overview);
  CHECK(fs::exists(p / ".karkas/.gitignore"));
}

TEST_CASE("index: a card change is picked up by the incremental update") {
  test::FixtureCopy p("wavkit");
  auto idx = open_index(p.path());
  update(*idx);
  auto card = p / "blocks/dsp/gain/block.ksl";
  auto text = test::read(card);
  test::write(card, core::replace_all(text, "multiply samples by gain", "scale samples by a gain factor"));
  auto st = update(*idx);
  CHECK(st.changed);
  auto v = idx->view("dsp", 1);
  REQUIRE(v);
  CHECK(core::contains(*v, "scale samples by a gain factor"));
}

TEST_CASE("index: tables follow the DDL of 7.1") {
  test::FixtureCopy p("wavkit");
  auto idx = open_index(p.path());
  update(*idx);
  auto& db = idx->db();
  auto count = [&](const char* sql) {
    auto s = db.q(sql);
    return s.step() ? s.i64(0) : -1;
  };
  CHECK(count("SELECT COUNT(*) FROM blocks") == 17);
  CHECK(count("SELECT COUNT(*) FROM ports WHERE block='stream.ring'") == 4);
  CHECK(count("SELECT COUNT(*) FROM links WHERE kind='call'") == 3);
  CHECK(count("SELECT COUNT(*) FROM links WHERE kind='data'") == 5);
  CHECK(count("SELECT COUNT(*) FROM invariants WHERE block='stream.ring'") == 3);
  CHECK(count("SELECT COUNT(*) FROM history WHERE block='stream.ring'") == 1);
  // Subtree with a recursive CTE (7.3).
  CHECK(count("WITH RECURSIVE sub(id) AS (SELECT 'dsp' UNION ALL SELECT b.id FROM blocks b JOIN sub ON b.parent = sub.id) "
              "SELECT COUNT(*) FROM sub") == 5);
  auto s = db.q("SELECT thread_src, thread_dst FROM links WHERE src_block='stream.pump' AND dst_block='stream.ring'");
  REQUIRE(s.step());
  CHECK(s.text(0) == "reader");
  CHECK(s.text(1) == "any");
}

TEST_CASE("index: statuses from approvals, files and test results") {
  test::FixtureCopy p("wavkit");
  test::write(p / "blocks/dsp/reverb/block.ksl",
              "@block dsp.reverb\npurpose apply Schroeder reverb to frames\nkind    leaf\nthread  main\n"
              "fn      process(Frame[]) -> void\nuse     shared.types\ninv     zero mix leaves samples unchanged\n");
  test::write(p / "blocks/dsp/reverb/api/reverb.hpp", "#pragma once\nstruct IReverb { virtual ~IReverb() = default; };\n");
  auto idx = open_index(p.path());
  update(*idx);
  auto status = [&](std::string_view id) { return idx->snapshot().block(id)->status; };
  CHECK(status("dsp.reverb") == "design");

  auto schema = core::load_schema(p.path());
  REQUIRE(schema);
  auto files = index::collect_code_files(*schema);
  index::StateStore store(p.path());
  REQUIRE(store.freeze_api("dsp.reverb", index::block_api_hash(files, "dsp.reverb")));
  update(*idx);
  CHECK(status("dsp.reverb") == "api_frozen");

  test::write(p / "blocks/dsp/reverb/tests/contract/reverb_contract.cpp", "// tests\n");
  update(*idx);
  CHECK(status("dsp.reverb") == "tests_ready");

  test::write(p / "blocks/dsp/reverb/impl/reverb.cpp", "// karkas:skeleton\n");
  update(*idx);
  CHECK(status("dsp.reverb") == "tests_ready");
  test::write(p / "blocks/dsp/reverb/impl/reverb.cpp", "#include \"reverb.hpp\"\n");
  update(*idx);
  CHECK(status("dsp.reverb") == "implemented");

  schema = core::load_schema(p.path());
  files = index::collect_code_files(*schema);
  index::TestResult r;
  r.block = "dsp.reverb";
  r.code_hash = index::block_code_hash(files, "dsp.reverb");
  r.contract = {1, 2};
  r.failures = {"contract: zero mix leaves samples unchanged"};
  REQUIRE(store.record_tests({r}));
  update(*idx);
  CHECK(status("dsp.reverb") == "failing");
  CHECK(status("dsp") == "failing");
  CHECK(idx->snapshot().block("dsp")->failed_tests == 1);

  r.contract = {2, 2};
  r.failures.clear();
  REQUIRE(store.record_tests({r}));
  update(*idx);
  CHECK(status("dsp.reverb") == "passing");

  // Code change makes the result stale: back to implemented.
  test::write(p / "blocks/dsp/reverb/impl/reverb.cpp", "#include \"reverb.hpp\"\n// edited\n");
  update(*idx);
  CHECK(status("dsp.reverb") == "implemented");

  // A changed api after freezing; needs_review marks.
  REQUIRE(store.mark_review("dsp.chain", "api of dsp.reverb changed"));
  update(*idx);
  CHECK(status("dsp.chain") == "needs_review");
}

TEST_CASE("index: tickets, probes and runs are loaded from .karkas") {
  test::FixtureCopy p("wavkit");
  test::write(p / ".karkas/tickets/T-0001.ksl",
              "@ticket T-0001\nkind    bug\norigin  human\nstatus  descending\nsymptom crackle\n--- trail\n"
              "L0 orchestrator at=root pick=stream difficulty=M why=x\n");
  test::write(p / ".karkas/probes/run1.jsonl",
              "{\"link\":\"stream.pump->stream.ring\",\"metric\":\"fill\",\"v\":0}\n"
              "{\"link\":\"stream.pump->stream.ring\",\"metric\":\"fill\",\"v\":8192}\n"
              "{\"link\":\"stream.pump->stream.ring\",\"metric\":\"fill\",\"v\":3238}\n");
  test::write(p / ".karkas/runs/20261107-worker-stream.ring.jsonl",
              "{\"type\":\"step\",\"n\":1}\n{\"type\":\"run_end\",\"ticket\":\"T-0001\",\"role\":\"worker\","
              "\"block\":\"stream.ring\",\"provider\":\"anthropic\",\"model\":\"m\",\"tier\":\"cheap\",\"attempt\":1,"
              "\"input_tokens\":100,\"output_tokens\":20,\"cache_write_tokens\":0,\"cache_read_tokens\":50,"
              "\"cost_usd\":0.01,\"duration_ms\":900,\"steps\":3,\"outcome\":\"fixed\",\"ts\":1730000000}\n");
  auto idx = open_index(p.path());
  update(*idx);
  auto snap = idx->snapshot();
  REQUIRE(snap.tickets.size() == 1);
  CHECK(snap.tickets[0].status == "descending");
  CHECK(snap.tickets[0].current_block == "stream");
  CHECK(snap.tickets[0].title == "crackle");
  REQUIRE(snap.probes.size() == 1);
  CHECK(snap.probes[0].str() == "stream.pump->stream.ring fill min=0 avg=3810 max=8192 anomaly=min0");
  CHECK(snap.probes[0].link_id > 0);
  auto runs = idx->runs();
  REQUIRE(runs.size() == 1);
  CHECK(runs[0].cache_read_tokens == 50);
  CHECK(runs[0].outcome == "fixed");
  // Rebuilding from scratch keeps the runs (they come from journals).
  idx.reset();
  fs::remove(p / ".karkas/index.db");
  auto idx2 = open_index(p.path());
  update(*idx2);
  CHECK(idx2->runs().size() == 1);
  CHECK(core::contains(idx2->probes("stream"), "anomaly=min0"));
}

TEST_CASE("index: schema queries (7.3)") {
  test::FixtureCopy p("wavkit");
  auto idx = open_index(p.path());
  update(*idx);
  auto deps = idx->dependent_blocks("stream.ring");
  CHECK(deps == std::vector<std::string>{"dsp.chain"});
  CHECK(idx->dependent_blocks("dsp.gain") == std::vector<std::string>{"dsp.chain"});
  auto shared = idx->dependent_blocks("shared.types");
  CHECK(shared.size() == 11);

  auto path = idx->path("io.reader", "mix");
  REQUIRE(path);
  CHECK(core::contains(*path, "path io.reader -> mix hops=4"));
  CHECK(core::contains(*path, "stream.ring:pop"));
  CHECK(core::contains(*path, "dsp.chain:in"));

  auto w = idx->writers("stream.ring:push");
  REQUIRE(w);
  CHECK(core::contains(*w, "stream.pump:frames"));
  auto r = idx->readers("stream.ring:pop");
  REQUIRE(r);
  CHECK(core::contains(*r, "dsp.chain:in"));
  CHECK(core::contains(idx->by_thread("reader"), "stream.pump"));
  CHECK(core::contains(idx->search("wraparound"), "stream.ring"));
  CHECK(core::contains(idx->search("lock-free ring"), "stream.ring"));
  CHECK(idx->search("nothing-like-this") == "(no matches)\n");
  auto h = idx->history("stream.ring");
  REQUIRE(h);
  CHECK(*h == "hist T-0042 drop at wraparound -> mask index with cap-1\n");
  auto n = idx->neighbors("stream", "", "out");
  REQUIRE(n);
  CHECK(core::contains(*n, "out  stream.ring:pop  -data->  dsp.chain:in"));
  CHECK_FALSE(core::contains(*n, "io.reader"));
  auto bad = idx->view("nope", 1);
  CHECK_FALSE(bad);
  CHECK(idx->drift() == "(no drift)\n");
}

TEST_CASE("index: schema errors are stored in the drift table and mark blocks") {
  test::FixtureCopy p("wavkit");
  auto card = p / "blocks/dsp/chain/block.ksl";
  test::write(card, core::replace_all(test::read(card), "call    dsp.echo:process", "call    dsp.echo:missing"));
  auto idx = open_index(p.path());
  update(*idx);
  auto snap = idx->snapshot();
  CHECK(snap.block("dsp.chain")->status == "drift");
  CHECK(snap.block("dsp")->status == "drift");
  CHECK(core::contains(idx->drift("dsp"), "drift E E03 dsp.chain blocks/dsp/chain/block.ksl:10 block 'dsp.echo' has no port 'missing'"));
}

#include <condition_variable>
#include <mutex>

#include "kk/index/watch.hpp"

TEST_CASE("watch: inotify reports card and ticket changes after a debounce (GUI live updates)") {
  test::FixtureCopy p("wavkit");
  std::mutex mu;
  std::condition_variable cv;
  int events = 0;
  index::Watcher w;
  REQUIRE(w.start(p.path(), [&] {
    std::lock_guard lk(mu);
    ++events;
    cv.notify_all();
  }, std::chrono::milliseconds(50)));
  auto wait_event = [&](int n) {
    std::unique_lock lk(mu);
    return cv.wait_for(lk, std::chrono::seconds(5), [&] { return events >= n; });
  };
  test::write(p / "blocks/dsp/gain/block.ksl", test::read(p / "blocks/dsp/gain/block.ksl") + "note    edited\n");
  CHECK(wait_event(1));
  test::write(p / ".karkas/tickets/T-0001.ksl", "@ticket T-0001\nkind    bug\n");  // new folder inside the tree
  CHECK(wait_event(2));
  w.stop();
  CHECK_FALSE(w.running());
}
