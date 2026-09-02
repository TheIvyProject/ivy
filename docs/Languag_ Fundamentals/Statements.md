# 🔄 Statements

Statements in Ivy control the flow of execution, create bindings, and manage scopes. Ivy supports selection statements, iteration loops, jump statements, structured bindings, and safety blocks.

---

### 1. Selection Statements

#### `if` / `else if` / `else`
Executes code blocks based on boolean conditions.

```ivy
if (condition) {
    // Statement
} else if (anotherCondition) {
    // Statement
} else {
    // Statement
}
```

#### `if constexpr`
Evaluates condition at compile-time and discards unselected branches.

```ivy
if constexpr (sizeof(int32) == 4) {
    // Evaluated and compiled only if true
}
```

#### `switch` / `case` / `default`
Branches execution based on integral/enum constant values.

> **Strict Fallthrough Rule:**  
> Ivy **disallows implicit fallthrough**. The compiler emits an error if a non-empty `case` branch does not explicitly terminate with `break`, `return`, `continue`, or `nextcase`.

##### Standard `switch` Example:

```ivy
switch (value) {
    case 1: {
        io::print("One");
        break;
    }
    case 2: {
        io::print("Two");
        break;
    }
    default: {
        io::print("Other");
        break;
    }
}
```

##### Explicit Fallthrough with `nextcase`

Ivy supports `nextcase` for explicit, safe case jumping:

1. **Unlabeled `nextcase;`**: Jumps directly to the next case branch.
2. **Labeled `nextcase <label>;`**: Jumps to a specific target case branch.

```ivy
// Unlabeled nextcase: jumps to the next case
switch (i) {
    case 1:
        doSomething();
        nextcase; // Jumps to case 2
    case 2:
        doSomethingElse();
        break;
    default:
        break;
}

// Labeled nextcase: jumps directly to the specified case
switch MAIN: {
    case FOO: {
        nextcase BAR; // Jumps to case BAR
    }
    case BAR: {
        io::print("BAR");
        break;
    }
    default:
        break;
}
```

*(Special thanks to [C3lang](https://c3-lang.org/language-fundamentals/statements/#nextcase-and-labelled-nextcase) for providing the inspiration for `nextcase` and labeled `nextcase`.)*

##### Interaction with Ownership, Borrowing & Lifetimes

To guarantee memory safety and zero resource leaks, `nextcase` adheres to strict borrow-checking and RAII rules:

1. **Scope Cleanup (RAII Unwind):**  
   Executing `nextcase` acts as an exit point for the current `case` block scope. All owned variables initialized inside that `case` are automatically destroyed (dropped/destructed) before execution jumps to the target case.
2. **Borrow Lifetimes:**  
   References borrowing local variables of the current `case` cannot outlive the jump. Any borrow into case-local data must end before `nextcase`. Only borrows referencing variables in scopes outer to the `switch` statement remain valid.
3. **Isolated Case Scopes:**  
   `nextcase` always jumps to the **entry point** of the target case block. It cannot jump past declarations or skip initializations in the target case, preventing uninitialized variable states.



---

### 2. Iteration Statements (Loops)

#### `for` Loop
Standard 3-clause loop for counted iteration.

```ivy
for (mutable int32 i = 0; i < 10; ++i) {
    io::print(i);
}
```

#### Range-based `for` Loop
Iterates over arrays or iterable collections.

```ivy
int32 numbers[5] = {1, 2, 3, 4, 5};

for (int32 x : numbers) {
    io::print(x);
}

// Iterate by reference to modify elements
for (int32& x : numbers) {
    x = x * 2;
}
```

#### `while` Loop
Repeats as long as condition evaluates to `true`.

```ivy
mutable int32 count = 5;
while (count > 0) {
    --count;
}
```

#### `do-while` Loop
Executes body once before testing condition.

```ivy
mutable int32 count = 0;
do {
    ++count;
} while (count < 5);
```

---

### 3. Jump Statements

| Statement | Description |
| :--- | :--- |
| `break;` | Exits the innermost loop or switch block immediately. |
| `continue;` | Skips to the next iteration of the loop. |
| `return [expr];` | Returns control (and optional value) from the function. |
| `nextcase;` / `nextcase <target>;` | Jumps to the next case branch or labeled case in a `switch` statement. |


```ivy
for (mutable int32 i = 0; i < 10; ++i) {
    if (i == 3) continue;
    if (i == 8) break;
    if (i == 7) return 0;
}
```

---

### 4. Structured Bindings

Deconstructs tuples, arrays, or structs into individual local variables.

```ivy
auto [x, y] = getPoint();
```

---

### 5. Declaration & Expression Statements

- **Declaration Statement:** Declares local variables or constants (`mutable int32 x = 5;`).
- **Expression Statement:** Evaluates expressions for side effects (`callFunction();`, `x = y + 1;`).
- **Compound Statement (Block):** Group of statements enclosed in `{ ... }` creating a lexical scope.
