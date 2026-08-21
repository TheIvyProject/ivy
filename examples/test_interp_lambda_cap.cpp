#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

int32_t main() {
    int32_t base = 10;
    int32_t r1 = [base](int32_t x) -> int32_t { return x + base; }(5);
    printf("by-value: %d\n", r1);
    return 0;
}
