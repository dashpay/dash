// Regenerates test-vectors/reference.txt.
//
// Build against the library whose behavior is to become the reference (the
// checked-in file was produced by the relic-based implementation) and
// redirect the output:
//
//   ./build/src/vectorgen > test-vectors/reference.txt
//
// runtest compares the current library against the file; this tool only
// generates data.

#include <cstdio>

#include "test-vectors.hpp"

int main()
{
    for (const std::string& line : bls_test_vectors::GenerateReferenceVectors()) {
        printf("%s\n", line.c_str());
    }
    return 0;
}
