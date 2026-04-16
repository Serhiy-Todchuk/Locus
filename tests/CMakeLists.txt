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
)

target_link_libraries(locus_tests PRIVATE
    Catch2::Catch2WithMain
    locus_core
)

include(Catch)
catch_discover_tests(locus_tests)
