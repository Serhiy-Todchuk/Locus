cmake_minimum_required(VERSION 4.0)
project(locus VERSION 0.1.0 LANGUAGES CXX C)

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
find_package(unofficial-tree-sitter CONFIG REQUIRED)

# -- Tree-sitter grammars (fetched at configure time) --------------------------

include(FetchContent)

# We only need the grammar source files (parser.c, scanner.c), not their
# CMake projects.  SOURCE_SUBDIR pointing to a non-existent path prevents
# FetchContent_MakeAvailable from calling add_subdirectory on the grammar.
set(TS_NO_CMAKE "_no_cmake_")

FetchContent_Declare(tree-sitter-c
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
    GIT_TAG        v0.23.5
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-cpp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    GIT_TAG        v0.23.4
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-python
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
    GIT_TAG        v0.23.6
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-javascript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
    GIT_TAG        v0.23.1
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-typescript
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
    GIT_TAG        v0.23.2
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-go
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-go.git
    GIT_TAG        v0.23.4
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-rust
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-rust.git
    GIT_TAG        v0.23.2
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-java
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-java.git
    GIT_TAG        v0.23.5
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-c-sharp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c-sharp.git
    GIT_TAG        v0.23.1
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)

FetchContent_MakeAvailable(
    tree-sitter-c
    tree-sitter-cpp
    tree-sitter-python
    tree-sitter-javascript
    tree-sitter-typescript
    tree-sitter-go
    tree-sitter-rust
    tree-sitter-java
    tree-sitter-c-sharp
)

# Collect parser.c and optional scanner.c from each grammar
set(TS_GRAMMAR_SOURCES
    ${tree-sitter-c_SOURCE_DIR}/src/parser.c
    ${tree-sitter-cpp_SOURCE_DIR}/src/parser.c
    ${tree-sitter-python_SOURCE_DIR}/src/parser.c
    ${tree-sitter-javascript_SOURCE_DIR}/src/parser.c
    ${tree-sitter-typescript_SOURCE_DIR}/typescript/src/parser.c
    ${tree-sitter-go_SOURCE_DIR}/src/parser.c
    ${tree-sitter-rust_SOURCE_DIR}/src/parser.c
    ${tree-sitter-java_SOURCE_DIR}/src/parser.c
    ${tree-sitter-c-sharp_SOURCE_DIR}/src/parser.c
)

# Add scanner files where they exist
foreach(SCANNER_PATH
    "${tree-sitter-cpp_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-python_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-javascript_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-typescript_SOURCE_DIR}/typescript/src/scanner.c"
    "${tree-sitter-go_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-rust_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-java_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-c-sharp_SOURCE_DIR}/src/scanner.c"
)
    if(EXISTS "${SCANNER_PATH}")
        list(APPEND TS_GRAMMAR_SOURCES "${SCANNER_PATH}")
    endif()
endforeach()

add_library(ts_grammars STATIC ${TS_GRAMMAR_SOURCES})
target_include_directories(ts_grammars PRIVATE
    ${tree-sitter-c_SOURCE_DIR}/src
    ${tree-sitter-cpp_SOURCE_DIR}/src
    ${tree-sitter-python_SOURCE_DIR}/src
    ${tree-sitter-javascript_SOURCE_DIR}/src
    ${tree-sitter-typescript_SOURCE_DIR}/typescript/src
    ${tree-sitter-go_SOURCE_DIR}/src
    ${tree-sitter-rust_SOURCE_DIR}/src
    ${tree-sitter-java_SOURCE_DIR}/src
    ${tree-sitter-c-sharp_SOURCE_DIR}/src
)
target_link_libraries(ts_grammars PRIVATE unofficial::tree-sitter::tree-sitter)
set_target_properties(ts_grammars PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    C_STANDARD 11
)

# -- Core library (shared between main exe and tests) --------------------------

add_library(locus_core STATIC
    src/workspace.cpp
    src/database.cpp
    src/file_watcher.cpp
    src/indexer.cpp
    src/index_query.cpp
)

target_include_directories(locus_core PUBLIC src)

target_link_libraries(locus_core PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    efsw::efsw
    unofficial::tree-sitter::tree-sitter
    ts_grammars
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
