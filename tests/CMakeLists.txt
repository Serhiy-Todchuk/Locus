add_executable(locus_tests
    test_placeholder.cpp
)

target_link_libraries(locus_tests PRIVATE
    Catch2::Catch2WithMain
    spdlog::spdlog
)

include(Catch)
catch_discover_tests(locus_tests)
