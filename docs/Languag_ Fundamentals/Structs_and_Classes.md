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
