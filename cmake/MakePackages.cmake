# Build-time script (target `packages`, docs/adr/0046): one archive per install component.
#   cmake -DBUILD_DIR=<build> -DOUT_DIR=<dir> -DCOMPONENTS=gui,cli -DSUFFIX=<version-os-arch> -DFORMAT=tar.gz|zip
#         -P MakePackages.cmake
# karkas-<component>-<suffix>.<format>, unpacking into a folder of the same name.
string(REPLACE "," ";" COMPONENTS "${COMPONENTS}")
set(stage "${OUT_DIR}/stage")
file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY "${stage}")
foreach(c IN LISTS COMPONENTS)
  set(name "karkas-${c}-${SUFFIX}")
  execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --component ${c} --prefix "${stage}/${name}" --strip
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "install of the component ${c} failed")
  endif()
  set(archive "${OUT_DIR}/${name}.${FORMAT}")
  file(REMOVE "${archive}")
  if(FORMAT STREQUAL "zip")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive}" --format=zip "${name}" WORKING_DIRECTORY "${stage}" RESULT_VARIABLE rc)
  else()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${archive}" "${name}" WORKING_DIRECTORY "${stage}" RESULT_VARIABLE rc)
  endif()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "archiving ${name} failed")
  endif()
  message(STATUS "package: ${archive}")
endforeach()
