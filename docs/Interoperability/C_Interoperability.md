# 🔗 C Interoperability (`import c`)

Ivy provides direct interoperability with C libraries and headers. Because C uses a different ABI from C++ (no name mangling, C calling convention), `import c` requires explicit `extern "C"` declarations in the `.ivy` file.

---

### 1. The `import c` Syntax

Ivy allows importation of C standard library headers and local C header files using the `import c` directive:

```ivy
// Importing C standard library headers
import c <stdio.h>;
import c <stdlib.h>;

// Importing local C header files
import c "graphics/renderer.h";
import c "legacy_lib.h";
```

---

### 2. ABI Difference: Why `extern "C"` Is Required

Ivy's native ABI is **C++ ABI** (MSVC ABI on Windows, Itanium ABI on Linux/macOS). C uses a fundamentally different ABI:
- **No name mangling:** C function names are exported as-is (e.g., `printf`), while C++ mangles names (e.g., `_Z6printfPKcz`).
- **C calling convention:** C functions use the platform's default calling convention, which differs from C++ member function calling conventions.

Because of this ABI mismatch, Ivy cannot directly call C functions without an explicit bridge. The `extern "C"` declaration in the `.ivy` file serves this purpose:
- It tells the Ivy compiler to use the C calling convention for the declared function.
- It disables C++ name mangling so the linker can resolve the symbol by its plain C name.
- It acts as a function prototype — you must declare the full signature.

> **Contrast with `import cpp`:** C++ headers share the same ABI as Ivy, so `import cpp` does **not** require `extern "C"` or function re-declaration. Symbols are placed in the `cpp::` virtual namespace. See [Cpp_Interoperability.md](./Cpp_Interoperability.md) for details.

---

### 3. No Virtual Namespace

Unlike `import cpp` (which places all symbols in the `cpp::` namespace), `import c` does **not** create a virtual namespace. Because you must declare each C function explicitly via `extern "C"` in the `.ivy` file, the functions are available directly by name — no namespace prefix needed.

```ivy
import c "math/geometry.h";

// Declare the C function's signature in Ivy
extern "C" double circle_area(double radius);

void main() {
    // Call directly — no namespace prefix
    double area = circle_area(5.0);
}
```

> **Rule:** `import c` only compiles and links the C header's object code. The function signatures must be re-declared in the `.ivy` file using `extern "C"`. The functions are then called by their plain name — no `c::` or `cpp::` prefix.

---

### 4. Compilation & Linking Pipeline

When you use `import c`, Ivy performs the following steps:

1. **Header Resolution:** The C header is searched for in the source file's directory (for quoted form `"file.h"`) or in `-I` include paths. System headers (angle form `<stdio.h>`) are declaration-only — no object code is generated from them.

2. **Compilation:** Each local C header found on disk is compiled into a temporary object file using:
   ```
   clang -x c -c <header.h> -o <temp.obj>
   ```
   The `-x c` flag forces the compiler to treat the input as C source (not C++).

3. **Linking:** All C object files are linked together with the Ivy object file:
   ```
   clang++ <ivy.o> <c0.o> <c1.o> ... -o <executable>
   ```

4. **Cleanup:** Temporary object files are removed after linking.

> **Note:** Ivy uses `clang++` (or the discovered C++ compiler) as the final linker even for C code. This ensures C++ runtime libraries are available when mixing C and C++ code. The C object files compiled with `-x c` are compatible with the C++ linker.

---

### 5. Complete Example

**C header (`c_helper.h`):**
```c
#pragma once
#include <stdint.h>

int32_t c_add(int32_t a, int32_t b) {
    return a + b;
}
```

**Ivy source (`test_import_c.ivy`):**
```ivy
// Import the local C header — compiles and links the object code
import c "c_helper.h";

// Declare the C function signature — required for C interop
extern "C" int32 c_add(int32 a, int32 b);

int32 main() {
    int32 result = c_add(100, 200);
    return result;  // = 300
}
```

---

### 6. Safety Classification of C APIs

C headers imported via `import c` are subject to the same safety tiers as C++ APIs:

| Tier | Description | Rule |
|------|-------------|------|
| **Tier 1: Safe** | Value-returning functions, no pointer params/returns | Can be called directly in safe Ivy code |
| **Tier 2: Unsafe** | Functions consuming/manipulating raw pointers (`T*`) | Must be enclosed in `unsafe { ... }` |
| **Tier 3: Ambiguous** | Functions returning raw pointers with unknown lifetime | Treated as `unsafe` by default |

```ivy
import c "lowlevel/io.h";

// C function with raw pointer — Tier 2: Unsafe
extern "C" void write_data(int32* ptr, int32 length);

void execute(int32* buffer, int32 length) {
    unsafe {
        write_data(buffer, length);
    }
}
```

> **Rule:** Ivy's `requireUnsafe()` enforcement applies equally to `import c` and `import cpp`. All raw pointer dereference, arithmetic, and indexing must occur inside `unsafe { ... }` blocks regardless of the source language.

---

### 7. `import c` vs `import cpp` — Summary

| Aspect | `import c` | `import cpp` |
|--------|-----------|-------------|
| **Target ABI** | C ABI (no mangling) | C++ ABI (name mangling) |
| **`extern "C"` required?** | ✅ Yes — must declare each function | ❌ No — direct call |
| **Function re-declaration?** | ✅ Required in `.ivy` | ❌ Not needed |
| **Virtual namespace** | None — call by plain name | `cpp::` prefix |
| **Compile flag** | `clang -x c -c` | `clang++ -x c++ -c` |
| **Linker** | `clang++` (C++ linker for compatibility) | `clang++` |
| **System headers** | Declaration-only (skip compilation) | Declaration-only (skip compilation) |
| **Safety enforcement** | `requireUnsafe()` for raw pointers | `requireUnsafe()` for raw pointers |
