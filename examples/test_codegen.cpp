// Codegen stress test — exercises every lowering path.
#pragma ivy cnumber

extern "C" void* malloc(unsigned long size);
extern "C" void free(void* p);
extern "C" int printf(const char* fmt, int a, int b, int c, int d, int e, int f, int g);

int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    // recursion + ternary (fib(5) = 5)
    int t = fib(5) > 4 ? 10 : 20;  // 5 > 4 -> t = 10
    // short-circuit &&
    if ((t == 10) && (t != 20) && (fib(0) == 0)) {
        t = t + 1;  // t = 11
    }
    // short-circuit ||
    if ((t == 99) || (t == 11)) {
        t = t * 2;  // t = 22
    }
    // new / delete / index / pointer arithmetic (unsafe API in the Ivy subset)
    int back = 0;
    [[ivy::unsafe]] {
        int* p = new int(7);
        p[0] = t;  // p[0] = 22
        int* q = p + 1;
        q[0] = 5;
        back = q[-1];  // 22
        delete p;
    }
    // unsigned division / remainder
    unsigned int u = 100;
    unsigned int v = u / 3;  // 33
    unsigned int w = u % 7;  // 2
    // char escape
    char c = '\n';
    // do-while + break
    int i = 0;
    do {
        i = i + 1;
        if (i == 3) break;
    } while (i < 10);
    // while + continue
    int j = 0;
    int sum = 0;
    while (j < 5) {
        j = j + 1;
        if (j == 2) continue;
        sum = sum + j;  // 1 + 3 + 4 + 5 = 13
    }
    // double arithmetic
    double d = 1.5;
    d = d + 1.5;  // 3.0
    if (t == 22 && v == 33 && w == 2 && i == 3 && sum == 13 && back == 22 &&
        c == '\n' && d == 3.0) {
        return 0;
    }
    printf("t=%d v=%d w=%d i=%d sum=%d back=%d c=%d\n", t, v, w, i, sum, back, c);
    return 1;
}
