cmake_minimum_required(VERSION 4.0)
project(locus VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# macOS distribution: build locus_gui as a Locus.app bundle (S6.9 Stage C).
# OFF by default so the everyday dev build still produces a flat, directly
# runnable build/release/locus_gui. Turn ON for a distributable .app.
option(LOCUS_MACOS_BUNDLE "Build locus_gui as a macOS .app bundle" OFF)

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

# llama-cpp / ggml upstream CMake configs use find_library with a single
# IMPORTED_LOCATION, which breaks multi-config generators (VS picks the
# debug .lib for Release builds -> MTd/MT CRT mismatch). Patch each target
# with IMPORTED_LOCATION_{DEBUG,RELEASE} derived from the vcpkg layout.
foreach(_llama_tgt llama ggml::ggml ggml::ggml-base ggml::ggml-cpu)
    if(NOT TARGET ${_llama_tgt})
        continue()
    endif()
    get_target_property(_loc ${_llama_tgt} IMPORTED_LOCATION)
    if(NOT _loc)
        continue()
    endif()
    if(_loc MATCHES "/debug/lib/")
        set(_debug_loc "${_loc}")
        string(REPLACE "/debug/lib/" "/lib/" _release_loc "${_loc}")
    else()
        set(_release_loc "${_loc}")
        string(REPLACE "/lib/" "/debug/lib/" _debug_loc "${_loc}")
    endif()
    if(EXISTS "${_release_loc}" AND EXISTS "${_debug_loc}")
        set_property(TARGET ${_llama_tgt} PROPERTY IMPORTED_LOCATION)
        set_target_properties(${_llama_tgt} PROPERTIES
            IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
            IMPORTED_LOCATION_RELEASE        "${_release_loc}"
            IMPORTED_LOCATION_RELWITHDEBINFO "${_release_loc}"
            IMPORTED_LOCATION_MINSIZEREL     "${_release_loc}"
            IMPORTED_LOCATION_DEBUG          "${_debug_loc}"
        )
    endif()
endforeach()

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

# S4.Y -- Tier 1 + Tier 2 grammars.  Same FetchContent pattern as the
# existing nine.  Sub-grammar repos (php, markdown) keep their parser.c in
# a named subdirectory; that path leaks into TS_GRAMMAR_SOURCES below.
FetchContent_Declare(tree-sitter-ruby
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-ruby.git
    GIT_TAG        v0.23.1
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-php
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-php.git
    GIT_TAG        v0.23.11
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-bash
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash.git
    GIT_TAG        v0.23.3
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-json
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-json.git
    GIT_TAG        v0.24.8
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-yaml
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-yaml.git
    GIT_TAG        v0.7.0
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
# NOTE: SQL deferred -- the canonical SQL grammar (DerekStride/tree-sitter-sql)
# gitignores parser.c in source form and expects `tree-sitter generate` at
# build time, which would add a Rust-based CLI dep we don't want here. Other
# SQL grammars either ship a C++ scanner (m-novikov) or only target one dialect
# (sqlite). Land SQL in a follow-up stage that introduces the CLI dep cleanly.
FetchContent_Declare(tree-sitter-markdown
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-markdown.git
    GIT_TAG        v0.4.1
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-swift
    GIT_REPOSITORY https://github.com/alex-pinkus/tree-sitter-swift.git
    # Plain "0.7.x" tags omit the generated parser.c (gitignored on main); use
    # the "-with-generated-files" tag variant that vendors parser.c + scanner.c.
    GIT_TAG        0.7.2-with-generated-files
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)
FetchContent_Declare(tree-sitter-kotlin
    GIT_REPOSITORY https://github.com/fwcd/tree-sitter-kotlin.git
    GIT_TAG        0.3.8
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  ${TS_NO_CMAKE}
)

# PDFium prebuilt binaries (BSD-3-Clause).  Not in vcpkg mainline; pulled
# from bblanchon/pdfium-binaries as a pinned release.  The runtime library is
# copied next to the executable at build time.  Same chromium/6996 tag across
# platforms so PDF extraction behavior stays identical (S6.9: macOS arm64-only).
if(APPLE)
    set(PDFIUM_URL  "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F6996/pdfium-mac-arm64.tgz")
    set(PDFIUM_HASH "SHA256=1b103d5ebfd8f7b5720ca9ba76888e7efaa80678a49c8f3da3b0ac64ba5c90fb")
elseif(WIN32)
    set(PDFIUM_URL  "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F6996/pdfium-win-x64.tgz")
    set(PDFIUM_HASH "SHA256=83e714c302ceacccf403826d5cb57ea39b77f393d83b8d5781283012774a9378")
endif()
FetchContent_Declare(pdfium
    URL       ${PDFIUM_URL}
    URL_HASH  ${PDFIUM_HASH}
    SOURCE_SUBDIR ${TS_NO_CMAKE}
)
FetchContent_MakeAvailable(pdfium)

# PDFium imported target -- headers + platform runtime library.
add_library(pdfium SHARED IMPORTED)
if(APPLE)
    # macOS ships a single libpdfium.dylib (no separate import lib). bblanchon's
    # build records a CWD-relative install name ("./libpdfium.dylib") the loader
    # can't resolve from an arbitrary working dir -- rewrite it to @rpath so the
    # executable's @executable_path rpath (set below) finds the copy POST_BUILD
    # drops next to the binary. Re-sign ad-hoc after: arm64 rejects a dylib whose
    # signature install_name_tool just invalidated.
    execute_process(COMMAND install_name_tool -id @rpath/libpdfium.dylib
                            "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib")
    execute_process(COMMAND codesign --force --sign -
                            "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib")
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION             "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib"
        INTERFACE_INCLUDE_DIRECTORIES "${pdfium_SOURCE_DIR}/include"
    )
    set(PDFIUM_RUNTIME_LIB "${pdfium_SOURCE_DIR}/lib/libpdfium.dylib" CACHE INTERNAL "")
elseif(WIN32)
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION             "${pdfium_SOURCE_DIR}/bin/pdfium.dll"
        IMPORTED_IMPLIB               "${pdfium_SOURCE_DIR}/lib/pdfium.dll.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${pdfium_SOURCE_DIR}/include"
    )
    set(PDFIUM_RUNTIME_LIB "${pdfium_SOURCE_DIR}/bin/pdfium.dll" CACHE INTERNAL "")
endif()

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
    tree-sitter-ruby
    tree-sitter-php
    tree-sitter-bash
    tree-sitter-json
    tree-sitter-yaml
    tree-sitter-markdown
    tree-sitter-swift
    tree-sitter-kotlin
)

# Collect parser.c and optional scanner.c from each grammar.
# Sub-grammar repos:
#   tree-sitter-php       -> php/src/parser.c (the embedded `php` grammar)
#   tree-sitter-markdown  -> tree-sitter-markdown/src/parser.c (block parser)
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
    ${tree-sitter-ruby_SOURCE_DIR}/src/parser.c
    ${tree-sitter-php_SOURCE_DIR}/php/src/parser.c
    ${tree-sitter-bash_SOURCE_DIR}/src/parser.c
    ${tree-sitter-json_SOURCE_DIR}/src/parser.c
    ${tree-sitter-yaml_SOURCE_DIR}/src/parser.c
    ${tree-sitter-markdown_SOURCE_DIR}/tree-sitter-markdown/src/parser.c
    ${tree-sitter-swift_SOURCE_DIR}/src/parser.c
    ${tree-sitter-kotlin_SOURCE_DIR}/src/parser.c
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
    "${tree-sitter-ruby_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-php_SOURCE_DIR}/php/src/scanner.c"
    "${tree-sitter-bash_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-yaml_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-markdown_SOURCE_DIR}/tree-sitter-markdown/src/scanner.c"
    "${tree-sitter-swift_SOURCE_DIR}/src/scanner.c"
    "${tree-sitter-kotlin_SOURCE_DIR}/src/scanner.c"
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
    ${tree-sitter-ruby_SOURCE_DIR}/src
    ${tree-sitter-php_SOURCE_DIR}/php/src
    ${tree-sitter-bash_SOURCE_DIR}/src
    ${tree-sitter-json_SOURCE_DIR}/src
    ${tree-sitter-yaml_SOURCE_DIR}/src
    ${tree-sitter-markdown_SOURCE_DIR}/tree-sitter-markdown/src
    ${tree-sitter-swift_SOURCE_DIR}/src
    ${tree-sitter-kotlin_SOURCE_DIR}/src
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
    src/core/workspace.cpp
    src/core/workspace_config_json.cpp
    src/core/watcher_pump.cpp
    src/core/workspace_lock.cpp
    src/core/locus_session.cpp
    src/core/locus_tab.cpp
    src/core/workspace_ui_state.cpp
    src/core/database.cpp
    src/core/file_watcher.cpp
    src/core/global_config.cpp
    src/core/global_paths.cpp
    src/core/log_channels.cpp
    src/core/memory_store.cpp
    src/core/memory_filter.cpp
    src/index/indexer.cpp
    src/index/index_query.cpp
    src/index/chunker.cpp
    src/index/gitignore.cpp
    src/index/tree_sitter_registry.cpp
    src/index/symbol_extractor.cpp
    src/index/symbol_extractors/c_extractor.cpp
    src/index/symbol_extractors/cpp_extractor.cpp
    src/index/symbol_extractors/python_extractor.cpp
    src/index/symbol_extractors/js_ts_extractor.cpp
    src/index/symbol_extractors/go_extractor.cpp
    src/index/symbol_extractors/rust_extractor.cpp
    src/index/symbol_extractors/java_extractor.cpp
    src/index/symbol_extractors/csharp_extractor.cpp
    src/index/symbol_extractors/ruby_extractor.cpp
    src/index/symbol_extractors/php_extractor.cpp
    src/index/symbol_extractors/bash_extractor.cpp
    src/index/symbol_extractors/swift_extractor.cpp
    src/index/symbol_extractors/kotlin_extractor.cpp
    src/index/prepared_statements.cpp
    src/index/embedder.cpp
    src/index/embedding_worker.cpp
    src/index/reranker.cpp
    src/llm/sse_parser.cpp
    src/llm/json_repair.cpp
    src/llm/tool_call_grammar.cpp
    src/llm/llm_client.cpp
    src/llm/endpoint_profile.cpp
    src/llm/openai_transport.cpp
    src/llm/xml_tool_call_extractor.cpp
    src/llm/stream_decoders/openai_decoder.cpp
    src/llm/stream_decoders/qwen_xml_decoder.cpp
    src/llm/stream_decoders/claude_xml_decoder.cpp
    src/llm/stream_decoders/auto_decoder.cpp
    src/llm/llm_router.cpp
    src/llm/token_counter.cpp
    src/llm/model_presets.cpp
    src/tools/tool_registry.cpp
    src/tools/shared.cpp
    src/tools/command_path_scanner.cpp
    src/tools/permission_presets.cpp
    src/tools/file_tools.cpp
    src/tools/search_tools.cpp
    src/tools/index_tools.cpp
    src/tools/process_tools.cpp
    src/tools/process_output_filter.cpp
    src/tools/truncation_detector.cpp
    src/tools/interactive_tools.cpp
    src/tools/plan_tools.cpp
    src/tools/memory_tools.cpp
    src/tools/describe_tool.cpp
    src/tools/tools.cpp
    src/tools/process_registry.cpp
    src/mcp/json_rpc.cpp
    src/mcp/mcp_config.cpp
    src/mcp/stdio_transport.cpp
    src/mcp/mcp_client.cpp
    src/mcp/mcp_tool.cpp
    src/mcp/mcp_manager.cpp
    src/agent/conversation.cpp
    src/agent/system_prompt.cpp
    src/agent/system_prompt_assembly.cpp
    src/agent/llm_context.cpp
    src/agent/activity_log.cpp
    src/agent/context_budget.cpp
    src/agent/metrics.cpp
    src/agent/tool_dispatcher.cpp
    src/agent/agent_loop.cpp
    src/agent/agent_turn.cpp
    src/agent/build_error_detector.cpp
    src/agent/slash_commands.cpp
    src/agent/prompt_templates.cpp
    src/agent/agent_core.cpp
    src/agent/session_manager.cpp
    src/agent/checkpoint_store.cpp
    src/agent/compaction_pipeline.cpp
    src/agent/file_change_tracker.cpp
    src/agent/history_archive.cpp
    src/agent/auto_commit.cpp
    src/agent/mention_parser.cpp
    src/agent/quality_monitor.cpp
    src/frontends/cli/cli_frontend.cpp
    src/frontends/gui/ansi_parser.cpp
    src/frontends/gui/terminal_panel_state.cpp
    src/frontends/gui/diff_renderer.cpp
    src/extractors/extractor_registry.cpp
    src/extractors/markdown_extractor.cpp
    src/extractors/html_extractor.cpp
    src/extractors/pdf_extractor.cpp
    src/extractors/docx_extractor.cpp
    src/extractors/xlsx_extractor.cpp
    src/extractors/zip_reader.cpp
    src/extractors/json_extractor.cpp
    src/extractors/yaml_extractor.cpp
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

# S5.P source-encoding lint -- fail the build if em-dashes or other prohibited
# non-ASCII characters appear in src/ or tests/ outside the allow-list.
add_custom_command(TARGET locus_core POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DALLOWLIST=${CMAKE_SOURCE_DIR}/scripts/non_ascii_allowlist.txt
        -P ${CMAKE_SOURCE_DIR}/scripts/check_no_em_dashes.cmake
    COMMENT "S5.P: checking source files for prohibited non-ASCII characters"
    VERBATIM
)

# -- Main executable -----------------------------------------------------------

add_executable(locus
    src/main.cpp
)

target_link_libraries(locus PRIVATE
    locus_core
)

# Copy the PDFium runtime library next to the locus binary so it loads at runtime.
add_custom_command(TARGET locus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_RUNTIME_LIB}" "$<TARGET_FILE_DIR:locus>"
)

# -- GUI executable (wxWidgets) ------------------------------------------------

find_package(wxWidgets CONFIG REQUIRED COMPONENTS core base aui stc webview)
if(WIN32)
    # Edge WebView2 backend for wxWebView. macOS/Linux wxWebView use system WebKit.
    find_package(unofficial-webview2 CONFIG REQUIRED)
endif()
find_package(md4c CONFIG REQUIRED)

set(LOCUS_GUI_WINDOWS_RESOURCES)
if(WIN32)
    configure_file(
        src/frontends/gui/locus_gui.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/locus_gui.rc
        @ONLY
    )
    list(APPEND LOCUS_GUI_WINDOWS_RESOURCES
        ${CMAKE_CURRENT_BINARY_DIR}/locus_gui.rc
    )
endif()

add_executable(locus_gui WIN32
    ${LOCUS_GUI_WINDOWS_RESOURCES}
    src/frontends/gui/app_icons.cpp
    src/frontends/gui/about_dialog.cpp
    src/frontends/gui/locus_app.cpp
    src/frontends/gui/locus_frame.cpp
    src/frontends/gui/agent_event_router.cpp
    src/frontends/gui/locus_tray.cpp
    src/frontends/gui/menu_controller.cpp
    src/frontends/gui/ops_status_view.cpp
    src/frontends/gui/ui_state.cpp
    src/frontends/gui/wx_frontend.cpp
    src/frontends/gui/activity_panel.cpp
    src/frontends/gui/metrics_view.cpp
    src/frontends/gui/chat_panel.cpp
    src/frontends/gui/chat_html_template.cpp
    src/frontends/gui/chat/chat_util.cpp
    src/frontends/gui/chat/chat_stream_renderer.cpp
    src/frontends/gui/chat/chat_footer_chips.cpp
    src/frontends/gui/chat/chat_popups.cpp
    src/frontends/gui/chat/chat_link_handler.cpp
    src/frontends/gui/slash_popup.cpp
    src/frontends/gui/mention_popup.cpp
    src/frontends/gui/tool_approval_dialog.cpp
    src/frontends/gui/ask_user_dialog.cpp
    src/frontends/gui/compaction_dialog.cpp
    src/frontends/gui/manage_sessions_dialog.cpp
    src/frontends/gui/file_tree_panel.cpp
    src/frontends/gui/memory_bank_panel.cpp
    src/frontends/gui/settings_dialog.cpp
    src/frontends/gui/settings/llm_settings_panel.cpp
    src/frontends/gui/settings/index_settings_panel.cpp
    src/frontends/gui/settings/capabilities_settings_panel.cpp
    src/frontends/gui/settings/tool_approvals_settings_panel.cpp
    src/frontends/gui/settings/mcp_settings_panel.cpp
    src/frontends/gui/settings/notifications_settings_panel.cpp
    src/frontends/gui/settings/sessions_settings_panel.cpp
    src/frontends/gui/settings/agent_settings_panel.cpp
    src/frontends/gui/settings/endpoints_settings_panel.cpp
    src/frontends/gui/notification_sounds.cpp
    src/frontends/gui/markdown.cpp
    src/frontends/gui/autostart.cpp
    src/frontends/gui/terminal_panel.cpp
    src/frontends/gui/capabilities_dialog.cpp
    src/frontends/gui/new_tab_button_tab_art.cpp
    # macOS-only Objective-C++ shim: system recent-documents (Dock menu).
    $<$<PLATFORM_ID:Darwin>:src/frontends/gui/recent_workspaces_mac.mm>
)

target_link_libraries(locus_gui PRIVATE
    locus_core
    wx::core wx::base wx::aui wx::stc wx::webview
    $<$<PLATFORM_ID:Windows>:unofficial::webview2::webview2>
    md4c::md4c-html
    $<$<PLATFORM_ID:Windows>:winmm>
)

# macOS GUI frameworks: AudioToolbox (notification_sounds AudioServices) +
# AppKit (recent-docs via NSDocumentController). find_library yields absolute
# framework paths -- the "-framework X" string form gets mangled by CMake's
# link-line tokenizer in a genex.
if(APPLE)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    find_library(APPKIT_FRAMEWORK       AppKit       REQUIRED)
    target_link_libraries(locus_gui PRIVATE
        ${AUDIOTOOLBOX_FRAMEWORK}
        ${APPKIT_FRAMEWORK}
    )
endif()

# Unbundled layout: dylib next to the executable. In .app-bundle mode the
# bundle block below puts it in Contents/Frameworks instead, so skip this.
if(NOT (APPLE AND LOCUS_MACOS_BUNDLE))
    add_custom_command(TARGET locus_gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFIUM_RUNTIME_LIB}" "$<TARGET_FILE_DIR:locus_gui>"
    )
endif()

# Copy bundled Prism (syntax highlighter for chat code blocks) next to
# locus_gui.exe so the chat WebView loads it via file:// at runtime - no
# CDN dependency. read_prism_asset() resolves <exe_dir>/resources/prism, which
# is Contents/MacOS/resources/prism inside a bundle -- so this works as-is for
# both layouts. Refresh via third_party/prism/build-bundle.ps1.
add_custom_command(TARGET locus_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:locus_gui>/resources/prism"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/prism/prism.min.js"
            "${CMAKE_SOURCE_DIR}/third_party/prism/prism.min.css"
            "${CMAKE_SOURCE_DIR}/third_party/prism/prism-tomorrow.min.css"
            "$<TARGET_FILE_DIR:locus_gui>/resources/prism/"
)

# -- macOS app bundle + runtime library search path ----------------------------
if(APPLE)
    # The CLI always uses the flat layout (dylib next to the binary).
    set_target_properties(locus PROPERTIES
        BUILD_RPATH   "@executable_path"
        INSTALL_RPATH "@executable_path"
    )

    if(LOCUS_MACOS_BUNDLE)
        # Locus.app/Contents/{MacOS/locus_gui, Frameworks/libpdfium.dylib,
        # Resources/{Locus.icns, prism/}, Info.plist}. The CLI `locus` stays a
        # sibling binary (embedding it + a /usr/local/bin symlink is the open
        # question in roadmap/M6/S6.9; out of scope for personal-use builds).
        set(LOCUS_VERSION "${PROJECT_VERSION}")
        configure_file(
            "${CMAKE_SOURCE_DIR}/cmake/macos/Info.plist.in"
            "${CMAKE_CURRENT_BINARY_DIR}/Locus-Info.plist" @ONLY)

        set_target_properties(locus_gui PROPERTIES
            MACOSX_BUNDLE ON
            MACOSX_BUNDLE_BUNDLE_NAME "Locus"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Locus-Info.plist"
            # Search MacOS/ (prism is not a dylib, but keep symmetry) and the
            # conventional Frameworks/ dir where libpdfium.dylib lands.
            BUILD_RPATH   "@executable_path;@executable_path/../Frameworks"
            INSTALL_RPATH "@executable_path;@executable_path/../Frameworks"
        )

        # App icon -> Contents/Resources/Locus.icns (matches CFBundleIconFile).
        add_custom_command(TARGET locus_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/assets/icons/macos/Locus.icns"
                    "$<TARGET_BUNDLE_CONTENT_DIR:locus_gui>/Resources/Locus.icns"
            VERBATIM)

        # PDFium -> Contents/Frameworks (install name is already
        # @rpath/libpdfium.dylib, rewritten + re-signed at configure time).
        add_custom_command(TARGET locus_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_BUNDLE_CONTENT_DIR:locus_gui>/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PDFIUM_RUNTIME_LIB}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:locus_gui>/Frameworks/"
            VERBATIM)

        # Code signing + notarization hooks. With no identity supplied the
        # bundle is left ad-hoc/unsigned (dev runs it via right-click -> Open);
        # set -DLOCUS_CODESIGN_IDENTITY="Developer ID Application: ..." to sign.
        # LOCUS_NOTARIZE_TEAM_ID is accepted for a future notarytool step but
        # not invoked here (notarization is interactive + account-gated).
        if(LOCUS_CODESIGN_IDENTITY)
            add_custom_command(TARGET locus_gui POST_BUILD
                COMMAND codesign --force --deep --timestamp
                        --options runtime
                        --sign "${LOCUS_CODESIGN_IDENTITY}"
                        "$<TARGET_BUNDLE_DIR:locus_gui>"
                COMMENT "codesign Locus.app (${LOCUS_CODESIGN_IDENTITY})"
                VERBATIM)
        else()
            message(STATUS
                "LOCUS_MACOS_BUNDLE: no LOCUS_CODESIGN_IDENTITY -- Locus.app "
                "will be unsigned (run via right-click -> Open).")
        endif()
    else()
        # Unbundled dev layout: dylib sits next to locus_gui.
        set_target_properties(locus_gui PROPERTIES
            BUILD_RPATH   "@executable_path"
            INSTALL_RPATH "@executable_path"
        )
    endif()
endif()

# -- Tests ---------------------------------------------------------------------

enable_testing()
add_subdirectory(tests)
