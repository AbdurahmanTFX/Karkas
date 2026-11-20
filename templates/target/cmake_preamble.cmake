cmake_minimum_required(VERSION 3.20)
project({{project}} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD {{std}})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(KK_PROBES "Karkas probes on links (12.5); OFF = zero cost" ON)
enable_testing()

# --- doctest (12.1): header from karkas, the system, or FetchContent -------------------
set(KARKAS_DOCTEST_DIR "" CACHE PATH "directory that holds doctest/doctest.h (set by karkas)")
# The environment may name it too (benchmarks, CI): no download when a local copy exists.
if(NOT KARKAS_DOCTEST_DIR AND DEFINED ENV{KARKAS_DOCTEST_DIR})
  set(KARKAS_DOCTEST_DIR "$ENV{KARKAS_DOCTEST_DIR}")
endif()
add_library(kk_doctest INTERFACE)
if(KARKAS_DOCTEST_DIR)
  target_include_directories(kk_doctest SYSTEM INTERFACE "${KARKAS_DOCTEST_DIR}")
else()
  find_package(doctest CONFIG QUIET)
  if(NOT doctest_FOUND)
    include(FetchContent)
    FetchContent_Declare(doctest
      URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
      URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70)
    FetchContent_MakeAvailable(doctest)
  endif()
  target_link_libraries(kk_doctest INTERFACE doctest::doctest)
endif()
# Written only when its text changes: a newer file would rebuild every test program at each configure.
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/kk_doctest_main.cpp" CONTENT
     "#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN\n#include <doctest/doctest.h>\n")
add_library(kk_doctest_main STATIC "${CMAKE_BINARY_DIR}/kk_doctest_main.cpp")
target_link_libraries(kk_doctest_main PUBLIC kk_doctest)

function(kk_probes target)
  if(KK_PROBES)
    target_compile_definitions(${target} PRIVATE KK_PROBES=1)
  else()
    target_compile_definitions(${target} PRIVATE KK_PROBES=0)
  endif()
endfunction()
