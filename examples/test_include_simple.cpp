// Test #include + LLVM IR emission (P1.1, no include guards yet).
#pragma ivy cnumber
//
// Run:
//   ivy -I examples examples\test_include_simple.cpp --llvm
//   ivy -I examples -o examples\test_include_simple.i examples\test_include_simple.cpp

#include "ivy_simple.h"

int main() {
    return 0;
}
