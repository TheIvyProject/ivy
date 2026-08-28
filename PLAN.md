# Kế hoạch phát triển Ivy

- **Trạng thái**: Active — triển khai
- **Cập nhật**: 2026-08-22
- **Người tạo**: User + AI

---

## Mục tiêu

Xây dựng trình biên dịch `ivyc` — C++ subset an toàn (Ivy), pipeline:

```
Lexer → Preprocessor → Parser → HIR → MIR → LLVM IR
                                    ↘ IvyInterpret v0.2 (MIR-based, --run)
```

Triết lý: Ivy là **subset an toàn** của C++ — giữ cú pháp quen thuộc, loại bỏ
các tính năng nguy hiểm (exception, goto, union, asm, RTTI). An toàn được đảm
bảo bằng lifetime checking ở tầng MIR và `[[ivy::unsafe]]` scope.

Đích đến dài hạn: đi cùng con đường mà C++ từng đi từ C ("C with Classes" →
ngôn ngữ độc lập) — Ivy bắt đầu là subset của C++, rồi dần tách ra thành
**ngôn ngữ Ivy độc lập** (Giai đoạn 10).

---

## Hiện trạng tổng quan (2026-08-22)

### Thành phần đã hoàn thành

| # | Thành phần | Trạng thái | Ghi chú |
|---|-----------|-----------|---------|
| 1 | `parsing/lexer` | ✅ | Token stream; decimal/hex/octal/binary int, digit separator, raw string `R"(...)"`, char escape đầy đủ |
| 2 | `parsing/preprocessor` | ✅ | `#include`, `#define` (object/function-like/variadic), `#ifdef/#ifndef/#else/#endif/#undef`, `#if/#elif`, predefined macros, `#pragma ivy cnumber` |
| 3 | `parsing/parser` | ✅ | AST đầy đủ cho subset; template declaration + template-id call với backtracking |
| 4 | `hir/hir_builder` | ✅ | Type check, name resolution (namespace), constexpr folding, template instantiation |
| 5 | `mir/mir_builder` | ✅ | CFG lowering, lifetime annotation |
| 6 | `mir/interpreter` | ✅ | IvyInterpret v0.2 — MIR-based, safety guarantee (`--run`) |
| 7 | `codegen/codegen` | ✅ | LLVM IR emitter; Itanium/MSVC ABI mangling |
| 8 | `app/main.cpp` | ✅ | CLI; nhận `.ivy` (canonical) + `.cpp/.cc/.cxx/.c` (legacy migration); warning extension lạ |

### CLI

```
usage: ivyc [options] <file.ivy|file.cpp>

Source files:
  .ivy        Ivy source (the safe C++ subset)
  .cpp/.cc/.cxx/.c  Legacy C/C++ source (migration)

options:
  --tokens/--ast/--hir/--mir   dump IR trung gian (debug)
  --llvm     emit LLVM IR ra stdout
  --run      thông dịch qua IvyInterpret v0.2
  --target <itanium|msvc>      chọn ABI mangling
  -o file    .ll -> LLVM IR, .i/.ii -> source sau tiền xử lý
  -I dir     thêm đường dẫn #include <...>
```

---

## Ma trận tính năng ngôn ngữ

### ✅ Đã hỗ trợ

| Nhóm | Tính năng |
|------|-----------|
| **Types** | Fixed-width `int8_t..int64_t`, `uint8_t..uint64_t`, `float16_t..float128_t`, `bfloat16_t`, `size_t`, `ptrdiff_t`, `nullptr_t`; `bool`, `void`; pointer đa cấp `T*`, reference `T&`/`const T&`; const qualifier |
| **C-style types** | `int`, `long`, `float`, `double`, `char`, `unsigned`... — chỉ khi có `#pragma ivy cnumber` |
| **Declarations** | function, biến local, struct/class (aggregate), enum/enum class (+ underlying type), namespace (lồng), `extern "C"`, template function, constexpr/consteval function |
| **Statements** | if/else, while, do-while, for (C-style), break, continue, return, compound, `[[ivy::unsafe]] { }` |
| **Expressions** | mọi literal (int dec/hex/oct/bin + suffix, float, string, char, bool, nullptr); binary đầy đủ precedence; unary prefix/postfix; ternary; assignment đầy đủ; call (+ template-id); member `.`/`->`; scope `::`; subscript `[]`; lambda `[x, &y](...)`; braced init `{ }`; new/delete (chỉ trong unsafe) |
| **Struct** | member layout, GEP access, aggregate init `{}` (basic/partial/empty), default member initializer, copy assignment |
| **Lambda** | capture by-value/by-ref/no-capture, closure struct + call-operator codegen |
| **Enum** | unscoped/scoped, constant-expression values, `Enum::Value`, explicit underlying type |
| **Namespace** | lồng nhau, qualified lookup, mangling `::` → `.` |
| **Mangling** | Itanium ABI (`_Z...`) + MSVC ABI (`?...@Z`), auto-detect host, enum encoding |
| **Preprocessor** | include expansion, macro object/function-like/variadic, conditional compilation, `#if EXPR` evaluator đầy đủ, `__LINE__/__FILE__/__DATE__/__TIME__/__cplusplus` |
| **Constexpr** | `constexpr`/`consteval` function, compile-time call folding, consteval skip codegen |
| **Template** | function template `template <typename T>`, explicit instantiation `func<int>(args)`, dedup instantiation, mangled name |
| **Interpreter** | `--run` trên MIR, builtins: printf/puts/putchar/exit/abort/malloc/free |

### ⚠️ Hỗ trợ hạn chế

| Tính năng | Giới hạn |
|-----------|----------|
| Lambda | KHÔNG `[=]`/`[&]` capture-all |
| new/delete | CHỈ trong `[[ivy::unsafe]]`; malloc/free thuần, không ctor/dtor |
| Template | chỉ function template; không deduction (`func(3,4)` phải viết `func<int>(3,4)`); không specialization; non-type param parse nhưng chưa dùng |
| Variadic | chỉ `...` trong `extern "C"`; extra args không type-check |
| Const | chỉ compile-time check; không global constexpr variable |
| String | narrow `"..."` + raw; `L"/u8"/u"/U"` prefix bị tách token |
| Suffix literal | strip u/U/l/L/f/F nhưng không validate thứ tự; không hex float `0x1.8p1` |
| Prescan `#if` | simplified 1 cấp, giới hạn 64 lần chain |

### 🚫 Bị loại bỏ có chủ đích (triết lý subset an toàn)

| Tính năng | Lý do |
|-----------|-------|
| Exception (`try`/`catch`/`throw`) | Cấm hoàn toàn (P4.9). Lỗi xử lý qua return code / `Result<T>` tương lai |
| `goto` | Phá hủy phân tích control-flow của MIR |
| `union` | Type-punning unsafe |
| `asm` | Không thể verify an toàn |
| RTTI (`dynamic_cast`, `typeid`) | Cần inheritance + runtime metadata |
| Virtual dispatch / inheritance | Cần vtable — hoãn đến khi có class model đầy đủ |

---

## Lộ trình còn thiếu (để tiến tới C++ hoàn chỉnh)

Kết quả khảo sát chi tiết 4 tầng (Lexer → Preprocessor → Parser → HIR/MIR/Codegen).
Xếp theo giai đoạn tăng dần độ khó.

### Giai đoạn 6 — Đóng gaps cơ bản (ưu tiên cao nhất)

| # | Task | Độ khó | Chi tiết triển khai |
|---|------|--------|---------------------|
| 6.1 | **`switch`/`case`/`default`** ✅ | ★★ | Parser: `Stmt::Switch` + case list. HIR: integral condition check, no-fallthrough enforcement (error nếu case không kết thúc bằng break/return/continue). MIR: `Inst::Switch` + `SwitchArm`. Codegen: LLVM `switch` instruction. MIR Interpreter: `K::Switch` dispatch. Test: `classify`, `fibonacci`, switch-with-break ✅ |
| 6.2 | **`auto` type deduction** ✅ | ★★ | Parser: `auto` vào `isTypeStart()` + `parseType()` (trả về `Type{"auto"}`), xóa khỏi `kUnsupported`. HIR: `buildDeclaration` detect `d.type.base=="auto"` → `buildExpr(init)` → infer type từ `expr.type`, propagate `const`/pointer/ref qualifiers, đăng ký type đã infer vào scope. Test: `auto i=42` (int), `auto f=3.14` (double), `auto b=true` (bool), `auto sum=i+8`, `auto r1=add(10,20)`, `auto j=0` in for loop, `const auto pi` ✅ |
| 6.3 | **Array type `T[N]`** ✅ | ★★ | `Type` struct: thêm `arraySize`. Parser: parse `T name[N]`. HIR: zero-init array (không cần init tường minh), index handler + element type infer. Codegen: `alloca [N x T]` + `zeroinitializer`, IdentRef decay to ptr, GEP `[N x T], ptr, i32 0, iXX idx`, bounds check inline (icmp uge → `__ivy_panic` + unreachable, Rust style), skip khi `[[ivy::unsafe]]`. MIR interpreter: `__array` struct representation, bounds check runtime. Test: sum_array, zero_init, data[2] — pass ✅ |
| 6.4 | **Global variables** | ★★ | AST: `TranslationUnit.globalVars`. Parser: parse top-level declaration. Codegen: emit `@global = global T init`. Static init order: `@llvm.global_ctors` nếu cần |
| 6.5 | **Function overloading** ✅ | ★★★ | `functions_`: đổi sang `unordered_map<name, vector<Function*>>` (overload set). Resolution: exact match (score 100) → promotion theo width closeness (score 90-diff) → implicit conversion (score 1). Ambiguous → error. Mangling sẵn sàng (mỗi overload có symbol riêng). HIR `buildSignature` kiểm redefinition theo signature. MIR builder + codegen + interpreter: back-fill target bằng name + param signature match. Test: add(2,3)/add(2,3,4), square(int32_t)/square(int64_t), power(int32_t)/power(int64_t) — pass ✅ |
| 6.6 | **Default arguments** ✅ | ★ | AST: `Param.defaultValue` (unique_ptr<Expr>). Parser: parse `= expr` trong param list. HIR: `hir::Param.defaultValue` (const ivy::Expr* — AST owned). `checkCall` fill missing trailing args: clone AST default expr → buildExpr tại call site (C++ semantics). `resolveOverload` chấp nhận candidates có default cho params thiếu (score 80/param). Clone lambda params sửa để handle unique_ptr. Test: greet(1)/greet(1,2)/greet(1,2,3), power(5)/power(2,10), compute(4)/compute(4,6)/compute(4,6,8) — pass ✅ |
| 6.7 | **Class methods** ✅ | ★★★ | Struct body: parse member functions. HIR: method = function với implicit `this` param (`ClassName::method`). Call: `obj.method(args)` → `method(&obj, args)`. Name mangling: nested-name theo ABI. AST: `StructDecl.methods` + `Expr::This`. Parser: register `structNames_` sớm trước khi parse body để method params có thể reference chính struct (vd `void set(Point other)`); method name lưu vào `nameStorage_` (string_view ổn định). `flattenMemberChain` phân biệt `::` (isScope) vs `.`/`->` để không nhầm `obj.method` thành qualified call `obj::method`. HIR: register method vào `functions_` + `StructDef.methods` (bare name); pass 2b build method bodies; `isAssignable` cho `T&` bind to `T`. MIR/Codegen/Interpreter: propagate `isScope`, `This` handler. Test: `p.sum()=7`, `p.set(10,20)`, `p.distance_squared(q)=500` — pass ✅ |
| 6.8 | **Constructor/Destructor (RAII)** ✅ | ★★★★ | AST: `Function.isCtor`/`isDtor` + `MemberInit` list (`: x(42), y(3)`). Parser: detect `Name(...)`/`~Name()` trong struct body (dựa `bareStructName`), member init list sau `:`, direct ctor call `Type name(args)`. HIR: `StructDef.ctors`/`dtor`/`astCtors`/`astDtor` (dùng parallel `methodFns` vector trong buildStruct — fix bug ambiguous name lookup cho overload ctors cùng `StructName::StructName`); `buildDeclaration` inject `ctor(&var, args)` wrap [Decl, CtorCall]; `dtorStacks_` per-scope tracking biến struct có dtor (sync với `scopes_` — for/lambda push cả hai); `emitDtorCalls(dtorStacks_.size()-1,...)` tại scope-exit (Compound end), Return (probe `emitDtorCalls(0,...)` — chỉ wrap khi có dtor, tránh phá switch no-fallthrough check), Break/Continue (wrap dtor + stmt). `resolveCtorOverload` riêng (skip param[0]=`this`, score 100 cho default ctor exact). Member init list → `this.field = arg` (isArrow=false vì `this` là reference). `cloneHirExpr` deep-copy cho arg extraction. Codegen: MSVC `??0`/`??1` + Itanium `_ZN...C1`/`D1` mangling. Test: Logger ctor/dtor member init, Pair default+parameterized ctor, dtor reverse-order scope-exit, RAII early return, RAII break/continue — all pass ✅ |
| 6.9 | **`using` alias** ✅ | ★ | Chỉ `using Name = Type;` (bỏ `typedef` — legacy, cú pháp lùi, không cần cho safe subset). AST: `UsingDecl` struct + `TranslationUnit.usingDecls` vector. Parser: `parseUsing()` + `typeAliases_` vector (lưu bare name, giống `structNames_`/`enumNames_`); dispatch trong `parseTopLevel` + `parseNamespace`; `isTypeStart()` + `parseType()` check `typeAliases_` (cả bare lẫn qualified); `typedef` reject với message "use 'using' instead of 'typedef'". HIR: `typeAliases_` map (name → Type) — `buildUsing()` lưu cả bare lẫn qualified key (qualified = `ud.name`, bare = last `::` segment) để `resolveTypeAlias` tìm được khi dùng cả 2 dạng; `resolveTypeAlias()` expand recursive (alias→alias, merge const/ref/pointer); gọi trong `declare()`, `buildSignature()` (return + params), `buildStruct()` (field layout + fieldMap + fields + `resolved` StructDecl copy — fix bug MIR/codegen thấy alias thay vì underlying type), `buildDeclaration()` (decl.type, toàn bộ downstream dùng `decl.type` thay `d.type`), method params trong `buildStruct`, overload matching pass 2a/2b (expand alias AST side để match HIR side đã resolve). Test: `Int=int32_t`, `Word=uint32_t`, `SizeT=uint64_t`, `IntPtr=int32_t*`, alias chain `AnotherInt=Int`, struct `Point` với aliased fields + ctor `Point(Int,Int)` + member init list, `Point pt(1,2)` ctor call — `--run` exit 248 (42+100+7+99), `--llvm` emit `%struct.Point={i32,i32}` + ctor `??0Point` + `call ctor` ✅ |

### Giai đoạn 7 — C++ hiện đại (C++17/20 core)

| # | Task | Độ khó | Chi tiết triển khai |
|---|------|--------|---------------------|
| 7.1 | **Range-based for** ✅ | ★★ | `for (T x : arr)` / `for (T& x : arr)` — chỉ hỗ trợ fixed-size array `T[N]`. Desugar tại HIR thành C-style index loop với bound N captured compile-time từ `arraySize` → **an toàn: không bị iterator-invalidation** như `std::vector::push_back` trong C++. By-value `T x` copy; by-ref `T& x` alias element (mutable). Interpreter: reference Alloca dùng `lvalueCell` để alias cell thay vì `evalExpr` copy |
| 7.2 | **Template class/struct** ✅ | ★★★ | Mở rộng registry `templates_` cho struct: `structTemplates_` + `instantiateStructTemplate` clone StructDecl + `substituteType` cho fields/methods + `cloneFunctionAst` deep-copy body + build body cho cloned methods trong khi save/restore `current_`/`hasReturnInBody_`. `resolveTemplateStructType` hook vào `buildStruct`/`buildSignature`/`buildDeclaration` trigger on-demand instantiation khi gặp `Box<int32_t>`. Codegen `escapeLlvmIdent` escape `<`/`>`/`,` → `_` cho LLVM IR identifier hợp lệ (`Box<int32_t>` → `Box_int32_t_`). Là nền cho `Result<T>`/container |
| 7.3 | **Template type deduction** ✅ | ★★ | `func(3, 4)` → infer `T=int32_t` từ arg types. `deduceTemplateArgs`: duyệt từng param, nếu `paramType.base` là type param (`T`) → deduce `T=argType`. 3 case: (1) exact match `T` param → `T=argType` (strip const nếu param là `const T`), (2) array `T[N]` param → `T=argType.base` (arg decay `T*`), (3) const/ref stripping (`const T&` param → `T=arg` strip ref/const). Hook vào call site: khi `resolveOverloads` empty → check `lookupTemplate` → deduce → `instantiateTemplate`. Consistency check: nếu `T` deduced từ nhiều args, phải khớp. Kết hợp với overload resolution (6.5) — template chỉ được xét khi không có non-template overload match |
| 7.4 | **Operator overloading** | ★★★ | Parse `operator+`/`==`/`[]`/`()`/`->` như method đặc biệt. HIR Binary handler: nếu operand là struct → lookup `operatorX` method. Ưu tiên member operator trước free function |
| 7.5 | **`if constexpr`** | ★★ | Parser: flag `isConstexpr` trên `Stmt::If`. HIR: trong template instantiation (7.2) — evaluate điều kiện constexpr, chỉ build nhánh true/false (discarded statement không instantiate). Ngoài template thì `if constexpr` == `if` thường |
| 7.6 | **Variadic templates** | ★★★★ | `typename... Args`, pack expansion `args...`, `sizeof...(args)`. Instantiation sinh N phiên bản theo arity. Fold expression `(args + ...)`. Là nền cho `format()`-style API |
| 7.7 | **Inheritance + virtual** | ★★★★ | `class A : public B` — base subobject layout, upcast. Vtable: bảng function pointer per polymorphic class, vptr là field ẩn đầu tiên. `virtual`/`override` check. Deviate từ C++: có thể yêu cầu `[[ivy::virtual]]` tường minh |
| 7.8 | **Structured bindings** | ★★ | `auto [a, b] = pair;` — cần `auto` (6.2). Với struct: bind từng field theo tên. Với tuple: cần stdlib tuple trước |

### Giai đoạn 8 — Toolchain hoàn chỉnh

| # | Task | Độ khó | Chi tiết triển khai |
|---|------|--------|---------------------|
| 8.1 | **Object file emission** | ★★ | Dùng LLVM library (thay vì emit text IR): `TargetMachine.emitToFile` → `.o`/`.obj`. Hoặc invoke `llc` external. Flag: `-c`, `-o out.o` |
| 8.2 | **Linking** | ★★ | Invoke linker hệ thống (`link.exe`/`ld`/`clang`). Link libc mặc định (malloc/free/printf). Flag: `-o app.exe` tự động link |
| 8.3 | **`sizeof` / `alignof`** | ★ | Compile-time: tra cứu layout đã compute ở HIR builder, fold thành integer literal |
| 8.4 | **Cast operators** | ★★ | `static_cast` (numeric conversion + pointer upcast/downcast checked), `reinterpret_cast` (chỉ unsafe). Vẫn cấm `dynamic_cast` (RTTI) |
| 8.5 | **Standard library tối thiểu** | ★★★ | Viết bằng chính Ivy (dogfooding): `ivy::string` (SSO), `ivy::vector<T>` (cần template class 7.4 + new/delete), `ivy::result<T,E>` thay exception, `ivy::print` thay printf |
| 8.6 | **`#error` / `#warning` / `#line`** | ★ | Preprocessor: parse directive, báo diagnostic / set line mapping |
| 8.7 | **Stringify `#` + paste `##`** | ★★ | Macro metaprogramming: stringify argument token sequence; paste hai token thành một (validate hợp lệ) |
| 8.8 | **Wide/UTF string prefixes** | ★ | Lexer: gộp `L"/u8"/u"/U"` + string thành một token. Codegen: emit global với đúng type (`[N x i16]` cho wide...) |

### Giai đoạn 9 — C++20/23 nâng cao

| # | Task | Độ khó | Chi tiết triển khai |
|---|------|--------|---------------------|
| 9.1 | **Concepts** | ★★★ | `concept Addable = requires(T a, T b) { a + b; };` — constraint check tại template instantiation, error message rõ ràng thay vì lỗi substitution sâu |
| 9.2 | **Modules** | ★★★★ | `export module`, `import` — binary module interface (.ivm) thay #include. Cần đổi preprocessor architecture. Có thể hoãn vô thời hạn (Ivy dùng #include vẫn ổn) |
| 9.3 | **Designated initializers** | ★ | `{.field = 42}` — parse trong InitList, map field name → offset, validate thứ tự |
| 9.4 | **User-defined literals** | ★★ | `operator""_km(long long)` — lexer gộp suffix, HIR lookup UDL operator |
| 9.5 | **Coroutines** | ★★★★+ | `co_await`/`co_return`/`co_yield` — cần transform CFG phức tạp. Có thể KHÔNG làm (không thuộc triết lý subset) |
| 9.6 | **`std::move` + rvalue ref `T&&`** | ★★★ | Move semantics cho ownership tracking: move-out state trong MIR, use-after-move error. Nền cho smart pointer `ivy::unique_ptr<T>` |

### Giai đoạn 10 — Độc lập hoàn toàn khỏi C++ (Ivy 1.0)

Giống như C++ thoát thai từ C ("C with Classes" 1979 → ngôn ngữ độc lập),
Giai đoạn 10 tách Ivy ra khỏi gốc C++ để trở thành **ngôn ngữ độc lập**:
không còn C-style types, không phụ thuộc libc/CRT, stdlib riêng, tự biên dịch
chính nó (self-hosting). Từ đây Ivy không phải "C++ subset" nữa mà là
"Ivy language".

| # | Task | Độ khó | Chi tiết triển khai |
|---|------|--------|---------------------|
| 10.1 | **Loại bỏ C-style types** | ★★ | Xóa `#pragma ivy cnumber` và các type `int`/`long`/`short`/`unsigned`. Chỉ giữ fixed-width `i8..i64`, `u8..u64`, `f16/f32/f64/f128` (đổi tên gọn hơn). Code cũ migrate bằng script đổi type |
| 10.2 | **Own runtime + entry point** | ★★★ | Bỏ CRT: entry `_start` → `ivy_main()`, không link libc. Own allocator (arena + free-list), own `print()`/`format()` thay printf. Codegen emit freestanding object |
| 10.3 | **Module system thật** | ★★★★ | Quay lại 9.2 nhưng bắt buộc: `export module ivy.io`, binary `.ivm` interface, bỏ preprocessor cho code Ivy (giữ macro chỉ trong unsafe/FFI). Import graph + incremental compile |
| 10.4 | **Stdlib hoàn chỉnh viết bằng Ivy** | ★★★★ | `ivy::string`, `ivy::vector<T>`, `ivy::result<T,E>`, `ivy::option<T>`, `ivy::unique_ptr<T>` (move semantics 9.6), `ivy::slice<T>` (thay array/pointer), collections (map/set), IO, time. Đủ để viết app thực dụng không cần libc |
| 10.5 | **Self-hosting** | ★★★★★+ | Viết lại ivyc bằng chính Ivy: lexer → preprocessor-lite → parser → HIR → MIR → codegen. Mốc cuối cùng của sự độc lập — như khi GCC/Clang tự biên dịch chính mình. Có thể bắt đầu bằng port từng tầng, dùng IvyInterpret để chạy bootstrap |
| 10.6 | **Bản sắc ngôn ngữ** | ★★ | Versioning chính thức (Ivy 1.0 spec), diagnostic style riêng (error messages có hint sửa lỗi), naming convention riêng, tooling: `ivyc fmt`, `ivyc doc`, LSP server. Đổi tagline từ "safe C++ subset" thành "the Ivy programming language" |

Tiêu chí hoàn thành Giai đoạn 10:
1. Một chương trình Ivy chạy được mà không link bất kỳ thư viện C nào.
2. ivyc được viết 100% bằng Ivy và tự biên dịch chính nó (`ivyc build ivyc.ivy`).
3. Spec ngôn ngữ độc lập tách khỏi tài liệu C++.

---

## Các mốc đã hoàn thành (lịch sử rút gọn)

| Giai đoạn | Nội dung | Trạng thái |
|-----------|----------|-----------|
| GĐ 1 | Skeleton pipeline: lexer → parser → HIR → MIR → LLVM IR emitter + demo | ✅ |
| GĐ 2 | Spec: cast, types, ownership + README | ✅ |
| GĐ 3 (P0) | Fixed-width types, reference `T&`/`const T&`, promotion | ✅ |
| GĐ 3 (P1) | Preprocessor: include, macro (object/function/variadic), conditionals, `#if EXPR`, predefined macros | ✅ |
| GĐ 3 (P3) | `#pragma ivy cnumber` gate cho C-style types | ✅ |
| GĐ 4 (P4.1) | enum + enum class | ✅ |
| GĐ 4 (P4.2) | namespace + qualified lookup | ✅ |
| GĐ 4 (P4.3) | Name mangling Itanium/MSVC ABI + `--target` | ✅ |
| GĐ 4 (P4.4) | struct/class aggregate + GEP + aggregate init | ✅ |
| GĐ 4 (P4.5) | lambda + closure codegen | ✅ |
| GĐ 4 (P4.6) | IvyInterpret v0.2 (MIR-based, safety guarantee) — xem [MIR_PLAN.md](file:///d:/project/Ivy/ivyc/MIR_PLAN.md) | ✅ |
| GĐ 4 (P4.7) | constexpr/consteval + compile-time folding | ✅ |
| GĐ 4 (P4.8) | Function template + explicit instantiation | ✅ |
| GĐ 4 (P4.9) | Cấm exception hoàn toàn | ✅ |
| Driver | `.ivy` extension nhận diện + warning extension lạ | ✅ |

Chi tiết triển khai từng mốc nằm trong git history (`git log --oneline`).

---

## Liên kết

- [README](file:///d:/project/Ivy/ivyc/README.md)
- [MIR_PLAN.md](file:///d:/project/Ivy/ivyc/MIR_PLAN.md) — kế hoạch IvyInterpret v0.2
- [spec/cast.md](file:///d:/project/Ivy/ivyc/spec/cast.md)
- [spec/types.md](file:///d:/project/Ivy/ivyc/spec/types.md)
- [spec/ownership.md](file:///d:/project/Ivy/ivyc/spec/ownership.md)
