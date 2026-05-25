# Manual-only LLM-driven integration tests. NOT registered with ctest /
# catch_discover_tests -- runs only when the user explicitly launches the exe.
#
# Requires LM Studio to be running at http://127.0.0.1:1234 (or the URL set
# via LOCUS_INT_TEST_ENDPOINT) with a tool-calling capable model loaded.

add_executable(locus_integration_tests
    integration_main.cpp
    harness_frontend.cpp
    harness_fixture.cpp
    test_int_smoke.cpp
    test_int_search.cpp
    test_int_outline.cpp
    test_int_fs_lifecycle.cpp
    test_int_shell.cpp
    test_int_bg.cpp
    test_int_interactive.cpp
    test_int_slash.cpp
    test_int_file_change_awareness.cpp
    test_int_undo.cpp
    test_int_metrics.cpp
    test_int_max_tokens.cpp
    test_int_plan.cpp
    test_int_mcp.cpp
    test_int_memory.cpp
    test_int_prompt_templates.cpp
    test_int_history_delete.cpp
    test_int_tabs_and_sessions.cpp
    test_int_filter_output.cpp
    test_int_lazy_manifest.cpp
    test_int_reasoning_watchdog.cpp
    test_int_tool_calls.cpp
)

target_include_directories(locus_integration_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Plain Catch2::Catch2 (not Catch2WithMain) because we own main() in
# integration_main.cpp to parse the -console flag before Catch2 takes over.
target_link_libraries(locus_integration_tests PRIVATE
    Catch2::Catch2
    locus_core
)

# Inject the repo root as the workspace path so the test binary can locate
# its testing workspace regardless of where it's launched from.
target_compile_definitions(locus_integration_tests PRIVATE
    LOCUS_INT_TEST_WORKSPACE_DIR="${CMAKE_SOURCE_DIR}"
)

# S4.G [mcp] test spawns the bundled mock MCP server. Inject its build-time
# absolute path so the test exe can resolve it regardless of cwd. Generator
# expression resolves at build time; locus_mock_mcp_server target is defined
# in ../CMakeLists.txt.
target_compile_definitions(locus_integration_tests PRIVATE
    LOCUS_MOCK_MCP_SERVER_PATH="$<TARGET_FILE:locus_mock_mcp_server>"
)
add_dependencies(locus_integration_tests locus_mock_mcp_server)

# pdfium.dll has to sit next to the exe at runtime (same as locus_tests).
add_custom_command(TARGET locus_integration_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_DLL}" "$<TARGET_FILE_DIR:locus_integration_tests>"
)

# Deliberately NOT calling catch_discover_tests() -- this target is manual-only.
