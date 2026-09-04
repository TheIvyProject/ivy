# 🏷️ Enums & Namespaces

Ivy provides scoped/unscoped enumerations and namespaces to structure constants, prevent identifier collisions, and organize software boundaries cleanly.

---

### 1. Enumerations (`enum` and `enum class`)

Ivy supports both C-style unscoped enums and modern scoped enums.

#### Unscoped Enums (`enum`)
Constants are exported directly into the enclosing scope:

```ivy
enum Color {
    Red,
    Green,
    Blue
};

Color c = Red;
```

#### Scoped Enums (`enum class` / `enum struct`)
Constants are encapsulated within the enum's type scope, preventing naming collisions:

```ivy
enum class Status : uint8 {
    Idle = 0,
    Running = 1,
    Finished = 2,
    Error = 255
};

Status s = Status::Running;
```

#### Constant Expression & Custom Values
Enum values can be computed using arithmetic and bitwise expressions during compilation:

```ivy
enum Flags : uint32 {
    None  = 0,
    Read  = 1 << 0,
    Write = 1 << 1,
    Exec  = 1 << 2,
    All   = Read | Write | Exec
};
```

---

### 2. Namespaces

Namespaces organize definitions into distinct scopes and prevent naming collisions across libraries.

#### Basic & Nested Namespaces

```ivy
namespace math {
    namespace geometry {
        constexpr float32 PI = 3.14159265f;

        fn circleArea(float32 radius) -> float32 {
            return PI * radius * radius;
        }
    }
}
```

#### Accessing Members via Scope Resolution (`::`)

```ivy
import ivy.io

void main() {
    float32 area = math::geometry::circleArea(5.0f);
    io::print(area);
}
```

#### Internal Resolution & Mangling
- Inside a namespace, unqualified member names resolve locally first.
- Symbol names are mangled according to the target ABI (Itanium/MSVC) to ensure safe, distinct links.
