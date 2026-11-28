# HSGM Measurements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A read-only library `kk::hsgm` and command `karkas hsgm` that compute five measurements of a code project from its index: `diag`, `views`, `propagation`, `consistency`, `width`.

**Architecture:** `gather()` takes everything from an updated `index::Index` once into an `Input` (snapshot, code files, runs, TEST_CASE names). Each measurement is a pure function `Input → result struct`, and `format(result)` prints compact text like the views. The CLI command only dispatches.

**Tech Stack:** C++23, CMake + Ninja (`kk_add_library`, `kk_add_tests`), doctest, golden files (`kk::test::matches_golden`), CLI11.

**Spec:** `docs/adr/0049-hsgm-measurements.md` (read it first: definitions of items, suites, supports, levels, edges, fits).

## Global Constraints

- C++23; namespace `kk::hsgm`; errors as `kk::Result<T>` / `kk::fail(code, message)`, never exceptions across the library boundary.
- No new third-party dependency (fmt or `std::format`, as the neighbouring libraries do).
- Headers light: no nlohmann/json in public headers.
- Never write a project file; the index is updated only through `cli::open_index(root)` as `karkas query` does.
- Deterministic output: ids sorted, same index → same text; no clock, no randomness.
- A printed list longer than 20 entries shows the first 20 and `… (<n> more)`; result structs keep everything.
- Every `format()` ends with its `assume …` lines (copy given in each task).
- Doctest case names must not contain `;` (CMake splits names at `;`).
- Tests use no network and no model; golden files live in `tests/golden/hsgm/`, are (re)written with `KK_UPDATE_GOLDEN=1`, and are reviewed before commit.
- Build: `cmake --preset dev && cmake --build --preset dev`. Tests of the library: `ctest --preset dev -L hsgm`.
- Work on branch `nightly`; one commit per task, message `hsgm: …`, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Portable code (the Windows cross build must still compile): `std::filesystem`, no POSIX-only calls.

## Review Focus

- A project without any test files (a fresh `karkas init`, an imported project): every item undetectable, `k` vacuous, no division by zero anywhere — test in Task 1.
- Stale test results (code changed since the run): outcomes "unknown", never "failed" — tests in Task 1 (`diag`) and Task 5 (`consistency`).
- A flat project (no groups): `views` prints "nothing lifted", `width` has no groups — tests in Task 3 and Task 6.
- A link whose end is not a code block (a group, or a block missing after an edit): ignored, not an item, no crash — test in Task 1.
- Navigator runs on groups that no longer exist, and runs with a session cost: left out of the fit — test in Task 6.

---

## File structure

```
libs/hsgm/CMakeLists.txt
libs/hsgm/block.ksl                       (Task 7)
libs/hsgm/include/kk/hsgm/input.hpp       Input, gather, shared helpers
libs/hsgm/include/kk/hsgm/diag.hpp        Task 1–2
libs/hsgm/include/kk/hsgm/views.hpp       Task 3
libs/hsgm/include/kk/hsgm/propagation.hpp Task 4
libs/hsgm/include/kk/hsgm/consistency.hpp Task 5
libs/hsgm/include/kk/hsgm/width.hpp       Task 6
libs/hsgm/include/kk/hsgm/summary.hpp     Task 7
libs/hsgm/src/{input,diag,graph,views,propagation,consistency,width,summary}.cpp
libs/hsgm/src/graph.hpp                   private graph helpers (Task 3, reused in Task 4)
libs/hsgm/tests/support.hpp, support.cpp  Make (hand-made Input), Wavkit (fixture with recorded state)
libs/hsgm/tests/{diag,views,propagation,consistency,width,summary}_test.cpp
apps/cli/cmd_hsgm.cpp                     Task 2, extended in Tasks 3–7
tests/golden/hsgm/wavkit_{diag,views,propagation,consistency,width,summary}.txt
```

---

### Task 1: Library, input, `diag` measurement

**Files:**
- Create: `libs/hsgm/CMakeLists.txt`, `libs/hsgm/include/kk/hsgm/input.hpp`, `libs/hsgm/include/kk/hsgm/diag.hpp`, `libs/hsgm/src/input.cpp`, `libs/hsgm/src/diag.cpp`, `libs/hsgm/tests/support.hpp`, `libs/hsgm/tests/support.cpp`, `libs/hsgm/tests/diag_test.cpp`
- Modify: `CMakeLists.txt:75` (library list), `docs/adr/0049-hsgm-measurements.md` (status, levels, expectation)

**Interfaces:**
- Produces (`input.hpp`):
  ```cpp
  struct Input {
    index::Snapshot snap;
    std::vector<index::CodeFile> files;
    std::vector<index::RunRow> runs;
    std::map<std::string, std::vector<std::string>> test_cases;  // block → TEST_CASE names of its contract test files
  };
  kk::Result<Input> gather(index::Index& idx);            // fails ("hsgm", "index not updated") when idx.schema() is null
  std::vector<std::string> test_case_names(std::string_view source);  // in order; \" and \\ unescaped
  bool is_code_block(const index::BlockRow& b);           // kind leaf, entry or shared
  bool is_code_project(const Input& in);                  // at least one code block
  std::string link_name(const index::LinkRow& l);         // "data io.reader:frames -> stream.pump:src"; empty ports left out
  std::string ancestor_on(std::string_view id, int level);// first level+1 segments of the id; the id itself when shorter
  ```
- Produces (`diag.hpp`):
  ```cpp
  struct DiagItem {
    std::string id, kind;            // block id / link_name(); leaf|entry|shared / data|event|call|state
    bool link = false;
    std::vector<std::string> suites; // covering suites, sorted
    bool undetectable = false, everywhere = false, alone = false;
    int cls = 0;                     // 1-based class number, 0 = none
  };
  struct Diag {
    std::vector<std::string> suites;                            // sorted
    std::map<std::string, std::vector<std::string>> support;    // suite → item ids, sorted
    std::vector<DiagItem> items;                                // blocks by id, then links by id
    int k = 0; bool k_computed = true; int dominated = 0; std::string witness;
    std::vector<std::string> passed, failed, unknown;           // suites by fresh outcome
    std::vector<std::string> surely, possibly, cannot_rule_out; // item ids
  };
  inline constexpr int kMaxItemsForK = 400;
  Diag diag(const Input& in);
  // Largest k in 0..max_k such that no column is inside the union of k other columns.
  // On failure fills `witness` with (i, L) of the first violation in index order.
  int max_disjunct(const std::vector<std::vector<int>>& cols, int max_k, std::pair<int, std::vector<int>>* witness);
  ```
- Produces (test support, `tests/support.hpp`, namespace `kk::hsgm::test`):
  ```cpp
  struct Make {                       // hand-made Input
    Input in;
    Make& block(const std::string& id, const std::string& kind);  // parent and depth from the id; groups get children
    Make& link(const std::string& kind, const std::string& src, const std::string& dst,
               const std::string& src_port = "", const std::string& dst_port = "");
    Make& suite(const std::string& block, const std::string& kind);  // "contract" | "unit" | "integration"
    Make& result(const std::string& block, index::TestCounts contract, index::TestCounts unit = {},
                 index::TestCounts integration = {}, bool fresh = true, bool build_failed = false);
    Make& drift(const std::string& block, const std::string& code);
    Make& inv(const std::string& block, const std::string& text);
    Make& test_case(const std::string& block, const std::string& name);
    Make& run(const std::string& role, const std::string& block, double cost_usd,
              std::int64_t input_tokens = 0, bool session_cost = false);
  };
  ```
  `suite()` adds a `CodeFile` with role `test`: `"<id with / for .>/tests/contract/x.cpp"`, `"…/tests/unit/x.cpp"`, or `"tests/integration/<group>/x.cpp"` with `block = group`.

Rules the implementer must follow (from the ADR):
- Suites: a `test` file whose path starts with `tests/integration/` → `integration:<file.block>`; else a path containing `/contract/` → `contract:<file.block>`; else → `unit:<file.block>`.
- Supports: `contract:v`/`unit:v` = `{v}` + shared blocks reachable from `v` through `use` links (transitively). `integration:g` = code blocks `b` with `Snapshot::under(g, b)`, their `use`-reachable shared blocks, and the data/event/call/state links with both ends among those code blocks.
- Items: code blocks, and data/event/call/state links whose two ends are code blocks.
- `everywhere` needs at least 2 suites. `alone` = non-empty column different from every other column.
- `k` over D = detectable items not `everywhere`, `max_k = 2`; `k = 2` when |D| ≤ 1; `k_computed = false` and `k = 0` when |D| > `kMaxItemsForK` (then skip the k = 2 pass only: compute k ≤ 1). `dominated` = items of D whose column is inside the column of another single item of D. `witness` text: `"<i> inside <j>"` or `"<i> inside <j> + <l>"`.
- Outcomes from `Snapshot::test(block)`: only rows with `fresh && !build_failed`; per kind `total > 0` → passed when `pass == total`, else failed; a suite with no such row or `total == 0` → unknown.
- COMP/DD: `possibly` = detectable items in no passed suite; `cannot_rule_out` = undetectable items; `surely` = items of `possibly` that are the only `possibly` item in the support of some failed suite.

- [ ] **Step 1: Write the failing tests** in `libs/hsgm/tests/diag_test.cpp` (and `support.cpp` implementing `Make`)

```cpp
TEST_CASE("diag: every block with its own suite is identifiable alone and k is 2") {
  Make m; m.block("a","leaf").block("b","leaf").block("c","leaf")
           .suite("a","contract").suite("b","contract").suite("c","contract");
  auto d = diag(m.in);
  CHECK(d.items.size() == 3);
  for (auto& i : d.items) { CHECK(i.alone); CHECK(i.cls == 0); }
  CHECK(d.k == 2);
}
TEST_CASE("diag: items with the same column form one class") {
  Make m; m.block("g","group").block("g.x","leaf").block("g.y","leaf")
           .link("call","g.x","g.y","","run").suite("g","integration");
  auto d = diag(m.in);
  CHECK(d.items.size() == 3);                         // g.x, g.y, the call
  for (auto& i : d.items) { CHECK(i.cls == 1); CHECK_FALSE(i.alone); }
  CHECK(d.k == 0);
  CHECK(d.witness == "g.x inside g.y");
}
TEST_CASE("diag: a block without suites is undetectable and a shared block in every suite is flagged") {
  Make m; m.block("a","leaf").block("b","leaf").block("c","leaf").block("s","shared")
           .link("use","a","s").link("use","b","s").suite("a","contract").suite("b","contract");
  auto d = diag(m.in);   // items sorted: a, b, c, s
  CHECK(d.items[2].undetectable);
  CHECK(d.items[3].everywhere);
  CHECK(d.items[3].alone);
}
TEST_CASE("diag: max_disjunct finds k 1 and its witness") {
  std::pair<int, std::vector<int>> w;
  CHECK(max_disjunct({{1,2},{1,3},{2,3}}, 2, &w) == 1);
  CHECK(w.first == 0); CHECK(w.second == std::vector<int>{1,2});
}
TEST_CASE("diag: COMP and DD read the fresh outcomes") {
  Make m; m.block("g","group").block("g.a","leaf").block("g.b","leaf").block("c","leaf")
           .link("data","g.a","g.b","out","in")
           .suite("g.a","contract").suite("g.b","contract").suite("c","contract").suite("g","integration")
           .result("g.a",{1,1}).result("g.b",{1,1}).result("c",{0,1}).result("g",{},{},{0,1});
  auto d = diag(m.in);
  CHECK(d.failed == std::vector<std::string>{"contract:c","integration:g"});
  CHECK(d.surely == std::vector<std::string>{"c","data g.a:out -> g.b:in"});
}
// Review focus
TEST_CASE("diag: a project without test files has every item undetectable and a vacuous k") {
  Make m; m.block("a","leaf").block("b","leaf").link("call","a","b","","f");
  auto d = diag(m.in);
  for (auto& i : d.items) CHECK(i.undetectable);
  CHECK(d.suites.empty()); CHECK(d.k == 2); CHECK(d.passed.empty());
}
TEST_CASE("diag: stale results are unknown, not failed") {
  Make m; m.block("a","leaf").suite("a","contract").result("a",{0,1},{},{},/*fresh*/false);
  auto d = diag(m.in);
  CHECK(d.failed.empty()); CHECK(d.unknown == std::vector<std::string>{"contract:a"});
}
TEST_CASE("diag: a link whose end is not a code block is no item") {
  Make m; m.block("g","group").block("a","leaf").link("call","a","g","","f").link("call","a","gone","","f");
  CHECK(diag(m.in).items.size() == 1);
}
TEST_CASE("input: TEST_CASE names are read in order and unescaped") {
  CHECK(test_case_names("TEST_CASE(\"x \\\"q\\\"\") {}\nTEST_CASE( \"y\" ) {}") ==
        std::vector<std::string>{"x \"q\"", "y"});
}
```

- [ ] **Step 2: Add the library to the build and run the tests to see them fail**

`libs/hsgm/CMakeLists.txt`:
```cmake
kk_add_library(hsgm SOURCES src/input.cpp src/diag.cpp PUBLIC_DEPS kk_core kk_index)
kk_add_tests(hsgm SOURCES tests/support.cpp tests/diag_test.cpp)
```
Top `CMakeLists.txt:75`: `foreach(lib core index code runner llm metrics hsgm agents mcp layout)`.

Run: `cmake --preset dev && cmake --build --preset dev --target kk_hsgm_tests`
Expected: compile errors (`diag`, `gather` not defined) — headers declared, sources empty.

- [ ] **Step 3: Implement `input.cpp` and `diag.cpp`** per the Interfaces and the rules above. `gather()` reads the TEST_CASE names with `core::read_file(idx.root() / file.path)` for `test` files containing `/contract/`. `max_disjunct` works on sorted column vectors; use bitsets (`std::vector<std::uint64_t>` per column) for the k = 2 pass.

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset dev --target kk_hsgm_tests && ctest --preset dev -L hsgm`
Expected: all `hsgm/…` cases pass.

- [ ] **Step 5: Update the ADR**
- `Status: accepted (branch nightly; experimental, outside the SPEC)`.
- In `### views`: "For each level `i` from 0 (the top level, `BlockRow.depth − 1`) to the deepest code block's level minus one: `a_i(v)` = the ancestor of `v` on level `i`, or `v` itself when it is not deeper." In the wavkit expectations: "level 0 precision 9/12".
- In the `diag` expectation replace the witness sentence with: "`k = 0`, 5 items dominated (`stream.pump` and `stream.ring` inside `shared.probe`, the 3 calls of `dsp.chain` inside the dsp leaves); first witness `stream.pump inside shared.probe`".

- [ ] **Step 6: Commit**

```bash
git add libs/hsgm CMakeLists.txt docs/adr/0049-hsgm-measurements.md
git commit -m "hsgm: library, input and the diag measurement (ADR 0049)"
```

---

### Task 2: `diag` text, `karkas hsgm`, wavkit golden

**Files:**
- Modify: `libs/hsgm/include/kk/hsgm/diag.hpp` (+ `std::string format(const Diag&)`), `libs/hsgm/src/diag.cpp`, `libs/hsgm/tests/support.{hpp,cpp}` (+ `Wavkit`), `libs/hsgm/tests/diag_test.cpp`, `apps/cli/CMakeLists.txt:4` (add `hsgm` to the list)
- Create: `apps/cli/cmd_hsgm.cpp`, `tests/golden/hsgm/wavkit_diag.txt`

**Interfaces:**
- Consumes: `gather`, `diag` (Task 1).
- Produces: `std::string format(const Diag& d);` and test fixture
  ```cpp
  struct Wavkit {                 // wavkit with the recorded state of libs/index/tests/views_test.cpp
    kk::test::FixtureCopy p{"wavkit"};
    std::unique_ptr<index::Index> idx;
    Input in;                     // gather(*idx) after update()
    Wavkit();
  };
  ```
  (copy the `record_tests` setup of `ViewFixture`: every code block passes its `inv` count, `stream.ring` contract `{2, 3}`, blocks with no `inv` unit `{1, 1}`; no probes.)
- Produces (CLI): `karkas hsgm [measurement]`; this task handles `diag` only; an unknown name → `die("unknown measurement '<name>': diag views propagation consistency width")`; a project with no code block prints `hsgm: not a code project` and returns 0.

Text of `format(Diag)` (columns padded like the views):
```
@hsgm diag suites=<n> blocks=<n> links=<n>
--- suites
<suite>  <item> <item> …
--- blocks
<id>  <kind>  alone|class <n>|undetectable [everywhere]  <suite> …
--- links
<link id>  alone|class <n>|undetectable  <suite> …
--- classes
class <n>  <item> | <item> | …
--- disjunct
k <k>  dominated <n>  witness <text>        (k 2: "k 2"; not computed: "k not computed: <n> items")
--- now
passed <n> failed <n> unknown <n>           (no outcome at all: "no fresh results")
surely <item> | …
possibly <item> | …
cannot rule out <item> | …
--- summary
blocks alone <a>/<n>, in classes <c>, undetectable <u>
links alone <a>/<n>, in classes <c>, undetectable <u>
assume a suite fails when it covers a broken item and passes otherwise
assume contract and unit suites isolate their block with mocks (12.2)
```
Empty sections are left out (except `--- summary`).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("diag: wavkit counts follow the research and match golden") {
  Wavkit w; auto d = diag(w.in);
  int blocks_alone = 0, links_alone = 0, links_class = 0, links_none = 0;
  for (auto& i : d.items) {
    if (!i.link && i.alone) ++blocks_alone;
    if (i.link) { links_alone += i.alone; links_class += i.cls > 0; links_none += i.undetectable; }
  }
  CHECK(d.suites.size() == 11);
  CHECK(blocks_alone == 11); CHECK(links_alone == 0); CHECK(links_class == 3); CHECK(links_none == 5);
  CHECK(d.k == 0); CHECK(d.dominated == 5); CHECK(d.witness == "stream.pump inside shared.probe");
  CHECK(d.surely == std::vector<std::string>{"stream.ring"});
  CHECK(kk::test::matches_golden("hsgm/wavkit_diag.txt", format(d)));
}
```

- [ ] **Step 2: Run to see it fail**

Run: `cmake --build --preset dev --target kk_hsgm_tests && ctest --preset dev -L hsgm`
Expected: compile error (`format`, `Wavkit` not defined).

- [ ] **Step 3: Implement `format(Diag)`, `Wavkit`, `cmd_hsgm.cpp`** (pattern of `cmd_stats.cpp`: `AutoRegister`, `setup_logging(app, root, false)`, `open_index(root)`, `hsgm::gather(*idx)`, print). Add `hsgm` to the library list in `apps/cli/CMakeLists.txt`.

- [ ] **Step 4: Write the golden file and review it**

Run: `KK_UPDATE_GOLDEN=1 ctest --preset dev -R "hsgm/diag: wavkit"` then `ctest --preset dev -L hsgm`
Expected: pass. Read `tests/golden/hsgm/wavkit_diag.txt`: 11 suites; blocks alone 11/13 with `app` and `shared.log` undetectable and `shared.types` everywhere; the 3 calls of `dsp.chain` in class 1; 5 data links undetectable; surely `stream.ring`. A difference from these numbers is explained (fix the code, or correct the ADR with the reason) before committing.

- [ ] **Step 5: Try the command**

Run: `D=$(mktemp -d) && cp -r tests/fixtures/wavkit "$D/" && ./build/dev/apps/cli/karkas hsgm diag --project "$D/wavkit"`
Expected: the text of the golden file without the "now" outcomes (a fresh copy has no recorded results: `no fresh results`).

- [ ] **Step 6: Commit**

```bash
git add libs/hsgm apps/cli tests/golden/hsgm
git commit -m "hsgm: diag text, the karkas hsgm command, wavkit golden"
```

---

### Task 3: `views` — precision of the lifted views

**Files:**
- Create: `libs/hsgm/include/kk/hsgm/views.hpp`, `libs/hsgm/src/views.cpp`, `libs/hsgm/src/graph.hpp`, `libs/hsgm/src/graph.cpp`, `libs/hsgm/tests/views_test.cpp`, `tests/golden/hsgm/wavkit_views.txt`
- Modify: `libs/hsgm/CMakeLists.txt`, `apps/cli/cmd_hsgm.cpp` (`views`)

**Interfaces:**
- Produces (private `src/graph.hpp`, namespace `kk::hsgm::detail`, reused by Task 4):
  ```cpp
  struct Graph { std::vector<std::string> nodes; std::map<std::string, int> at; std::vector<std::vector<int>> out; };
  Graph drawn_graph(const Input& in);       // code blocks; data/event/call/state links, src -> dst as stored
  Graph dependency_graph(const Input& in);  // i -> j when i depends on j (orientation table of the ADR), all kinds but ref
  std::vector<std::vector<char>> reach(const Graph& g, bool reflexive);  // BFS from every node
  std::vector<std::vector<int>> sccs(const Graph& g);                    // Tarjan; members and list sorted
  ```
- Produces (`views.hpp`):
  ```cpp
  struct LevelView {
    int level = 0, pairs = 0, true_pairs = 0;
    std::vector<std::string> false_pairs;                            // "x -> y", sorted
    std::vector<std::vector<std::string>> real_cycles, false_cycles; // node ids sorted; lists sorted
  };
  struct Views { std::vector<LevelView> levels; };   // empty: nothing lifted
  Views views(const Input& in);
  std::string format(const Views& v);
  ```
- Rules: levels `0 … max(depth of code blocks) − 2`; node of a block on level `i` = `ancestor_on(id, i)`; a pair is true when some leaf-graph path (length ≥ 1) runs from a block under `x` to a block under `y`; a lifted SCC of size ≥ 2 is real when some leaf SCC has members under at least two of its nodes.

Text:
```
@hsgm views levels=<n>
level <i>  precision <t>/<p>  false <n>  cycles real <r> false <f>
false <x -> y> | …
real cycle <ids…>
false cycle <ids…>
assume arrows as the views draw them (src -> dst); use links left out (3.7)
```
No levels: `@hsgm views levels=0` then `nothing lifted: every code block is on the top level`, then the `assume` line.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("views: a source and a sink in one group make false pairs and a false cycle") {
  Make m; m.block("p","group").block("p.src","leaf").block("p.sink","leaf")
           .block("q","group").block("q.x","leaf").block("r","group").block("r.y","leaf")
           .link("data","p.src","q.x").link("data","q.x","r.y").link("data","r.y","p.sink");
  auto v = views(m.in);
  REQUIRE(v.levels.size() == 1);
  CHECK(v.levels[0].pairs == 6); CHECK(v.levels[0].true_pairs == 5);
  CHECK(v.levels[0].false_pairs == std::vector<std::string>{"r -> q"});
  CHECK(v.levels[0].false_cycles == std::vector<std::vector<std::string>>{{"p","q","r"}});
}
TEST_CASE("views: a real cycle between two groups stays real") {
  Make m; m.block("g","group").block("g.a","leaf").block("h","group").block("h.c","leaf")
           .link("call","g.a","h.c","","f").link("call","h.c","g.a","","f");
  auto v = views(m.in);
  CHECK(v.levels[0].real_cycles.size() == 1); CHECK(v.levels[0].false_cycles.empty());
}
TEST_CASE("views: a grouping that adds nothing has precision 1") {
  Make m; m.block("g","group").block("g.a","leaf").block("h","group").block("h.b","leaf")
           .link("data","g.a","h.b");
  auto l = views(m.in).levels[0];
  CHECK(l.pairs == 1); CHECK(l.true_pairs == 1); CHECK(l.false_cycles.empty());
}
TEST_CASE("views: a flat project has nothing lifted") {                  // review focus
  Make m; m.block("a","leaf").block("b","leaf").link("data","a","b");
  auto v = views(m.in);
  CHECK(v.levels.empty());
  CHECK(format(v).find("nothing lifted") != std::string::npos);
}
TEST_CASE("views: wavkit level 0 is 9 of 12 with one false cycle and matches golden") {
  Wavkit w; auto v = views(w.in);
  CHECK(v.levels[0].true_pairs == 9); CHECK(v.levels[0].pairs == 12);
  CHECK(v.levels[0].false_cycles == std::vector<std::vector<std::string>>{{"dsp","io","mix","stream"}});
  CHECK(kk::test::matches_golden("hsgm/wavkit_views.txt", format(v)));
}
```

- [ ] **Step 2: Run to see them fail** — `cmake --build --preset dev --target kk_hsgm_tests` → compile errors (`views` undefined).
- [ ] **Step 3: Implement** `graph.cpp`, `views.cpp`, the `views` branch of `cmd_hsgm.cpp`; add the sources to `libs/hsgm/CMakeLists.txt`.
- [ ] **Step 4: Golden and run** — `KK_UPDATE_GOLDEN=1 ctest --preset dev -R "hsgm/views: wavkit"`, review the file (precision 9/12; false pairs `dsp -> stream`, `mix -> dsp`, `mix -> stream`), then `ctest --preset dev -L hsgm` → pass.
- [ ] **Step 5: Commit** — `git add libs/hsgm apps/cli tests/golden/hsgm && git commit -m "hsgm: views — precision of lifted views, false cycles"`

---

### Task 4: `propagation` — how far a change spreads

**Files:**
- Create: `libs/hsgm/include/kk/hsgm/propagation.hpp`, `libs/hsgm/src/propagation.cpp`, `libs/hsgm/tests/propagation_test.cpp`, `tests/golden/hsgm/wavkit_propagation.txt`
- Modify: `libs/hsgm/CMakeLists.txt`, `apps/cli/cmd_hsgm.cpp` (`propagation`)

**Interfaces:**
- Consumes: `detail::dependency_graph`, `detail::reach` (Task 3).
- Produces:
  ```cpp
  struct Propagation {
    int n = 0;
    long long transitive = 0, direct = 0;               // affected entries; self = n
    std::vector<std::pair<std::string, int>> reach;     // block → blocks a change to it can affect (itself included);
                                                        // largest first, then by id
  };
  Propagation propagation(const Input& in);
  std::string format(const Propagation& p);
  ```

Text:
```
@hsgm propagation blocks=<n>
transitive <t>/<n²> <0.xx>
direct     <d>/<n²> <0.xx>
self       <n>/<n²> <0.xx>
--- reach
<id> <count>          (five largest)
assume a change reaches every block that depends on it through the schema's links (data, event, call, state, use)
assume complete contracts stop a change at the direct dependents (direct) or at the block itself (self)
```
Empty project (`n = 0`): one line `no code blocks` then the `assume` lines.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("propagation: the example of MacCormack et al. gives 15 of 36") {
  Make m; for (auto id : {"a","b","c","d","e","f"}) m.block(id, "leaf");
  m.link("call","a","b","","f").link("call","a","c","","f").link("call","b","d","","f")
   .link("call","c","e","","f").link("call","e","f","","f");
  auto p = propagation(m.in);
  CHECK(p.n == 6); CHECK(p.transitive == 15); CHECK(p.direct == 11);
  CHECK(p.reach[0] == std::pair<std::string,int>{"f", 4});
}
TEST_CASE("propagation: data makes the consumer depend on the provider, state works both ways") {
  Make m; m.block("p","leaf").block("c","leaf").block("x","leaf").block("y","leaf")
           .link("data","p","c","out","in").link("state","x","y","s");
  auto p = propagation(m.in);
  auto r = [&](std::string id) { for (auto& [b, n] : p.reach) if (b == id) return n; return -1; };
  CHECK(r("p") == 2); CHECK(r("c") == 1); CHECK(r("x") == 2); CHECK(r("y") == 2);
}
TEST_CASE("propagation: wavkit is 54, 35 and 13 of 169 and matches golden") {
  Wavkit w; auto p = propagation(w.in);
  CHECK(p.n == 13); CHECK(p.transitive == 54); CHECK(p.direct == 35);
  CHECK(kk::test::matches_golden("hsgm/wavkit_propagation.txt", format(p)));
}
```

- [ ] **Step 2: Run to see them fail** — compile errors (`propagation` undefined).
- [ ] **Step 3: Implement** `propagation.cpp` (`transitive` = entries of `reach(dependency_graph, true)`; `direct` = n + number of distinct dependency pairs; `reach(j)` = column sums) and the CLI branch.
- [ ] **Step 4: Golden and run** — `KK_UPDATE_GOLDEN=1 ctest --preset dev -R "hsgm/propagation: wavkit"`, review (0.32 / 0.21 / 0.08), `ctest --preset dev -L hsgm` → pass.
- [ ] **Step 5: Commit** — `git commit -m "hsgm: propagation — how far a change can spread"`

---

### Task 5: `consistency` — where the forms of a leaf disagree

**Files:**
- Create: `libs/hsgm/include/kk/hsgm/consistency.hpp`, `libs/hsgm/src/consistency.cpp`, `libs/hsgm/tests/consistency_test.cpp`, `tests/golden/hsgm/wavkit_consistency.txt`
- Modify: `libs/hsgm/CMakeLists.txt`, `apps/cli/cmd_hsgm.cpp` (`consistency`)

**Interfaces:**
- Produces:
  ```cpp
  struct LeafForms {
    std::string block;
    std::vector<std::string> ca;        // D03, D04, D05 found on the block, sorted, unique
    bool d08 = false;
    std::vector<std::string> untested;  // inv with no TEST_CASE of the same normalized name
    bool build = false, ti = false;     // fresh build failed / fresh contract or unit failures
    std::vector<std::string> regenerate;// minimal sets over a, t, i (order a < t < i), by size then order: "{a, t}"
    [[nodiscard]] bool fails() const;
  };
  struct Consistency { int checked = 0; std::vector<LeafForms> disagree; };  // checked: leaf and entry blocks
  Consistency consistency(const Input& in);
  std::string format(const Consistency& c);
  std::string normalize_name(std::string_view s);  // whitespace runs → one space, trimmed, ';' → ','
  ```
- Edges as sets of forms: `ca` → {a}; `ct` (d08 or untested) → {t}; `build` → {a, t, i}; `ti` → {t, i}. `regenerate` = every minimal subset of {a, t, i} that meets each failed edge.
- `build` = a fresh row with `build_failed`; `ti` = a fresh row without `build_failed` whose contract or unit counts have `pass < total` (a failed build says nothing about the cases).

Text:
```
@hsgm consistency checked=<n> disagree=<n>
<block>  c-a <codes>  c-t D08|untested <n>  build  t-i   regenerate {…} | {…}   (only the failed edges)
  untested <inv text>                                                            (one line each)
--- unchecked
c-i: no check compares a card's invariants with its code
assume the card is the anchor: it is never regenerated
assume only fresh test results count
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("consistency: each failed edge names the forms to regenerate") {
  Make m; for (auto id : {"a","b","c","d","e"}) m.block(id, "leaf");
  m.drift("a","D04").drift("b","D08")
   .suite("c","contract").result("c",{0,1},{},{},true,/*build_failed*/true)
   .suite("d","contract").result("d",{0,1})
   .drift("e","D05").suite("e","contract").result("e",{0,1});
  auto c = consistency(m.in);
  auto regen = [&](std::string id) { for (auto& f : c.disagree) if (f.block == id) return f.regenerate; return std::vector<std::string>{}; };
  CHECK(regen("a") == std::vector<std::string>{"{a}"});
  CHECK(regen("b") == std::vector<std::string>{"{t}"});
  CHECK(regen("c") == std::vector<std::string>{"{a}","{t}","{i}"});
  CHECK(regen("d") == std::vector<std::string>{"{t}","{i}"});
  CHECK(regen("e") == std::vector<std::string>{"{a, t}","{a, i}"});
}
TEST_CASE("consistency: test names match invariants with whitespace collapsed and ';' read as ','") {
  Make m; m.block("a","leaf").inv("a","x stays in [0, n]; y holds").test_case("a","x  stays in [0, n], y holds");
  CHECK(consistency(m.in).disagree.empty());
  Make u; u.block("a","leaf").inv("a","never blocks");
  CHECK(consistency(u.in).disagree[0].untested == std::vector<std::string>{"never blocks"});
}
TEST_CASE("consistency: stale results do not fail an edge") {                 // review focus
  Make m; m.block("a","leaf").suite("a","contract").result("a",{0,1},{},{},/*fresh*/false);
  CHECK(consistency(m.in).disagree.empty());
}
TEST_CASE("consistency: wavkit has stream.ring failing t-i and matches golden") {
  Wavkit w; auto c = consistency(w.in);
  CHECK(c.checked == 10);
  REQUIRE(c.disagree.size() == 1);
  CHECK(c.disagree[0].block == "stream.ring");
  CHECK(c.disagree[0].regenerate == std::vector<std::string>{"{t}","{i}"});
  CHECK(kk::test::matches_golden("hsgm/wavkit_consistency.txt", format(c)));
}
```
(The wavkit index of the fixture is built without the code analyzer, so no D codes appear there; drift rules are covered by the hand-made cases.)

- [ ] **Step 2: Run to see them fail** — compile errors (`consistency` undefined).
- [ ] **Step 3: Implement** `consistency.cpp` (invariants from `Snapshot::invariants`, drift from `Snapshot::drift`, TEST_CASE names from `Input::test_cases`, outcomes from fresh `TestRow`s) and the CLI branch.
- [ ] **Step 4: Golden and run** — `KK_UPDATE_GOLDEN=1 ctest --preset dev -R "hsgm/consistency: wavkit"`, review: one block, `stream.ring t-i regenerate {t} | {i}`; if another block appears with `untested`, a test name in the fixture differs from its `inv` — record it as a finding in the commit message, do not edit the fixture. `ctest --preset dev -L hsgm` → pass.
- [ ] **Step 5: Commit** — `git commit -m "hsgm: consistency — forms of a leaf, what to regenerate"`

---

### Task 6: `width` — the tree width that minimizes the navigator's cost

**Files:**
- Create: `libs/hsgm/include/kk/hsgm/width.hpp`, `libs/hsgm/src/width.cpp`, `libs/hsgm/tests/width_test.cpp`, `tests/golden/hsgm/wavkit_width.txt`
- Modify: `libs/hsgm/CMakeLists.txt`, `apps/cli/cmd_hsgm.cpp` (`width`)

**Interfaces:**
- Produces:
  ```cpp
  struct Width {
    int runs = 0, widths = 0;          // runs used, distinct widths among them
    std::string unit;                  // "usd" | "tokens"
    bool enough = false, grows = false;
    double c0 = 0, c1 = 0, best = 0, mean = 0, ratio = 0;  // ratio = T(mean) / T(best), T(b) = (c0 + c1 b) / ln b
  };
  Width width(const Input& in);
  std::string format(const Width& w);
  double optimal_width(double c0, double c1);  // root of ln b − 1 − c0/(c1 b) on [2, 64] by bisection; 2 or 64 at the ends
  ```
- Rules: runs with `role == "navigator"`, `!session_cost`, `block` a group of the snapshot; `b` = its children count; `y` = `cost_usd` when every selected run has `cost_usd > 0`, else `input_tokens + cache_write_tokens + cache_read_tokens`. Least squares fit. `enough` = runs ≥ 5 and widths ≥ 2. `grows` = `c1 > 0`. `mean` = mean children count of groups with at least one child (0 when none).

Text:
```
@hsgm width runs=<n> widths=<k> unit=<unit>
fit c0=<v> c1=<v>
best <b>  mean <m>  cost at mean / best <r>
assume one navigator run per level of a descent, its cost c0 + c1 × children
```
Not enough: `not enough data: <n> runs, <k> widths`. Not growing: `cost does not grow with width (c1 <= 0)`.

- [ ] **Step 1: Write the failing tests**

```cpp
Make widths_project() {
  Make m;
  for (auto [g, n] : {std::pair{"g2", 2}, std::pair{"g4", 4}, std::pair{"g8", 8}}) {
    m.block(g, "group");
    for (int i = 0; i < n; ++i) m.block(std::string(g) + ".l" + std::to_string(i), "leaf");
  }
  return m;
}
TEST_CASE("width: runs made from known c0 and c1 give the known best width") {
  auto m = widths_project();
  for (auto [g, b] : {std::pair{"g2", 2}, std::pair{"g4", 4}, std::pair{"g8", 8}})
    for (int k = 0; k < 2; ++k) m.run("navigator", g, 0.01 + 0.002 * b);
  auto w = width(m.in);
  CHECK(w.enough); CHECK(w.unit == "usd");
  CHECK(w.c0 == doctest::Approx(0.01)); CHECK(w.c1 == doctest::Approx(0.002));
  CHECK(w.best == doctest::Approx(6.14).epsilon(0.01));
  CHECK(w.ratio == doctest::Approx(1.022).epsilon(0.005));
}
TEST_CASE("width: too few runs or one width is not enough data") {
  auto m = widths_project();
  for (int k = 0; k < 5; ++k) m.run("navigator", "g4", 0.02);
  CHECK_FALSE(width(m.in).enough);
}
TEST_CASE("width: session-cost runs and runs on unknown groups are left out") {   // review focus
  auto m = widths_project();
  m.run("navigator", "gone", 0.5).run("navigator", "g2", 0.5, 0, /*session_cost*/true).run("worker", "g2", 0.5);
  CHECK(width(m.in).runs == 0);
}
TEST_CASE("width: runs without a cost are fitted in tokens") {
  auto m = widths_project();
  for (auto [g, b] : {std::pair{"g2", 2}, std::pair{"g8", 8}})
    for (int k = 0; k < 3; ++k) m.run("navigator", g, 0, 1000 + 200 * b);
  CHECK(width(m.in).unit == "tokens");
}
TEST_CASE("width: a flat project has no groups and not enough data") {          // review focus
  Make m; m.block("a","leaf");
  auto w = width(m.in);
  CHECK(w.mean == 0); CHECK_FALSE(w.enough);
}
TEST_CASE("width: wavkit has no runs and matches golden") {
  Wavkit w;
  CHECK(kk::test::matches_golden("hsgm/wavkit_width.txt", format(width(w.in))));
}
```

- [ ] **Step 2: Run to see them fail** — compile errors (`width` undefined).
- [ ] **Step 3: Implement** `width.cpp` and the CLI branch. Before finishing, read `libs/agents/src/engine.cpp` around the navigator decision (`role = orch ? "orchestrator" : "navigator"`) and confirm one run is recorded per level; if a run spans several levels, change the `assume` line to say what one run is.
- [ ] **Step 4: Golden and run** — `KK_UPDATE_GOLDEN=1 ctest --preset dev -R "hsgm/width: wavkit"` (expect `not enough data: 0 runs, 0 widths`), `ctest --preset dev -L hsgm` → pass.
- [ ] **Step 5: Commit** — `git commit -m "hsgm: width — the tree width that minimizes the navigator's cost"`

---

### Task 7: Summary, the library's card, progress

**Files:**
- Create: `libs/hsgm/include/kk/hsgm/summary.hpp`, `libs/hsgm/src/summary.cpp`, `libs/hsgm/tests/summary_test.cpp`, `tests/golden/hsgm/wavkit_summary.txt`, `libs/hsgm/block.ksl`
- Modify: `libs/hsgm/CMakeLists.txt`, `apps/cli/cmd_hsgm.cpp` (no argument → summary), `libs/index/block.ksl` (+ `fn snapshot() -> Snapshot`), `docs/progress.md` (new section at the end)

**Interfaces:**
- Consumes: all measurements.
- Produces: `std::string summary(const Input& in);`

Text:
```
@hsgm summary
diag         blocks alone <a>/<n>, links alone <a>/<n> (<c> in classes, <u> undetectable), k <k>
views        level 0 precision <t>/<p>, false cycles <f>          (flat: "nothing lifted")
propagation  transitive <0.xx>, direct <0.xx>, self <0.xx>
consistency  <d> of <n> disagree
width        <the first line of format(width) after the header>
```

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("summary: wavkit has one line per measurement and matches golden") {
  Wavkit w; auto s = summary(w.in);
  CHECK(std::count(s.begin(), s.end(), '\n') == 6);
  CHECK(s.find("propagation  transitive 0.32, direct 0.21, self 0.08") != std::string::npos);
  CHECK(kk::test::matches_golden("hsgm/wavkit_summary.txt", s));
}
```

- [ ] **Step 2: Run to see it fail** — compile error (`summary` undefined).
- [ ] **Step 3: Implement** `summary.cpp` and the no-argument branch of the command.
- [ ] **Step 4: Golden and run** — `KK_UPDATE_GOLDEN=1 ctest --preset dev -R "hsgm/summary"`, review, `ctest --preset dev -L hsgm` → pass.
- [ ] **Step 5: The library's card**

`libs/hsgm/block.ksl`:
```
@block libs.hsgm
purpose experimental HSGM measurements of a code project from its index (ADR 0049)
kind    leaf
thread  any
est     M
fn      gather(Index) -> Result<Input>
fn      diag(Input) -> Diag
fn      views(Input) -> Views
fn      propagation(Input) -> Propagation
fn      consistency(Input) -> Consistency
fn      width(Input) -> Width
fn      summary(Input) -> string
call    libs.index:snapshot
use     libs.core
inv     the same index gives the same text
inv     a measurement never writes a project file
```
`libs/index/block.ksl`: add `fn      snapshot() -> Snapshot` after `fn      overview(options) -> string`.

Run (before the card, then after): `./build/dev/apps/cli/karkas check -q` in the repository root.
Expected: no error that names `libs.hsgm` or `libs.index` after the change; the rest of the list unchanged.

- [ ] **Step 6: Run the measurements on Karkas itself**

Run: `./build/dev/apps/cli/karkas hsgm` in the repository root.
Expected: five lines, no crash; note the numbers for the progress entry.

- [ ] **Step 7: Progress, full tests, commit**

Append to `docs/progress.md`:
```markdown
## Nightly — HSGM measurements (docs/adr/0049)

- `kk::hsgm` and `karkas hsgm [diag|views|propagation|consistency|width]`: read-only measurements of a code
  project from its index; every output ends with its assumptions.
- wavkit: <the five summary lines>.
- Karkas itself: <the five summary lines from Step 6>.
- Tests: <number> in kk_hsgm_tests; golden files in tests/golden/hsgm.
```
Run: `ctest --preset dev-fast` (all but the slow reference builds)
Expected: every test passes (the count grows by the new hsgm cases).

```bash
git add libs/hsgm libs/index/block.ksl apps/cli tests/golden/hsgm docs/progress.md
git commit -m "hsgm: summary, the library's card, progress (ADR 0049)"
```
