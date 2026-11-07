# 0003 — Persistent state outside index.db and index schema additions

Status: implemented

5.6 says index.db can be deleted at any time, but 5.3 keeps the frozen api hash "in the
index". To keep both promises, approvals live in files and the index copies them:

- `.karkas/approvals.json` (in git): approved L0 hash, frozen api hashes, needs_review marks.
- `.karkas/cache/tests.json` (not in git): last test results per block with the hash of the
  block's code; a result is "fresh" only while the code hash matches.
- runs are re-read from `.karkas/runs/*.jsonl` journals when the index is rebuilt.

Additions to the DDL of 7.1: tables `test_results` and `facts` (code facts cached by content
hash), columns `runs.journal` (unique, prevents duplicates), `probes.link`, `tickets.title`.

Statuses (3.2) are computed as: drift (any E issue) > needs_review > fresh test result
(failing / passing) > implemented (impl/ without the `karkas:skeleton` marker) > tests_ready
(contract tests exist) > api_frozen (frozen hash == current api hash) > design. A group
shows the worst status of its descendants.
