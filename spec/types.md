# Kiểu số trong Ivy

- **Trạng thái**: 0.5 draft — đã update sau E3 (bỏ `_t` suffix)
- **Ngày**: 2026-08-17
- **Cập nhật**: 2026-10-16 — E3: tên không suffix là chuẩn duy nhất

---

## 1. Mục tiêu

Ivy không dùng `int`, `float`, `double` làm kiểu mặc định (C++-style).
Thay vào đó, Ivy cung cấp **builtin type** có kích thước cố định
(E3: tên **không** suffix `_t`; biến thể `_t` báo lỗi trong `.ivy`):

- `int8`, `int16`, `int32`, `int64` — signed, bit chính xác
- `uint8`, `uint16`, `uint32`, `uint64` — unsigned
- `float16`, `float32`, `float64`, `float128` — IEEE 754
- `bfloat16` — brain float (LLVM `@llvm.bfloat`)
- `int128`, `uint128`, `iptr`, `uptr`, `size`
- `nullptr_t`, `max_align_t` (không có alias — giữ nguyên, chỉ dùng trong unsafe)
- `nullptr` (constant — chỉ trong unsafe), `offsetof` (macro — chỉ trong unsafe)

C++ compatibility type (`int`, `short`, `float`, `double`, `bool`, `char`...)
chỉ được dùng khi bật `#pragma ivy cnumber`.

---

## 2. Danh sách builtin

### 2.1. Ivy builtins (luôn có, không cần `#include`)

| Tên | Kích thước | LLVM type | Ghi chú |
|-----|-----------|-----------|---------|
| `int8` | 1 byte | `i8` | signed 8-bit |
| `int16` | 2 byte | `i16` | signed 16-bit |
| `int32` | 4 byte | `i32` | signed 32-bit |
| `int64` | 8 byte | `i64` | signed 64-bit |
| `uint8` | 1 byte | `i8` | unsigned |
| `uint16` | 2 byte | `i16` | |
| `uint32` | 4 byte | `i32` | |
| `uint64` | 8 byte | `i64` | |
| `int128` / `uint128` | 16 byte | `i128` | |
| `float16` | 2 byte | `half` | IEEE 754 half-precision |
| `float32` | 4 byte | `float` | IEEE 754 single |
| `float64` | 8 byte | `double` | IEEE 754 double |
| `float128` | 16 byte | `fp128` | IEEE 754 quad |
| `bfloat16` | 2 byte | `bfloat` | Brain float (Google) |
| `iptr` / `uptr` | platform | pointer-width int | thay `intptr_t` / `uintptr_t` |
| `size` | platform | `i64` (x64) / `i32` (x86) | `sizeof` result, thay `size_t` |
| `nullptr_t` | platform | `ptr` | `nullptr` type (không alias, unsafe only) |
| `max_align_t` | — | — | largest alignment (không alias, trong unsafe) |

Nội bộ compiler chuẩn hóa tên mới về dạng canonical `_t` cũ
(`int32` → `int32_t`) để HIR/MIR/codegen giữ nguyên identity.
Đây là chi tiết triển khai, không phải syntax (xem [ivy05.md](ivy05.md) §6.1).

### 2.2. C++ compatibility types (phải bật)

| Tên | Kích thước | Ánh xạ tới | Ghi chú |
|-----|-----------|-----------|---------|
| `int` | 4 byte | `int32` | default signed |
| `short` | 2 byte | `int16` | |
| `long` | 8 byte (x64) | `int64` | 4 byte (x86) |
| `long long` | 8 byte | `int64` | |
| `unsigned` | — | `uint32` | khi không có base |
| `float` | 4 byte | `float32` | |
| `double` | 8 byte | `float64` | |
| `bool` | 1 byte | `i1` | |
| `char` | 1 byte | `i8` | |

### 2.3. fast/exact types (từ `#include <cstdint>`)

| Tên | Kích thước | Ghi chú |
|-----|-----------|---------|
| `int_fast8_t` | ≥ 1 byte | |
| `int_fast16_t` | ≥ 2 byte | |
| `int_fast32_t` | ≥ 4 byte | |
| `int_fast64_t` | ≥ 8 byte | |
| `uint_fast8_t` | — | unsigned |
| `int_least8_t` | — | |
| ... | — | |

Các type này **không phải builtin** — Ivy chỉ nhận diện qua `#include <cstdint>` và
bỏ qua (giữ nguyên định nghĩa của thư viện C++).

---

## 3. Cách dùng

### 3.1. Ivy style (luôn dùng builtin)

```ivy
int32 x = 42;             // OK — kiểu Ivy
float64 d = 3.14;         // OK
```

### 3.2. C++ style (phải bật `#pragma ivy cnumber`)

```cpp
#pragma ivy cnumber
int main() {
    int a = 5;             // OK — int là int32
    float b = 3.0f;        // OK
    return 0;
}
```

Nếu không có `#pragma ivy cnumber`, `int` và `float` **báo lỗi**:
"C-style type 'int' requires #pragma ivy cnumber; use int32" (tương tự).

### 3.3. Trộn (Ivy style + C++ style)

```ivy
int32 x = 42;
#pragma ivy cnumber
int y = x + 5;             // int → int32
```

---

## 4. Parser

### 4.1. Danh sách type builtin

Parser nhận diện các từ khóa sau làm type:

```
int8      int16      int32      int64      int128
uint8     uint16     uint32     uint64     uint128
float16   float32    float64    float128
bfloat16
iptr      uptr       size
nullptr_t max_align_t   (không alias — giữ nguyên)
```

Biến thể `_t` (`int32_t`, `size_t`, `ptrdiff_t`, ...) báo lỗi trong `.ivy`
(E3) — gợi ý tên mới. `cpp::std::int32_t` vẫn hợp lệ qua `import cpp`.

C++ compatibility:

```
int   short   long   long long   unsigned   signed
float double   bool   char        wchar_t
```

### 4.2. Cấp bậc ưu tiên

1. Nếu token là `int32`... → type (Ivy builtin)
2. Nếu token là `int` / `short` / `long`... → type (C++ compat, phải có `#pragma ivy cnumber`)
3. Nếu token là `unsigned` / `signed`... → modifier trên type

---

## 5. Type promotion

- **Ivy type** (kích thước cố định): `int32` + `int64` → `int64`
- **C++ type** (khi có `cnumber`): `int` + `long` → `long`

Quy tắc:

| Ivy type | C++ type | Kết quả |
|---------|---------|---------|
| `int32` + `int64` | — | `int64` |
| `int32` + `int` | `int` | `int32` |
| `float32` + `float64` | — | `float64` |

---

## 6. Tương thích với kiểu C++ `_t` (E3/E4)

`#include` bị cấm trong `.ivy` (E4) nên xung đột `#include <cstdint>`
không còn tồn tại. Kiểu `_t` chỉ xuất hiện qua `import cpp`:

```ivy
import cpp <cstdint>;
// cpp::std::int32_t — kiểu C++, tương thích ABI với int32
// (cùng kích thước, cùng biểu diễn — không xung đột)
```

Trong `.ivy`, viết `int32_t` trực tiếp → lỗi, gợi ý dùng `int32`.

---

## 7. MIR

- `int32` (surface) → canonical `int32_t` nội bộ (giữ nguyên identity)
- MIR có `Type` gốc (không phân giải) → cho codegen biết

---

## 8. Codegen

- `int32` → `i32`
- `float16` → `half` (LLVM intrinsic: `@llvm.fpext`, `@llvm.fptrunc`)
- `bfloat16` → `bfloat` (LLVM: `@llvm.fpext` với bfloat)
- `float128` → `fp128` (LLVM: `@llvm.fpext`)

---

## 9. TODO

- [x] Thêm builtin types vào parser (A1)
- [x] `#pragma ivy cnumber` gate cho C-style types
- [x] Codegen map `float16` → `half`, `bfloat16` → `bfloat`
- [x] E3: `_t` variant báo lỗi trong `.ivy` (giữ `nullptr_t`/`max_align_t`)
- [x] E4: `#include` cấm trong `.ivy` — xung đột `<cstdint>` không còn tồn tại