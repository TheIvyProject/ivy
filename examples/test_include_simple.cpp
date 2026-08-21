// Test #include + LLVM IR emission (P1.1, no include guards yet).
#pragma ivy cnumber
//
// Run:
//   ivyc -I examples examples\test_include_simple.cpp --llvm
//   ivyc -I examples -o examples\test_include_simple.i examples\test_include_simple.cpp

#include "ivy_simple.h"

int main() {
    return 0;
}
