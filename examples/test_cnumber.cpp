// Test #pragma ivy cnumber (B3): C-style number types opt-in.
//
// Without `#pragma ivy cnumber`, Ivy only accepts fixed-width types
// (int8_t, int16_t, int32_t, int64_t, uint8_t, ..., float32_t, float64_t,
// ..., size_t, ptrdiff_t, bool, void). C-style types like int, unsigned,
// long, short, char, float, double, long long are rejected.
//
// With `#pragma ivy cnumber`, all C-style types become accepted.
// Hex/octal/binary integer literals are always allowed (they are syntax,
// not a type).
//
// Run:
//   ivyc --llvm examples\test_cnumber.cpp -o examples\test_cnumber.ll
//   ivyc --llvm examples\test_no_cnumber.cpp -o examples\test_no_cnumber.ll  (should fail)

#pragma ivy cnumber

// --- C-style types now allowed ---
int c_int() { return 42; }
unsigned c_unsigned() { return 7; }
long c_long() { return 100; }
long long c_long_long() { return 1000; }
short c_short() { return 3; }
char c_char() { return 65; }
float c_float() { return 1.5; }
double c_double() { return 2.5; }

// --- Combinations ---
unsigned long c_ulong() { return 99; }
signed int c_sint() { return 11; }
unsigned long long c_ull() { return 999; }

// --- C-style type combinations with trailing `int` ---
short int c_short_int() { return 5; }
long int c_long_int() { return 200; }
long long int c_ll_int() { return 2000; }
signed short int c_sshort_int() { return 13; }
unsigned long long int c_ull_int() { return 9999; }

// --- long double ---
long double c_ldouble() { return 3.14; }

// --- Fixed-width types still work ---
int32_t fixed32() { return 32; }
uint64_t fixed64() { return 64; }
float64_t fixedf64() { return 64.0; }

// --- Hex/octal/binary literals always allowed (even without pragma) ---
int hex_lit() { return 0xFF; }
int oct_lit() { return 010; }
int bin_lit() { return 0b1010; }

int main() {
    return c_int() + c_unsigned() + c_long() + c_long_long() + c_short() +
           c_char() + fixed32() + hex_lit() + bin_lit();
}
