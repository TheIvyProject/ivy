// Test: lambda expression — parsing only
#pragma ivy cnumber

extern "C" int printf(const char* fmt, ...);

int32_t main() {
    // Direct call of lambda — no captures
    int32_t r = [](int32_t x) -> int32_t { return x + 1; }(5);
    printf("result: %d\n", r);

    return 0;
}
