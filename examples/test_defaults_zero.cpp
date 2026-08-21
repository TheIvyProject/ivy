// Test: struct default member initializers with zero-init field
#pragma ivy cnumber

struct Defaults {
    int a = 111;
    int b = 222;
    int c;
};

extern "C" int printf(const char* fmt, ...);

int main() {
    Defaults d;
    printf("defaults: %d %d %d\n", d.a, d.b, d.c);
    return 0;
}
