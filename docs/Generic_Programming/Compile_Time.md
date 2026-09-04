# ⏱️ Compile-Time Programming

Ivy features powerful compile-time evaluation mechanisms to compute values, specialize algorithms, and eliminate runtime overhead without sacrificing safety.

---

### 1. `constexpr` Variables and Functions

- **`constexpr` Variables:** Must be initialized with a compile-time constant expression.
- **`constexpr` Functions:** Can be evaluated at compile time if called with constant arguments, or evaluated at runtime if called with runtime arguments.

```ivy
constexpr int32 factorial(int32 n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

constexpr int32 FACT_5 = factorial(5); // Evaluated at compile time (120)

int32 runtime_input = 4;
int32 fact_val = factorial(runtime_input); // Evaluated at runtime
```

---

### 2. Immediate Functions (`consteval`)

Functions marked `consteval` are **immediate functions**. They are guaranteed to be evaluated strictly at compile time. Calling a `consteval` function with non-constant arguments produces a compile-time error.

```ivy
consteval int32 square(int32 n) {
    return n * n;
}

constexpr int32 SQ_10 = square(10); // ✅ OK

int32 x = 5;
// int32 bad = square(x); // ❌ Error: argument is not a compile-time constant
```

> **Note:** `consteval` functions exist purely at compile time and are stripped before code generation (never emitted to LLVM IR).

---

### 3. Compile-Time Initialization (`constinit`)

`constinit` enforces that a variable with static or thread-local storage duration must be initialized at compile time. Unlike `constexpr`, `constinit` **does not make the variable immutable**; the variable can still be modified at runtime after program startup.

```ivy
constexpr int32 getInitialSeed() {
    return 1337;
}

// Guaranteed compile-time initialization, but mutable at runtime
constinit int32 globalCounter = getInitialSeed();

void increment() {
    globalCounter += 1; // ✅ Modifiable at runtime
}
```

> **Safety Benefit:** `constinit` completely eliminates the **Static Initialization Order Fiasco** of C++, ensuring that global state is safely initialized before any runtime code executes.

---

### 4. Compile-Time Conditional Branching (`if constexpr`)

`if constexpr` evaluates a condition during compilation. The discarded branch is never compiled into the binary, preventing type errors or unused code bloat.

```ivy
template<typename T>
void processValue(T val) {
    if constexpr (sizeof(T) > 4) {
        // Compiled only for types larger than 4 bytes (e.g. int64, float64)
        io::print("Large type processing");
    } else {
        // Compiled for small types (e.g. int8, int16, int32)
        io::print("Small type processing");
    }
}
```

---

### 5. Compile-Time Assertions (`static_assert`)

`static_assert` verifies invariant conditions during compilation. If the condition evaluates to `false`, compilation fails with the provided diagnostic message.

```ivy
static_assert(sizeof(int32) == 4, "int32 must be exactly 4 bytes");
static_assert(sizeof(iptr) == sizeof(void*), "iptr width must match pointer size");
```

---

### 6. Template Metaprogramming & Non-Type Template Parameters

Templates accept non-type parameters (such as integers, pointers, and enums) to parameterize types and algorithms at compile time.

```ivy
template<typename T, int32 N>
struct FixedArray {
    T data[N];

    constexpr int32 size() const {
        return N;
    }
};

FixedArray<int32, 10> buffer;
static_assert(buffer.size() == 10, "Buffer size mismatch");
```

---

### 7. Comptime Execution with IvyInterpret

Unlike traditional C++ template meta-evaluation limits, `ivyc` integrates **IvyInterpret** directly into the compiler pipeline. Non-trivial constant expressions and comptime functions are executed directly on MIR data structures at compile time, enabling fast and comprehensive meta-programming.
