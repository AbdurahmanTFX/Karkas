# Karkas benchmarks (section 17)

    karkas bench run --scenario s1|s2|s3 --method A|B|C --model <name> --reps 5 [--auth api|subscription]
    karkas bench report

- A — Claude Code alone (`claude -p ... --output-format json --bare`), no Karkas files.
- B — Claude Code + Karkas: the MCP server, the guide (CLAUDE.md), the subagents of the model
  tiers (`--agents`); `karkas init` in S1; the agent approves its own proposals and each approval
  counts as an intervention (docs/adr/0014).
- C — the full Karkas pipeline (architect, orchestrator, navigators, workers, testers).

Data: `wavkit/spec.md` (the task description, exact CLI and formats), `wavkit/s2_tasks.txt`
(five extensions), `wavkit/bugs/*.json` (five injected bugs for S3, applied to the
reference project `tests/fixtures/wavkit`). Results: `results/<date>/<scenario>-<method>-<n>.json`;
`karkas bench report` writes `results/report.csv`, `results/report.txt`, `results/report.html`.

Hidden acceptance tests live OUTSIDE this repository (17.6). `[bench] hidden_tests` in
karkas.toml points to a folder with `run.py`; `wavkit/hidden_tests.example/` shows the format —
copy it elsewhere, extend it, and delete the copy from the working tree before measuring.

Every run uses a fresh copy, a unique marker at the start of every prompt (no prompt cache
across runs), the same budget and models; approvals in method C count as human interventions.
By default A and B use an API key (ANTHROPIC_API_KEY, `claude --bare`). `--auth subscription`
runs them on the claude.ai login instead (the key is removed from Claude Code's environment, the
user's own settings, skills and MCP servers are left out); those costs are estimates and the
report lists them apart. Method C always needs an API key.
