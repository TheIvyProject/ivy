# ivy::optional\<T\>

Kiểu dữ liệu đại diện cho giá trị **có thể có hoặc không** — thay thế an toàn cho `nullptr` trong safe zone.

```ivy
import ivy.optional;
```

---

### 1. Tại sao cần `optional`?

Trong safe zone, `nullptr` bị **cấm hoàn toàn**. Khi một hàm có thể không trả về giá trị hợp lệ, dùng `ivy::optional<T>` thay vì trả về con trỏ null:

```ivy
// ❌ Safe zone — KHÔNG được phép
int32* findById(int32 id) {
    if (!exists(id)) return nullptr; // Compile error!
    // ...
}

// ✅ Cách đúng — dùng optional
ivy::optional<int32> findById(int32 id) {
    if (!exists(id)) return ivy::nullopt;
    return 42;
}
```

---

### 2. Tạo giá trị

```ivy
import ivy.optional;

// Có giá trị
ivy::optional<int32> a = 42;           // Implicit conversion
ivy::optional<int32> b = {42};         // Aggregate init

// Không có giá trị
ivy::optional<int32> c = ivy::nullopt; // Explicit empty
ivy::optional<int32> d;                // Default = nullopt
```

---

### 3. Kiểm tra & truy cập

```ivy
ivy::optional<int32> result = findIndex(true);

if (result.hasValue) {
    io::println(result.value);   // Truy cập giá trị
} else {
    io::println("No value");
}
```

| Thành viên | Kiểu | Mô tả |
|-----------|------|-------|
| `hasValue` | `bool` | `true` nếu chứa giá trị, `false` nếu empty |
| `value` | `T` | Giá trị bên trong. **Undefined behavior nếu `hasValue == false`** — compiler cảnh báo nếu truy cập mà chưa check |

---

### 4. Methods

```ivy
ivy::optional<int32> opt = 42;

// valueOr — trả về giá trị hoặc giá trị mặc định
int32 x = opt.valueOr(0);  // 42 (vì hasValue == true)

ivy::optional<int32> empty;
int32 y = empty.valueOr(-1); // -1 (vì hasValue == false)

// reset — đặt về trạng thái empty
opt.reset();
// opt.hasValue == false
```

| Method | Signature | Mô tả |
|--------|-----------|-------|
| `valueOr(T defaultVal)` | `T valueOr(T defaultVal) const` | Trả `value` nếu có, `defaultVal` nếu empty |
| `reset()` | `void reset()` | Đặt về `nullopt`, gọi destructor nếu `T` có dtor |

---

### 5. Ownership & Move semantics

`optional<T>` tuân thủ đầy đủ ownership model của Ivy:

```ivy
ivy::optional<ivy::string> name = "Alice";

// Move vào optional khác
ivy::optional<ivy::string> other = move(name);
// name giờ là moved-out — truy cập name → compile error

// Move ra ngoài
ivy::string extracted = move(other.value);
other.reset(); // Nên reset sau khi move value ra
```

- Copy: `optional<T>` copyable nếu `T` copyable
- Move: `optional<T>` movable nếu `T` movable
- RAII: Destructor `~optional()` gọi `~T()` nếu `hasValue == true`

---

### 6. Dùng với template & concept

```ivy
template<typename T>
ivy::optional<T> tryParse(const ivy::string& input) {
    // ... parse logic ...
    if (success) return parsedValue;
    return ivy::nullopt;
}

ivy::optional<int32> num = tryParse<int32>("123"); // optional<int32>{123}
ivy::optional<int32> bad = tryParse<int32>("abc"); // nullopt
```

---

### 7. Layout (nội bộ)

```
┌──────────────────────────────┐
│  optional<T>                 │
│  ┌────────┐  ┌────────────┐  │
│  │hasValue│  │   value     │  │
│  │ (bool) │  │   (T)      │  │
│  └────────┘  └────────────┘  │
└──────────────────────────────┘
Size = sizeof(bool) + padding + sizeof(T)
```

---

### 8. So sánh với các ngôn ngữ khác

| Ivy | Rust | C++ | Swift |
|-----|------|-----|-------|
| `ivy::optional<T>` | `Option<T>` | `std::optional<T>` | `T?` |
| `ivy::nullopt` | `None` | `std::nullopt` | `nil` |
| `hasValue` | `.is_some()` | `.has_value()` | `!= nil` |
| `value` | `.unwrap()` | `.value()` | Force unwrap `!` |
| `valueOr(d)` | `.unwrap_or(d)` | `.value_or(d)` | `?? d` |
