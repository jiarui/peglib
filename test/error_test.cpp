#include "doctest.h"

// ---------------------------------------------------------------------------
// Error reporting tests (placeholder).
//
// peglib does not yet expose a structured error API. These tests will be
// filled in during Phase 1 once `peg::ParseError`, furthest-failure tracking,
// and the SourceMap (byte offset -> line/col) helper land. Kept here so the
// test target compiles and the CI matrix has a home for future coverage.
// ---------------------------------------------------------------------------

TEST_CASE("error-reporting-module-placeholder")
{
    CHECK(true);
}
