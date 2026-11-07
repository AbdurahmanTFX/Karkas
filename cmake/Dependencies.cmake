# Third-party dependencies (section 18).
# Rule: system package first (find_package / pkg-config), otherwise FetchContent
# with a pinned version and hash. KARKAS_SYSTEM_DEPS=OFF forces FetchContent.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)
if(NOT KARKAS_SYSTEM_DEPS)
  set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
endif()
cmake_policy(SET CMP0135 NEW)

find_package(PkgConfig REQUIRED)

# --- header-only / small libraries ------------------------------------------
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  FIND_PACKAGE_ARGS 3.11 CONFIG)

FetchContent_Declare(doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
  URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70
  FIND_PACKAGE_ARGS CONFIG)

FetchContent_Declare(CLI11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.5.0.tar.gz
  URL_HASH SHA256=17e02b4cddc2fa348e5dbdbb582c59a3486fa2b2433e70a0c3bacb871334fd55
  FIND_PACKAGE_ARGS 2.4 CONFIG)

FetchContent_Declare(tomlplusplus
  URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
  URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
  FIND_PACKAGE_ARGS 3.4 CONFIG)

FetchContent_Declare(fmt
  URL https://github.com/fmtlib/fmt/archive/refs/tags/11.2.0.tar.gz
  URL_HASH SHA256=bc23066d87ab3168f27cef3e97d545fa63314f5c79df5ea444d41d56f962c6af
  FIND_PACKAGE_ARGS CONFIG)

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
  URL_HASH SHA256=15a04e69c222eb6c01094b5c7ff8a249b36bb22788d72519646fb85feb267e67
  FIND_PACKAGE_ARGS CONFIG)

set(REPROC++ ON CACHE BOOL "" FORCE)
set(REPROC_TEST OFF CACHE BOOL "" FORCE)
set(REPROC_EXAMPLES OFF CACHE BOOL "" FORCE)
set(REPROC_INSTALL OFF CACHE BOOL "" FORCE)
set(REPROC_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(reproc
  URL https://github.com/DaanDeMeyer/reproc/archive/refs/tags/v14.2.5.tar.gz
  URL_HASH SHA256=69467be0cfc80734b821c54ada263c8f1439f964314063f76b7cf256c3dc7ee8
  FIND_PACKAGE_ARGS NAMES reproc++ CONFIG)

FetchContent_MakeAvailable(nlohmann_json doctest CLI11 tomlplusplus fmt spdlog reproc)

foreach(_t reproc reproc++)
  if(TARGET ${_t})
    get_target_property(_aliased ${_t} ALIASED_TARGET)
    if(NOT _aliased)
      set_target_properties(${_t} PROPERTIES SYSTEM TRUE)
    endif()
  endif()
endforeach()
if(TARGET reproc++ AND NOT TARGET reproc++::reproc++)
  add_library(reproc++::reproc++ ALIAS reproc++)
endif()
if(NOT TARGET reproc++::reproc++ AND TARGET reproc++)
  add_library(reproc++::reproc++ ALIAS reproc++)
endif()

# doctest.cmake (doctest_discover_tests) and the header directory that Karkas
# hands to generated target projects (so they build offline).
if(doctest_SOURCE_DIR)
  list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
  set(KK_DOCTEST_INCLUDE_DIR "${doctest_SOURCE_DIR}")
else()
  list(APPEND CMAKE_MODULE_PATH "${doctest_DIR}")
  find_path(KK_DOCTEST_INCLUDE_DIR doctest/doctest.h)
endif()
include(doctest)

# --- system libraries ---------------------------------------------------------
find_package(SQLite3 REQUIRED)
if(TARGET SQLite3::SQLite3)
  set(KK_SQLITE_TARGET SQLite3::SQLite3)
else()
  set(KK_SQLITE_TARGET SQLite::SQLite3)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# xxHash (xxh3) ---------------------------------------------------------------
add_library(kk_xxhash INTERFACE)
pkg_check_modules(XXHASH IMPORTED_TARGET libxxhash)
if(XXHASH_FOUND AND KARKAS_SYSTEM_DEPS)
  target_link_libraries(kk_xxhash INTERFACE PkgConfig::XXHASH)
else()
  FetchContent_Declare(xxhash
    URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz
    URL_HASH SHA256=aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80
    SOURCE_SUBDIR kk-populate-only)
  FetchContent_MakeAvailable(xxhash)
  target_include_directories(kk_xxhash SYSTEM INTERFACE "${xxhash_SOURCE_DIR}")
  target_compile_definitions(kk_xxhash INTERFACE XXH_INLINE_ALL)
endif()

# tree-sitter runtime -----------------------------------------------------------
add_library(kk_tree_sitter INTERFACE)
pkg_check_modules(TREE_SITTER IMPORTED_TARGET tree-sitter)
if(TREE_SITTER_FOUND AND KARKAS_SYSTEM_DEPS)
  target_link_libraries(kk_tree_sitter INTERFACE PkgConfig::TREE_SITTER)
else()
  FetchContent_Declare(tree_sitter
    URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.25.10.tar.gz
    URL_HASH SHA256=ad5040537537012b16ef6e1210a572b927c7cdc2b99d1ee88d44a7dcdc3ff44c
    SOURCE_SUBDIR kk-populate-only)
  FetchContent_MakeAvailable(tree_sitter)
  add_library(kk_tree_sitter_rt STATIC "${tree_sitter_SOURCE_DIR}/lib/src/lib.c")
  target_include_directories(kk_tree_sitter_rt SYSTEM PUBLIC "${tree_sitter_SOURCE_DIR}/lib/include"
                                               PRIVATE "${tree_sitter_SOURCE_DIR}/lib/src")
  set_target_properties(kk_tree_sitter_rt PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
  target_compile_options(kk_tree_sitter_rt PRIVATE -w)
  target_link_libraries(kk_tree_sitter INTERFACE kk_tree_sitter_rt)
endif()

# tree-sitter-cpp grammar: always our own target (ThirdPartyTargets.cmake).
FetchContent_Declare(tree_sitter_cpp
  URL https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz
  URL_HASH SHA256=7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051
    SOURCE_SUBDIR kk-populate-only)
FetchContent_MakeAvailable(tree_sitter_cpp)

# Graphviz (cgraph, gvc) via pkg-config: optional. Without it kk_layout uses
# its built-in Sugiyama engine (see docs/adr/0002-layout-engine-fallback.md).
pkg_check_modules(GRAPHVIZ IMPORTED_TARGET libgvc libcgraph)
if(GRAPHVIZ_FOUND)
  message(STATUS "karkas: Graphviz found (${GRAPHVIZ_VERSION}), dot layout enabled")
else()
  message(STATUS "karkas: Graphviz not found, using the built-in Sugiyama layout")
endif()

# --- GUI ------------------------------------------------------------------------
if(KARKAS_GUI)
  find_package(OpenGL REQUIRED)
  FetchContent_Declare(glfw3
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
    FIND_PACKAGE_ARGS 3.4 CONFIG)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glfw3)

  # Pinned compatible trio (14.1): ImGui docking 1.91.9b + imgui-node-editor
  # develop@27bcace + ImGuiColorTextEdit master@ca2f9f1.
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz
    URL_HASH SHA256=466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5
    SOURCE_SUBDIR kk-populate-only)
  FetchContent_Declare(imgui_node_editor
    URL https://github.com/thedmd/imgui-node-editor/archive/27bcacec350a37db43e3afb8d37223237912577c.tar.gz
    URL_HASH SHA256=293cb219c0993e0c1ba0845875ce070a92c0652c24ee29bce40d3e56a214fced
    SOURCE_SUBDIR kk-populate-only)
  FetchContent_Declare(imgui_color_text_edit
    URL https://github.com/BalazsJako/ImGuiColorTextEdit/archive/ca2f9f1462e3b60e56351bc466acda448c5ea50d.tar.gz
    URL_HASH SHA256=ebfef53e45a6f58ab54060b056b3b3f59ff7b5c8305b5c50f79ade94d4c4efbd
    SOURCE_SUBDIR kk-populate-only)
  FetchContent_MakeAvailable(imgui)
  FetchContent_MakeAvailable(imgui_node_editor)
  FetchContent_MakeAvailable(imgui_color_text_edit)
endif()
