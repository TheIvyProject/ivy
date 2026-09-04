# 🔄 C++ Interoperability (`import cpp`)

Ivy provides native, seamless interoperability with C++ libraries and standard headers without requiring manual FFI wrappers or C-shim glue code.

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

### 2. Under the Hood: Auxiliary Compilation & ABI Sharing

Unlike traditional FFI (Foreign Function Interface) which requires `extern "C"` bindings:
1. **Auxiliary Clang Compiler:** `ivyc` delegates C++ header parsing and compilation to an auxiliary C++ compiler (such as Clang) to produce object files or module interfaces.
2. **Direct ABI Compatibility:** Ivy natively follows the platform's C++ ABI (Itanium ABI for Linux/macOS, MSVC ABI for Windows). Name mangling, memory layout, and calling conventions match 100%, allowing direct zero-overhead linking.

---

### 3. Safety Classification of C++ APIs

Because C++ does not enforce Ivy's compile-time memory safety and lifetime guarantees, Ivy classifies imported C++ APIs into three distinct safety tiers:

---

#### Tier 1: Safe C++ APIs
Encapsulated types, value types, and member functions without raw pointer exposure can be used directly in safe Ivy code.

```ivy
import cpp "geometry/vector.hpp";

void main() {
    // Vector manages its own memory safely via RAII
    Vector v;
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
