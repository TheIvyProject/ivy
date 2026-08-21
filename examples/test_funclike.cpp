// Test preprocessor function-like macro expansion (P1.3).
#pragma ivy cnumber
//
// Covers:
//   - #define SQUARE(x) ((x)*(x))        — single param, body with parens
//   - #define ADD(a, b) ((a) + (b))      — two params
//   - #define ID(x) x                    — minimal body
//   - Macro call with an argument that itself is a macro (argument prescan)
//   - Macro call with nested parens in arguments: F((1, 2)) style
//   - Bare function-like macro name without `(` → emitted verbatim
//   - Chained: outer macro expands to an inner macro call
//
// Run:
//   ivyc -o examples\test_funclike.i examples\test_funclike.cpp   (inspect expansion)
//   ivyc --llvm examples\test_funclike.cpp -o examples\test_funclike.ll

#define SQUARE(x) ((x) * (x))
#define ADD(a, b) ((a) + (b))
#define ID(x) x
#define TWO 2
#define DBL_SQUARE(x) SQUARE(x)

int main() {
    int a = SQUARE(5);          // ((5) * (5))
    int b = ADD(3, 4);          // ((3) + (4))
    int c = ID(42);             // 42
    int d = SQUARE(TWO);        // SQUARE(2) -> ((2) * (2))   (arg prescan)
    int e = DBL_SQUARE(7);      // SQUARE(7) -> ((7) * (7))   (chained)
    int f = ADD(SQUARE(2), 1);  // (( SQUARE(2) ) + (1)) -> ((((2)*(2))) + (1))
    return a + b + c + d + e + f;
}
