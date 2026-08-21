#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

int32_t add(int32_t a, int32_t b) {
    return a + b;
}

int32_t main() {
    int32_t x = 10;
    int32_t y = 20;
    int32_t z = add(x, y);
    printf("result: %d\n", z);
    return 0;
}
