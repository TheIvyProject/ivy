#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

int32_t main() {
    int32_t sum = 0;
    for (int32_t i = 1; i <= 3; i++) {
        sum += i;
    }
    printf("sum=%d\n", sum);
    return 0;
}
