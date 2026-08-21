// Test preprocessor #include expansion (P1.1).
#pragma ivy cnumber
//
// Two forms are exercised:
//   - `#include "ivy_local.h"` : relative to this file's directory.
//   - `#include <ivy.h>`       : resolved via -I lib/.
//
// After preprocessing, the resulting token stream should parse cleanly and
// emit LLVM IR. Run with:
//   ivyc -I lib examples/test_include.cpp --llvm
//   ivyc -I lib -o examples/test_include.i examples/test_include.cpp
//
// Then verify the expanded text with a C++ compiler, e.g.:
//   clang++ -std=c++23 examples/test_include.i -o /tmp/a.out

#include "ivy_local.h"
#include <ivy.h>

int ivy_version() {
    return 1;
}

int main() {
    ivy_local_init();
    return ivy_version();
}
