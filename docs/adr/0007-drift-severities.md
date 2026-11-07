# 0007 — Severities of the drift checks

Status: proposed

8.2 marks D01, D02 as errors and D09 as a warning. Chosen for the rest: D03, D04 (missing
fn), D05, D07 (manual edit) are errors and block the agents; D04 parameter-count mismatch,
D06 (orphan file), D07 (stale CMakeLists.txt), D08 (leaf without contract tests — normal
before the tester runs) are warnings. D03/D04 are skipped for blocks without api/ yet.
