#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

struct Point {
    int32_t x;
    int32_t y;
};

int32_t sum(Point p) {
    return p.x + p.y;
}

int32_t main() {
    Point pt = {3, 4};
    int32_t s = sum(pt);
    printf("sum: %d\n", s);

    int32_t total = 0;
    for (int32_t i = 1; i <= 5; ++i) {
        total += i;
    }
    printf("loop total: %d\n", total);

    int32_t a = 10;
    int32_t b = 20;
    if (a < b) {
        printf("a is smaller\n");
    } else {
        printf("b is smaller\n");
    }
    return 0;
}
