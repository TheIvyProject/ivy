# 📦 Structs & Classes

Structs and classes in Ivy represent aggregate data structures with encapsulation, member functions, constructors, destructors (RAII), and memory layout guarantees conforming to standard C ABI.

---

### 1. Defining Structs and Classes

In Ivy, `struct` and `class` define compound types. All members are public by default in safe Ivy code.

```ivy
struct Point {
    int32 x;
    int32 y = 0; // Default member initializer
};

class Rectangle {
    int32 width;
    int32 height;
};
```

---

### 2. Initialization Styles

Ivy supports **Positional Aggregate Initialization** and **Designated Initializers**:

```ivy
// Positional Aggregate Initialization
Point p1 = {10, 20};

// Default Initialization (fields without values take defaults or zero)
Point p2 = {5}; // x = 5, y = 0

// Designated Initializers (C++20 style)
Point p3 = {.x = 100, .y = 200};
```

---

### 3. Member Functions & `this`

Structs can contain member functions. The compiler automatically passes an implicit `this` reference to member methods.

```ivy
struct Vector2D {
    int32 x;
    int32 y;

    int32 lengthSquared() const {
        return this.x * this.x + this.y * this.y;
    }

    void scale(int32 factor) {
        this.x *= factor;
        this.y *= factor;
    }
};

Vector2D v = {3, 4};
int32 sq = v.lengthSquared(); // 25
v.scale(2); // v.x = 6, v.y = 8
```

---

### 4. Constructors & Member Initializer Lists

Constructors initialize objects when instantiated. Member initializer lists directly construct fields before the constructor body runs.

```ivy
struct Logger {
    int32 id;
    bool active;

    // Default constructor
    Logger() : id(0), active(true) {}

    // Parameterized constructor
    Logger(int32 initialId) : id(initialId), active(true) {}
};

Logger log1;          // Calls default ctor
Logger log2 = {42};   // Direct ctor initialization
```

---

### 5. Destructors & Scope-Based RAII

Destructors (`~ClassName()`) execute deterministically in reverse order of declaration when variables exit lexical scope, break/continue loops, or early return.

```ivy
struct FileHandle {
    int32 descriptor;

    FileHandle(int32 fd) : descriptor(fd) {}

    ~FileHandle() {
        if (this.descriptor >= 0) {
            unsafe {
                // Free OS handle
            }
        }
    }
};

void process() {
    FileHandle f = {10}; // ctor called
    if (condition) {
        return; // ~FileHandle() automatically called here
    }
} // ~FileHandle() automatically called at scope exit
```

---

### 6. Inheritance & Virtual Dispatch

Ivy supports single/multiple inheritance and virtual methods:

```ivy
struct Shape {
    virtual int32 area() const {
        return 0;
    }
    virtual ~Shape() {}
};

struct Square : public Shape {
    int32 side;

    Square(int32 s) : side(s) {}

    int32 area() const override {
        return this.side * this.side;
    }
};
```

---

### 7. OOP Direction: No `as` / `is` Keywords

Ivy keeps object-oriented syntax aligned with C++ and **does not introduce `as` / `is` keywords** for polymorphism or casting. Those keywords are intentionally rejected because they push Ivy toward the style of other languages instead of preserving its C++-shaped mental model.

#### Design Rules

1. **Upcasting stays implicit**, just like C++:

```ivy
Shape& shape = square;
```

2. **Compile-time checked conversions use C++-style casts**:

```ivy
float32 x = static_cast<float32>(value);
```

3. **Low-level reinterpretation remains explicit and unsafe-only**:

```ivy
unsafe {
    Foo* ptr = reinterpret_cast<Foo*>(raw);
}
```

4. **`dynamic_cast` remains banned** to avoid RTTI overhead and hidden runtime cost.

#### Safe Polymorphism Without New Keywords

For object-oriented code, Ivy prefers:

- implicit upcast to base references/pointers,
- `virtual` dispatch,
- `override` for derived methods,
- explicit helper APIs for checked downcasts if Ivy later adds them, using **C++-style library forms** rather than new language keywords.

Preferred direction:

```ivy
Shape& base = square;

// Possible future library design, not new syntax
auto derived = ivy::dyn_cast<Square>(base);
```

This keeps the language surface small, preserves C++ familiarity, and avoids turning Ivy into a hybrid syntax that imitates unrelated languages.
