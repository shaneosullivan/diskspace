cmake_minimum_required(VERSION 3.20)
project(diskspace-linux CXX)

# CMake here rather than the hand-written batch file the Windows build uses,
# because on Linux it is what finds the X libraries. Nothing else about it is
# clever.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# -O2 rather than -O3: measured no faster here, and the scan is bound by the
# kernel's directory reads rather than by anything the optimiser can reach.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(SHARED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../core)
set(CLI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../cli)

# The platform-neutral half, shared with the Windows build unchanged.
set(SHARED_SOURCES
  ${SHARED_DIR}/node_arena.cpp
  ${SHARED_DIR}/file_kind.cpp
  ${SHARED_DIR}/queues.cpp
  ${SHARED_DIR}/tree_scanner.cpp
  ${SHARED_DIR}/json.cpp
)

# What only this platform has.
set(PLATFORM_SOURCES
  src/platform/posix_util.cpp
  src/core/tree_scanner_posix.cpp
  src/core/scan_history_posix.cpp
)

add_library(diskspace_core STATIC ${SHARED_SOURCES} ${PLATFORM_SOURCES})
target_include_directories(diskspace_core PUBLIC
  ${SHARED_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
)

find_package(Threads REQUIRED)
target_link_libraries(diskspace_core PUBLIC Threads::Threads)

# statx and getdents64 both need the GNU extensions visible.
target_compile_definitions(diskspace_core PUBLIC _GNU_SOURCE)

add_executable(diskscan ${CLI_DIR}/diskscan.cpp)
# For product.h, so `diskscan --version` answers with the same number the
# application and the packaging script read.
target_include_directories(diskscan PRIVATE src/app)
target_link_libraries(diskscan PRIVATE diskspace_core)

# ---- the application ---------------------------------------------------------

find_package(PkgConfig REQUIRED)

# Xlib, Xft and what Xft is built on. Every one of these is a dependency of any
# X desktop already - Xfce and GTK 3 both pull all four - which is the whole
# point of the arrangement: see docs/linux-x11-plan.md.
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(XRENDER REQUIRED IMPORTED_TARGET xrender)
# Already in the process - freetype links it for colour fonts - so asking for it
# by name adds a line here and nothing at runtime.
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

# ---- the icon, compiled in ---------------------------------------------------
#
# The application is one file that needs nothing beside it, so the icon cannot be
# something it opens at a path: an AppImage is mounted somewhere different every
# launch. The 150px source rather than the 300px one, because it is drawn at 64
# and a quarter of the bytes is a quarter of the binary growth.

set(ICON_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/../assets/images/logo_150x150.png)
set(ICON_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/app_icon_png.h)
add_custom_command(
  OUTPUT ${ICON_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
  COMMAND ${CMAKE_COMMAND} -DINPUT=${ICON_SOURCE} -DOUTPUT=${ICON_HEADER}
          -DSYMBOL=kAppIconPng
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
  DEPENDS ${ICON_SOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
  COMMENT "Embedding the application icon"
  VERBATIM)
add_custom_target(app_icon_header DEPENDS ${ICON_HEADER})

# The treemap layout is shared with the Windows build; only the blit differs, and
# on a little-endian machine the renderer's 0x00RRGGBB pixels are already exactly
# what an X image of a 24-bit true colour visual expects.
add_executable(diskspace
  ${SHARED_DIR}/treemap.cpp
  src/ui/x11.cpp
  src/app/main.cpp
  src/app/window.cpp
  src/app/widgets.cpp
  src/app/tree_pane.cpp
  src/app/treemap_area.cpp
  src/app/kind_pane.cpp
  src/app/folder_chooser.cpp
  src/app/preferences_window.cpp
  src/app/scan_controller.cpp
  src/app/format.cpp
  src/app/theme.cpp
  src/app/icon.cpp
  src/app/shell.cpp
  src/app/preferences.cpp
  src/app/update_check.cpp
)
add_dependencies(diskspace app_icon_header)
target_include_directories(diskspace PRIVATE src/app src/ui
                                             ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(diskspace PRIVATE diskspace_core PkgConfig::X11 PkgConfig::XFT
                                        PkgConfig::FONTCONFIG PkgConfig::XRENDER
                                        PkgConfig::PNG)

# ---- installation ------------------------------------------------------------
#
# Needed by flatpak-builder, which builds into a prefix and then packages
# whatever was installed into it. Useful on its own too: `cmake --install` puts
# the application where a desktop expects to find it.

include(GNUInstallDirs)

set(APP_ID com.chofter.DiskSpace)

install(TARGETS diskspace diskscan RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES packaging/${APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(FILES packaging/${APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# The icon comes from the same source the other two platforms draw theirs from,
# so all three show the same picture.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../assets/images/logo_300x300.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME ${APP_ID}.png)
