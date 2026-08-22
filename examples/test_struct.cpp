// Test: struct & class (P4.4)
#pragma ivy cnumber

// Basic struct with member access
struct Point {
    int x;
    int y;
};

// Struct with mixed types
struct Vec3 {
    int x;
    int y;
    int z;
};

// Class (treated same as struct in Ivy)
class Color {
public:
    int r;
    int g;
    int b;
};

// Struct with pointer field
struct Node {
    int value;
    int* next;
};

// Nested struct usage
struct Line {
    Point start;
    Point end;
};

// Struct in namespace
namespace geom {
    struct Rect {
        int w;
        int h;
    };
}

extern "C" int printf(const char* fmt, ...);

int main() {
    // Basic struct: declare, assign fields, read fields
    Point p;
    p.x = 10;
    p.y = 20;
    printf("struct: %d %d\n", p.x, p.y);

    // Struct with mixed types
    Vec3 v;
    v.x = 1;
    v.y = 2;
    v.z = 3;
    printf("vec3: %d %d %d\n", v.x, v.y, v.z);

    // Class
    Color c;
    c.r = 255;
    c.g = 128;
    c.b = 0;
    printf("class: %d %d %d\n", c.r, c.g, c.b);

    // Struct with pointer field + deref
    Node n;
    int nextVal = 42;
    n.value = 100;
    [[ivy::unsafe]] { n.next = &nextVal; }
    int deref = 0;
    [[ivy::unsafe]] { deref = *n.next; }
    printf("node: %d %d\n", n.value, deref);

    // Pointer to struct + arrow access
    Point* pp = &p;
    printf("ptr: %d %d\n", pp->x, pp->y);

    // Nested struct member access
    Line line;
    line.start.x = 1;
    line.start.y = 2;
    line.end.x = 3;
    line.end.y = 4;
    printf("nested: %d %d %d %d\n", line.start.x, line.start.y, line.end.x, line.end.y);

    // Struct in namespace
    geom::Rect r;
    r.w = 100;
    r.h = 200;
    printf("ns: %d %d\n", r.w, r.h);

    // Assign one struct to another (copy)
    Point p2;
    p2.x = 999;
    p2.y = 888;
    p = p2;
    printf("assign: %d %d\n", p.x, p.y);

    return 0;
}
