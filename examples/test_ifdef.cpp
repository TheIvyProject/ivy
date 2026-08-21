// Test preprocessor conditional compilation (P1.4): #ifdef / #ifndef /
// #else / #endif.
#pragma ivy cnumber
//
// Covers:
//   - #ifdef on a defined macro (block taken)
//   - #ifdef on an undefined macro (block skipped)
//   - #ifndef on a defined macro (block skipped)
//   - #ifndef on an undefined macro (block taken)
//   - #else branch taken when #ifdef condition is false
//   - #undef removes a macro so a later #ifdef sees it as undefined
//   - Nested #ifdef inside #ifdef
//   - Include-guard pattern (the common use of #ifndef / #define / #endif)
//
// Note: Ivy's parser only accepts function definitions at top level (no
// global variables yet), so the conditionally-compiled code is wrapped
// in functions.
//
// Run:
//   ivyc -o examples\test_ifdef.i examples\test_ifdef.cpp
//   ivyc --llvm examples\test_ifdef.cpp -o examples\test_ifdef.ll

#define DEBUG 1
#define FEATURE 1

#ifdef DEBUG
int dbg() { return 1; }
#else
int dbg() { return 0; }
#endif

#ifdef NOT_DEFINED
int never_seen() { return 42; }
#else
int seen_here() { return 7; }
#endif

#ifndef FEATURE
int feature_missing() { return 1; }
#else
int feature_present() { return 2; }
#endif

#undef DEBUG
#ifdef DEBUG
int debug_again() { return 1; }
#else
int debug_off() { return 0; }
#endif

// Nested — outer taken, inner not-taken (DEBUG was #undef'd above)
#ifdef FEATURE
  #ifdef DEBUG
    int both() { return 5; }
  #else
    int only_feature() { return 5; }
  #endif
#else
  int neither() { return 5; }
#endif

// Include-guard pattern
#ifndef IVY_TEST_IFDEF_H
#define IVY_TEST_IFDEF_H
int guarded() { return 99; }
#endif

int main() {
    return dbg() + seen_here() + feature_present() + debug_off() + only_feature() + guarded();
}
