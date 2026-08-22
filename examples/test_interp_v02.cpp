// Comprehensive IvyInterpret v0.2 test — MIR-based interpreter
#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

int32_t add(int32_t a, int32_t b) { return a + b; }

int32_t factorial(int32_t n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int32_t main() {
    // Function call + arithmetic
    int32_t r1 = add(15, 15);  // 30
    printf("r1=%d\n", r1);

    // Recursion
    int32_t r2 = factorial(5);  // 120
    printf("r2=%d\n", r2);

    // For loop + compound assignment
    int32_t sum = 0;
    for (int32_t i = 1; i <= 10; i++) {
        sum += i;  // 55
    }
    printf("sum=%d\n", sum);

    // While loop + decrement
    int32_t x = 5;
    while (x > 0) {
        x--;
    }
    printf("x=%d\n", x);

    // Do-while + compound multiply
    int32_t y = 1;
    do {
        y *= 2;
    } while (y < 100);
    printf("y=%d\n", y);  // 128

    // Break + continue
    int32_t total = 0;
    for (int32_t i = 0; i < 10; i++) {
        if (i == 5) break;
        if (i == 2) continue;
        total += i;  // 0+1+3+4 = 8
    }
    printf("total=%d\n", total);

    // Ternary + short-circuit
    int32_t z = (sum == 55) ? 100 : 200;
    if (z == 100 && r1 == 30) {
        z += 1;  // 101
    }
    printf("z=%d\n", z);

    // Bitwise ops
    int32_t b1 = 0xF0;
    int32_t b2 = 0x0F;
    printf("and=%d or=%d xor=%d shl=%d shr=%d\n",
           b1 & b2, b1 | b2, b1 ^ b2, b1 << 1, b1 >> 4);

    // Float arithmetic
    double d1 = 3.14;
    double d2 = d1 * 2.0;  // 6.28
    printf("d2=%.2f\n", d2);

    return 0;
}
