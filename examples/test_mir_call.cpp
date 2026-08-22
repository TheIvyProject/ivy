#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

int32_t add(int32_t a, int32_t b) {
    return a + b;
}

int32_t main() {
    int32_t z = add(10, 20);
    printf("z=%d\n", z);
    return 0;
}
