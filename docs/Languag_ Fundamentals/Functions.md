
# ⚡ Functions

Functions in Ivy are self-contained blocks of code that perform specific tasks. Ivy supports standard declarations, definitions, lambda/anonymous functions, and trailing return types.

---

### Function Prototype (Declaration)

```ivy
[return_type] [function_name]([parameter_list]);
```

**Example:**
```ivy
int32 add(int32 a, int32 b);
void greet(string message);
```

---

### Function Definition

```ivy
[return_type] [function_name]([parameter_list]) {
    // Function body
    return [return_value];
}
```

**Example:**
```ivy
import ivy.io

int32 add(int32 a, int32 b) {
    return a + b;
}

void printHello() {
    io::print("Hello!");
}
```

---

### Trailing Return Type (`fn`)

Ivy uses the `fn` keyword for trailing return type declarations instead of C++'s `auto`.

```ivy
fn [function_name]([parameter_list]) -> [return_type] {
    // Function body
}
```

**Example:**
```ivy
fn multiply(int32 x, int32 y) -> int32 {
    return x * y;
}
```

**Why `fn` instead of `auto`:**
- **Clearer Intent:** Explicitly marks the construct as a function declaration rather than variable type deduction.
- **Better Readability & Consistency:** Aligns modern function syntax (`fn name() -> type`) without overloading the semantic meaning of `auto`.
- **Cleaner Parser:** Reduces parsing ambiguity between type-deduced variables and trailing return type functions.


---

### Anonymous Functions (Lambdas)

Ivy supports lambda expressions with capture lists:

```ivy
[captures](parameters) -> return_type {
    // Lambda body
};
```

**Example:**
```ivy
auto square = [](int32 n) -> int32 {
    return n * n;
};

int32 result = square(5);
```

---

### External C Functions (`extern "C"`)

Ivy supports declaring external functions with C linkage using `extern "C"`. This disables name mangling and enforces the standard C ABI for foreign function calls (FFI) and standard C library interoperability.

> **C Data Types & `#pragma ivy cnumber`:**  
> In Ivy, primitive types like `int32` are fixed-width types and are **not** type aliases for C's `int`. When declaring external C functions that expect native C types (`int`, `char`, `long`, `float`, `double`), you must enable `#pragma ivy cnumber` in that scope.

#### 1. Calling C Functions from Ivy (`unsafe` Required)
C functions lack compile-time lifetime and safety guarantees. Therefore, **calling an `extern "C"` function from Ivy must be wrapped inside an `unsafe { ... }` block**:

```ivy
#pragma ivy cnumber

extern "C" int puts(const char* s);
extern "C" int printf(const char* format, ...);
extern "C" void* malloc(size size);
extern "C" void free(void* ptr);

void printMessage() {
    unsafe {
        puts("Hello from C FFI!");
    }
}
```

#### 2. Exporting Ivy Functions to C (Safe)
Defining an `extern "C"` Ivy function to be called from C/C++ does **not** require an `unsafe` block:

```ivy
#pragma ivy cnumber

// Exported with standard C ABI and unmangled name
extern "C" int ivy_add(int a, int b) {
    return a + b;
}
```


