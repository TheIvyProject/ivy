# ivy::unique_ptr\<T\>

Con trỏ thông minh sở hữu duy nhất (single-owner smart pointer) — RAII, move-only, an toàn bộ nhớ.

```ivy
import ivy.unique_ptr;
```

---

### 1. Tại sao cần `unique_ptr`?

Khi cần **heap allocation** mà vẫn đảm bảo tự động free:

```ivy
// ❌ Raw pointer — unsafe, dễ leak/double-free
unsafe {
    int32* p = alloc(sizeof(int32));
    *p = 42;
    free(p); // Quên free → memory leak
}

// ✅ unique_ptr — RAII, tự free khi ra scope
ivy::unique_ptr<int32> p = ivy::make_unique<int32>(42);
// p ra scope → tự free, không cần gọi free()
```

---

### 2. Tạo unique_ptr

```ivy
import ivy.unique_ptr;

// make_unique — cách tạo chính (an toàn)
ivy::unique_ptr<int32> a = ivy::make_unique<int32>(42);
ivy::unique_ptr<Point> b = ivy::make_unique<Point>(10, 20);

// Default — null unique_ptr (empty)
ivy::unique_ptr<int32> c;  // Không sở hữu gì
```

> `ivy::make_unique<T>(args...)` allocate trên heap + construct `T` với `args`.

---

### 3. Truy cập giá trị

```ivy
ivy::unique_ptr<int32> p = ivy::make_unique<int32>(42);

// Deref — đọc giá trị bên trong
int32 val = p.get();    // 42 (safe accessor)

// Deref qua operator* (chỉ trong unsafe hoặc khi compiler chứng minh non-null)
unsafe {
    int32 val2 = *p;    // 42
}

// Member access cho struct/class
ivy::unique_ptr<Point> pt = ivy::make_unique<Point>(3, 4);
int32 x = pt.get().x;  // 3
```

---

### 4. Move semantics (không copy)

`unique_ptr` là **move-only** — chỉ có duy nhất một owner:

```ivy
ivy::unique_ptr<int32> a = ivy::make_unique<int32>(42);

// ❌ Copy bị cấm — chỉ có 1 owner
// ivy::unique_ptr<int32> b = a; // Compile error!

// ✅ Move — transfer ownership
ivy::unique_ptr<int32> b = move(a);
// a giờ moved-out — truy cập a → compile error
// b sở hữu giá trị 42
```

---

### 5. Kiểm tra null

```ivy
ivy::unique_ptr<int32> p;  // Empty (null)

if (p.hasValue) {
    io::println(p.get());
} else {
    io::println("null unique_ptr");
}

// Sau khi gán
p = ivy::make_unique<int32>(99);
// p.hasValue == true
```

---

### 6. Methods

| Method | Signature | Mô tả |
|--------|-----------|-------|
| `get()` | `T& get()` | Truy cập giá trị (panic nếu null) |
| `get() const` | `const T& get() const` | Truy cập read-only |
| `hasValue` | `bool hasValue` | `true` nếu sở hữu giá trị |
| `reset()` | `void reset()` | Free giá trị, đặt về null |
| `release()` | `T* release()` | **Unsafe** — trả raw pointer, từ bỏ ownership |

---

### 7. RAII & scope

```ivy
void example() {
    ivy::unique_ptr<FileHandle> f = ivy::make_unique<FileHandle>(openFile("data.txt"));

    if (error) {
        return; // ~unique_ptr() → ~FileHandle() → tự đóng file
    }

    processFile(f.get());
} // ~unique_ptr() → ~FileHandle() — tự cleanup
```

Destructor `~unique_ptr()`:
1. Nếu `hasValue`: gọi `~T()` (destructor của T), rồi free heap memory
2. Nếu `!hasValue`: không làm gì

---

### 8. Truyền vào hàm

```ivy
// Transfer ownership — nhận unique_ptr by value (move)
fn takeOwnership(ivy::unique_ptr<int32> p) {
    io::println(p.get()); // 42
} // p ra scope → free

// Borrow — nhận reference
fn borrowValue(const int32& val) {
    io::println(val);
}

ivy::unique_ptr<int32> p = ivy::make_unique<int32>(42);
borrowValue(p.get());      // ✅ Borrow — p vẫn sống
takeOwnership(move(p));    // ✅ Move — p moved-out
```

---

### 9. So sánh với các ngôn ngữ khác

| Ivy | Rust | C++ | Swift |
|-----|------|-----|-------|
| `ivy::unique_ptr<T>` | `Box<T>` | `std::unique_ptr<T>` | — (ARC thay thế) |
| `ivy::make_unique<T>(args)` | `Box::new(val)` | `std::make_unique<T>(args)` | — |
| Move-only | Move-only | Move-only | — |
| `get()` | `*b` (deref) | `*p` / `p->` | — |
