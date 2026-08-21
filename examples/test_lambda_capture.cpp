// Test: lambda expression with captures
#pragma ivy cnumber

extern "C" int printf(const char* fmt, ...);

int32_t main() {
    // Capture by value
    int32_t base = 10;
    int32_t r1 = [base](int32_t x) -> int32_t { return x + base; }(5);
    printf("by-value: %d\n", r1);

    // Capture by reference
    int32_t counter = 0;
    [&counter](int32_t n) -> int32_t { counter += n; return counter; }(3);
    printf("by-ref counter: %d\n", counter);

    // No captures, direct call
    int32_t r3 = [](int32_t x) -> int32_t { return x * 2; }(7);
    printf("no-capture: %d\n", r3);

    return 0;
}
