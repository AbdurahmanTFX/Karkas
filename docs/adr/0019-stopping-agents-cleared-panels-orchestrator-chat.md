# 0019 — Stopping agents, clearing the panels, the orchestrator chat

Status: accepted (addition to 9.4, 13.2/13.3, 14.4; not in the spec)

## Context

The GUI showed agents at work but could not stop them: a run that went the wrong way kept spending
money until its step, token or time limit. The only way was Ctrl+C in the terminal of the command,
which killed the process mid-step (the journal had no run_end, the active marker stayed, the
ticket stayed `descending`). The bottom panels (tickets, agents, build & tests, costs) only grow;
old rows of finished work hide the current ones. And every task had to be typed in a terminal as
`karkas task "..."` while the GUI was open next to it.

## Decision

Stopping (kk_core, kk_llm, kk_metrics, kk_agents, CLI):
- `core::request_cancel()` — a process-wide flag, async-signal-safe; `core::CancelScope` — a check
  per thread; `core::cancelled()` asks both. The agent loop opens a scope for its run that looks
  at the run's stop file `.karkas/runs/active/<journal>.stop` (`metrics::request_stop`).
- The loop checks before every step and ends with outcome `cancelled` (journal: a `cancelled`
  step, `end`, `run_end`; marker and stop file removed). The curl transport aborts a request in
  flight through its progress callback (about once a second, so a long answer does not hold the
  stop); the retry back-off sleeps in slices and gives up.
- The engine never retries or escalates a cancelled agent. A ticket gets a note "stopped by the
  user at <block> (<role>)" and goes back to `open` (`karkas ticket run` continues it); the command
  fails with code `cancelled` (exit 130). `implement` skips the leaves not started yet and stops
  between phases; `expand --all` stops between blocks.
- `karkas` agent commands: the first SIGINT/SIGTERM sets the flag (graceful), a second one ends
  the process at once. Active markers carry the pid (stale markers of dead processes are dropped).
- Not stopped from inside: a build or test run already started by a tool finishes (bounded by the
  build/test timeouts); then the loop sees the stop.

GUI, agents tab: per running agent "Stop" (its stop file: only this agent), "Stop command" (SIGINT
to the pid of its process, like Ctrl+C: all its agents) and, when the command still runs 5 s
later, "Kill" (SIGKILL).

Clearing the panels (display only): a row's × button or the panel's "Clear list" (with a
confirmation) hides tickets, runs (agents and costs) and test results; "Restore (N)" shows them
again. Stored in `.karkas/cleared.json` (in .karkas/.gitignore): ticket id → time of the ticket
file, journal names, block → time of the test result, and "everything up to" times per panel. A
row with newer activity (the ticket file changed, tests ran again) shows again. Ticket files, run
journals, test results, `.karkas/karkas.log` and `karkas cost` are untouched. Drift is the state of
the code now and cannot be cleared.

Orchestrator chat (window "Orchestrator", Ctrl+Shift+O, activity bar): a message becomes exactly
the command a user would type and runs as a child process of the GUI with `--project <root>`:
`task "<text>"`, or `debug "<text>"` in bug mode, or `/<command> args` for the agent commands
(task, debug, ticket, ask, approve, change, implement, expand, design, test, check, cost, view —
not gui, mcp, bench, init, import, edit, index, gen, llm). So the protocol is the CLI's: ticket,
descent by the orchestrator and navigators, workers, tests, changes for approval. Progress lines
(stderr "· ...") stream in, log lines without time stamps, the result (stdout, the ticket) shows
as KSL with next steps: open the ticket, Continue (`/ticket run`), show / approve the C-NNNN it
names. Stop = SIGINT to the command, Kill = SIGKILL to its process group (it runs in its own
group). One command at a time. The conversation is kept in `.karkas/chat.jsonl` (in
.karkas/.gitignore; "clear" removes only it). Closing the GUI stops a running command (SIGINT,
SIGKILL after 5 s). The CLI is found next to karkas-gui, in `../cli/` of the build tree, or on PATH.

## Consequences

- A stop costs at most the current request (aborted) and leaves complete journals and an open
  ticket with the reason on its trail.
- The chat adds no new way of running agents: whatever works in the terminal works in the chat,
  and its tickets, journals and costs are the same files.
- `TestRow` carries the time of the test run (needed to show a cleared result again).
- Tests: cancel scopes; no retry after a stop; stop files and markers; the loop ends with
  `cancelled`; a stopped worker and a SIGINT during a navigator end the descent with the ticket
  open and nothing escalated; cleared rows (hide, reappear, restore, nothing but cleared.json
  written); the chat's commands, streaming, persistence, Stop and Kill with a fake `karkas`.
- Checked by hand: `karkas task` against a model server that never answers stops in < 0.1 s on
  SIGINT (exit 130, ticket open, journal `cancelled`); in karkas-gui under Xvfb a chat task, the
  agents tab Stop (stop file) and Stop command, removing runs and clearing a panel.
