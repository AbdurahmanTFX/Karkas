# Karkas — instructions for the coding agent

Spec: docs/SPEC.txt. Read sections 0 and 20 first, then the section
of the current stage. Current stage and status: docs/progress.md.

Build:  cmake --preset dev && cmake --build --preset dev
Test:   ctest --preset dev
One lib: ctest --preset dev -L core

Rules:
- Work stage by stage. Small steps; after each step the build and
  tests are green and the change is committed.
- C++23, namespace kk::<module>, no exceptions across module
  boundaries (std::expected).
- No new dependencies without docs/adr/NNNN-*.md.
- Deterministic first: never call an LLM where an algorithm works.
- Tests for agent logic use recorded LLM responses (replay); no
  network in tests.
- In MCP mode never write to stdout except protocol messages.
- Keep headers light: nlohmann/json_fwd.hpp in headers.
- At the end of each stage update docs/progress.md; record any
  deviation from the spec in docs/adr/.

Layout notes:
- libs/<lib>/{include/kk/<lib>, src, tests, block.ksl}; apps/cli, apps/gui.
- templates/prompts and templates/target are embedded into the binary
  (kk_templates); edit the files, the build re-embeds them.
- Golden files: tests/golden; regenerate with KK_UPDATE_GOLDEN=1 and
  review the diff before committing.
- Fixtures: tests/fixtures/wavkit is the reference target project.
