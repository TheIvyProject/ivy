# ivy::expected\<T, E\>

Kiểu dữ liệu đại diện cho kết quả **thành công (T) hoặc lỗi (E)** — thay thế an toàn cho `try/catch/throw` trong safe zone.

```ivy
import ivy.expected;
```

---

### 1. Tại sao cần `expected`?

Ivy **cấm** `try/catch/throw` trong safe zone vì:
- Ẩn control flow (exception có thể bay qua nhiều tầng call stack)
- Chi phí phân bổ bộ nhớ cho exception object
- Khó reason về error path

Thay vào đó, dùng `ivy::expected<T, E>` — error là **giá trị**, không phải **exception**:

```ivy
// ❌ KHÔNG được phép trong safe zone
int32 readFile(string path) {
    throw FileError::NotFound; // Compile error!
}

// ✅ Cách đúng — trả expected
ivy::expected<int32, FileError> readFile(ivy::string path) {
    if (path.empty()) {
        return FileError::NotFound;
    }
    return 1024; // Trả giá trị thành công
}
```

---

### 2. Định nghĩa Error type

Error type `E` thường là `enum class`:

```ivy
enum class FileError {
    NotFound,
    PermissionDenied,
    IoError
};

enum class ParseError {
    InvalidFormat,
    Overflow,
    EmptyInput
};
```

---

### 3. Tạo giá trị

```ivy
import ivy.expected;

// Thành công — trả giá trị T
ivy::expected<int32, FileError> ok = 1024;

// Thất bại — trả lỗi E
ivy::expected<int32, FileError> err = FileError::NotFound;
```

---

### 4. Kiểm tra & truy cập

```ivy
ivy::expected<int32, FileError> result = readFile("data.txt");

if (result.hasValue) {
    io::println(result.value);  // Truy cập giá trị thành công
} else {
    // Xử lý lỗi
    if (result.error == FileError::NotFound) {
        io::println("File not found");
    } else if (result.error == FileError::PermissionDenied) {
        io::println("Permission denied");
    }
}
```

| Thành viên | Kiểu | Mô tả |
|-----------|------|-------|
| `hasValue` | `bool` | `true` nếu chứa giá trị `T`, `false` nếu chứa lỗi `E` |
| `value` | `T` | Giá trị thành công. **Undefined behavior nếu `hasValue == false`** |
| `error` | `E` | Giá trị lỗi. **Undefined behavior nếu `hasValue == true`** |

---

### 5. Methods

```ivy
ivy::expected<int32, FileError> result = readFile("data.txt");

// valueOr — giá trị mặc định khi lỗi
int32 size = result.valueOr(0);

// transform — biến đổi giá trị thành công (giữ error type)
ivy::expected<ivy::string, FileError> msg =
    result.transform(fn(int32 n) -> ivy::string {
        return "Size: " + ivy::toString(n);
    });
// Nếu result có lỗi → msg cũng có cùng lỗi, không gọi lambda
```

| Method | Signature | Mô tả |
|--------|-----------|-------|
| `valueOr(T defaultVal)` | `T valueOr(T defaultVal) const` | Trả `value` nếu thành công, `defaultVal` nếu lỗi |
| `transform(Fn f)` | `expected<U, E> transform(Fn f) const` | Áp dụng `f(value)` nếu thành công, pass-through error nếu lỗi |

---

### 6. Chuỗi nhiều thao tác (chaining)

Pattern phổ biến: chuỗi nhiều hàm trả `expected`, dừng tại lỗi đầu tiên:

```ivy
ivy::expected<ivy::string, ParseError> readConfig(ivy::string path) {
    auto fileResult = readFile(path);
    if (!fileResult.hasValue) {
        return ParseError::InvalidFormat; // Convert error type
    }

    auto parseResult = parseJson(fileResult.value);
    if (!parseResult.hasValue) {
        return parseResult.error;
    }

    return parseResult.value;
}
```

---

### 7. Ownership & Move semantics

`expected<T, E>` tuân thủ ownership model:

```ivy
ivy::expected<ivy::string, FileError> result = readFile("data.txt");

if (result.hasValue) {
    // Move giá trị ra
    ivy::string content = move(result.value);
    // result.value giờ là moved-out
}
```

- Copy: copyable nếu cả `T` và `E` copyable
- Move: movable nếu cả `T` và `E` movable
- RAII: Destructor gọi `~T()` hoặc `~E()` tùy theo `hasValue`

---

### 8. Layout (nội bộ)

```
┌──────────────────────────────────────┐
│  expected<T, E>                      │
│  ┌────────┐  ┌────────────────────┐  │
│  │hasValue│  │  value (T)         │  │
│  │ (bool) │  │    — or —          │  │
│  │        │  │  error (E)         │  │
│  └────────┘  └────────────────────┘  │
└──────────────────────────────────────┘
Size = sizeof(bool) + padding + max(sizeof(T), sizeof(E))
Internally uses tagged union
```

---

### 9. So sánh với các ngôn ngữ khác

| Ivy | Rust | C++ | Go | Swift |
|-----|------|-----|----|-------|
| `ivy::expected<T, E>` | `Result<T, E>` | `std::expected<T, E>` (C++23) | `(T, error)` | `throws` / `Result<T, E>` |
| `result.hasValue` | `.is_ok()` | `.has_value()` | `err != nil` | — |
| `result.value` | `.unwrap()` | `.value()` | `val` | `try` |
| `result.error` | `.unwrap_err()` | `.error()` | `err` | `catch` |
| `return Error` | `Err(e)` | `std::unexpected(e)` | `return err` | `throw` |

---

### 10. Quy tắc thiết kế

1. **Mọi hàm có thể thất bại** nên trả `expected<T, E>` thay vì dùng error code hoặc exception
2. **Error type nên là `enum class`** — phong phú, type-safe, exhaustive
3. **Luôn kiểm tra `hasValue`** trước khi truy cập `value` — compiler cảnh báo nếu không check
4. **Không lồng `expected`** quá sâu — nếu cần chuỗi nhiều bước, tách thành helper functions
