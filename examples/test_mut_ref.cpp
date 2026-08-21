// Test mutable reference — T& can modify the referent.
void incr(int32_t& x) { x = x + 1; }

int main() {
    int32_t a = 5;
    incr(a);
    // After incr, a should be 6.
    return a - 6;  // expect 0
}
