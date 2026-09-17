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

### 7. Reverse Interoperability: Calling Ivy from C

Ivy functions can also be exported to plain C. Because C does not support C++ name mangling, you export functions with `extern "C"` linkage in Ivy.

#### Why C Can Use Ivy:
1. **Identical POD Memory Layout:** Plain Old Data (POD) structs in Ivy share identical byte-level layout, alignment, and padding with C `struct`.
2. **C Calling Convention via `extern "C"`:** Exporting an Ivy function with `extern "C"` disables C++ name mangling and forces standard C calling conventions, making the object file 100% link-compatible with any C compiler (`clang`, `gcc`, `cl.exe`).

#### Workflow:

1. **Write and Export Ivy functions with `extern "C"`:**
   ```ivy
   // crypto.ivy
   export module crypto;

   // Export with C linkage (no mangling)
   export extern "C" int32 ivy_hash(const char* data, int32 len) {
       int32 hash = 5381;
       unsafe {
           for (int32 i = 0; i < len; i += 1) {
               hash = ((hash << 5) + hash) + static_cast<int32>(data[i]);
           }
       }
       return hash;
   }

   export extern "C" struct IvyBuffer {
       int32 capacity;
       int32 length;
   };
   ```

2. **C Header Declaration (`crypto.h`):**
   ```c
   // crypto.h
   #pragma once
   #include <stdint.h>

   typedef struct {
       int32_t capacity;
       int32_t length;
   } IvyBuffer;

   int32_t ivy_hash(const char* data, int32_t len);
   ```

3. **Include and Call from C:**
   ```c
   // main.c
   #include <stdio.h>
   #include "crypto.h"

   int main(void) {
       const char* text = "Hello from C";
       int32_t h = ivy_hash(text, 12);
       printf("Hash: %d\n", h);
       return 0;
   }
   ```

4. **Compile and Link:**
   ```bash
   ivyc -c crypto.ivy -o crypto.obj
   clang main.c crypto.obj -o app
   ```

---

### 8. `import c` vs `import cpp` — Summary

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
