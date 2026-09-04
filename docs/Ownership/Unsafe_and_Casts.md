# 🛡️ Unsafe & Cast Operators

Ivy enforces memory safety by default. Low-level memory manipulations, raw pointers, manual allocations, and unsafe type coercions are restricted strictly to explicit `unsafe` blocks.

---

### 1. The `unsafe` Block

Any operation outside the safe subset of Ivy must be encapsulated inside `unsafe { ... }`:

```ivy
void rawMemoryExample() {
    unsafe {
        // Raw pointer arithmetic and direct allocation allowed only here
        void* ptr = malloc(1024);
        // ...
        free(ptr);
    }
}
```

#### What is restricted to `unsafe`:
- Raw pointer dereferencing and pointer arithmetic (`ptr + offset`).
- Calling external C functions (`extern "C"` declarations).
- Manual memory management (`malloc`, `free`, `new`, `delete`).
- Type-punning and non-trivial pointer casting (`reinterpret_cast`).
- Bypassing const correctness (`const_cast`).

---

### 2. Cast Operators

Ivy provides explicit casting mechanisms to handle conversions safely and transparently.

| Cast Operator | Safety Status | Use Case |
| :--- | :--- | :--- |
| `static_cast<T>(expr)` | **Safe** | Numeric promotions/truncations, enum-to-integer, and valid upcasts. |
| `reinterpret_cast<T>(expr)` | **Unsafe only** | Low-level bit reinterpretation and arbitrary pointer casting. |
| `const_cast<T>(expr)` | **Unsafe only** | Explicit removal/addition of `const` qualifiers. |
| C-style `(T)expr` | **Unsafe only** | Legacy conversions; strictly forbidden in safe code. |

#### Safe Casting (`static_cast`)

```ivy
int32 wholeNumber = 100;
float32 decimalNumber = static_cast<float32>(wholeNumber);

enum class Kind : uint8 { Primary = 1 };
uint8 raw = static_cast<uint8>(Kind::Primary);
```

#### Unsafe Casting (`reinterpret_cast` & `const_cast`)

```ivy
unsafe {
    iptr address = 0xDEADBEEF;
    int32* ptr = reinterpret_cast<int32*>(address);

    const int32 original = 42;
    int32* mutableRef = const_cast<int32*>(&original);
}
```

---

### 3. 🚫 Forbidden Casts (No RTTI)

`dynamic_cast` is **banned entirely** in Ivy because it requires Run-Time Type Information (RTTI) and introduces runtime overhead conflicting with Ivy's zero-cost abstraction philosophy.
