add_executable(karkas-gui main.cpp worker.cpp ui.cpp png.cpp i18n.cpp settings.cpp script.cpp picker.cpp
                          theme.cpp icons.cpp widgets.cpp ui_shell.cpp ui_models.cpp cleared.cpp chat.cpp
                          ui_panels.cpp ui_chat.cpp feed.cpp cursors.cpp selectable.cpp canvas_fit.cpp
                          viewports.cpp ui_prototype.cpp run.cpp ui_run.cpp ui_notes.cpp markdown_view.cpp
                          markdown_lang.cpp images.cpp image_decode.cpp drop.cpp)
target_link_libraries(karkas-gui PRIVATE kk_core kk_index kk_code kk_agents kk_layout kk_metrics
                      kk_imgui kk_imgui_node_editor kk_imgui_text_edit kk_stb_image spdlog::spdlog Threads::Threads
                      nlohmann_json::nlohmann_json)
target_compile_definitions(karkas-gui PRIVATE KARKAS_VERSION="${KARKAS_VERSION}")
kk_set_warnings(karkas-gui)

# The application icon in the window's title and task bar (ADR 0046): PNGs embedded, set through GLFW.
set(_kk_icons "")
foreach(s 16 32 48 64 128 256)
  list(APPEND _kk_icons "${PROJECT_SOURCE_DIR}/packaging/icons/karkas-${s}.png")
endforeach()
set(_kk_icon_src "${CMAKE_CURRENT_BINARY_DIR}/app_icon_data.cpp")
set(_kk_icon_list "${CMAKE_CURRENT_BINARY_DIR}/app_icon.list")
string(REPLACE ";" "\n" _kk_icons_text "${_kk_icons}")
file(CONFIGURE OUTPUT "${_kk_icon_list}" CONTENT "${_kk_icons_text}\n")
add_custom_command(OUTPUT "${_kk_icon_src}"
  COMMAND "${CMAKE_COMMAND}" "-DOUT=${_kk_icon_src}" -DNAME=app_icon_pngs "-DLIST=${_kk_icon_list}"
          -P "${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
  DEPENDS ${_kk_icons} "${_kk_icon_list}" "${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake"
  COMMENT "Embedding the Karkas icon")
target_sources(karkas-gui PRIVATE "${_kk_icon_src}")

# Translations and settings (no window needed).
if(KARKAS_TESTS)
  add_executable(kk_gui_tests tests/i18n_test.cpp tests/cleared_test.cpp tests/chat_test.cpp tests/feed_test.cpp tests/cursors_test.cpp tests/run_test.cpp tests/image_test.cpp i18n.cpp settings.cpp
                            cleared.cpp chat.cpp feed.cpp cursors.cpp run.cpp image_decode.cpp png.cpp)
  target_include_directories(kk_gui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(kk_gui_tests PRIVATE kk_core kk_index kk_test_support nlohmann_json::nlohmann_json
                      kk_stb_image Threads::Threads)
  target_compile_definitions(kk_gui_tests PRIVATE KK_GUI_SRC="${CMAKE_CURRENT_SOURCE_DIR}")
  kk_set_warnings(kk_gui_tests)
  doctest_discover_tests(kk_gui_tests TEST_PREFIX "gui/" PROPERTIES LABELS gui WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
