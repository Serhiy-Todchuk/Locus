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
    test_search_ast.cpp
    test_reranker.cpp
    test_checkpoint_store.cpp
    test_process_registry.cpp
    test_process_output_filter.cpp
    test_retrieval_eval.cpp
    test_metrics.cpp
    test_file_change_tracker.cpp
    test_stream_decoders.cpp
    test_plan_mode.cpp
    test_mcp.cpp
    test_memory_store.cpp
    test_memory_bank_filter.cpp
    test_gitignore.cpp
    test_auto_commit.cpp
    test_list_directory_tool.cpp
    test_prompt_templates.cpp
    test_grammar_coverage.cpp
    test_mention_parser.cpp
    test_command_path_scanner.cpp
    test_model_presets.cpp
    test_ansi_parser.cpp
    test_capability_toggles.cpp
    test_permission_presets.cpp
    test_global_config.cpp
    test_chat_diff_rendering.cpp
    test_system_prompt_assembly.cpp
    test_llm_context.cpp
    test_tool_safety.cpp
    test_reserve_enforcement.cpp
    test_compaction_pipeline.cpp
    test_chat_activity_restructure.cpp
    test_activity_log.cpp
    test_tabs_and_sessions.cpp
    test_session_restore.cpp
    test_source_hygiene.cpp
    test_terminal_per_tab.cpp
    test_describe_tool.cpp
    test_lazy_manifest.cpp
    test_manifest_budget.cpp
    test_system_prompt_profiles.cpp
    test_reasoning_watchdog.cpp
    test_json_repair.cpp
    test_tool_call_grammar.cpp
    test_truncation_detector.cpp
    test_thinking_strip.cpp
    test_quality_monitor.cpp
    test_outline_race.cpp
    test_tool_arg_validation.cpp
    test_short_descriptions.cpp
    retrieval_eval/metrics.cpp
    support/shared_embedder.cpp
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

# -- Mock MCP server (S4.G) ---------------------------------------------------
# Tiny stdio JSON-RPC server used by [s4.g][mcp] tests. Built next to the
# test exe so the tests can spawn it from $<TARGET_FILE_DIR:locus_tests>.

add_executable(locus_mock_mcp_server
    mock_mcp_server/main.cpp
)
target_link_libraries(locus_mock_mcp_server PRIVATE
    nlohmann_json::nlohmann_json
)
set_target_properties(locus_mock_mcp_server PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:locus_tests>
)
add_dependencies(locus_tests locus_mock_mcp_server)

include(Catch)
catch_discover_tests(locus_tests)

# Manual-only LLM-driven integration tests (locus_integration_tests).
add_subdirectory(integration)

# Manual-only retrieval evaluation harness (locus_retrieval_eval).
add_subdirectory(retrieval_eval)

# Manual-only UI Automation test driver (locus_ui_tests, S5.L).
add_subdirectory(ui_automation)
