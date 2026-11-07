# Karkas — progress by stage

## Stage 0 — repository skeleton
- Folder layout of 19.1, CMakeLists + CMakePresets (dev, release, asan, headless),
  cmake/{Dependencies,ThirdPartyTargets,Warnings,Sanitizers,KarkasHelpers,EmbedFiles}.cmake.
- Dependencies: system packages first (sqlite, curl, fmt, spdlog, toml++, xxhash,
  tree-sitter, glfw); FetchContent with pinned hashes for nlohmann_json, doctest,
  CLI11, reproc, tree-sitter-cpp, ImGui docking, imgui-node-editor, ImGuiColorTextEdit.
- `karkas --version`, CLAUDE.md, .clang-format, .clang-tidy, .gitignore.
- compile_commands.json is symlinked into the repo root by the dev preset.

## Stage 1 — data model and KSL (kk_core)
- Generic record lexer/parser (4.5) with file:line:col errors (S01–S07),
  typed records (project, block, ticket, report, change), canonical writer (4.8),
  comments preserved as trivia, checks E01–E08 and W01–W06 (4.6), `karkas check`.
- Tests: 71 test cases in kk_core_tests; every E and W code has a dedicated test;
  round trip of every fixture .ksl file is byte-identical.
