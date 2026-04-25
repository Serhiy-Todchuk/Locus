# Manual-only retrieval evaluation harness (S4.K).
#
# NOT registered with ctest — running it requires a fully indexed workspace +
# the bge-m3 embedding GGUF, which takes minutes and many GB of RAM. Run by
# hand when iterating on retrieval (chunker, embedder, reranker, RRF):
#
#   cmake --build build/release --target locus_retrieval_eval
#   ./build/release/tests/retrieval_eval/Release/locus_retrieval_eval.exe \
#       --workspace . --queries tests/retrieval_eval/queries.json
#
# Returns:
#   0  — pass
#   1  — regression detected vs. tests/retrieval_eval/baseline.json
#   2  — harness error

add_executable(locus_retrieval_eval
    main.cpp
    metrics.cpp
    eval_runner.cpp
    curate.cpp
)

target_include_directories(locus_retrieval_eval PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(locus_retrieval_eval PRIVATE
    locus_core
)

# pdfium.dll has to sit next to the exe at runtime (Workspace ctor pulls
# extractors that link pdfium even though this binary doesn't query PDFs).
add_custom_command(TARGET locus_retrieval_eval POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFIUM_DLL}" "$<TARGET_FILE_DIR:locus_retrieval_eval>"
)

# Deliberately NOT calling catch_discover_tests / add_test() — manual-only.
