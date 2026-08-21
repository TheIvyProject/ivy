// Test: type promotion — should all pass
#pragma ivy cnumber
// extern "C" int printf(const char* format, ...);
// ^ not supported (varargs)

int main() {
    // int + unsigned -> unsigned int
    unsigned int u = 3;
    int a = 2;
    unsigned int r = a + u;     // r = 5u

    // long + int -> long
    long l = 10;
    int i = 5;
    long lr = l + i;            // lr = 15

    // double + float -> double
    double d = 3.14;
    float f = 2.5f;
    double dr = d + f;         // dr = 5.64

    // bool + int -> int (bool promotes to int)
    int sum = true + 1;         // 2

    // char * short -> int
    char c = 'A';
    short s = 1;
    int cs = c + s;             // 'A' (65) + 1 = 66

    // unsigned int / unsigned int -> unsigned int
    unsigned int u2 = 7;
    unsigned int ud = u2 / 3;  // 2

    // long long + int -> long long
    long long ll = 1LL;
    int i2 = 2;
    long long llr = ll + i2;   // 3

    // struct { unsigned int u; } -> check unsigned promotion in compound
    unsigned int u3 = 10;
    unsigned int ur = u3 - 1;   // 9

    return 0;
}