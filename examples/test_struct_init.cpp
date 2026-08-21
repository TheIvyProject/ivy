// Test: struct aggregate initialization (P4.4 limit fix)
#pragma ivy cnumber

struct Point {
    int x;
    int y;
};

struct Vec3 {
    int x;
    int y;
    int z;
};

extern "C" int printf(const char* fmt, ...);

int main() {
    // Basic aggregate init: `Point p = {1, 2};`
    Point p = {10, 20};
    printf("init: %d %d\n", p.x, p.y);

    // Constructor-style init: `Point p2{30, 40};`
    Point p2 = {30, 40};
    printf("ctor: %d %d\n", p2.x, p2.y);

    // Partial init: only first field specified, rest zero
    Vec3 v = {100};
    printf("partial: %d %d %d\n", v.x, v.y, v.z);

    // Empty init: all fields zero
    Vec3 v2 = {};
    printf("empty: %d %d %d\n", v2.x, v2.y, v2.z);

    // Zero-init struct variable (no explicit initializer)
    Point p3;
    printf("zero: %d %d\n", p3.x, p3.y);

    // Reassign with aggregate init
    p = {777, 888};
    printf("reassign: %d %d\n", p.x, p.y);

    return 0;
}
