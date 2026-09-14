# 🔄 C++ Interoperability (`import cpp`)

Ivy provides native, seamless interoperability with C++ libraries and standard headers without requiring manual FFI wrappers, C-shim glue code, or redundant function re-declarations.

---

### 1. The `import cpp` Syntax

Ivy allows direct importation of C++ standard libraries and header files using the `import cpp` directive:

```ivy
// Importing C++ standard library headers
import cpp <vector>;
import cpp <string>;

// Importing local C++ header files
import cpp "graphics/renderer.hpp";
import cpp "legacy_library.h";
```

---

### 2. Zero-Cost Binary Compatibility: Why No FFI Is Needed

Ivy originally originated as a safe subset of C++. Although it has branched out into an independent language, its underlying **memory layout, struct alignment, calling conventions, and ABI are identical to C++**.

Because Ivy and C++ are functionally one and the same at the machine level:
- **No FFI Glue Layer:** There is zero marshalling, no wrapper overhead, and no runtime conversion cost. Ivy binaries and C++ binaries are 100% interoperable natively.
- **Direct ABI Matching:** Ivy strictly targets the host platform's C++ ABI (MSVC ABI on Windows, Itanium ABI on Linux/macOS).
- **No Redundant Declarations:** Because Ivy uses C++ ABI natively, you do **not** need to re-declare functions from imported C++ headers in your `.ivy` file, and you do **not** need `extern "C"` wrappers. The C++ header's function signatures are directly callable from Ivy code.
- **Auxiliary Compiler Pipeline:** Ivy does not ship with a built-in C++ compiler. When downloading Ivy, users can optionally bundle Clang. If Clang is not bundled, Ivy automatically discovers an existing C++ compiler available on the host machine (e.g. Clang, GCC, MSVC `cl.exe`) to compile C++ source/headers into compatible object files, and the linker directly joins them together seamlessly.

---

### 3. The `cpp::` Virtual Namespace

All symbols imported via `import cpp` are placed into the **`cpp::` virtual namespace**. This means:
- C++ functions and types are accessible via the `cpp::` prefix.
- No `extern "C"` declaration is needed — Ivy's ABI is C++, so the linker resolves C++ mangled symbols directly.
- No function re-declaration is needed in the `.ivy` file — the header's declarations are used as-is.

```ivy
import cpp "math/geometry.hpp";

void main() {
    // Call C++ function directly via cpp:: namespace
    double area = cpp::circle_area(5.0);

    // Use C++ types directly
    cpp::Point p;
    p.x = 3.0;
    p.y = 4.0;
}
```

> **Contrast with `import c`:** C headers use a different ABI (C calling convention, no name mangling). Because of this ABI mismatch, `import c` **requires** explicit `extern "C"` declarations in the `.ivy` file and does **not** use the `cpp::` namespace. See [C_Interoperability.md](./C_Interoperability.md) for details.

---

### 4. Safety Classification of C++ APIs

Because C++ does not enforce Ivy's compile-time memory safety and lifetime guarantees, Ivy classifies imported C++ APIs into three distinct safety tiers:

---

#### Tier 1: Safe C++ APIs
Encapsulated types, value types, and member functions without raw pointer exposure can be used directly in safe Ivy code.

```ivy
import cpp "geometry/vector.hpp";

void main() {
    // Vector manages its own memory safely via RAII
    cpp::Vector v;
    v.push_back(42); // ✅ Safe: direct execution
}
```
> **Rule:** Originating from C++ does not automatically mean unsafe. High-level, well-encapsulated C++ types remain safe to use in Ivy.

---

#### Tier 2: Unsafe / Raw Pointer APIs
C++ APIs that consume or manipulate raw pointers (`T*`), raw buffers, or unchecked memory addresses require manual verification.

```ivy
// C++ declaration: void write_data(int32* ptr, size n);
import cpp "lowlevel/io.hpp";

void execute(int32* buffer, size length) {
    unsafe {
        // Compiler cannot guarantee pointer validity or bounds
        cpp::write_data(buffer, length);
    }
}
```
> **Rule:** Ivy compiler cannot verify pointer validity across foreign boundaries. All raw pointer invocations must be enclosed within `unsafe { ... }`.

---

#### Tier 3: Ambiguous Ownership & Lifetimes
C++ functions that return raw pointers without explicit lifetime annotations or known ownership semantics are treated as unsafe by default.

```ivy
// C++ declaration: const char* get_name();
import cpp "system/info.hpp";

void retrieveInfo() {
    unsafe {
        // Unknown lifetime: pointer might be static, heap-allocated, or transient
        auto name = cpp::get_name();
    }
}
```

```text
[Importer Declaration Metadata]
cpp::get_name:
    return: const char*
    lifetime: unknown
    safety: unsafe
```

> **Rule:** When lifetime or ownership cannot be formally modeled by Ivy's Borrow Checker, the compiler conservatively flags the API as `unsafe`.
