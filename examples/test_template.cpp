#pragma ivy cnumber
extern "C" int printf(const char* fmt, ...);

// Template function: type parameter
template <typename T>
T add(T a, T b) {
    return a + b;
}

// Template function with explicit instantiation
template <typename T>
T identity(T x) {
    return x;
}

int main() {
    int a = add<int>(3, 4);
    double b = add<double>(1.5, 2.5);
    int c = identity<int>(42);
    printf("a=%d\n", a);
    printf("b=%.1f\n", b);
    printf("c=%d\n", c);
    return 0;
}
