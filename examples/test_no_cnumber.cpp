// Test that WITHOUT #pragma ivy cnumber, C-style types are rejected.
//
// This file should FAIL to compile with errors like:
//   "C-style type 'int' requires #pragma ivy cnumber"
//
// Run:
//   ivyc --llvm examples\test_no_cnumber.cpp -o examples\test_no_cnumber.ll
//   (expect exit code 1)

int bad_int() { return 42; }

int main() { return bad_int(); }
