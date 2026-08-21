// Test P4.2: namespace support
#pragma ivy cnumber

// --- Basic namespace with functions ---
namespace math {
    int32_t add(int32_t a, int32_t b) { return a + b; }
    int32_t sub(int32_t a, int32_t b) { return a - b; }
}

int32_t test_basic() {
    return math::add(3, 4);  // 7
}

// --- Bare call within same namespace ---
namespace calc {
    int32_t helper(int32_t x) { return x * 2; }
    int32_t main_func(int32_t x) { return helper(x) + 1; }  // bare call
}

int32_t test_bare_call() {
    return calc::main_func(5);  // helper(5)=10 + 1 = 11
}

// --- Nested namespaces ---
namespace outer {
    namespace inner {
        int32_t deep() { return 42; }
    }
}

int32_t test_nested() {
    return outer::inner::deep();  // 42
}

// --- Namespace with enum ---
namespace colors {
    enum Color { Red, Green, Blue };
    int32_t get_red() { return Red; }  // bare enum constant in namespace
}

int32_t test_ns_enum() {
    return colors::get_red();  // 0
}

int32_t test_ns_scoped_enum() {
    return colors::Green;  // bare unscoped enum constant via qualified ns
}

// --- Namespace with scoped enum ---
namespace dirs {
    enum class Direction { Up, Down, Left = 10, Right };
    int32_t get_left() { return Direction::Left; }  // scoped enum in namespace
}

int32_t test_ns_scoped_enum_value() {
    return dirs::Direction::Left;  // 10 — fully qualified
}

int32_t test_ns_scoped_enum_func() {
    return dirs::get_left();  // 10
}

// --- Multiple namespaces, same function name ---
namespace ns_a {
    int32_t value() { return 1; }
}
namespace ns_b {
    int32_t value() { return 2; }
}

int32_t test_disambig() {
    return ns_a::value() + ns_b::value();  // 3
}

// --- Global function calling namespaced function ---
int32_t global_helper() { return 100; }

int32_t test_global_calls_ns() {
    return math::add(global_helper(), 1);  // 101
}

// --- Namespace function calling global function ---
namespace ns_c {
    int32_t call_global() { return global_helper(); }  // bare global call
}

int32_t test_ns_calls_global() {
    return ns_c::call_global();  // 100
}

int main() {
    return test_basic();  // 7
}
