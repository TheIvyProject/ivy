// Test P4.1: enum support
#pragma ivy cnumber

// --- Unscoped enum with implicit values ---
enum Color { Red, Green, Blue };

int32_t test_implicit() {
    Color c = Green;
    return c;  // Green == 1
}

// --- Unscoped enum with explicit values ---
enum Flags { FlagA = 1, FlagB = 2, FlagC = 4, FlagD = 8 };

int32_t test_explicit() {
    return FlagC;  // 4
}

// --- Mixed implicit/explicit ---
enum Status { Active = 10, Inactive, Pending = 20, Closed };

int32_t test_mixed() {
    return Inactive;  // 11 (Active + 1)
}

int32_t test_closed() {
    return Closed;  // 21 (Pending + 1)
}

// --- Enum with expression values ---
enum Perm { Read = 4, Write = 2, Exec = 1, All = Read | Write | Exec };

int32_t test_expr() {
    return All;  // 7
}

// --- Enum used in arithmetic ---
int32_t test_arith() {
    int32_t x = FlagA + FlagB;  // 1 + 2 = 3
    return x;
}

// --- Enum used in comparison ---
bool test_compare() {
    return FlagC > FlagB;  // 4 > 2 = true
}

// --- Scoped enum (enum class) ---
enum class Direction { Up, Down, Left = 10, Right };

int32_t test_scoped() {
    Direction d = Direction::Left;
    return d;  // 10
}

// --- Enum with explicit underlying type ---
enum BigNum : int64_t { Small = 100, Big = 1000000 };

int64_t test_underlying() {
    return Big;  // 1000000
}

// --- Multiple enums in same TU ---
enum Fruit { Apple, Banana = 5, Cherry };
enum Animal { Cat = 1, Dog = 2, Bird = 4 };

int32_t test_multi_enum() {
    return Banana + Dog;  // 5 + 2 = 7
}

int main() {
    return test_implicit();  // 1
}
