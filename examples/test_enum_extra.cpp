// Test P4.1: additional enum edge cases
#pragma ivy cnumber

// --- enum struct (equivalent to enum class) ---
enum struct Shape { Circle, Square = 5, Triangle };

int32_t test_enum_struct() {
    Shape s = Shape::Square;
    return s;  // 5
}

// --- enum with negative values ---
enum Signed { Minus1 = -1, Minus2 = -2, Zero = 0 };

int32_t test_negative() {
    return Minus2;  // -2
}

// --- enum used in ternary ---
enum Mode { Fast, Slow };

int32_t test_ternary(int32_t x) {
    return x > 0 ? Fast : Slow;
}

// --- enum constant in bitwise expression ---
enum Perm2 { R2 = 1, W2 = 2, X2 = 4, RW2 = R2 | W2, RWX2 = R2 | W2 | X2 };

int32_t test_bitwise() {
    return RWX2;  // 7
}

// --- enum used in if condition ---
enum Bool { FalseB = 0, TrueB = 1 };

int32_t test_if_cond() {
    if (TrueB) return 42;
    return 0;
}

// --- multiple enums with same constant name ---
enum A { A1 = 1, A2 = 2 };
enum B { B1 = 10, B2 = 20 };

int32_t test_same_name() {
    return A1 + B1;  // 1 + 10 = 11
}

int main() {
    return test_enum_struct();  // 5
}
