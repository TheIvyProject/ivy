#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

int32_t main() {
    int32_t x = 10;
    int32_t y = 20;
    printf("x+y=%d\n", x + y);
    return 0;
}
