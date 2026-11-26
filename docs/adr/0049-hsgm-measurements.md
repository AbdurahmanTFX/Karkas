# 0049 — HSGM measurements: what the checks can find, how true the views are, how far a change spreads

Status: proposed (branch nightly; experimental, outside the SPEC)

## Context

The author develops a theory of the project-as-schema (HSGM: a hierarchy of blocks, typed links,
contracts, checks). The research behind it (notes kept by the author outside the repository) took the
mathematics from existing work: group testing and model-based diagnosis, the operad of wiring diagrams,
assume/guarantee contracts, sheaves of consistent data, coincident failures of N-version software,
the propagation cost of design structure matrices. Part of it can be computed from the index today,
without new data and without a model.

Before any of it changes how Karkas works (explicit assumptions in KSL, obligations computed for
workers, checks on links — later stages), these quantities are measured on real projects: the reference
project wavkit, Karkas itself, imported projects. P7 holds: everything here is an algorithm, no model is
called.

## Goals, non-goals, anti-goals

Goals
- A read-only library and command that compute five measurements of a code project from its index:
  what its tests can detect and tell apart (`diag`), how true the lifted views are (`views`), how far a
  change can spread with and without contracts (`propagation`), where the forms of a leaf disagree and
  which to regenerate (`consistency`), and the tree width that minimizes the navigator's cost (`width`).
- Outputs that state their assumptions, deterministic and compact like the views.

Non-goals (this ADR)
- No change of KSL, checks, agents, generation, the GUI or MCP. No `assume`, no quotient of contracts,
  no checks on links: those are later stages and need their own ADRs.
- No model calls; no experiments with models (coincident failures of generated code and tests).
- No JSON output, no code-level dependency matrix (`code_refs`); the schema is the source.

Anti-goals
- A measurement never writes project files; the index is updated as `karkas query` updates it.
- A model quantity is never printed as a fact: every output ends with the assumptions it rests on.
- Nothing here slows the other commands down or adds a dependency.

## Approaches considered

1. **Measurements only** (chosen): read the index, compute, print. Cheap, deterministic, testable on
   wavkit; tells which behavior changes are worth making.
2. Behavior-changing models (`assume`, obligations by quotient, link checks, questions to the human on
   disagreement): changes the language and the agents; waits for the measurements and for the author's
   decision on how the human fixes meaning.
3. Experiments with models (several models write code and tests from one card, cross runs): costs
   tokens; gives data for the reliability layer of the theory.

Inside approach 1:
- a) a new library `kk::hsgm` over `index::Snapshot` (chosen): the experimental code stays apart from
  the index, which every other library depends on;
- b) the same functions inside `kk_index`: mixes experimental code into a core library;
- c) a script over `index.db`: fast to write, but outside the C++ code, its tests and the packages.

## Decision

### Library and command

- `libs/hsgm` → target `kk_hsgm`, namespace `kk::hsgm`, depends on `karkas::index` (and so on
  `karkas::core`). The input is gathered once into one struct (the snapshot, the code files, the runs,
  and the `TEST_CASE` names that one helper reads from the contract test files); each measurement is
  then a pure function of it that returns a result struct, and a formatter prints the result.
- `karkas hsgm [diag|views|propagation|consistency|width]`: updates the index like `karkas query`, runs
  one measurement and prints it; without an argument prints a summary, one line per measurement. Exit
  code 0 whatever the findings: these are measurements, not checks.
- A notes project (no leaf, entry or shared block) gets one line: not a code project.

### Common ground

- Input: `Index::snapshot()` after `Index::update`, the code files of
  `index::collect_code_files(*idx->schema())`, and for `width` the rows of `Index::runs`.
- Code blocks: kinds leaf, entry, shared. Groups only organize.
- Link kinds: data, event, call, state, use; `ref` (notes) is ignored.
- Who depends on whom, from how `Schema::links()` stores a link:
  - data, event: `src` = provider, `dst` = consumer (declared in the consumer) → `dst` depends on `src`;
  - call: `src` = caller, `dst` = callee → `src` depends on `dst`;
  - use: `src` = user, `dst` = shared block → `src` depends on `dst`;
  - state: each end depends on the other.
- Drawn direction (for `views`): `src → dst` as stored, the way the views draw their arrows.

### `diag` — what the tests can detect and tell apart

Group testing (Aldridge, Johnson, Scarlett 2019, ch. 1–2): a test fails when at least one item it
covers is broken.

- Items: code blocks, and every link of kind data, event, call or state (a fault can sit inside a block
  or on the boundary between two). `use` links are not items: a fault there is a fault of the shared
  block.
- Suites, from code files with role `test`:
  - a file of block `v` under a `contract/` folder → `contract:v`; under `unit/` → `unit:v`;
  - a file of group `g` from `tests/integration/<g>/` → `integration:g`.
  One suite per kind and block, however many files and cases: the cases of a suite share its support.
- Support `S(t)`:
  - `contract:v`, `unit:v`: `{v}` and the shared blocks reachable from `v` through `use` links (the
    generated CMake links them; mocks stand in for every other neighbor, 12.2);
  - `integration:g`: the code blocks under `g`, the shared blocks they use, and the data, event, call
    and state links with both ends under `g`.
- Column of an item: `C(i) = {t : i ∈ S(t)}`. Per item:
  - undetectable: `C(i)` is empty;
  - in every suite: `C(i)` is every suite (and there are at least two);
  - identifiable alone: `C(i)` is not empty and differs from every other column — if exactly one item
    is broken, the failing suites name it;
  - class: two or more items with the same non-empty column.
- Global `k`: the largest `k ∈ {0, 1, 2}` such that the matrix over the detectable items that are not
  in every suite is k-disjunct — for every set `L` of at most `k` of them and every other item `i`,
  `C(i)` is not inside the union of the columns of `L`. With at most `k` broken items among them, COMP
  finds exactly those. When `k < 2`, one witness (`i`, `L`) is printed.
- Now, when there are fresh results: a suite failed if the fresh `TestRow` of its block has
  `pass < total` in that kind; it passed if `total > 0` and `pass == total`; otherwise (not run, stale,
  build failed) it is left out and counted.
  - COMP: possibly broken = items in no passed suite. Undetectable items always stay; they are listed
    apart as "cannot be ruled out".
  - DD: surely broken = a possibly broken detectable item that is the only such item in a failed suite.
- Summary: blocks and links identifiable alone, in classes, undetectable.

### `views` — how true the lifted views are

- For each depth `i` from 0 to the deepest code block's depth minus one: `a_i(v)` = the ancestor of `v`
  at depth `i`, or `v` itself when it is not deeper.
- Leaf graph: code blocks, links of kinds data, event, call, state in drawn direction (`use` stays out,
  as the views hide it, 3.7). Lifted graph `G_i`: an edge `a_i(u) → a_i(v)` for each such link with
  `a_i(u) ≠ a_i(v)`.
- Pairs: `P_i` = pairs `(x, y)`, `x ≠ y`, with `y` reachable from `x` in `G_i`. True pairs: those with a
  block `u` under `x` and `v` under `y` such that `v` is reachable from `u` in the leaf graph.
  Precision = true / all (1 when there are none). The false pairs are listed (sorted, capped).
- Cycles: every strongly connected component of `G_i` with at least two nodes. Real if the leaf graph
  restricted to the blocks under it has a strongly connected component that spans blocks under at least
  two different nodes of it; otherwise false.
- Lifting is sound (a path between blocks always shows between their ancestors) and not exact (it adds
  pairs and cycles); the measurement shows how much it adds.

### `propagation` — how far a change can spread

MacCormack, Baldwin, Rusnak 2012 (§3.1).

- Nodes: the `N` code blocks; `M[i][j] = 1` when `i` depends on `j`, all link kinds (a change to a
  shared block reaches its users).
- Visibility `V = Σ_{n=0..N} M^n` (boolean; `n = 0` gives the diagonal).
- Three numbers, each "affected entries / N²":
  - transitive = density of `V`: no contracts, a change reaches everything downstream;
  - direct = density of `I ∨ M`: complete contracts, the changed block's contract changed, only its
    direct dependents are checked again;
  - self = `1/N`: complete contracts, the contract kept, only the block itself.
- Per block: `reach(j)` = blocks a change to `j` can affect (column sum of `V`); the five largest.

### `consistency` — where the forms of a leaf disagree

For leaf and entry blocks. The forms: card `c` (the anchor: the human approves it), api `a`, tests `t`
(the contract suite), impl `i`.

- Edges and when they fail:
  - `c–a`: drift D03, D04 or D05 on the block;
  - `c–t`: D08 on the block, or an `inv` with no contract test case named after it (names from
    `TEST_CASE("…")` in the block's contract test files, compared the way the tester names them, 12.1);
  - build `{a, t, i}`: fresh results with the build failed;
  - `t–i`: fresh contract or unit failures;
  - `c–i`: no check exists; always reported as unchecked.
- Decoder: every minimal set `R ⊆ {a, t, i}` that touches each failed edge; `c` is never chosen (the
  card is fixed). Printed as "regenerate {i} | {t}".
- Listed: blocks with a failed edge; counted: the rest.

### `width` — the tree width that minimizes the navigator's cost

- Runs with role `navigator` whose block is a group of the snapshot; `b` = its number of children;
  `y` = the run's cost in USD when every such run has one, else its input and cache tokens (one unit for
  all, printed). Runs with a session cost (ADR 0031) are left out.
- Fit `y = c0 + c1·b` by least squares. At least 5 runs with at least 2 different `b`, else "not
  enough data: n runs, k widths".
- With a descent of depth `log_b n`, the cost is `T(b) = (c0 + c1·b)·ln n / ln b`; its minimum `b*`
  solves `ln b = 1 + c0 / (c1·b)` (bisection on [2, 64]). Printed: `c0`, `c1`, `b*`, the mean width of
  the project's groups, `T(mean)/T(b*)`. When `c1 ≤ 0`: "cost does not grow with width".

### Output

Compact text like the views: a header `@hsgm <measurement> key=value …`, lines, `--- <section>` blocks,
ids sorted, long lists capped with "… (N more)". Every measurement ends with `assume …` lines, e.g.
`assume contract suites isolate their block with mocks (12.2)`, `assume a failed suite saw every broken
item it covers`.

### Steps

Each step builds, its tests pass, and it is committed on `nightly`.

1. Library, command, `diag` with its tests and golden file.
2. `views`.
3. `propagation`.
4. `consistency`.
5. `width`.
6. The summary, the card of the library, `docs/progress.md`, this ADR accepted.

### Tests (`kk_hsgm_tests`, label `hsgm`)

- Small hand-made snapshots for the exact mathematics:
  - `diag`: an identity-like matrix (all identifiable, `k = 2`), equal columns (a class), an empty
    column, an item in every suite, a matrix with `k = 1` and its witness, COMP and DD on a given set of
    outcomes;
  - `views`: a source and a sink in one group (a false cycle, precision below 1) and a grouping that
    adds nothing (precision 1);
  - `propagation`: the example of MacCormack et al. 2012, Figures 2–3 (A→B, A→C, B→D, C→E, E→F):
    transitive 15/36;
  - `consistency`: each edge rule and the minimal sets of the decoder;
  - `width`: runs made with a known `c0, c1` give the known `b*`; both "not enough data" cases.
- Golden files on wavkit (`tests/golden/hsgm/wavkit_<measurement>.txt`) with the recorded state of the
  view tests (`stream.ring` fails one contract test). Expected from the research, worked out by hand; the
  code must confirm them, and a difference is understood before a golden file is accepted:
  - `diag`: blocks 11 of 13 identifiable alone (the 9 tested leaves, `shared.probe`, and
    `shared.types`, which is also in every suite); `app` and `shared.log` undetectable; links: 0 of 8
    identifiable — the 3 calls of `dsp.chain` in one class, the 5 data links undetectable; `k = 0`,
    witness: a call of `dsp.chain` is covered only by a suite that also covers `dsp.chain`; now: surely
    broken `stream.ring`;
  - `views`: depth 0 precision 9/12, false cycle `io → stream → dsp → mix → io`;
  - `propagation`: transitive 54/169, direct 35/169, self 13/169.

### Self-description

`libs/hsgm/block.ksl` (leaf, thread any, an `fn` per measurement); the cards stay valid (`karkas check`
on the repository has no errors — a `call` into the index needs the `fn` on the index's card). The
library joins the list in the top `CMakeLists.txt` after `index`; `apps/cli` links it.

## Consequences

- Karkas behaves as before; one new command and one library.
- The numbers are model quantities with their assumptions printed. They test hypotheses H1 and H2 of
  the theory (local re-checking, localization of faults) on real projects and pick the next stage: if
  links come out undetectable on real projects, link checks come next; if propagation drops a lot under
  complete contracts, `assume` and obligations by quotient do.
- Limits:
  - suites are taken per block, not per test case;
  - a contract suite is assumed to isolate its block; a hand-written contract test that uses a real
    neighbor is not seen as covering it;
  - imported projects whose tests run in their own suite (ADR 0016, 0048) show no suites: everything
    undetectable by Karkas's checks, which is true but says nothing about their own tests;
  - the dependency matrix is the schema's, not the code's;
  - `width` needs navigator runs.
- Next, each with its own ADR: an MCP tool and a GUI panel for these measurements; `assume` and
  obligations by quotient; checks on links from the consumers' mocks; experiments with models.

## Left to the builder

- Names of link items (unique and stable, e.g. `data stream.pump:frames -> stream.ring:push`), the
  exact text lines, caps of long lists.
- Bitsets and limits for `k = 2` on large projects (print "not computed" above a limit).
- Whether the golden test of `consistency` runs the code analyzer (drift) or only test results.
- Checking in the engine that a navigator run covers one level of a descent; if not, `width` says what
  it measures.
- The exact name-matching rule of tests and invariants, from the tester's prompt.
