// Test preprocessor object-like macro expansion (P1.2).
#pragma ivy cnumber
//
// Covers:
//   - #define with an integer body
//   - #define with a float body
//   - #define referencing another macro (chained expansion)
//   - #define with empty body (EMPTY)
//
// Note on macro self-reference (`#define SELF SELF + 1`): the preprocessor
// expands it to `SELF + 1` (the self-reference is "painted blue" per C++
// rules and left literal), which then fails to compile because `SELF` is
// not an identifier the parser knows. That case is therefore intentionally
// omitted from the full-pipeline test; it is only meaningful for the
// `-o .i` expansion check.
//
// Run:
//   ivyc -o examples\test_define.i examples\test_define.cpp   (inspect expansion)
//   ivyc --llvm examples\test_define.cpp -o examples\test_define.ll

#define ANSWER 42
#define PI 3.14
#define DOUBLE_ANSWER ANSWER + ANSWER
#define EMPTY

int main() {
    int x = ANSWER;
    double y = PI;
    int z = DOUBLE_ANSWER;
    EMPTY int w = 0;
    return x + z + w;
}
