
# 📦 Variables

Variables in Ivy store data in named memory locations. Ivy uses explicit keywords for mutability, storage duration, and evaluation semantics.

---

### Syntax

```ivy
[Storage_Specifier] [Mutability_Specifier] Type Name = Value;
```

---

### Mutability Specifiers

| Keyword | Description |
| :--- | :--- |
| `mutable` | Variable can be modified after initialization. |
| `const` | Variable is read-only after initialization. |
| `constexpr` | Variable is a compile-time constant evaluated during compilation. |

```ivy
mutable int32 a = 10;
const int32 b = 20;
constexpr int32 c = 30;
```

---

### Storage Specifiers

| Keyword | Description |
| :--- | :--- |
| `static` | Preserves value across function calls and limits visibility to current translation unit / scope. |
| `extern` | Declares a variable defined in another translation unit. |
| `thread_local` | Creates a separate instance of the variable for each thread. |
| `volatile` | Informs compiler that variable may change asynchronously (prevents caching in registers). |
| `constinit` | Enforces compile-time initialization without making the variable immutable. |
| `register` | Hint to store variable in CPU register (legacy support). |

```ivy
static mutable int32 counter = 0;
static const int32 maxLimit = 100;
static constexpr int32 bufferSize = 1024;
```

---

### Initialization Styles

#### 1. Copy Initialization (Recommended)

```ivy
int32 a = 10;
```

#### 2. Uniform (Braced) Initialization

```ivy
int32 a{10};
```

---

### ⚠️ No Direct Initialization

Ivy **does not support direct initialization** with parentheses (`int32 a(10);`).

**Reason:**  
In C++, direct initialization with parentheses causes parsing ambiguity known as the **"Most Vexing Parse"**, where variable definitions can be mistakenly interpreted as function declarations. To eliminate this ambiguity and source of bugs, Ivy strictly disallows direct initialization.

