# ⚙️ Operator Overloading

Ivy supports operator overloading to allow user-defined types (such as structs and classes) to interact naturally with standard operators. The declaration syntax is directly inherited from C++, with explicit safety restrictions on dangerous operators.

---

### 1. Allowed Operators

Ivy permits overloading for the following operators:

#### Arithmetic & Modulo
- `+`, `-`, `*`, `/`, `%`
- `++` (Prefix/Postfix increment), `--` (Prefix/Postfix decrement)

#### Comparison
- `==`, `!=`, `<`, `<=`, `>`, `>=`

#### Bitwise Operators
- `&` (Bitwise AND), `|` (Bitwise OR), `^` (Bitwise XOR), `~` (Bitwise NOT)
- `<<` (Left shift), `>>` (Right shift)

#### Assignment & Compound Assignment
- `=` (Assignment)
- `+=`, `-=`, `*=`, `/=`, `%=`
- `<<=`, `>>=`, `&=`, `^=`, `|=`

---

### 2. 🚫 Forbidden and Unsupported Operators

To preserve short-circuit evaluation semantics, memory safety, and predictable code flow, Ivy **strictly restricts operator overloading exclusively to the whitelist above**. Any operator not explicitly listed in the allowed section is either completely forbidden or unsupported for overloading:

| Operator | Status / Reason |
| :--- | :--- |
| `,` (Comma) | **Forbidden:** Obscures evaluation order and introduces confusing side-effects. |
| `&&` (Logical AND) | **Forbidden:** Overloading breaks built-in boolean short-circuit evaluation guarantees. |
| `||` (Logical OR) | **Forbidden:** Overloading breaks built-in boolean short-circuit evaluation guarantees. |
| `&` (Unary Address-of) | **Forbidden:** Overloading breaks the compiler's ability to take real memory addresses safely. |
| `::`, `.`, `.*`, `?:`, `sizeof` | **Unsupported:** Core language operators that cannot be customized by design. |
| *Any other operator* | **Unsupported:** Ivy adheres strictly to a whitelist policy. If an operator is not in Section 1, it cannot be overloaded. |


---

### 3. Syntax and Declaration

Operator overloading syntax is directly inherited from C++:

#### Member Function Overload
```ivy
struct Vector2 {
    float32 x;
    float32 y;

    // Binary arithmetic operator
    Vector2 operator+(const Vector2& other) const {
        return Vector2{x + other.x, y + other.y};
    }

    // Compound assignment operator
    Vector2& operator+=(const Vector2& other) {
        x += other.x;
        y += other.y;
        return *this;
    }

    // Comparison operator
    bool operator==(const Vector2& other) const {
        return x == other.x && y == other.y;
    }
};
```

#### Non-Member (Free) Function Overload
```ivy
struct Point {
    int32 x;
    int32 y;
};

// Free function operator
bool operator==(const Point& lhs, const Point& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
}
```

---

### 4. Ownership & Lifetime Rules for Operators

- **Return by Reference:** Operators returning `T&` (such as `operator=`, `operator+=`) must return a valid reference to an existing object (typically `*this`).
- **No Dangling References:** Operators returning `const T&` must respect lifetime boundaries and cannot return references to temporary stack objects created inside the operator function.
