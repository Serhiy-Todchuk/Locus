# Manual-only LLM-driven integration tests. NOT registered with ctest /
# catch_discover_tests — runs only when the user explicitly launches the exe.
#
# Requires LM Studio to be running at http://127.0.0.1:1234 (or the URL set
# via LOCUS_INT_TEST_ENDPOINT) with a tool-calling capable model loaded.

add_executable(locus_integration_tests
    harness_frontend.cpp
    harness_fixture.cpp
    session_listener.cpp
    test_int_smoke.cpp
    test_int_search.cpp
    test_int_outline.cpp
    test_int_fs_lifecycle.cpp
    test_int_shell.cpp
    test_int_interactive.cpp
    test_int_slash.cpp
)

target_include_directories(locus_integration_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(locus_integration_tests PRIVATE
    Catch2::Catch2WithMain
    locus_core
)

# Inject the repo root as the workspace path so the test binary can locate
# its testing workspace regardless of where it's launched from.
target_compile_definitions(locus_integration_tests PRIVATE
    LOCUS_INT_TEST_WORKSPACE_DIR="${CMAKE_SOURCE_DIR}"
)

# pdfium.dll has to sit next to the exe at runtime (same as locus_tests).
add_custom_command(TARGET locus_integration_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_DLL}" "$<TARGET_FILE_DIR:locus_integration_tests>"
)

# Deliberately NOT calling catch_discover_tests() — this target is manual-only.
