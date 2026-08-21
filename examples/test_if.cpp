// Test preprocessor #if / #elif constant-expression evaluation (P1.5).
#pragma ivy cnumber
//
// Covers:
//   - #if with integer literal (true / false)
//   - #if with arithmetic (+, -, *, /, %)
//   - #if with bitwise (&, |, ^, ~, <<, >>)
//   - #if with logical (&&, ||, !)
//   - #if with comparison (==, !=, <, <=, >, >=)
//   - #if with ternary (?:)
//   - #if defined(NAME) and defined NAME (no parens)
//   - #if with macro expansion (macro body used in expression)
//   - #elif chains (first taken, middle taken, last taken, none taken → else)
//   - #elif after a taken branch is skipped (C++ rule)
//   - Nesting #if inside #if
//   - Undefined identifier → 0 (C++ rule)
//   - true / false keywords in #if
//
// Note: Ivy's parser only accepts function definitions at top level (no
// global variables yet), so the conditionally-compiled code is wrapped
// in functions.
//
// Run:
//   ivyc -o examples\test_if.i examples\test_if.cpp
//   ivyc --llvm examples\test_if.cpp -o examples\test_if.ll

#define VERSION 3
#define FEATURE_X 1
#define FEATURE_Y 0

// --- #if with integer literal ---
#if 1
int lit_true() { return 1; }
#else
int lit_true() { return 0; }
#endif

#if 0
int lit_false() { return 0; }
#else
int lit_false() { return 1; }
#endif

// --- #if with arithmetic ---
#if 2 + 3 == 5
int arith() { return 1; }
#else
int arith() { return 0; }
#endif

#if 10 % 3 == 1
int mod() { return 1; }
#else
int mod() { return 0; }
#endif

// --- #if with bitwise ---
#if (0xFF & 0x0F) == 0x0F
int bit_and() { return 1; }
#else
int bit_and() { return 0; }
#endif

#if (1 << 4) == 16
int shift() { return 1; }
#else
int shift() { return 0; }
#endif

// --- #if with logical ---
#if 1 && 1
int and_t() { return 1; }
#else
int and_t() { return 0; }
#endif

#if 0 || 1
int or_t() { return 1; }
#else
int or_t() { return 0; }
#endif

#if !0
int not_f() { return 1; }
#else
int not_f() { return 0; }
#endif

// --- #if with comparison ---
#if 3 < 5
int lt() { return 1; }
#else
int lt() { return 0; }
#endif

#if 5 >= 5
int ge() { return 1; }
#else
int ge() { return 0; }
#endif

// --- #if with ternary ---
#if (1 ? 42 : 0) == 42
int tern() { return 1; }
#else
int tern() { return 0; }
#endif

// --- #if defined(...) ---
#if defined(VERSION)
int def_paren() { return 1; }
#else
int def_paren() { return 0; }
#endif

#if defined VERSION
int def_noparen() { return 1; }
#else
int def_noparen() { return 0; }
#endif

#if defined(NOPE)
int def_undef() { return 0; }
#else
int def_undef() { return 1; }
#endif

// --- #if with macro expansion (VERSION expands to 3) ---
#if VERSION == 3
int macro_eq() { return 1; }
#else
int macro_eq() { return 0; }
#endif

#if VERSION > 2 && VERSION < 4
int macro_range() { return 1; }
#else
int macro_range() { return 0; }
#endif

// --- #elif chains ---
#if VERSION == 1
int chain_first() { return 1; }
#elif VERSION == 2
int chain_first() { return 2; }
#elif VERSION == 3
int chain_first() { return 3; }
#else
int chain_first() { return 99; }
#endif

#if VERSION == 99
int chain_none() { return 0; }
#elif VERSION == 99
int chain_none() { return 0; }
#else
int chain_none() { return 1; }
#endif

// --- #elif after taken branch is skipped (C++ rule) ---
#if VERSION == 3
int skip_taken() { return 1; }
#elif VERSION == 3
int skip_taken() { return 2; }
#else
int skip_taken() { return 3; }
#endif

// --- Nesting #if inside #if ---
#if VERSION >= 2
  #if FEATURE_X
    int nested_inner() { return 1; }
  #else
    int nested_inner() { return 0; }
  #endif
#else
  int nested_inner() { return 0; }
#endif

// --- Undefined identifier → 0 ---
#if UNDEFINED_MACRO
int undef_zero() { return 0; }
#else
int undef_zero() { return 1; }
#endif

// --- true / false keywords in #if ---
#if true
int kw_true() { return 1; }
#else
int kw_true() { return 0; }
#endif

#if false
int kw_false() { return 0; }
#else
int kw_false() { return 1; }
#endif

// --- defined() with ! and combination ---
#if !defined(NOPE) && defined(VERSION)
int combo() { return 1; }
#else
int combo() { return 0; }
#endif

int main() {
    return lit_true() + lit_false() + arith() + mod() + bit_and() + shift() +
           and_t() + or_t() + not_f() + lt() + ge() + tern() +
           def_paren() + def_noparen() + def_undef() + macro_eq() +
           macro_range() + chain_first() + chain_none() + skip_taken() +
           nested_inner() + undef_zero() + kw_true() + kw_false() + combo();
}
