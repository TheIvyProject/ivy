# 🧬 Templates & Concepts

Templates enable type-safe, zero-cost generic programming in Ivy. Concepts provide compile-time constraints with clear error diagnostics.

---

### 1. Function Templates

Function templates generate specialized concrete functions on demand for specified data types.

```ivy
template<typename T>
T add(T a, T b) {
    return a + b;
}

// Explicit template instantiation
int32 res1 = add<int32>(5, 10);

// Template argument deduction (inferred from arguments)
float32 res2 = add(1.5f, 2.5f);
```

---

### 2. Class / Struct Templates

Templates can parameterize whole aggregate data structures:

```ivy
template<typename T>
struct Box {
    T value;

    T getValue() const {
        return this.value;
    }

    void setValue(T newVal) {
        this.value = newVal;
    }
};

Box<int32> intBox = {42};
int32 val = intBox.getValue();
```

---

### 3. Variadic Templates & Parameter Packs

Ivy supports variadic templates (`typename... Args`) and parameter pack expansions:

```ivy
template<typename T>
T sum(T val) {
    return val;
}

template<typename T, typename... Rest>
T sum(T first, Rest... rest) {
    return first + sum(rest...);
}

int32 total = sum(1, 2, 3, 4, 5); // 15
```

---

### 4. Compile-Time Constraints: Concepts

Concepts constrain template arguments to types that satisfy specific structural or behavioral requirements, producing concise, easy-to-read error messages instead of deep template instantiations.

```ivy
concept Addable = requires(T a, T b) {
    a + b;
};

template<Addable T>
T addValues(T x, T y) {
    return x + y;
}
```

If a type without `operator+` is passed, Ivy outputs an immediate, informative error:
```text
error: concept 'Addable' not satisfied: type 'MyStruct' does not support operator '+'
```
