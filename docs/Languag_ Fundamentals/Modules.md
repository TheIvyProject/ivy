# 📦 Modules

Ivy includes a first-class, modern module system designed for fast compilation, clean namespace encapsulation, and zero header file overhead.

---

### 1. Module Declaration (`export module`)

A module is declared using `export module <module_name>;` at the top of the file:

```ivy
// math.ivy
export module math;

// Exported function (public interface)
export int32 add(int32 a, int32 b) {
    return a + b;
}

// Internal helper function (private to this module)
int32 helper(int32 x) {
    return x * 2;
}

// Exported struct type
export struct Vec2 {
    int32 x;
    int32 y;
};

// Exported type alias
export using Scalar = int32;
```

---

### 2. Module Import (`import`)

Modules are consumed using the `import <module_name>;` statement:

```ivy
// main.ivy
import math;
import ivy.io;

int32 main() {
    Vec2 v;
    v.x = 10;
    v.y = 20;

    int32 result = add(v.x, v.y);
    io::print(result);

    return 0;
}
```

---

### 3. Module Compilation & Interface Files (`.ivm`)

Ivy uses binary/text module interface files (`.ivm` — *Ivy Module Interface*), similar to Clang's `.pcm` files:

1. **Compilation Phase:**
   When compiling a module unit, `ivyc` generates an `.ivm` interface file containing only public `export` declarations (functions, structs, enums, aliases, concepts).
2. **Import Phase:**
   When consuming an `import`, `ivyc` parses the precompiled `.ivm` file directly instead of re-parsing source files or header files.
3. **Link Phase:**
   Function bodies are compiled into object files (`.obj` / `.o`) and linked at link-time.

---

### 4. Visibility & Export Rules

| Construct | Exportable? | Description |
| :--- | :--- | :--- |
| `export fn / export Type func()` | ✅ Yes | Function declaration/signature exposed to consumers |
| `export struct / export class` | ✅ Yes | Type layout and members exposed to consumers |
| `export enum` | ✅ Yes | Enumeration values exposed to consumers |
| `export using Name = Type;` | ✅ Yes | Type alias exposed to consumers |
| Non-exported declarations | ❌ No | Strictly private to the module translation unit |
