// Projects in languages other than C/C++ (docs/adr/0051): import, facts, drift and status.
#include <doctest/doctest.h>

#include <algorithm>

#include "kk/code/analyzer.hpp"
#include "kk/code/drift.hpp"
#include "kk/code/import.hpp"
#include "kk/code/init.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/index/index.hpp"
#include "kk/test/support.hpp"

using namespace kk;

namespace {

const core::Block* find(const code::ImportPlan& p, std::string_view id) {
  for (const auto& b : p.blocks) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

bool calls(const core::Block& b, std::string_view target) {
  return std::any_of(b.calls.begin(), b.calls.end(), [&](const core::CallDecl& c) { return c.target.str() == target; });
}

bool has_port(const core::Block& b, std::string_view sig) {
  return std::any_of(b.ports.begin(), b.ports.end(), [&](const core::Port& p) { return p.signature() == sig; });
}

// A Python package, two shell scripts and a TypeScript tool the project does not name in `lang`.
struct Polyglot : test::TempDir {
  Polyglot() {
    test::write(path() / "project.ksl", "@project poly\nlang    python bash\n");
    test::write(path() / "app" / "mixer.py", R"(""" Mixes tracks with a gain each. """
from .ring import push


class Mixer:
    def set_gain(self, track, gain):
        push(track)
        return gain
)");
    test::write(path() / "app" / "ring.py", R"(# A ring of frames between the reader and the mixer.


def push(frame):
    return _wrap(frame)


def _wrap(frame):
    return frame
)");
    test::write(path() / "scripts" / "run.sh", R"(#!/bin/bash
# Starts or stops the mixer service.
source "$HOME/proj/lib/common.sh"

case "$1" in
    start) log "start" ;;
    stop)  log "stop" ;;
esac
)");
    test::write(path() / "lib" / "common.sh", R"(# Helpers every script sources.
log() {
    echo "$*" >&2
}
)");
    test::write(path() / "app" / "test_mixer.py", "from .mixer import Mixer\n\ndef test_gain():\n    assert Mixer().set_gain(0, 1) == 1\n");
    test::write(path() / "tools" / "gen.ts", "export function gen(): void {}\n");
  }
};

void write_cards(const std::filesystem::path& root, const code::ImportPlan& plan) {
  for (const auto& b : plan.blocks) test::write(root / "blocks" / core::id_to_rel_path(b.id) / "block.ksl", core::write_block(b));
}

}  // namespace

TEST_CASE("polyglot: import cuts Python and shell code into blocks with ports and links (ADR 0051)") {
  Polyglot p;
  auto plan = code::plan_import(p.path());
  REQUIRE_MESSAGE(plan, (plan ? "" : plan.error().str()));
  for (const auto& w : plan->warnings) MESSAGE(w);
  // tools/gen.ts is no project language; app/test_mixer.py is a test program.
  CHECK(std::find(plan->files.begin(), plan->files.end(), "tools/gen.ts") == plan->files.end());
  CHECK(std::find(plan->tests.begin(), plan->tests.end(), "app/test_mixer.py") != plan->tests.end());

  const auto* mixer = find(*plan, "app.mixer");
  const auto* ring = find(*plan, "app.ring");
  const auto* run = find(*plan, "scripts.run");
  const auto* common = find(*plan, "lib.common");
  REQUIRE(mixer != nullptr);
  REQUIRE(ring != nullptr);
  REQUIRE(run != nullptr);
  REQUIRE(common != nullptr);
  CHECK(mixer->purpose.value == "Mixes tracks with a gain each");
  CHECK(ring->purpose.value == "A ring of frames between the reader and the mixer");
  CHECK(has_port(*ring, "push(any)"));
  CHECK_FALSE(has_port(*ring, "wrap(any)"));  // private by its leading underscore
  CHECK(has_port(*mixer, "set_gain(any, any)"));
  CHECK(calls(*mixer, "app.ring:push"));
  CHECK(has_port(*run, "start()"));  // the script's subcommands
  CHECK(has_port(*run, "stop()"));
  CHECK(has_port(*common, "log()"));
  CHECK(calls(*run, "lib.common:log"));
}

TEST_CASE("polyglot: the imported schema checks clean, indexes as implemented and keeps imports as drift") {
  Polyglot p;
  auto plan = code::plan_import(p.path());
  REQUIRE(plan);
  write_cards(p.path(), *plan);
  auto s = core::load_schema(p.path());
  REQUIRE_MESSAGE(s, (s ? "" : s.error().str()));
  auto issues = core::check_schema(*s);
  for (const auto& i : issues) MESSAGE(i.str());
  CHECK(std::none_of(issues.begin(), issues.end(), [](const core::Issue& i) { return i.severity == 'E'; }));
  auto drift = code::check_drift(*s);
  for (const auto& i : drift) MESSAGE(i.str());
  CHECK(drift.empty());  // no D06 for tools/gen.ts (not a project language) or the test program

  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  auto an = std::make_unique<code::CppAnalyzer>();
  REQUIRE((*idx)->update({false, an.get()}));
  auto snap = (*idx)->snapshot();
  for (const char* id : {"app.mixer", "app.ring", "scripts.run", "lib.common"}) {
    INFO(id);
    REQUIRE(snap.block(id) != nullptr);
    CHECK(snap.block(id)->status == "implemented");
  }
  auto r = (*idx)->db().q("SELECT COUNT(*) FROM code_refs WHERE from_block='app.mixer' AND to_block='app.ring' AND kind='import'");
  REQUIRE(r.step());
  CHECK(r.i64(0) == 1);

  // Without its link the import of app.ring is D02, and a card fn the code lacks is D04.
  auto card = test::read(p.path() / "blocks" / "app" / "mixer" / "block.ksl");
  card = core::replace_all(card, "call    app.ring:push\n", "");
  card += "fn      reset() -> void\n";
  test::write(p.path() / "blocks" / "app" / "mixer" / "block.ksl", card);
  auto s2 = core::load_schema(p.path());
  REQUIRE(s2);
  auto d2 = code::check_drift(*s2);
  for (const auto& i : d2) MESSAGE(i.str());
  CHECK(std::any_of(d2.begin(), d2.end(), [](const core::Issue& i) {
    return i.code == "D02" && i.block == "app.mixer" && core::contains(i.message, "imports app.ring");
  }));
  CHECK(std::any_of(d2.begin(), d2.end(), [](const core::Issue& i) { return i.code == "D04" && core::contains(i.message, "reset"); }));
}

TEST_CASE("polyglot: an imported block of data only is implemented") {
  Polyglot p;
  test::write(p.path() / "conf" / "idle.conf", "timeout = 600\n");
  test::write(p.path() / "blocks" / "idle" / "block.ksl",
              "@block idle\npurpose idle timeouts of the session\nkind    leaf\nthread  any\nfiles   conf/idle.conf\ninv     the lock comes at 10 minutes\n");
  auto idx = index::Index::open(p.path());
  REQUIRE(idx);
  auto an = std::make_unique<code::CppAnalyzer>();
  REQUIRE((*idx)->update({false, an.get()}));
  auto snap = (*idx)->snapshot();
  REQUIRE(snap.block("idle") != nullptr);
  CHECK(snap.block("idle")->status == "implemented");
}

TEST_CASE("polyglot: init writes the languages it finds, the C++ build lines only for C++") {
  test::TempDir d;
  auto root = d.path() / "hypr";
  test::write(root / "a.sh", "echo a\n");
  test::write(root / "b.sh", "echo b\n");
  test::write(root / "conf.lua", "x = 1\n");
  test::write(root / "build" / "gen.py", "print(1)\n");  // a build tree does not count
  auto r = code::init_project(root);
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(r->existing_code);
  auto project = test::read(root / "project.ksl");
  CHECK(core::contains(project, "lang    bash lua\n"));
  CHECK_FALSE(core::contains(project, "cmake-ninja"));
  CHECK_FALSE(std::filesystem::exists(root / "cmake" / "user.cmake"));
}
