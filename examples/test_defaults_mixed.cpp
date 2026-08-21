// Test: default member initializers — mixed scenarios
#pragma ivy cnumber

struct Mixed {
    int a = 111;
    int b;          // no default — zero-init
    int c = 333;
};

extern "C" int printf(const char* fmt, ...);

int main() {
    // 1. No init — apply all defaults, zero-init b
    Mixed m1;
    printf("m1 (no init): %d %d %d\n", m1.a, m1.b, m1.c);

    // 2. Partial init — override a, keep defaults for c, zero b
    Mixed m2 = {555};
    printf("m2 (partial): %d %d %d\n", m2.a, m2.b, m2.c);

    // 3. Empty init — all defaults + zero b
    Mixed m3 = {};
    printf("m3 (empty): %d %d %d\n", m3.a, m3.b, m3.c);

    return 0;
}
