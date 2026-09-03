# 🛡️ Ownership & Memory Model

Ivy implements an **Ownership & Borrowing** model designed to combine zero-cost abstractions with compile-time memory safety.

---

### 1. Core Principles

1. **Single Owner:** Each piece of data has exactly one owner variable at any given moment.
2. **RAII Scope Destruction:** When the owner variable goes out of scope, its destructor (`~T()`) is automatically invoked to reclaim memory and system resources.
3. **C++ Compatible Copy Semantics:** Assignment `T x = y;` follows C++ copy semantics by default.
4. **Explicit Move Semantics:** Move operations happen strictly when:
   - The right-hand side is an **rvalue** (temporary expression): `x = Foo();`
   - The developer explicitly invokes **`move(a)`** (built-in move): `b = move(a);`

---

### 2. Move Semantics & Moved-out State

When a variable is moved via built-in `move(a)`:
- `a` transitions into a **Moved-out (Uninitialized)** state.
- Reading or accessing `a` produces a **compile-time error**.
- Reassigning `a = value;` re-initializes the variable, making it valid again.

```ivy
int32 a = 100;
int32 b = move(a); // 'a' is moved-out

// int32 c = a;         // ❌ Compile Error: use of moved-out variable 'a'
a = 200;                // ✅ OK: 'a' re-initialized with a new value
int32 d = a;            // ✅ OK: 'a' is now valid
```

---

### 3. Safe Zone vs Unsafe Zone

Ivy provides a first-class `unsafe { ... }` block for low-level system operations:

```ivy
unsafe {
    int32* rawPtr = nullptr;
    // Direct pointer arithmetic and manual memory management
}
```

| Concept | Safe Zone (Default) | Unsafe Zone (`unsafe { ... }`) |
| :--- | :--- | :--- |
| **`nullptr`** | ❌ Forbidden (use `ivy::optional<T>`) | ✅ Allowed (`nullptr_t` support) |
| **Raw Pointers** | ❌ Forbidden | ✅ Allowed |
| **Manual `free()` / Pointer Arithmetic** | ❌ Forbidden | ✅ Allowed |
| **Unsafe Blocks** | Disallowed | `unsafe { ... }` |
