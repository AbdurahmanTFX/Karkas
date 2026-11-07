# 0005 — Claude API and Claude Code details checked at implementation time

Status: implemented

- API version header `anthropic-version: 2026-10-21` (the current one; 9.8 named 2023-06-01).
  It rejects temperature/top_p/top_k, so Karkas never sends sampling parameters.
- Prompt caching: `cache_control {"type":"ephemeral"}` (+ `"ttl":"1h"`), at most 4 markers per
  request (Karkas keeps the first and moves the last three), no beta header.
- Prices: cache reads of some models are not 0.1× input (e.g. 0.05×), so [prices] accepts
  absolute per-model `cache_read`, `cache_write_5m`, `cache_write_1h` next to the multipliers.
- Server-side refusal fallback (`fallbacks: "default"`, beta header
  `server-side-fallback-2026-07-01`) is enabled by default for model families that support it
  (`[models.<tier>] fallbacks = "auto" | "default" | "off"`); a final `refusal` stop reason is
  treated as a failed run and escalates.
- Forced tool choice is not used (rejected by current models); the loop nudges once when a
  model answers in prose.
- Claude Code for benchmarks (17.6): this version has no `--max-turns`, so runs are limited
  by `--max-budget-usd`; `--bare` keeps an explicit `--mcp-config` (method B with
  `--strict-mcp-config`); `--permission-mode bypassPermissions` because every run works in a
  throwaway copy.
