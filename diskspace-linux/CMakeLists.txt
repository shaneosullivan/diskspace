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
target_link_libraries(diskscan PRIVATE diskspace_core)
