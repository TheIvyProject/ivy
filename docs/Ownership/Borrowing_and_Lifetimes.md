# 🔍 Borrowing & Lifetimes

Borrowing allows referencing data without transferring ownership. Ivy enforces the **Aliasing XOR Mutability** rule at compile-time to eliminate data races and dangling pointer bugs.

---

### 1. Borrowing Semantics

#### Immutable Borrow (`const T&`)
Multiple immutable references can coexist simultaneously for read-only access:

```ivy
int32 a = 42;
const int32& r1 = a; // Immutable borrow
const int32& r2 = a; // Multiple immutable borrows allowed

// r1 = 50;          // ❌ Error: read-only reference
```

#### Mutable Borrow (`T&`)
Only **one** active mutable reference is permitted at any given time:

```ivy
int32 a = 42;
int32& r = a;        // Mutable borrow

// const int32& r2 = a; // ❌ Error: cannot borrow immutably while mutably borrowed
// int32& r3 = a;       // ❌ Error: only one mutable borrow allowed
```

---

### 2. Aliasing XOR Mutability Rule

```
┌─────────────────────────────────────────────────────────┐
│        Either many read-only references (const T&)      │
│                         - OR -                          │
│           Exactly one mutable reference (T&)            │
└─────────────────────────────────────────────────────────┘
```

The Ivy Borrow Checker analyzes reference usage to ensure references never outlive the data they point to and that mutable references maintain exclusive access.

---

### 3. Lifetime Syntax (`lifetime<...>`)

Ivy uses native generic lifetime parameters declared with `lifetime<$...>` to express reference lifetimes across function boundaries.

#### Syntax Rules:
1. **Lifetime Variables:** Prefixed with `$` (e.g., `$a`, `$b`).
2. **Placement:** Lifetime annotations are placed **between the type and the variable/function name**:
   - `const int32& $a x` (reference to `int32` with lifetime `$a`).
   - Note: `&` is not part of the lifetime syntax; it retains standard C++ alias/reference semantics.
3. **Declaration Ordering:** **Lifetime parameters MUST be declared before template parameters**.

#### Function Lifetimes Example:

```ivy
lifetime<$a, $b>
const int32& $a foo(
    const int32& $a x,
    const int32& $b y
) {
    return x; // ✅ OK: lifetime $a satisfies return lifetime $a
    // return y; // ❌ Error: lifetime $b cannot satisfy return lifetime $a
}
```

#### Lifetime with Templates:

```ivy
lifetime<$a>
template<typename T>
const T& $a selectValue(const T& $a x, const T& $a y) {
    return x;
}
```
