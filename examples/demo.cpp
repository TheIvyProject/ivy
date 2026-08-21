// Ivy subset demo — lexing sample.
// Restrictions: no classes, no templates, no exceptions, no preprocessor.
#pragma ivy cnumber

extern "C" void* malloc(unsigned long n);
extern "C" void free(void* p);

// Test Lifetime Attributes
[[ivy::lt_def(a)]] const char* [[ivy::lt_ret(a)]] select_first(
    const char* x [[ivy::lt(a)]],
    const char* y [[ivy::lt(a)]]
) {
    return x;
}

// Test Unsafe Block and C API Allocation
void raw_alloc() {
    [[ivy::unsafe]] {
        void* p = malloc(1024);
        free(p);
    }
}

int main() {
    const char* a = "ivy";
    const char* b = "lang";
    const char* r = select_first(a, b);
    for (int i = 0; i < 3; ++i) {
        if (r != nullptr) {
            return 0;
        }
    }
    return 1;
}