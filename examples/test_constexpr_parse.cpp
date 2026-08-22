// Test constexpr/consteval parsing
#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

// constexpr function
constexpr int square(int n) {
    return n * n;
}

// consteval function
consteval int cube(int n) {
    return n * n * n;
}

int main() {
    // constexpr variable
    constexpr int x = 5;
    printf("x=%d\n", x);
    printf("square=%d\n", square(4));
    printf("cube=%d\n", cube(3));
    return 0;
}
