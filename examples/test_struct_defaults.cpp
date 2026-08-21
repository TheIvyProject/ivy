// Test: struct default member initializers (P4.4 limit fix)
#pragma ivy cnumber

struct Point {
    int x;
    int y;
};

struct Defaults {
    int a = 111;
    int b = 222;
    int c;
};

struct Mixed {
    int first;
    int second = 999;
    int third;
};

extern "C" int printf(const char* fmt, ...);

int main() {
    // Struct with default member initializers — no explicit init.
    // a=111 (default), b=222 (default), c=0 (zero-init, no default).
    Defaults d;
    printf("defaults: %d %d %d\n", d.a, d.b, d.c);

    // Partial aggregate init overrides defaults for explicit fields.
    // a=555 (explicit), b=222 (default), c=0 (zero-init).
    Defaults d2 = {555};
    printf("partial-defaults: %d %d %d\n", d2.a, d2.b, d2.c);

    // Empty init list — all defaults applied.
    // a=111 (default), b=222 (default), c=0 (zero-init).
    Defaults d3 = {};
    printf("empty-defaults: %d %d %d\n", d3.a, d3.b, d3.c);

    // Mixed: first=0 (zero), second=999 (default), third=0 (zero).
    Mixed m;
    printf("mixed: %d %d %d\n", m.first, m.second, m.third);

    // Mixed partial: first=42 (explicit), second=999 (default), third=0 (zero).
    Mixed m2 = {42};
    printf("mixed-partial: %d %d %d\n", m2.first, m2.second, m2.third);

    // Reassign with partial init — defaults apply for trailing.
    m = {100};
    printf("reassign: %d %d %d\n", m.first, m.second, m.third);

    return 0;
}
