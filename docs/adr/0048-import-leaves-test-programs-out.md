# 0048 — Import: test programs stay out, lone sources pair by class, no orphans for what import skips

Status: accepted (extends ADR 0009 import, 0016 imported projects keep their own build)

## Context

Import of leveldb (132 files, 2026-11-22 check of v0.1.0) gave 100 blocks; 35 of them were test
programs (`db/db_test.cc`, `util/cache_test.cc`, ...) that leveldb keeps next to the code. ADR 0009 skipped
test *folders* only, so these became leaves, cluttered the schema and competed for the entry (the
shallowest `main()` wins: in a probe `src/calc_test.cpp` took the entry from `src/main.cpp`, sorted first).
Test code in a skipped `tests/` folder was reported as D06 orphans right after the import.

`db/skiplist.h` became `util.skiplist` together with `util/cache_test.cc`: a lone source joined the one
header that declared any function name it defined, and `CacheTest::Insert` matched `SkipList::Insert`.

## Decision

- `code::is_test_source`: a source (never a header) named `*_test`, `*_tests`, `*_unittest(s)`, `*-test(s)`,
  `*.test(s)`, `test_*`, `tests_*`, `tst_*`, `FooTest(s)` (a capital T after a small letter, so not `latest`)
  or `test` / `tests` / `unittest(s)`. Import leaves such a file to the project's own suite unless a header
  of the same stem exists (then it has an api: a test helper is code). The plan lists them in `tests`;
  `describe_plan` prints "test programs left to the project's own suite: N (first four, ...)".
- A lone source joins a header module only when the names match with their class or namespace
  (`plain_scope`: template arguments dropped, so `SkipList<K, C>::Insert` defined out of its class matches
  `SkipList::Insert` declared in it). The rule of ADR 0009, "a source that only defines what one module's
  header declares", now holds for methods.
- The D06 orphan walk of an imported project (any block with `files`) passes over what import leaves out
  on purpose: folders named like vendored or test code (`import_skips_dir`: third_party, vendor, external,
  deps, test, tests, testing, ...) and test programs. Native projects are checked as before.

## Consequences

- leveldb: 71 blocks in 7 groups (was 101 in 8), 31 test programs left out, `db.skiplist` = `db/skiplist.h`
  alone, `check`: 0 errors, no D06.
- Tests of an imported project run through its own `[test] command` (ADR 0016, 0029); Karkas does not show
  them as blocks. A project that wants a test file as a block can add it to a card's `files` by hand.
- Not changed: placeholder purposes (`code of db/recovery_test.cc`) for headers without a leading comment;
  a model-written purpose (`--describe`, ADR 0009) is still not implemented.
