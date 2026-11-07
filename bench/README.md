# Karkas benchmarks (section 17)

    karkas bench run --scenario s1|s2|s3 --method A|B|C --model <name> --reps 5
    karkas bench report

- A — Claude Code alone (`claude -p ... --output-format json --bare`), no Karkas files.
- B — Claude Code + the Karkas MCP server (`--mcp-config`, `--strict-mcp-config`).
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
All methods use an API key (ANTHROPIC_API_KEY), not a subscription.
