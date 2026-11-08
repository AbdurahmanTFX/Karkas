# 0011 — Models and APIs per level of the block tree and per block

Status: accepted (deviation from spec 11.2 and appendix E)

## Context

The spec picks a model by role ([roles]) and, for workers, by routing on the size of the
task (11.2–11.4). Users want to decide which model, and which API, works on which part of
the project: a strong model for the top levels, a cheap or local one for the leaves, or a
separate API for one subsystem ("the audio blocks go to a local model").

## Decision

- karkas.toml gets two sections:
  - `[levels]`: `<level> = "<tier>"`. Level of a block = number of segments of its path
    (`game` = 1, `game.flow` = 2); level 0 = the project (orchestrator, architect of the top
    level).
  - `[blocks]`: `"<block path>" = "<tier>"`, for the block and everything inside it; the
    longest matching path wins.
- Order: own block (or nearest ancestor in [blocks]) → [levels] → [roles]/routing. It applies
  to every agent working on that block (orchestrator, navigator, worker, tester, composer,
  architect, ask). The worker journal shows `pinned by level 2` / `pinned by block game`
  instead of the routing reason.
- A pin is the starting tier: escalation after failed attempts still follows
  cheap → medium → strong. A custom tier (`local`, `deepseek`) does not escalate.
- `[models.<tier>]` gets `api_key_env`: the name of the environment variable with the key of
  that API (default ANTHROPIC_API_KEY / OPENAI_API_KEY by provider). Keys still never go into
  files (6.5): only the variable's name is stored.
- An empty value (`2 = ""`) cancels a pin made in ~/.config/karkas/config.toml.
- The GUI edits these sections in place (`core::set_config_value`): one key at a time,
  comments and order of karkas.toml are kept, an invalid result is refused.

## GUI

- Settings are a separate window (gear, Ctrl+,) with pages; "Models and API" lists the APIs
  (add/edit/delete; shows whether the key variable is set, never the key), the model of each
  level, the blocks with their own model and the role defaults.
- The schema toolbar has a model chip: the model of the level shown, and of the selected
  block; the inspector has a MODEL section for the selected block.

## Consequences

- Routing by size (11.2) still decides where nothing is pinned; self-tuning statistics only
  count routed runs.
- Prices of new models are unknown until added to [prices]; their cost shows as 0.
