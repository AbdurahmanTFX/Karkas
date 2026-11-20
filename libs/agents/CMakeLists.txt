kk_add_library(agents
  SOURCES src/sandbox.cpp src/header_contract.cpp src/loop.cpp src/tools.cpp src/router.cpp src/testing.cpp src/changes.cpp src/edit.cpp src/engine.cpp src/bench.cpp src/brief.cpp src/mcp_turns.cpp src/launcher.cpp src/api_edit.cpp src/build_files.cpp src/prototype.cpp src/run.cpp src/notes_agent.cpp
  PUBLIC_DEPS kk_core kk_index kk_code kk_runner kk_llm kk_metrics
  PRIVATE_DEPS kk_templates nlohmann_json::nlohmann_json spdlog::spdlog)
target_compile_definitions(kk_agents PRIVATE KK_DOCTEST_INCLUDE_DIR="${KK_DOCTEST_INCLUDE_DIR}")

kk_add_tests(agents
  SOURCES tests/scripted.cpp tests/router_test.cpp tests/testing_test.cpp tests/brief_test.cpp tests/sandbox_test.cpp tests/imported_test.cpp tests/loop_test.cpp tests/engine_test.cpp tests/changes_test.cpp tests/edit_test.cpp tests/bench_test.cpp tests/mcp_mode_test.cpp tests/economy_test.cpp tests/build_files_test.cpp tests/scope_test.cpp tests/prototype_test.cpp tests/run_test.cpp tests/notes_agent_test.cpp
  DEPS nlohmann_json::nlohmann_json
  SLOW "engine: real build — the ring wraparound bug is found by descent and fixed (stage 6/7 acceptance)"
       "bench: method C on S3 runs the pipeline, builds and runs the hidden tests (scripted model)"
       "bench: method B on S3 with a subscription prepares Karkas and hides the API key (fake claude)")
