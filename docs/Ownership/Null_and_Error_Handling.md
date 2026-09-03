# 🛡️ Safe Error Handling & Null Safety

Ivy replaces error-prone runtime constructs (like null pointer exceptions and unchecked exception unwinding) with static type safety mechanisms in the safe zone.

---

### 1. Null Safety: `ivy::optional<T>`

In the Safe Zone, `nullptr` is strictly disallowed. Optional values must be explicitly encapsulated using `ivy::optional<T>`:

```ivy
// Safe Optional Representation
ivy::optional<int32> findIndex(bool condition) {
    if (condition) {
        return 10;
    }
    return ivy::nullopt; // Explicit empty state
}
```

Raw pointers with `nullptr` are permitted only inside explicit `unsafe` blocks:

```ivy
unsafe {
    int32* rawPtr = nullptr; // Allowed only in unsafe block
}
```

---

### 2. Error Handling: `ivy::expected<T, E>`

Ivy rejects traditional `try/catch/throw` exception handling in the safe zone to avoid hidden control flow branches and allocation overhead. Instead, Ivy provides `ivy::expected<T, E>` for deterministic value-or-error returns:

```ivy
enum class FileError {
    NotFound,
    PermissionDenied
};

ivy::expected<int32, FileError> readFile(string path) {
    if (path.empty()) {
        return FileError::NotFound;
    }
    return 1024; // Return value
}

void process() {
    auto result = readFile("data.txt");
    if (result.hasValue) {
        io::print(result.value);
    } else {
        // Handle result.error
    }
}
```
