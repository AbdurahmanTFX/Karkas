# 0004 — Module dependencies that differ from 6.2

Status: implemented

- kk_core parses karkas.toml with toml++ privately (6.2 lists only fmt): every module needs
  the config, and the header stays a plain struct.
- kk_index, kk_code, kk_metrics, kk_layout use nlohmann/json privately for their files
  (state, facts, journals, layout cache).
- kk_index owns the ICodeAnalyzer hook; kk_code implements it (keeps dependencies top-down
  while the index stores drift results and code facts).
- kk_mcp does not link kk_runner: kk_run_tests is a callback that the CLI wires to the test
  pipeline (gen cmake + build + ctest + recording), which lives in kk_agents because it needs
  kk_code, kk_runner and kk_index together.
- Schema edit operations (stage 9) and the benchmark harness (stage 10) live in kk_agents so
  that the GUI, the CLI and the tests share them.
