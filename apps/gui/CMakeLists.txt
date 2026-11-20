add_executable(karkas-gui main.cpp worker.cpp ui.cpp png.cpp i18n.cpp settings.cpp script.cpp picker.cpp
                          theme.cpp icons.cpp widgets.cpp ui_shell.cpp ui_models.cpp cleared.cpp chat.cpp
                          ui_panels.cpp ui_chat.cpp feed.cpp cursors.cpp selectable.cpp canvas_fit.cpp
                          viewports.cpp ui_prototype.cpp)
target_link_libraries(karkas-gui PRIVATE kk_core kk_index kk_code kk_agents kk_layout kk_metrics
                      kk_imgui kk_imgui_node_editor kk_imgui_text_edit spdlog::spdlog Threads::Threads
                      nlohmann_json::nlohmann_json)
target_compile_definitions(karkas-gui PRIVATE KARKAS_VERSION="${PROJECT_VERSION}")
kk_set_warnings(karkas-gui)

# Translations and settings (no window needed).
if(KARKAS_TESTS)
  add_executable(kk_gui_tests tests/i18n_test.cpp tests/cleared_test.cpp tests/chat_test.cpp tests/feed_test.cpp tests/cursors_test.cpp i18n.cpp settings.cpp
                            cleared.cpp chat.cpp feed.cpp cursors.cpp)
  target_include_directories(kk_gui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(kk_gui_tests PRIVATE kk_core kk_index kk_test_support nlohmann_json::nlohmann_json
                      Threads::Threads)
  target_compile_definitions(kk_gui_tests PRIVATE KK_GUI_SRC="${CMAKE_CURRENT_SOURCE_DIR}")
  kk_set_warnings(kk_gui_tests)
  doctest_discover_tests(kk_gui_tests TEST_PREFIX "gui/" PROPERTIES LABELS gui WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
