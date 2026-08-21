// Test: struct default member initializers (minimal)
#pragma ivy cnumber

struct Defaults {
    int a = 111;
    int b = 222;
};

extern "C" int printf(const char* fmt, ...);

int main() {
    Defaults d;
    printf("defaults: %d %d\n", d.a, d.b);
    return 0;
}
