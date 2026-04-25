add_executable(locus_tests
    test_placeholder.cpp
    test_workspace.cpp
    test_indexer.cpp
    test_index_query.cpp
    test_llm_client.cpp
    test_tools.cpp
    test_agent_core.cpp
    test_s1_1.cpp
    test_semantic.cpp
    test_extractors.cpp
    test_attached_context.cpp
    test_slash_commands.cpp
    test_workspace_lock.cpp
    test_search_regex.cpp
    test_reranker.cpp
    test_checkpoint_store.cpp
)

target_include_directories(locus_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(locus_tests PRIVATE
    Catch2::Catch2WithMain
    locus_core
)

add_custom_command(TARGET locus_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_DLL}" "$<TARGET_FILE_DIR:locus_tests>"
)

include(Catch)
catch_discover_tests(locus_tests)

# Manual-only LLM-driven integration tests (locus_integration_tests).
add_subdirectory(integration)
