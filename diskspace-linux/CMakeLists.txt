cmake_minimum_required(VERSION 3.20)
project(diskspace-linux CXX)

# CMake here rather than the hand-written batch file the Windows build uses,
# because on Linux it is what finds GTK. Nothing else about it is clever.

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
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

# The treemap layout is shared with the Windows build; only the blit differs, and
# on a little-endian machine the renderer's 0x00RRGGBB pixels are already exactly
# what Cairo calls RGB24.
add_executable(diskspace
  ${SHARED_DIR}/treemap.cpp
  src/app/main.cpp
  src/app/window.cpp
  src/app/tree_pane.cpp
  src/app/treemap_area.cpp
  src/app/scan_controller.cpp
  src/app/format.cpp
  src/app/theme.cpp
  src/app/kind_pane.cpp
  src/app/preferences.cpp
  src/app/preferences_window.cpp
  src/app/update_check.cpp
)
target_include_directories(diskspace PRIVATE src/app)
target_link_libraries(diskspace PRIVATE diskspace_core PkgConfig::GTK4)
