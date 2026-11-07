# Embeds prompts/ and target/ into the kk_templates library (9.7, 8.3).
file(GLOB_RECURSE KK_TEMPLATE_FILES CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/prompts/*" "${CMAKE_CURRENT_SOURCE_DIR}/target/*")
list(SORT KK_TEMPLATE_FILES)
set(_abs "")
foreach(f IN LISTS KK_TEMPLATE_FILES)
  list(APPEND _abs "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
endforeach()
string(REPLACE ";" "|" _files_arg "${KK_TEMPLATE_FILES}")
set(_out "${CMAKE_CURRENT_BINARY_DIR}/kk_templates.cpp")
add_custom_command(OUTPUT "${_out}"
  COMMAND "${CMAKE_COMMAND}" "-DOUT=${_out}" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DFILES=${_files_arg}" -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
  DEPENDS ${_abs} "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
  COMMENT "Embedding Karkas templates")
add_library(kk_templates STATIC "${_out}")
add_library(karkas::templates ALIAS kk_templates)
target_include_directories(kk_templates PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(kk_templates PUBLIC cxx_std_23)
