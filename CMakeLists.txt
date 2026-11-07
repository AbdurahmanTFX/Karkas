cmake_minimum_required(VERSION 3.29)
project(karkas VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KARKAS_GUI "Build GUI" ON)
option(KARKAS_TESTS "Build tests" ON)
option(KARKAS_SANITIZERS "ASan + UBSan" OFF)
option(KARKAS_SYSTEM_DEPS "Prefer system packages" ON)
option(KARKAS_WERROR "Treat warnings as errors (CI only)" OFF)
option(KARKAS_CCACHE "Use ccache when available" ON)
option(KARKAS_MOLD "Use mold linker when available" ON)
option(KARKAS_LINK_COMPILE_COMMANDS "Symlink compile_commands.json into the source root" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- Faster builds on weak machines (19.4) ---------------------------------
if(KARKAS_CCACHE)
  find_program(KK_CCACHE ccache)
  if(KK_CCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER "${KK_CCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${KK_CCACHE}")
  endif()
endif()
if(KARKAS_MOLD)
  find_program(KK_MOLD mold)
  if(KK_MOLD)
    set(CMAKE_LINKER_TYPE MOLD)
  else()
    message(STATUS "karkas: mold not found, using the default linker")
  endif()
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS link=1)
set(CMAKE_JOB_POOL_LINK link)

include(Warnings)
include(Sanitizers)
include(Dependencies)
include(ThirdPartyTargets)
include(KarkasHelpers)

if(KARKAS_TESTS)
  enable_testing()
endif()

# Embedded templates (prompts and target-project templates) ----------------
add_subdirectory(templates)

if(KARKAS_TESTS)
  add_subdirectory(tests/support)
endif()

# Libraries bottom-up (6.1): core → index → code/runner/llm/metrics → agents/mcp/layout.
foreach(lib core index code runner llm metrics agents mcp layout)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/${lib}/CMakeLists.txt")
    add_subdirectory(libs/${lib})
  endif()
endforeach()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/cli/CMakeLists.txt")
  add_subdirectory(apps/cli)
endif()
if(KARKAS_GUI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/gui/CMakeLists.txt")
  add_subdirectory(apps/gui)
endif()

if(KARKAS_LINK_COMPILE_COMMANDS)
  file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
       "${CMAKE_SOURCE_DIR}/compile_commands.json" SYMBOLIC)
endif()
