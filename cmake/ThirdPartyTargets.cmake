# Own CMake targets for third-party sources whose CMakeLists are not used (18):
# tree-sitter-cpp grammar, Dear ImGui (docking), imgui-node-editor, ImGuiColorTextEdit.
# They are built once as static libraries without Karkas warnings.

add_library(kk_ts_cpp STATIC
  "${tree_sitter_cpp_SOURCE_DIR}/src/parser.c"
  "${tree_sitter_cpp_SOURCE_DIR}/src/scanner.c")
target_include_directories(kk_ts_cpp PRIVATE "${tree_sitter_cpp_SOURCE_DIR}/src")
set_target_properties(kk_ts_cpp PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(kk_ts_cpp PRIVATE -w -O2)
target_link_libraries(kk_ts_cpp PUBLIC kk_tree_sitter)

if(KARKAS_GUI)
  set(_imgui "${imgui_SOURCE_DIR}")
  add_library(kk_imgui STATIC
    "${_imgui}/imgui.cpp" "${_imgui}/imgui_draw.cpp" "${_imgui}/imgui_tables.cpp"
    "${_imgui}/imgui_widgets.cpp" "${_imgui}/imgui_demo.cpp"
    "${_imgui}/misc/cpp/imgui_stdlib.cpp"
    "${_imgui}/backends/imgui_impl_glfw.cpp" "${_imgui}/backends/imgui_impl_opengl3.cpp")
  target_include_directories(kk_imgui SYSTEM PUBLIC "${_imgui}" "${_imgui}/backends" "${_imgui}/misc/cpp")
  target_link_libraries(kk_imgui PUBLIC glfw OpenGL::GL)
  target_compile_options(kk_imgui PRIVATE -w)

  set(_ned "${imgui_node_editor_SOURCE_DIR}")
  add_library(kk_imgui_node_editor STATIC
    "${_ned}/imgui_node_editor.cpp" "${_ned}/imgui_node_editor_api.cpp"
    "${_ned}/imgui_canvas.cpp" "${_ned}/crude_json.cpp")
  target_include_directories(kk_imgui_node_editor SYSTEM PUBLIC "${_ned}")
  target_link_libraries(kk_imgui_node_editor PUBLIC kk_imgui)
  target_compile_options(kk_imgui_node_editor PRIVATE -w)

  set(_cte "${imgui_color_text_edit_SOURCE_DIR}")
  add_library(kk_imgui_text_edit STATIC "${_cte}/TextEditor.cpp")
  target_include_directories(kk_imgui_text_edit SYSTEM PUBLIC "${_cte}")
  target_link_libraries(kk_imgui_text_edit PUBLIC kk_imgui)
  target_compile_options(kk_imgui_text_edit PRIVATE -w)
endif()
