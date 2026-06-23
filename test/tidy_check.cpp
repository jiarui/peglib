// tidy_check.cpp — dedicated translation unit for clang-tidy analysis.
//
// This file's sole purpose is to give clang-tidy a TU that exercises every
// library header via the peglib.h umbrella include. It is NOT a test (it
// contains no assertions and is not registered with ctest). Scoping tidy to
// this single TU keeps it focused on the public API and avoids the
// false-positives and version-specific noise that arise from running tidy
// over test code (doctest macros, __COUNTER__ C2y warnings, etc.).

#include "peglib.h"

int main()
{
    return 0;
}
