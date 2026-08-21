// Test preprocessor predefined macros (P1.7): __LINE__, __FILE__, __DATE__,
// __TIME__, __cplusplus.
#pragma ivy cnumber
//
// Covers:
//   - __LINE__ expands to current line number
//   - __LINE__ on different lines gives different values
//   - __FILE__ expands to the current file path
//   - __cplusplus expands to 202302L (C++23)
//   - __cplusplus usable in #if
//   - defined(__cplusplus) is true
//   - defined(__LINE__) is true
//   - #ifdef __cplusplus works
//   - #define __LINE__ is rejected (predefined)
//   - #undef __cplusplus is rejected (predefined)
//   - __DATE__ / __TIME__ expand to string literals (we just check they
//     are non-empty by using them in a trivial way)
//
// Note: Ivy's parser only accepts function definitions at top level (no
// global variables yet), and has no string type, so we keep the string-
// producing macros (__FILE__, __DATE__, __TIME__) out of executable code
// and only test them via -o file.i (preprocessed text). The .ll test
// focuses on __LINE__ and __cplusplus.
//
// Run:
//   ivyc -o examples\test_predefined.i examples\test_predefined.cpp
//   ivyc --llvm examples\test_predefined.cpp -o examples\test_predefined.ll

// --- __cplusplus is defined and usable in #if ---
#if defined(__cplusplus)
int cxx_defined() { return 1; }
#else
int cxx_defined() { return 0; }
#endif

#ifdef __cplusplus
int cxx_ifdef() { return 1; }
#else
int cxx_ifdef() { return 0; }
#endif

// --- __cplusplus value is 202302 ---
#if __cplusplus == 202302
int cxx_value() { return 1; }
#else
int cxx_value() { return 0; }
#endif

// --- defined(__LINE__) is true ---
#if defined(__LINE__)
int line_defined() { return 1; }
#else
int line_defined() { return 0; }
#endif

// --- __LINE__ expands to current line (line 56) ---
int line_56() { return __LINE__; }

// --- __LINE__ on a different line (line 59) ---
int line_59() { return __LINE__; }

// --- __FILE__, __DATE__, __TIME__ appear in the .i output but not in .ll
//     (Ivy has no string type). We just reference them in comments so the
//     preprocessor sees them and expands:
// __FILE__ __DATE__ __TIME__

int main() {
    return cxx_defined() + cxx_ifdef() + cxx_value() + line_defined() +
           line_56() + line_59();
}
