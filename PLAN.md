# Kế hoạch phát triển Ivy

- **Trạng thái**: Active — triển khai
- **Ngày**: 2026-08-17
- **Người tạo**: User + AI

---

## Mục tiêu

Xây dựng trình biên dịch `ivyc` — C++ subset an toàn (Ivy), pipeline:
```
Lexer → Parser → HIR → MIR → LLVM IR
```

---

## Kế hoạch

### Giai đoạn 1 — Hoàn thành (codegen + test)

| # | Task | Trạng thái | Ghi chú |
|---|------|-----------|---------|
| 1 | `lexer.h/cpp` | ✅ | Token stream |
| 2 | `parser.h/cpp` | ✅ | AST |
| 3 | `hir.h/cpp` | ✅ | Type-checked IR |
| 4 | `hir_builder.h/cpp` | ✅ | Build HIR |
| 5 | `mir.h/cpp` | ✅ | MIR (CFG) |
| 6 | `mir_builder.h/cpp` | ✅ | Build MIR |
| 7 | `codegen.h/cpp` | ✅ | LLVM IR emitter |
| 8 | `main.cpp` | ✅ | CLI (--llvm -o) |
| 9 | `CMakeLists.txt` | ✅ | C++23 |
| 10 | `examples/demo.cpp` | ✅ | Test pass |

### Giai đoạn 2 — Spec viết xong

| # | Task | Trạng thái | Ghi chú |
|---|------|-----------|---------|
| 1 | `spec/cast.md` | ✅ | C-style cast trong/ngoài unsafe |
| 2 | `spec/types.md` | ✅ | Ivy builtin types |
| 3 | `spec/ownership.md` | ✅ | Ownership + Borrow + Move |
| 4 | `README.md` | ✅ | Pipeline + subset |

### Giai đoạn 3 — Triển khai ngay

| # | Task | Mức ưu tiên | Mô tả | File |
|---|------|-----------|-------|------|
| 1 | Thêm `int8_t..int64_t` vào parser | **✅ P0** | `int8_t`, `int16_t`, `int32_t`, `int64_t` | `src/parser.cpp` |
| 2 | Thêm `uint8_t..uint64_t` vào parser | **✅ P0** | `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` | `src/parser.cpp` |
| 3 | Thêm `float16_t..float128_t`, `bfloat16_t` | **✅ P0** | `float16_t`, `float32_t`, `float64_t`, `float128_t`, `bfloat16_t` | `src/parser.cpp` |
| 4 | Thêm `size_t`, `ptrdiff_t`, `nullptr_t`, `max_align_t` | **✅ P0** | Builtin | `src/parser.cpp` |
| 5 | Thêm `const T&` / `T&` vào type | **✅ P0** | Type modifier (reference) | `src/ast.h` |
| 6 | **Preprocessor: `#include`** | **✅ P1** | Include directive + `-o .i/.ii` + `-I` | `src/preprocessor.cpp` |
| 7 | **Preprocessor: Object-like macro** | **✅ P1** | `#define PI 3.14` | `src/preprocessor.cpp` |
| 8 | **Preprocessor: Function-like macro** | **✅ P1** | `#define SQUARE(x) ((x)*(x))` | `src/preprocessor.cpp` |
| 9 | **Preprocessor: `#ifdef` / `#ifndef` / `#else` / `#endif` / `#undef`** | **✅ P1** | Conditional compilation | `src/preprocessor.cpp` |
| 10 | **Preprocessor: `#if` / `#elif` constant expression** | **✅ P1** | `#if VERSION == 3`, `defined()`, arithmetic/bitwise/logical/ternary | `src/preprocessor.cpp` |
| 11 | **Preprocessor: Variadic macro** | **✅ P1** | `#define PRINT(fmt, ...)` → `__VA_ARGS__` | `src/preprocessor.cpp` |
| 12 | **Preprocessor: Predefined macros** | **✅ P1** | `__LINE__`, `__FILE__`, `__DATE__`, `__TIME__`, `__cplusplus` | `src/preprocessor.cpp` |

### Giai đoạn 4 — Test + Regression

| # | Task | Mức ưu tiên | Mô tả |
|---|------|-----------|-------|
| 1 | `test_promotion.cpp` | **✅** | Promotion + types mới |
| 2 | `test_ref.cpp` | **✅** | `const T&` / `T&` param + local |
| 3 | `test_mut_ref.cpp` | **✅** | Mutable reference (incr) |
i| 4 | `test_cast.cpp` | **Pending P0** | C-style cast trong/ngoài unsafe |
| 5 | `test_ownership.cpp` | **Tương lai** | Move + borrow |
| 6 | `test_llvm.cpp` | **Tương lai** | LLVM IR + clang |
| 7 | `test_errors.cpp` | **Pending P3** | Error messages |

### Giai đoạn 5 — Mở rộng

| # | Task | Mức ưu tiên | Mô tả |
|---|------|-----------|-------|
| 1 | `IvyInterpret` | **P5** | Bộ thông dịch MIR cho REPL (`ivyc --repl`) & Fast Test (`ivyc --run`) |
| 2 | `constexpr` & `consteval` | **P5** | Thực thi compile-time evaluation dùng `IvyInterpret` trên tầng MIR. **Hoàn thành (P4.7)**: HIR-based tree-walking evaluator (v0.1, sẽ nâng cấp MIR-based sau) |
| 3 | `IvyMake` | **P5** | Hệ thống build cấu hình bằng C++/Ivy (`build.ivy`), thực thi qua `IvyInterpret` |
| 4 | `ivy::expected<T, E>` | **P5** | Template (sau) |
| 5 | `std::optional` | **P5** | |
| 6 | `T&&` (rvalue reference) | **P5** | |
| 7 | `float16_t` → `half` LLVM | **P5** | `@llvm.fpext` |
| 8 | `bfloat16_t` → LLVM `bfloat` | **P5** | |
| 9 | `int8_t` + `float16_t` — arithmetic | **P5** | |
| 10 | `unsigned int` — promotion | **P5** | |

---

## Chi tiết

### ✅ P0 — Đã hoàn thành

1. **Parser** (`src/parser.cpp`):
   - Thêm `int8_t`, `int16_t`, `int32_t`, `int64_t` → `type`
   - Thêm `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` → `type`
   - Thêm `float16_t`, `float32_t`, `float64_t`, `float128_t`, `bfloat16_t` → `type`
   - Thêm `size_t`, `ptrdiff_t`, `nullptr_t`, `max_align_t` → `type`
   - Thêm `const T&` / `T&` → `type` (reference) — `&` sau type trong `parseType`
   - Thêm Ivy types vào keyword set (lexer) + `isTypeStart()` (parser)
2. **HIR** (`src/hir_builder.cpp`):
   - `promoteTypes`: strip reference cho arithmetic
   - `isAssignable`: viết lại — hỗ trợ reference binding (`T&` bind lvalue, `const T&` bind rvalue)
3. **MIR** (`src/mir_builder.cpp`):
   - `lowerType`: `const T&` → `[lt: a]` (tương tự C++) — **mặc kệ**, MIR chỉ là HIR copy
   - `T&` → `[lt: a]` (mutable) — **không cần thay đổi**, MIR không phân biệt
4. **Codegen** (`src/codegen.cpp`):
   - `llvmType`: reference → `ptr`
   - `valueType()` / `valueLlvmType()`: strip reference cho value semantics
   - `IdentRef` reference: load addr từ slot, load value qua addr
   - `lowerLValue` reference: load ptr từ slot
   - **Alloca** reference: `alloca ptr`, store `lowerLValue(init)` vào slot
   - **Call**: tra cứu callee param → nếu reference, truyền địa chỉ (`lowerLValue`)
   - **Binary / Store / Ret / Alloca-init**: dùng `valueLlvmType` cho operand type
5. **Test**:
   - `test_types.cpp` — `int8_t x = 42;` → OK
   - `test_ref.cpp` — `const T&` + `T&` → OK
   - `test_mut_ref.cpp` — `T&` mutable → OK
   - `demo.cpp` — regression → OK

### P1 — Preprocessor Macro

1. **`#include`** ✅:
   - File: `src/preprocessor.h/cpp`
   - Pipeline: `Lexer → Preprocessor → Parser → ...` (chạy trước parser)
   - Xử lý `#include "file"` (relative đến file hiện tại, rồi `-I`) và `#include <file>` (chỉ `-I`)
   - Đệ quy với cycle guard (canonical path) + giới hạn depth 200
   - Buffer của file include được lưu trong `Preprocessor` để `Token::lexeme` (string_view) còn hợp lệ
   - CLI: `-I <dir>` (lặp lại được, cả dạng `-Idir`), `-o file.i` / `file.ii` → emit C++ đã tiền xử lý (như `g++ -E`)
   - Các directive chưa hỗ trợ (`#define`, `#ifdef`, `#endif`, ...) được giữ lại dạng token thô để parser báo lỗi rõ ràng
   - Test: `examples/test_include_simple.cpp` + `examples/ivy_simple.h` → sinh LLVM IR đúng; `examples/test_include.cpp` + `lib/ivy.h` (có guard) chỉ dùng được ở chế độ `-o .i` vì `#ifndef`/`#endif` chưa hỗ trợ
2. **Object-like macro** (`#define NAME value`) ✅:
   - Token: `TokenKind::Identifier` tra cứu trong bảng `macros_` → thay bằng body
   - Lưu `{name, token_seq}` vào `std::unordered_map<std::string, Macro> macros_`
   - Thay thế token stream trước parser (trong `emitToken`, đệ quy với cycle guard)
   - Tự tham chiếu (`#define SELF SELF + 1`) → "painted blue" (không loop, để lại literal theo chuẩn C++)
   - Chained expansion (`#define DOUBLE ANSWER + ANSWER` với `#define ANSWER 42` → `42 + 42`)
   - Empty body (`#define EMPTY`) → không emit token gì
   - Function-like macro (`#define F(x) ...`) → phát hiện (check `(` ngay sau NAME không khoảng trắng) + báo "not supported yet"
   - Macro trong file include → available trong file chính (scope toàn TU)
   - Test: `examples/test_define.cpp` → sinh LLVM IR đúng
3. **Function-like macro** (`#define NAME(args) body`) ✅:
   - `Macro` struct: thêm `isFunctionLike` + `params`
   - `parseDefine`: phát hiện function-like (check `(` ngay sau NAME không khoảng trắng), parse param list, thu body
   - `expandFunctionLike`: parse args (balanced parens, comma-separated), arity check, substitute params vào body
   - `expandTokenVector`: chạy expansion trên một token vector (body đã substitute) — cho phép function-like macro trong body expand được (chained: `DBL_SQUARE(7)` → `SQUARE(7)` → `((7)*(7))`)
   - Argument prescan: argument chứa macro → expand trước khi substitute (`SQUARE(TWO)` → `SQUARE(2)` → `((2)*(2))`)
   - Bare function-like name (không có `(` sau) → emit verbatim (theo C++)
   - Arity mismatch → diagnostic rõ ràng
   - Cycle guard + depth guard như object-like
   - Test: `examples/test_funclike.cpp` → sinh LLVM IR đúng
4. **`#ifdef` / `#ifndef` / `#else` / `#endif` / `#undef`** ✅:
   - `CondFrame {taken, seenElse, parentActive}` stack theo dõi trạng thái conditional
   - `active()` kiểm tra toàn bộ frame `taken` — chỉ emit token khi active
   - `parseConditional` xử lý `#ifdef`/`#ifndef` (check `isDefined`), `#else` (flip `taken` nếu parent active), `#endif` (pop frame), `#undef` (xóa macro khi active)
   - Directive names: chấp nhận cả `TokenKind::Identifier` (`ifdef`/`ifndef`/`undef`) và `TokenKind::Keyword` (`if`/`else`/`elif`) vì lexer tag khác nhau
   - Nested `#ifdef` inside `#ifdef` — `parentActive` propagated, inactive parent → frame luôn inactive
   - Include-guard pattern (`#ifndef X / #define X / ... / #endif`) hoạt động đúng
   - `#if` / `#elif` nhận diện nhưng báo "not supported yet" (P1.5) — vẫn push/skip frame để nesting không leak
   - `#undef` trong block inactive bị bỏ qua (chỉ xóa macro khi active)
   - Unterminated block → cảnh báo ở cuối `run()` + khi include không cân bằng
   - Test: `examples/test_ifdef.cpp` → sinh `.i` đúng (conditional skip đúng nhánh) + `.ll` đúng (6 function + main)
5. **`#if EXPR` / `#elif EXPR`** (constant expression) ✅:
   - `evalConstExpr`: 3 bước theo chuẩn C++ [cpp.cond]:
     1. Thay `defined NAME` / `defined(NAME)` bằng 1/0 (trước macro expansion)
     2. Macro expansion object-like trên token còn lại; identifier không define → 0 (C++ rule); `true`→1, `false`→0
     3. Recursive-descent parser (`ExprParser`) đánh giá integral constant expression
   - Toán tử hỗ trợ (theo precedence tăng dần):
     - Ternary `?:`
     - Logical `||`, `&&`
     - Bitwise `|`, `^`, `&`
     - Comparison `==`, `!=`, `<`, `<=`, `>`, `>=`
     - Shift `<<`, `>>`
     - Arithmetic `+`, `-`, `*`, `/`, `%`
     - Unary `!`, `-`, `+`, `~`
     - Parenthesized `( expr )`
   - Integer literal: decimal, hex `0x`, octal `0`, binary `0b`; suffix U/L bỏ qua
   - `#elif` đúng chuẩn C++: chỉ eval nếu chưa branch nào taken (`anyTaken`); branch đã taken → skip
   - `#elif after #else` → diagnostic lỗi
   - Division/modulo by zero → diagnostic
   - `CondFrame` thêm `anyTaken` để track
   - Test: `examples/test_if.cpp` → 25 test case (literal, arithmetic, bitwise, logical, comparison, ternary, defined(), macro expansion, #elif chains, nested, undefined→0, true/false) → `.i` + `.ll` đúng
6. **Variadic macro** (`...` / `__VA_ARGS__`) ✅:
   - `Macro` struct thêm `bool isVariadic`
   - `parseDefine`: nhận diện `...` ở cuối param list → set `isVariadic = true` (không thêm `...` vào `params`)
   - Hỗ trợ cả `name...` (GNU extension) standalone
   - `expandFunctionLike`: arity check cho phép `args.size() >= params.size()` (variadic); extra args join bằng comma token → `__VA_ARGS__`
   - `__VA_ARGS__` thay thế trong body như param bình thường
   - Empty `__VA_ARGS__` (0 extra args) — C++20 cho phép, expand thành empty
   - `__VA_ARGS__` ở giữa body, không chỉ cuối
   - Test: `examples/test_variadic.cpp` → 7 test case (zero named params, one named param, `__VA_ARGS__` mid-body, empty `__VA_ARGS__`, fallback) → `.i` + `.ll` đúng
7. **Predefined macros** ✅:
   - `__LINE__` → integer literal của dòng hiện tại (context-sensitive)
   - `__FILE__` → string literal của đường dẫn file hiện tại (context-sensitive)
   - `__DATE__` → string literal `"Mmm dd yyyy"` (cố định lúc bắt đầu preprocessing)
   - `__TIME__` → string literal `"HH:MM:SS"` (cố định lúc bắt đầu preprocessing)
   - `__cplusplus` → `202302L` (C++23)
   - `initPredefinedMacros()`: khởi tạo `__cplusplus`/`__DATE__`/`__TIME__` vào `macros_` (object-like); `__LINE__`/`__FILE__` context-sensitive → `tryExpandPredefined()`
   - `isPredefined()`: check 5 tên macro; `isDefined()` include predefined
   - `tryExpandPredefined()`: expand `__LINE__`/`__FILE__` context-sensitively; `__DATE__`/`__TIME__`/`__cplusplus` qua object-like path
   - Tích hợp vào `emitToken`, `expandTokenVector`, vòng lặp `run()` + `processFile()`
   - Ngăn `#define`/`#undef` predefined macro → diagnostic lỗi
   - `buffers_` đổi từ `std::vector` sang `std::deque` để string_view stable khi push_back thêm buffer
   - `defined(__cplusplus)`, `#ifdef __cplusplus`, `#if __cplusplus == 202302` hoạt động
   - Test: `examples/test_predefined.cpp` → 6 test case (`__cplusplus` defined/value, `__LINE__` defined/line 56/59) → `.i` + `.ll` đúng

### P2 — Tương lai

> Chuyển từ "Cần làm" — compiler chưa hoàn thiện, các mục này hoãn lại sau.

1. **`std::move`**:
   - Parser: `std::move(x)` → `Expr::Move`
   - HIR: `Move(a)` → đánh dấu moved-out
   - MIR: đọc sau move → lỗi
2. **`Moved-out`**:
   - MIR: `Move(a)` → `Uninitialized` (không thể đọc)
3. **`nullptr`**:
   - `nullptr` → chỉ trong `[[ivy::unsafe]]`
   - `nullptr_t` → type (trong unsafe)

### P3 — Có thể

1. **`#pragma ivy cnumber`** ✅:
   - `#pragma ivy cnumber` → enable C++ compat types
   - `int`, `long`, `long long`, `float`, `double`, `signed short int`, `unsigned long long int`,... → OK
   - Mặc định: Ivy chỉ chấp nhận fixed-width types (`int8_t`…`int64_t`, `uint8_t`…`uint64_t`, `float16_t`…`float128_t`, `bfloat16_t`, `size_t`, `ptrdiff_t`, `bool`, `void`)
   - C-style types (`int`, `unsigned`, `long`, `short`, `char`, `float`, `double`, `long long`) bị cấm khi chưa có pragma
   - Hex/octal/binary literal (`0xFF`, `017`, `0b1010`) luôn được phép (syntax, không phải type)
   - `Preprocessor`: `parsePragma` nhận diện `#pragma ivy cnumber` → set `cnumberEnabled_`; unknown pragma → warning + drop
   - `Preprocessor.cnumberEnabled()` → truyền cho `Parser` qua `main.cpp`
   - `Parser`: `isTypeStart()` + `parseType()` gate C-style types theo `cnumberEnabled_`; diagnostic rõ ràng gợi ý fixed-width type
   - Pragma trong inactive `#if` block không có hiệu lực (C++ rule)
   - Test: `examples/test_cnumber.cpp` (có pragma → pass) + `examples/test_no_cnumber.cpp` (không pragma → fail đúng)

### P4 — Mở rộng (độ khó tăng dần)

> Các tính năng C++ chưa có, xếp theo độ khó triển khai — từ dễ đến khó.
> (Chèn giữa P3 và P4 cũ; P4 cũ đổi thành P5.)

| # | Tính năng | Độ khó | Ghi chú |
|---|-----------|--------|---------|
| 1 | `enum` ✅ | ★ Dễ | Kiểu hằng số nguyên — parser + codegen như literal, không cần runtime. Đã hoàn thành (P4.1): unscoped/scoped enum (`enum`/`enum class`/`enum struct`), implicit/explicit values, constant expression values (arithmetic, bitwise), `EnumName::Value` cho scoped, explicit underlying type (`enum E : int64_t`), enum-typed variables, enum trong arithmetic/comparison/ternary/if |
| 2 | `namespace` ✅ | ★ Dễ | Chỉ parser/lookup `ns::name` vào HIR symbol table; codegen không đổi. Đã hoàn thành (P4.2): nested namespace, bare call trong namespace, `ns::func(args)`, `ns::EnumName::Value`, `ns::enum_constant`, LLVM name mangling (`::` → `.`), `extern "C"` không mangle |
| 3 | Name mangling ABI ✅ | ★ Trung bình | Đổi mangling sang ABI chuẩn. Đã hoàn thành (P4.3): Itanium ABI cho POSIX (`_Z` + `N...E` nested + type codes), MSVC ABI cho Windows (`?` + `@` scopes + type codes + `@Z`), `--target itanium|msvc` flag, auto-detect từ host platform, scoped enum mangling (nested type encoding), unscoped enum collapse to underlying type, `extern "C"` skip mangling |
| 4 | `struct` / `class` ✅ | ★★ Trung bình | Aggregate type: member layout, codegen `member` access (GEP), init `{ }`. Đã hoàn thành (P4.4): struct/class declaration (fields, access specifiers parsed/ignored), member access `.` (struct lvalue) và `->` (pointer-to-struct), LLVM GEP codegen, nested struct member access (GEP chaining), namespace-qualified struct (`ns::Struct`), struct copy assignment (aggregate load/store), zero-init struct variables, named LLVM struct type (`%struct.Name = type { ... }`), variadic params (`...`) trong extern "C", `main` không bị mangle, aggregate init `{ }` (basic/partial/empty), struct reassign với init list, default member initializers (`int x = 42;` trong struct body — apply cho struct var không init + trailing fields trong partial init) |
| 5 | `lambda` ✅ | ★★★ Trung bình–Khó | Closure: capture list, function type ẩn danh, codegen struct-of-captures + call convention. Đã hoàn thành (P4.5): lambda expression `[caps](params) -> ret { body }` trong `parsePrimary()`, capture modes `[x]` (by-value) + `[&x]` (by-reference) + `[]` (no captures), AST node `Expr::Lambda` + `Expr::Capture` + `cloneStmt()` helper, HIR builder lower lambda thành closure struct type (`__lambdaN_closure`) + call-operator function (`__lambdaN(closure_ptr, params...)`), inject capture locals (`T cap = __closure->cap` by-value / `T& cap = *(__closure->cap)` by-ref) vào đầu body dưới implicit unsafe scope, Lambda callee trong Call handler (thêm `&lambda` làm first arg), MIR builder propagate Lambda variant, codegen: `alloca` closure + GEP init captures + `llvmGlobalName()` quote ký tự đặc biệt, Unary `&` handle Lambda operand (return closure slot), test no-capture + by-value + by-ref captures |
| 6 | `IvyInterpret` (MIR Interpreter & REPL) | ★★★ Khó | **v0.1** (hoàn thành): HIR-based interpreter — fast-path, không safety guarantee. **v0.2** (hoàn thành): MIR-based interpreter — safety check (lifetime + unsafe), dùng cho `--run`/consteval/IvyMake. Xem [MIR_PLAN.md](file:///d:/project/Ivy/ivyc/MIR_PLAN.md) |
| 7 | `constexpr` & `consteval` ✅ | ★★★ Khó | Đánh giá hằng số compile-time. Đã hoàn thành (P4.7): Parser parse `constexpr`/`consteval` keyword (`parseConstexprSpec()`), AST flags (`Function.isConstexpr`/`isConsteval`, `Stmt::Decl.isConstexpr`), HIR/MIR propagate flags, HIR Builder constexpr call folding (`tryEvalConstexprCall` — tree-walking evaluator với parameter substitution, hỗ trợ Unary/Binary/Ternary/If/Return/Compound), codegen skip `consteval` functions (không emit LLVM IR — đã được fold tại compile-time), `constexpr` functions vẫn emit (fallback cho runtime calls). Test: `square(4)→16`, `cube(3)→27` |
| 8 | `IvyMake` | ★★★ Khó | Hệ thống build cấu hình trực tiếp bằng C++/Ivy (file `build.ivy`), tự thông dịch qua `IvyInterpret` v0.2 thay thế cho CMake |
| 9 | ~~`exception`~~ | ★★★ Khó | **Cấm hoàn toàn** — Ivy là subset an toàn, không hỗ trợ `try`/`catch`/`throw`/exception ABI. Lỗi được xử lý qua return code / `Result<T>` (sẽ thêm sau). Loại khỏi Ivy theo triết lý subset |
| 10 | `template` ✅ | ★★★ Khó nhất | Generics chạm cả pipeline: parameterization, instantiation, type deduction. Đã hoàn thành (P4.8): Parser parse `template <typename T>` declaration (`parseTemplate`, `parseTemplateParams`), template-id call `func<int>(args)` với backtracking parser (phân biệt `<` comparison vs template-arg), `templateParamNames_` cho `parseType()` nhận type params. AST: `TemplateParam`, `Function.tplParams`, `Call.tplArgs`. HIR: template registry (`templates_` map), `instantiateTemplate` — clone AST body + substitute types (`substituteType` mapping `T→int`), tạo concrete HIR function với mangled name (`add<int>`), `buildBody` trên cloned body. MIR/Codegen: không cần sửa (instantiated functions là concrete). Test: `add<int>(3,4)→7`, `add<double>(1.5,2.5)→4.0`, `identity<int>(42)→42` |

---

### P4.6 — IvyInterpret (MIR Interpreter)

> **Chi tiết đầy đủ**: [MIR_PLAN.md](file:///d:/project/Ivy/ivyc/MIR_PLAN.md)
>
> **Phiên bản**:
> - **v0.1** (hoàn thành) — HIR-based, fast-path cho REPL, không safety guarantee
> - **v0.2** (hoàn thành) — MIR-based, có safety guarantee (lifetime + unsafe enforcement)
>
> **Triết lý**: IvyInterpret v0.2 **phải** thông dịch MIR, không phải HIR. Nếu chạy thẳng từ HIR, interpreter bỏ qua safety check ở MIR → mất ý nghĩa "Ivy an toàn". Tham khảo kiến trúc **Miri** của Rust (POPL 2026).

**Tóm tắt 6 bước triển khai v0.2** (chi tiết trong MIR_PLAN.md):
1. ✅ Hoàn thiện MIR data structures — thêm `Call::target`, decode string/char lit
2. ✅ Back-fill pass trong MirBuilder — resolve call targets, decode literals
3. ✅ Mở rộng lifetime checker — check store/arg (không chỉ return)
4. ✅ Runtime Value + Memory model — `interp_value.h`, `interp_memory.h`, `Machine` trait
5. ✅ MIR Interpreter engine — `mir/interpreter.h/.cpp` (port từ v0.1 + CFG traversal)
6. ✅ Tích hợp `--run` vào main.cpp + regression test (14 examples pass)



### P5 — Sau

1. **LLVM**:
   - `@llvm.fpext` / `@llvm.fptrunc` cho `float16_t`

---

## Liên kết

- [MIR_PLAN.md](file:///d:/project/Ivy/ivyc/MIR_PLAN.md) — Kế hoạch hoàn thiện MIR (IvyMiri)
- [README](file:///d:/project/Ivy/ivyc/README.md)
- [spec/cast.md](file:///d:/project/Ivy/ivyc/spec/cast.md)
- [spec/types.md](file:///d:/project/Ivy/ivyc/spec/types.md)
- [spec/ownership.md](file:///d:/project/Ivy/ivyc/spec/ownership.md)
- [src/parser.cpp](file:///d:/project/Ivy/ivyc/src/parser.cpp)
- [src/codegen.cpp](file:///d:/project/Ivy/ivyc/src/codegen.cpp)