# 📦 Variables

Variables in Ivy store data in named memory locations. Ivy uses clean, familiar syntax for variable declarations and enforces strict immutability checks at compile-time.

---

### Syntax

```ivy
[Storage_Specifier] [const | constexpr] Type Name = Value;
```

Variables are mutable by default without needing a special keyword. Use `const` or `constexpr` to make a variable immutable.

---

### Mutability & Compiler Enforcement

| Keyword | Description |
| :--- | :--- |
| *(none)* | Variable is mutable by default. |
| `const` | Variable is read-only after initialization. |
| `constexpr` | Variable is evaluated as a constant expression at compile-time. |

```ivy
int32 count = 0;           // Mutable variable
count = 1;                 // Valid

const int32 maxLimit = 20; // Immutable variable
constexpr int32 size = 30; // Compile-time constant
```

#### 🛡️ Immutability Rules

If a variable is declared mutable but is **never modified/reassigned** in its scope, Ivy enforces const-correctness:

- **Debug Build / IDE Mode:** Emits a **Compiler Warning** (does not interrupt active prototyping/debugging).
- **Release Build / Strict Mode:** Emits a **Hard Error** (enforces code optimization and const-correctness before production release).

---

### Type Inference (`auto`)

Ivy requires explicit use of the `auto` keyword for type deduction. Leaving the type blank is not permitted:

```ivy
auto a = 10;                     // Inferred as mutable int32
const auto b = "Hello Ivy";       // Inferred as const string
static auto counter = 0;         // Inferred as static mutable int32
static const auto maxLimit = 100;// Inferred as static const int32
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
static int32 counter = 0;
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
