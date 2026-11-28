# Installing and packaging (docs/adr/0046): two packages, one per install component.
#   karkas-gui — the application: karkas-gui and the karkas CLI it runs (chat, run, MCP), the icon,
#                on Linux the .desktop entry and install.sh / uninstall.sh;
#   karkas-cli — the karkas CLI alone.
# Linux: .tar.gz; Windows (MinGW): .zip.  cmake --workflow --preset package-linux | package-windows
# A plain `cmake --install` puts both programs, the .desktop entry and the icons under the prefix.
include(GNUInstallDirs)

# Windows: the icon, the version and the manifest (UTF-8 code page) in each .exe; the window without a console.
if(WIN32)
  function(kk_windows_resources target description)
    set(KK_RC_NAME ${target})
    set(KK_RC_DESCRIPTION "${description}")
    set(KK_RC_ICON "${PROJECT_SOURCE_DIR}/packaging/icons/karkas.ico")
    set(KK_RC_MANIFEST "${PROJECT_SOURCE_DIR}/packaging/windows/karkas.manifest")
    configure_file("${PROJECT_SOURCE_DIR}/packaging/windows/karkas.rc.in" "${PROJECT_BINARY_DIR}/rc/${target}.rc" @ONLY)
    target_sources(${target} PRIVATE "${PROJECT_BINARY_DIR}/rc/${target}.rc")
  endfunction()
  kk_windows_resources(karkas "Karkas command line")
  if(TARGET karkas-gui)
    kk_windows_resources(karkas-gui "Karkas")
    set_target_properties(karkas-gui PROPERTIES WIN32_EXECUTABLE ON)
  endif()
endif()

# Linux packages run on glibc 2.38 and newer: the math functions glibc 2.43 re-versioned are bound to
# their old versions (packaging/linux/glibc_compat.c).
if(KARKAS_STATIC_RUNTIME AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  foreach(t karkas karkas-gui)
    if(TARGET ${t})
      target_sources(${t} PRIVATE "${PROJECT_SOURCE_DIR}/packaging/linux/glibc_compat.c")
      set_source_files_properties("${PROJECT_SOURCE_DIR}/packaging/linux/glibc_compat.c" TARGET_DIRECTORY ${t}
                                  PROPERTIES COMPILE_OPTIONS "-fno-lto")
      target_link_options(${t} PRIVATE "LINKER:--wrap=acosf,--wrap=sqrtf")
    endif()
  endforeach()
endif()

if(WIN32)
  set(_kk_bin .)  # the .exe files at the top of the folder, next to README.txt
else()
  set(_kk_bin ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS karkas RUNTIME DESTINATION ${_kk_bin} COMPONENT cli)
if(TARGET karkas-gui)
  install(TARGETS karkas-gui RUNTIME DESTINATION ${_kk_bin} COMPONENT gui)
  install(TARGETS karkas RUNTIME DESTINATION ${_kk_bin} COMPONENT gui)
  if(NOT WIN32)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/karkas.desktop"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT gui)
    foreach(s 16 24 32 48 64 128 256 512)
      install(FILES "${PROJECT_SOURCE_DIR}/packaging/icons/karkas-${s}.png"
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${s}x${s}/apps RENAME karkas.png COMPONENT gui)
    endforeach()
  endif()
endif()
if(NOT WIN32)
  foreach(c gui cli)
    install(PROGRAMS "${PROJECT_SOURCE_DIR}/packaging/linux/install.sh" "${PROJECT_SOURCE_DIR}/packaging/linux/uninstall.sh"
            DESTINATION . COMPONENT ${c} EXCLUDE_FROM_ALL)
  endforeach()
endif()
foreach(c gui cli)
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/README-${c}.txt" DESTINATION . RENAME README.txt COMPONENT ${c} EXCLUDE_FROM_ALL)
  install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . COMPONENT ${c} EXCLUDE_FROM_ALL)  # GNU AGPL-3.0
endforeach()

# `packages`: each component installed into a folder named like its archive, then archived
# (cmake/MakePackages.cmake) into build/<preset>/packages.
if(WIN32)
  set(_kk_os windows)
  set(_kk_format zip)
else()
  set(_kk_os linux)
  set(_kk_format tar.gz)
endif()
set(_kk_pkg_deps karkas)
if(TARGET karkas-gui)
  list(APPEND _kk_pkg_deps karkas-gui)
  set(_kk_components gui cli)
else()
  set(_kk_components cli)
endif()
string(REPLACE ";" "," _kk_components_arg "${_kk_components}")
add_custom_target(packages
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DOUT_DIR=${PROJECT_BINARY_DIR}/packages"
          "-DCOMPONENTS=${_kk_components_arg}" "-DSUFFIX=${KARKAS_VERSION}-${_kk_os}-x86_64" "-DFORMAT=${_kk_format}"
          -P "${PROJECT_SOURCE_DIR}/cmake/MakePackages.cmake"
  DEPENDS ${_kk_pkg_deps}
  USES_TERMINAL
  COMMENT "Packaging Karkas (${_kk_components_arg})")
