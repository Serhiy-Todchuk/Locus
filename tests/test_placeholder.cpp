#include <catch2/catch_test_macros.hpp>

// S0.1 smoke test — verifies the project compiles and Catch2 links correctly.
// Real tests are added in S0.3 (FTS index), S0.4 (query API), S0.5 (SSE parser), etc.

TEST_CASE("S0.1 - project compiles and links", "[s0.1]")
{
    REQUIRE(1 + 1 == 2);
}
