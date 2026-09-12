# Deprecation Tracking

Tài liệu này ghi chép các tính năng đã có giải pháp thay thế (deprecated alias).
Khi sẵn sàng loại bỏ hoàn toàn, đánh dấu "Removed" và xóa code path tương ứng.

---

## 1. `[[ivy::unsafe]]` attribute → `unsafe { }` keyword block

| | |
|---|---|
| **Task** | A2 |
| **Form mới (preferred)** | `unsafe { ... }` — keyword block |
| **Form cũ (deprecated)** | `[[ivy::unsafe]] { ... }` — attribute form |
| **File ảnh hưởng** | `src/parsing/parser.cpp` — `parseStatement()` xử lý cả hai dạng |
| **Cơ chế** | `unsafe` đã thành keyword. `parseAttributeList()` được sửa để chấp nhận `Keyword` token làm attribute name (chỉ dành cho `unsafe`). |
| **Khi nào loại bỏ** | Sau khi migration toàn bộ codebase sang `unsafe { }`. |
| **Cách loại bỏ** | 1) Xóa branch `[[ivy::unsafe]]` trong `parseStatement()`. 2) Sửa `parseAttributeList()` không còn cần chấp nhận `Keyword` token. 3) Cập nhật error messages. |

---

## 2. Type `_t` suffix → không suffix

| | |
|---|---|
| **Task** | A1 |
| **Form mới (preferred)** | `int32`, `uint64`, `float32`, `bfloat16`, `iptr`, `uptr`, `size`, ... |
| **Form cũ (deprecated)** | `int32_t`, `uint64_t`, `float32_t`, `bfloat16_t`, `intptr_t`, `uintptr_t`, `size_t`, ... |
| **File ảnh hưởng** | `src/parsing/parser.cpp` — `parseType()` normalize new name → canonical `_t` form ngay tại parse time |
| **Cơ chế** | Parser nhận cả hai dạng. New name được normalize về `_t` canonical để downstream (HIR/MIR/codegen) không cần sửa. |
| **Toàn bộ danh sách mapping** | `int8`→`int8_t`, `int16`→`int16_t`, `int32`→`int32_t`, `int64`→`int64_t`, `uint8`→`uint8_t`, `uint16`→`uint16_t`, `uint32`→`uint32_t`, `uint64`→`uint64_t`, `int128`→`int128_t`, `uint128`→`uint128_t`, `float16`→`float16_t`, `float32`→`float32_t`, `float64`→`float64_t`, `float128`→`float128_t`, `bfloat16`→`bfloat16_t`, `iptr`→`intptr_t`, `uptr`→`uintptr_t`, `size`→`size_t` |
| **Khi nào loại bỏ** | Sau khi migration toàn bộ codebase và examples sang form mới. |
| **Cách loại bỏ** | 1) Xóa normalize logic trong `parseType()` — chỉ chấp nhận form mới. 2) Xóa `_t` keywords khỏi lexer (nếu đã là keywords). 3) Đổi canonical name trong HIR/MIR/codegen từ `_t` → form mới. |

---

## 3. `auto` trailing return type (C++ style) → `fn` keyword

| | |
|---|---|
| **Task** | A3 |
| **Form mới (preferred)** | `fn name(params) -> ReturnType { body }` |
| **Form cũ (deprecated)** | `auto name(params) -> ReturnType { body }` — C++ style `auto` trailing return |
| **File ảnh hưởng** | `src/parsing/parser.cpp` — `parseType()` chấp nhận `auto` làm type |
| **Cơ chế** | Theo [Functions.md](file:///d:/project/Ivy/ivyc/docs/Languag_%20Fundamentals/Functions.md), Ivy dùng `fn` keyword thay cho `auto` để tránh ambiguity giữa type deduction và trailing return type. `auto` vẫn được dùng cho variable deduction (`auto x = 42;`), chỉ loại bỏ `auto` làm **function trailing return**. |
| **Khi nào loại bỏ** | Khi muốn ép dùng `fn` cho mọi function có trailing return type. |
| **Cách loại bỏ** | 1) Trong `parseFunctionTrailing()`, nếu return type là `auto` → error, gợi ý dùng `fn` + trailing return tường minh. 2) Giữ `auto` cho variable deduction (`auto x = ...`). |

---

## Ghi chú: Các syntax KHÔNG bị deprecated

Các syntax sau có form mới nhưng **vẫn được giữ lâu dài**, không nằm trong diện loại bỏ:

- **`ReturnType name(params) { body }`** — C-style leading return type. Đây là syntax chính thức của Ivy (xem [Functions.md](file:///d:/project/Ivy/ivyc/docs/Languag_%20Fundamentals/Functions.md)), hoạt động song song với `fn` trailing return type. Không có kế hoạch loại bỏ.
- **`auto x = expr;`** — Variable type deduction. `auto` cho biến vẫn được dùng bình thường, không bị thay thế.

---

## Template để thêm entry mới

```
## N. `Form cũ` → `Form mới`

| | |
|---|---|
| **Task** | Tx |
| **Form mới (preferred)** | ... |
| **Form cũ (deprecated)** | ... |
| **File ảnh hưởng** | ... |
| **Cơ chế** | ... |
| **Khi nào loại bỏ** | ... |
| **Cách loại bỏ** | ... |
```
