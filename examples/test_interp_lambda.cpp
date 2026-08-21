#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

int32_t main() {
    int32_t r1 = [](int32_t x) -> int32_t { return x + 1; }(5);
    printf("no-capture: %d\n", r1);
    return 0;
}
