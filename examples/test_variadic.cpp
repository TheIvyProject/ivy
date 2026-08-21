// Test preprocessor variadic macros (P1.6): `#define M(...)` /
// `#define M(fmt, ...)` with `__VA_ARGS__`.
#pragma ivy cnumber
//
// Covers:
//   - Variadic with zero named params: `#define LOG(...) ...__VA_ARGS__...`
//   - Variadic with one named param: `#define LOG(fmt, ...) ...__VA_ARGS__...`
//   - `__VA_ARGS__` with 0 extra args (empty expansion)
//   - `__VA_ARGS__` with 1 extra arg
//   - `__VA_ARGS__` in the middle of the body (not just at end)
//   - Arity check: too few args → error (but we just test the valid cases here)
//
// Note: Ivy's parser only accepts function definitions at top level (no
// global variables yet), so the expanded code is wrapped in functions.
//
// Run:
//   ivyc -o examples\test_variadic.i examples\test_variadic.cpp
//   ivyc --llvm examples\test_variadic.cpp -o examples\test_variadic.ll

// --- Variadic with zero named params ---
#define VA0(...) __VA_ARGS__
// VA0(1)         → 1
// VA0()          → (empty)

// --- Variadic with one named param ---
#define VA1(x, ...) x + __VA_ARGS__
// VA1(10, 20)    → 10 + 20

// --- __VA_ARGS__ in the middle of body ---
#define WRAP(prefix, ...) prefix + __VA_ARGS__ + 0
// WRAP(10, 20) → 10 + 20 + 0

// --- __VA_ARGS__ with 0 extra args (C++20 allows) ---
#define MAYBE(x, ...) x
// MAYBE(5)       → 5

// --- Variadic that just returns first arg ---
#define COUNT(first, ...) first
// COUNT(1, 2, 3) → 1

// --- Empty __VA_ARGS__ ---
#define EMPTY_VA(...) 42
// EMPTY_VA() → 42

// --- __VA_ARGS__ empty but body has fallback ---
#define FALLBACK(x, ...) x + 0
// FALLBACK(7) → 7 + 0 (empty __VA_ARGS__ before `+ 0`)

// Use the expansions in functions so the parser accepts them.
int va_zero_one() { return VA0(1); }

int va_one_two() { return VA1(10, 20); }

int wrap_test() { return WRAP(10, 20); }

int maybe_test() { return MAYBE(5); }

int count_test() { return COUNT(1, 2, 3); }

int empty_va_test() { return EMPTY_VA(); }

int fallback_test() { return FALLBACK(7); }

int main() {
    return va_zero_one() + va_one_two() + wrap_test() +
           maybe_test() + count_test() + empty_va_test() + fallback_test();
}
