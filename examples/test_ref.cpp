// Test: const T& / T& (reference)
#pragma ivy cnumber

int32_t square(int32_t x) {
    return x * x;
}

int32_t add_ref(const int32_t& a, const int32_t& b) {
    return a + b;
}

int32_t mut_ref(int32_t& x) {
    return x + 1;
}

int main() {
    int32_t a = 10;
    int32_t b = 20;

    // const reference
    const int32_t& ref_a = a;
    int32_t sum = add_ref(a, b);

    // mutable reference
    int32_t c = 5;
    int32_t d = mut_ref(c);

    return 0;
}
