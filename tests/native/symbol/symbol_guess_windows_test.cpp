#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

#include <cstdio>
#include <string_view>

namespace windows_test = spark::symbol_guess::windows_test;

int main(int argc, char **argv)
{
    if (argc >= 3 && std::string_view(argv[1]) == "--evaluate") {
        return windows_test::evaluateMappedPe(argc, argv);
    }
    if (!windows_test::testPeAndFunctionRanges() || !windows_test::testChainedAndMalformedUnwind() ||
        !windows_test::testDuplicateOverlapAndDeterminism() || !windows_test::testRttiVtableAmbiguity() ||
        !windows_test::testInvalidRttiAndThunk() || !windows_test::testRttiNameCoverage() ||
        !windows_test::testRttiNameCacheBudgetAndConcurrency() || !windows_test::testRttiNameCollisions() ||
        !windows_test::testAslrIndependence() || !windows_test::testDecodedStringsAndScoring() ||
        !windows_test::testDecodedStringLeaForms() || !windows_test::testStringOwnershipProofs() ||
        !windows_test::testWindowsBudgetBoundaries() || !windows_test::testWindowsMandatoryEvidenceCases() ||
        !windows_test::testInstructionMiddleAndSharedString() || !windows_test::testChainedRootStringUniqueness() ||
        !windows_test::testLargeRangeLookup() || !windows_test::testShortReadOnlySectionBounds() ||
        !windows_test::testSymbolGuessApplicationPolicy()) {
        return 1;
    }
    std::puts("Windows symbol guess tests passed");
    return 0;
}

#endif
