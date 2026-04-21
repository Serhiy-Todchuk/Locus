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
find_package(cpr CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(Llama CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)

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

# PDFium prebuilt binaries (BSD-3-Clause).  Not in vcpkg mainline; pulled
# from bblanchon/pdfium-binaries as a pinned release.  Ships as a DLL +
# import lib + headers.  The DLL is copied next to locus.exe at build time.
FetchContent_Declare(pdfium
    URL       https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F6996/pdfium-win-x64.tgz
    URL_HASH  SHA256=83e714c302ceacccf403826d5cb57ea39b77f393d83b8d5781283012774a9378
    SOURCE_SUBDIR ${TS_NO_CMAKE}
)
FetchContent_MakeAvailable(pdfium)

# PDFium imported target — headers, import lib, DLL path.
add_library(pdfium SHARED IMPORTED)
set_target_properties(pdfium PROPERTIES
    IMPORTED_LOCATION             "${pdfium_SOURCE_DIR}/bin/pdfium.dll"
    IMPORTED_IMPLIB               "${pdfium_SOURCE_DIR}/lib/pdfium.dll.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${pdfium_SOURCE_DIR}/include"
)
set(PDFIUM_DLL "${pdfium_SOURCE_DIR}/bin/pdfium.dll" CACHE INTERNAL "")

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

# -- sqlite-vec (compiled from amalgamation) -----------------------------------

add_library(sqlite_vec STATIC
    third_party/sqlite-vec/sqlite-vec.c
)
target_compile_definitions(sqlite_vec PRIVATE SQLITE_CORE)
target_link_libraries(sqlite_vec PRIVATE unofficial::sqlite3::sqlite3)
set_target_properties(sqlite_vec PROPERTIES
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
    src/sse_parser.cpp
    src/llm_client.cpp
    src/tool_registry.cpp
    src/tools.cpp
    src/conversation.cpp
    src/system_prompt.cpp
    src/agent_core.cpp
    src/session_manager.cpp
    src/frontends/cli_frontend.cpp
    src/extractors/extractor_registry.cpp
    src/extractors/markdown_extractor.cpp
    src/extractors/html_extractor.cpp
    src/extractors/pdf_extractor.cpp
    src/extractors/docx_extractor.cpp
    src/extractors/xlsx_extractor.cpp
    src/extractors/zip_reader.cpp
    src/embedder.cpp
    src/chunker.cpp
    src/embedding_worker.cpp
)

target_include_directories(locus_core PUBLIC src third_party/sqlite-vec)

target_link_libraries(locus_core PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    efsw::efsw
    unofficial::tree-sitter::tree-sitter
    ts_grammars
    cpr::cpr
    llama
    sqlite_vec
    pdfium
    miniz::miniz
    pugixml::pugixml
)

# -- Main executable -----------------------------------------------------------

add_executable(locus
    src/main.cpp
)

target_link_libraries(locus PRIVATE
    locus_core
)

# Copy pdfium.dll next to locus.exe so it loads at runtime.
add_custom_command(TARGET locus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_DLL}" "$<TARGET_FILE_DIR:locus>"
)

# -- GUI executable (wxWidgets) ------------------------------------------------

find_package(wxWidgets CONFIG REQUIRED COMPONENTS core base aui stc webview)
find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(md4c CONFIG REQUIRED)

add_executable(locus_gui WIN32
    src/gui/locus_app.cpp
    src/gui/locus_frame.cpp
    src/gui/locus_tray.cpp
    src/gui/wx_frontend.cpp
    src/gui/activity_panel.cpp
    src/gui/chat_panel.cpp
    src/gui/slash_popup.cpp
    src/gui/tool_approval_panel.cpp
    src/gui/ask_user_dialog.cpp
    src/gui/compaction_dialog.cpp
    src/gui/file_tree_panel.cpp
    src/gui/settings_dialog.cpp
    src/gui/markdown.cpp
    src/gui/autostart.cpp
)

target_link_libraries(locus_gui PRIVATE
    locus_core
    wx::core wx::base wx::aui wx::stc wx::webview
    unofficial::webview2::webview2
    md4c::md4c-html
)

add_custom_command(TARGET locus_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_DLL}" "$<TARGET_FILE_DIR:locus_gui>"
)

# -- Tests ---------------------------------------------------------------------

enable_testing()
add_subdirectory(tests)
