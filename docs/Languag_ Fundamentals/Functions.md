
# ⚡ Functions

Functions in Ivy are self-contained blocks of code that perform specific tasks. Ivy supports standard declarations, definitions, lambda/anonymous functions, and trailing return types.

---

### Function Prototype (Declaration)

```ivy
[return_type] [function_name]([parameter_list]);
```

**Example:**
```ivy
int32 add(int32 a, int32 b);
void greet(string message);
```

---

### Function Definition

```ivy
[return_type] [function_name]([parameter_list]) {
    // Function body
    return [return_value];
}
```

**Example:**
```ivy
import ivy.io

int32 add(int32 a, int32 b) {
    return a + b;
}

void printHello() {
    io::print("Hello!");
}
```

---

### Trailing Return Type (`fn`)

Ivy uses the `fn` keyword for trailing return type declarations instead of C++'s `auto`.

```ivy
fn [function_name]([parameter_list]) -> [return_type] {
    // Function body
}
```

**Example:**
```ivy
fn multiply(int32 x, int32 y) -> int32 {
    return x * y;
}
```

**Why `fn` instead of `auto`:**
- **Clearer Intent:** Explicitly marks the construct as a function declaration rather than variable type deduction.
- **Better Readability & Consistency:** Aligns modern function syntax (`fn name() -> type`) without overloading the semantic meaning of `auto`.
- **Cleaner Parser:** Reduces parsing ambiguity between type-deduced variables and trailing return type functions.


---

### Anonymous Functions (Lambdas)

Ivy supports lambda expressions with capture lists:

```ivy
[captures](parameters) -> return_type {
    // Lambda body
};
```

**Example:**
```ivy
auto square = [](int32 n) -> int32 {
    return n * n;
};

int32 result = square(5);
```

