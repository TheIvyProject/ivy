# Ivy 0.5 Language Specification (Draft)

- **Trạng thái**: Draft — tài liệu chuẩn cho port self-hosting (F1). Port theo
  spec này, không port theo code C++.
- **Ngày**: 2026-10-16
- **Phiên bản ngôn ngữ**: `__ivy__ = 202604L`
- **Tài liệu liên quan**:
  - [types.md](types.md) — kiểu số (đã update 0.5: bỏ `_t`)
  - [ownership.md](ownership.md) — ownership, borrow, lifetime, move
  - [cast.md](cast.md) — cast operators
  - [dropped.md](dropped.md) — cấu trúc bị loại bỏ (rationale log)

---

## 1. Phạm vi và đóng băng

Ivy 0.5 là ngôn ngữ lập trình độc lập (không còn là "safe C++ subset").
Từ thời điểm spec này, **syntax core được đóng băng**: tới 0.5 chỉ được
thêm, không được đổi. Mọi thay đổi breaking phải dời sang sau 0.5.

Quy ước version (0ver — 0.x được phép breaking change):

| Version | Nội dung |
|---------|----------|
| 0.1 | Syntax độc lập + compiler C++ + dùng CRT/libc (hiện tại) |
| 0.5 | Compiler tự host + std/runtime bằng Ivy (backend vẫn LLVM) |
| 1.0 | Chỉ đặt khi cam kết ổn định (semver) — tương lai |

Binary duy nhất là `ivy` (Zig-style). Không còn tên `ivyc`.

---

## 2. Versioning

Phiên bản ngôn ngữ được expose qua predefined macro:

| Macro | Giá trị | Ý nghĩa |
|-------|---------|---------|
| `__ivy__` | `202604L` | Phiên bản chuẩn ngôn ngữ Ivy (dạng `YYYYMML`) |
| `__cplusplus` | `202302L` | Backend C++23 (tương thích toolchain) |

`__ivy__` là object-like macro cố định: luôn defined, không thể `#undef`
hay `#define` lại. Dùng trong `#if` để gate theo version:

```ivy
#if __ivy__ >= 202604L
// code cho Ivy 0.5+
#endif
```

---

## 3. File và translation unit

- CLI chỉ nhận file **`.ivy`**. `.cpp`/`.cc`/`.cxx`/`.c` và extension lạ
  bị từ chối ngay ở CLI (E5).
- File `.ivy` là **module-only**: cấm `#include` (E4). Dùng `import`
  (§9). Preprocessor vẫn xử lý `#define`, conditional, `#pragma`,
  `#error`/`#warning`/`#line`, predefined macros trong mọi file.
- Một translation unit = một file `.ivy` + các module nó `import`.

---

## 4. Lexical

### 4.1. Từ khóa kiểu số (E3: không suffix `_t`)

```
int8  int16  int32  int64  int128
uint8 uint16 uint32 uint64 uint128
float16 float32 float64 float128 bfloat16
iptr uptr size
```

Các biến thể `_t` (`int32_t`, `size_t`, `ptrdiff_t`, ...) **báo lỗi** trong
`.ivy` (gợi ý tên mới). Ngoại lệ: `nullptr_t`, `max_align_t` không có alias
nên giữ nguyên. `cpp::std::int32_t` vẫn hợp lệ khi đi qua `import cpp`.

### 4.2. Từ khóa ngôn ngữ

```
unsafe fn import module export move lifetime
nextcase switch case default break continue return
if else while do for switch struct class enum namespace
template concept constexpr consteval auto extern
new delete sizeof
```

`move` là builtin function (gọi như hàm thường), không phải keyword đặc biệt
ở lexer. `lifetime` mở lifetime declaration; `$identifier` là lifetime
variable token.

### 4.3. Literal

- Integer: thập phân, hex `0x`, octal `0`, binary `0b`; suffix `u/U/l/L`
  (bỏ qua giá trị, lưu `long long`). Ví dụ: `42`, `0xFF`, `202604L`.
- Float, string (`"..."` + raw string), char với escape, `true`/`false`.
- `nullptr`: **chỉ trong `unsafe { }`** (§11). Safe zone dùng
  `ivy::optional<T>` + `ivy::nullopt`.

---

## 5. Grammar (EBNF)

Quy ước: `=` định nghĩa, `|` lựa chọn, `[ ]` optional, `{ }` lặp,
`"..."` terminal. Expression tuân precedence/associativity của C++.

```ebnf
translation_unit = { import_decl | declaration } ;

import_decl  = "import" ( identifier { "." identifier }
                        | string_literal
                        | "cpp" ( "<" header ">" | string_literal )
                        | "c"   ( "<" header ">" | string_literal ) ) ";" ;

declaration = function_def | variable_def | struct_def | enum_def
            | namespace_def | template_def | concept_def | using_decl
            | extern_c_block ;

(* Hai dạng function — leading return (C-style) và trailing return (fn) *)
function_def = leading_func | fn_func ;
leading_func = type identifier "(" [ param { "," param } ] ")"
               [ lifetime_decl ] ( compound_stmt | ";" ) ;
fn_func      = "fn" identifier "(" [ param { "," param } ] ")"
               "->" type [ lifetime_decl ] compound_stmt ;
param        = type [ "$" lifetime_var ] [ identifier ] [ "=" expr ] ;

lifetime_decl = "lifetime" "<" "$" lifetime_var
                { "," "$" lifetime_var } ">" ;

type = [ "const" ] base_type [ "*" ] [ "&" ] ;
base_type = numeric_type | "bool" | "void" | "auto"
          | identifier [ "<" type { "," type } ">" ] ;

compound_stmt = "{" { statement } "}" ;
statement = compound_stmt | decl_stmt | if_stmt | while_stmt | do_stmt
          | for_stmt | range_for_stmt | switch_stmt | return_stmt
          | break_stmt | continue_stmt | nextcase_stmt
          | unsafe_block | expr_stmt ";" | ";" ;

unsafe_block  = "unsafe" compound_stmt ;
nextcase_stmt = "nextcase" [ identifier ] ";" ;
switch_stmt   = "switch" [ identifier ":" ] "(" expr ")" "{"
                { ("case" expr | "default") ":" { statement } } "}" ;
```

Ràng buộc ngữ nghĩa (không thể hiện trong EBNF):

- Mọi nhánh `case` non-empty phải kết thúc bằng `break`, `return`,
  `continue`, hoặc `nextcase` (cấm implicit fallthrough).
- `nextcase LABEL;` nhảy tới case của switch dán nhãn `LABEL:`.
- `unsafe { }` là cách duy nhất vào unsafe zone (attribute
  `[[ivy::unsafe]]` đã xóa — E2).

---

## 6. Type system

### 6.1. Tên bề mặt (sau E3) và ánh xạ LLVM

| Ivy | LLVM | Ghi chú |
|-----|------|---------|
| `int8/16/32/64` | `i8/i16/i32/i64` | signed |
| `uint8/16/32/64` | `i8/i16/i32/i64` | unsigned |
| `int128/uint128` | `i128` | |
| `float16/32/64/128` | `half/float/double/fp128` | IEEE 754 |
| `bfloat16` | `bfloat` | |
| `iptr/uptr/size` | target pointer-width int | `size` = kết quả `sizeof` |
| `bool` | `i1` | |
| `nullptr_t/max_align_t` | `ptr`/— | chỉ dùng trong unsafe |

Nội bộ compiler chuẩn hóa tên mới về dạng canonical `_t` cũ
(`int32` → `int32_t`) để HIR/MIR/codegen giữ nguyên identity. Đây là chi
tiết triển khai, không phải syntax.

### 6.2. C-style types (tương thích)

`int`, `short`, `long`, `unsigned`, `float`, `double`, `char`... chỉ hợp lệ
khi có `#pragma ivy cnumber` trong file. Không có pragma → báo lỗi
(gợi ý tên Ivy). Pragma này được giữ lâu dài cho `extern "C"` và system
interop (không thuộc diện xóa).

### 6.3. Reference và lifetime annotation

- `T&` = mutable borrow, `const T&` = immutable borrow (§10).
- Annotation: `const T& $a x` (param), `-> const T& $a` (return),
  khai báo lifetimes qua `lifetime<$a, $b>` ở function.
- Reference ở codegen là `ptr`.

### 6.4. Promotion

Kiểu cố định: `int32 + int64 → int64`, `float32 + float64 → float64`.
Kiểu C (dưới pragma): theo luật C++.

---

## 7. Functions

- Hai dạng tương đương: `int32 add(int32 a, int32 b) { }` và
  `fn add(int32 a, int32 b) -> int32 { }`.
- `auto` return type cho phép deduction (giữ cho biến: `auto x = 42;`).
- Default arguments được hỗ trợ (`int32 f(int32 x = 0)`).
- `extern "C" { }` cho khai báo hàm C (đi với `import c`).
- Overload, template function, `constexpr`/`consteval` được hỗ trợ.

---

## 8. Entry point

User viết `main`; compiler emit `ivy_main` + wrapper C `main()` gọi vào.
CRT của nền tảng cung cấp `_start` (Ivy không tự viết lại — học theo Rust).

```ivy
int32 main() { return 0; }   // exit code được forward
```

`void main()` → tự return 0.

---

## 9. Modules và interop

- `import math;`, `import ivy.io;`, `import "name";` — nạp `.ivm`
  precompiled interface (struct/enum/alias/function declarations).
- `import cpp <vector>;` / `import cpp "foo.hpp";` — symbols vào namespace
  `cpp::`, ABI tương thích trực tiếp, không cần re-declare, không cần
  `extern "C"`.
- `import c <stdio.h>;` — ABI C, phải re-declare qua `extern "C"`,
  gọi bằng tên trần (không namespace).
- Safety tiers cho API ngoại lai: safe / unsafe / unknown. Mọi raw pointer
  deref/arithmetic/indexing từ API ngoại lai phải nằm trong `unsafe { }`
  (áp dụng đồng đều `import c` và `import cpp`).

---

## 10. Ownership, borrow, move (normative summary)

Chi tiết đầy đủ: [ownership.md](ownership.md). Tóm tắt ràng buộc:

1. **Một owner** tại một thời điểm. `T x = y;` là **copy** (ngữ nghĩa C++).
2. **Move** chỉ qua rvalue hoặc `move(a)` chủ động. Sau `move(a)`, `a` ở
   trạng thái moved-out: đọc → lỗi compile-time; gán lại → re-initialize.
3. **Aliasing XOR mutability**: nhiều `const T&` OK; chỉ một `T&`;
   không trộn `T&` với `const T&` đang sống.
4. **Lifetime**: reference không được outlive data. Return reference thì
   lifetime phải thỏa; trả reference tới local → lỗi (dangling).
5. Trong `unsafe { }`, move-state checking được bypass (năng lực unsafe),
   nhưng borrow/lifetime của macro expansion vẫn phải pass pipeline.

---

## 11. Null và error handling

- Safe zone: **không `nullptr`, không raw pointer**. Nullable → 
...[truncated 3073 chars]