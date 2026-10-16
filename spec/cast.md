# C-style Cast trong Ivy

- **Trạng thái**: Draft — cần review
- **Ngày**: 2026-08-17

---

## 1. Tổng quan

Ivy **cấm** C-style cast `(T)expr` trong safe zone (không có `[[ivy::unsafe]]`).
C-style cast chỉ hợp lệ bên trong `[[ivy::unsafe]] { ... }`.

> Lý do: C-style cast có thể thực hiện nhiều thao tác nguy hiểm mà parser/compiler
> không kiểm soát được — đọc địa chỉ tùy ý, bỏ const, ép số học mất precision,
> hoặc reinterpret con trỏ. Trong safe zone, Ivy chỉ cho phép implicit conversion
> (promotion số học, `T* → const T*`).

---

## 2. Các dạng cast

### 2.1. Implicit conversion (safe, không cần `unsafe`)

| Từ | Đến | Mô tả |
|----|-----|-------|
| `int` | `long` | Promotion số học (mở rộng) |
| `int` | `unsigned int` | Promotion số học (mở rộng) |
| `char` | `int` | Promotion `char → int` (trong arithmetic) |
| `short` | `int` | Promotion `short → int` (trong arithmetic) |
| `float` | `double` | Promotion float → double (trong arithmetic) |
| `T*` | `const T*` | Thêm const (an toàn) |
| `T*` | `void*` | Implicit `T* → void*` (an toàn, pointer chuyển đổi) |
| `nullptr` | `T*` | `nullptr` → con trỏ |

### 2.2. Explicit cast (chỉ trong `[[ivy::unsafe]]`)

| Dạng | Mô tả | Ví dụ |
|------|-------|-------|
| `(T)expr` | C-style cast — tất cả đều cho phép | `(int)3.14`, `(char*)ptr`, `(unsigned int)-1` |
| `(T&)expr` | Tham chiếu C-style | `(int&)f` |
| `(T*)expr` | Con trỏ C-style | `(double*)p` |
| reinterpret | Đọc địa chỉ khác | `(char*)&i` |
| const_cast | Bỏ const | `(char*)str` |
| static_cast | Ép kiểu tĩnh (C++-style) | `(int)d` |

### 2.3. Cast giữa pointer và integer

| Từ | Đến | Ví dụ |
|----|-----|-------|
| `void*` | `int` | `(int)ptr` — **cấm trong safe** |
| `int` | `void*` | `(void*)i` — **cấm trong safe** |
| `int*` | `char*` | **cấm trong safe** (alias) |

---

## 3. Quy tắc an toàn

### 3.1. Safe zone (không `unsafe`)

- **Cấm** mọi dạng `(T)expr` — parser báo lỗi "C-style cast requires [[ivy::unsafe]]"
- Chỉ implicit conversion hoạt động:
  - Promotion số học (theo usual arithmetic conversions)
  - `T* → const T*`
  - `T* → void*`
  - `nullptr` → `T*`
- **Không** được:
  - `(int)3.14` (float → int: mất precision)
  - `(char)65` (int → char: mất dữ liệu)
  - `(unsigned int)-1` (signed → unsigned: thay đổi interpretation)
  - `(void*)&i` (con trỏ → integer)
  - `(char*)p` (alias, đọc địa chỉ khác)

### 3.2. Unsafe zone (`[[ivy::unsafe]] { ... }`)

- **Cho phép** mọi C-style cast (trong C++):
  - `(int)3.14` → `3` (truncate)
  - `(unsigned int)-1` → `0xFFFFFFFF`
  - `(char*)&i` → alias (reinterpret)
  - `(void*)i` → integer → con trỏ
- **Không** cảnh báo — unsafe zone bỏ qua tất cả kiểm tra

### 3.3. Unsafe zone + pointer cast

- `(int*)p` (con trỏ → con trỏ khác) → cho phép
- `(char*)&i` — cho phép
- `(double*)&f` — cho phép

---

## 4. Parser

### 4.1. Cấp độ ưu tiên

Trong parser hiện tại (`parser.cpp`), `(T)expr` được phân tích theo thứ tự:

1. Nếu token là `(` và tiếp theo là **type** → đây là C-style cast
2. Nếu token là `(` và tiếp theo là **not type** → đây là expression `(expr)`

**Hiện tại**: parser đã có `Expr::Cast` (chưa dùng) — cần thêm `Stmt::Cast` và xử lý ở `buildStmt`.

### 4.2. Ví dụ phân tích

```cpp
int x = (int)3.14;        // C-style cast → error (safe)
[[ivy::unsafe]] {
    int x = (int)3.14;   // OK
}
```

### 4.3. AST

- `Expr::Cast` — dạng C-style:
  - `type`: kiểu đích
  - `operand`: biểu thức nguồn
  - `loc`: vị trí

---

## 5. HIR

- HIR không thay đổi — `Expr::Cast` được build (như `Unary`/`Binary`)
- Trong safe: `buildCast` gọi `requireUnsafe` với `"C-style cast"`
- Trong unsafe: `buildCast` xác định kiểu đích và cho phép

---

## 6. MIR

- MIR không thay đổi — `Expr::Cast` lưu `type` và `operand`
- Safe: không emit cast (giữ nguyên `operand`)
- Unsafe: `lowerExpr` gặp `Cast` → emit biểu thức bình thường

---

## 7. Codegen

- `lowerExpr` gặp `Cast`:
  - Unsafe: `load + store` vào biến tạm (correct C99 behavior)
  - Safe: error (không thể cast)

---

## 8. Ví dụ

### 8.1. Safe zone (lỗi)

```cpp
int main() {
    double d = 3.14;
    int x = (int)d;      // error: C-style cast requires [[ivy::unsafe]]
    return 0;
}
```

### 8.2. Unsafe zone (OK)

```cpp
int main() {
    [[ivy::unsafe]] {
        double d = 3.14;
        int x = (int)d;  // OK
    }
    return 0;
}
```

### 8.3. Test

```cpp
extern "C" void free(void* p);

int main() {
    [[ivy::unsafe]] {
        int a = 65;
        char c = (char)a;        // OK: unsafe cast
        void* p = (void*)0x1234; // OK
    }
    return 0;
}
```

---

## 9. TODO

- [ ] Thêm `Expr::Cast` vào parser
- [ ] `buildStmt` → `buildCast` kiểm tra safe/unsafe
- [ ] `requireUnsafe` với "C-style cast"
- [ ] Test với `--tokens --hir --mir --llvm`
- [ ] Update README