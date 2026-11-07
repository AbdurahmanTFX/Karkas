add_executable(karkas-gui main.cpp worker.cpp ui.cpp png.cpp)
target_link_libraries(karkas-gui PRIVATE kk_core kk_index kk_code kk_agents kk_layout kk_metrics
                      kk_imgui kk_imgui_node_editor kk_imgui_text_edit spdlog::spdlog Threads::Threads)
target_compile_definitions(karkas-gui PRIVATE KARKAS_VERSION="${PROJECT_VERSION}")
kk_set_warnings(karkas-gui)
