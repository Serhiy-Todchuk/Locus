cmake_minimum_required(VERSION 4.0)
project(locus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match static CRT used by vcpkg x64-windows-static triplet
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MSVC STL headers (17.7+, including VS 2026) reference SIMD intrinsics
# (__std_find_last_not_ch_pos_1, etc.) absent from libcpmt.lib (static CRT).
add_compile_definitions(_USE_STD_VECTOR_ALGORITHMS=0)

# -- Dependencies --------------------------------------------------------------

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(efsw CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)

# -- Core library (shared between main exe and tests) --------------------------

add_library(locus_core STATIC
    src/workspace.cpp
    src/database.cpp
    src/file_watcher.cpp
)

target_include_directories(locus_core PUBLIC src)

target_link_libraries(locus_core PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    efsw::efsw
)

# -- Main executable -----------------------------------------------------------

add_executable(locus
    src/main.cpp
)

target_link_libraries(locus PRIVATE
    locus_core
)

# -- Tests ---------------------------------------------------------------------

enable_testing()
add_subdirectory(tests)
