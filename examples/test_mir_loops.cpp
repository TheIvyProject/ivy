#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

int32_t main() {
    int32_t sum = 0;
    for (int32_t i = 1; i <= 10; i++) {
        sum += i;
    }
    printf("sum=%d\n", sum);

    int32_t x = 10;
    while (x > 0) {
        x--;
    }
    printf("x=%d\n", x);

    int32_t y = 5;
    do {
        y *= 2;
    } while (y < 100);
    printf("y=%d\n", y);

    return 0;
}
