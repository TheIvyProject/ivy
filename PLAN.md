# Kế hoạch Ivy 0.5 — Self-hosting theo tính năng (cây đối xứng)

- **Trạng thái**: Active — self-host 0.5; dừng hướng module-first
- **Cập nhật**: 2026-10-17
- **Người tạo**: User + AI
- **Tiền đề**: [OLD_PLAN1.md](file:///d:/project/Ivy/ivyc/OLD_PLAN1.md) (0.1 — syntax độc lập, ✅)

---

## 1. Vì sao đổi phương pháp

Giai đoạn 0.1 đã xong: syntax độc lập, compiler C++ + CRT/libc
(chi tiết ở [OLD_PLAN1.md](file:///d:/project/Ivy/ivyc/OLD_PLAN1.md)).

Lần port self-host đầu tiên (`bootstrap/lexmod.ivy`, `ppmod.ivy`) làm theo
**module-first**: viết trọn một module rồi so output end-to-end. Ba vấn đề:

1. **Đơn vị quá lớn** — module ~1000–3000 dòng; khi lệch chỉ thấy "output khác
   ở dòng N", không khoanh vùng được tính năng nào hỏng.
2. **Hai nguồn sự thật tồn tại lâu** — bản C++ và bản Ivy song song, không có
   cơ chế kiểm chứng ở mức đơn vị nên khó tin bản port.
3. **Không dogfood sớm** — phải xong cả tầng mới có giá trị sử dụng.

**Quyết định**: bỏ hướng module-first và cây `bootstrap/`. Chuyển sang
**feature-first + cây đối xứng**: port theo *tính năng*, mỗi bên nằm ở vị trí
đối xứng để đối chiếu cơ học.

---

## 2. Nguyên tắc

| # | Nguyên tắc | Nội dung |
|---|-----------|----------|
| P1 | **Feature-sliced** | Đơn vị công việc = **1 tính năng**, không phải 1 module. Mỗi tính năng có corpus + oracle + tiêu chí "xong" riêng |
| P2 | **Layer-sequenced** | Thứ tự theo phụ thuộc tầng: lexer → preprocessor → parser → HIR → MIR/interp → codegen → driver. Không nhảy tầng |
| P3 | **Symmetric tree** | `src/<path>` ↔ `selfhost/<path>`, ánh xạ 1:1 để đối chiếu. Mirror là **bản đồ điều hướng**, không phải nghĩa vụ 1:1 (spec 0.5 bỏ/teo một số phần, ví dụ preprocessor) |
| P4 | **Oracle mức tính năng** | Diff ở mức đơn vị qua dump shim mỏng `extern "C"`, **không** bind STL/class C++ |
| P5 | **Byte-identical gate** | Mỗi tầng xong phải pass `difftest` end-to-end byte-identical với baseline C++ |
| P6 | **Freeze baseline** | Đóng băng compiler C++ (oracle) tại một tag trong suốt quá trình port |
| P7 | **Spec-first** | Port theo [spec/ivy05.md](file:///d:/project/Ivy/ivyc/spec/ivy05.md), không port theo code C++ |
| P8 | **Strangler + dogfood** | Tính năng/tầng nào xong thì thay vào binary thật (sau cờ), chạy suite qua nhánh Ivy, rồi xóa bản C++ tương ứng |

---

## 3. Cấu trúc cây đối xứng

```
src/                              selfhost/
  parsing/lexer.cpp            ↔    parsing/lexer.ivy
  parsing/preprocessor.cpp     ↔    parsing/preprocessor.ivy
  parsing/parser.cpp           ↔    parsing/parser.ivy
  parsing/ast.h                ↔    parsing/ast.ivy
  parsing/token.h              ↔    parsing/token.ivy
  parsing/module_io.cpp        ↔    parsing/module_io.ivy
  hir/hir.h                    ↔    hir/hir.ivy
  hir/hir_builder.cpp          ↔    hir/hir_builder.ivy
  mir/mir.h                    ↔    mir/mir.ivy
  mir/mir_builder.cpp          ↔    mir/mir_builder.ivy
  mir/interpreter.cpp          ↔    mir/interpreter.ivy
  codegen/codegen.cpp          ↔    codegen/codegen.ivy
  app/main.cpp                 ↔    app/main.ivy
  common/diagnostic.h          ↔    common/diagnostic.ivy
```

- `src/` = tham chiếu C++ (oracle). `selfhost/` = bản port Ivy.
- Cây `bootstrap/` (lexmod/ppmod/lexdrive/ppdrive) **gỡ bỏ** — xem H0.
- Một số file `src/` không có mirror đối ứng vì spec 0.5 đã bỏ/teo
  (preprocessor trong `.ivy` bị cấm `#include` theo E4). Ghi rõ trong bảng §5.

---

## 4. Cơ chế đối chiếu (oracle)

1. **Oracle**: tái dùng chế độ dump sẵn có của compiler C++ (`--lex`, `--tokens`,
   `--ast`, `--hir`, `--mir`) và **lọc theo token/đơn vị đang port** ở harness.
   Chỉ thêm `extern "C"` shim khi chế độ dump sẵn có không đủ độ phân giải
   (bind class C++ là phương án cuối, không phải mặc định).
2. **Corpus theo tính năng**: `selfhost/corpus/<tầng>/<tính năng>/*.ivy` — nhỏ,
   nhanh, đúng chỗ. Corpus toàn cục (`examples/`, `std/`) dùng cho gate tầng.
3. **Harness**: `selfhost/difftest.ps1` — build driver, chạy oracle + port, lọc
   cả hai theo `-Kinds` (danh sách token-kind), rồi so byte. Driver của tính
   năng đặt cạnh module, ví dụ `selfhost/parsing/lexdrive.ivy`.
4. **Bảng ánh xạ** (§5) là nguồn sự thật duy nhất về tiến độ.

### Hiện trạng

- H1.1: oracle = `ivy --lex` lọc kind `identifier,keyword,lifetime`; port =
  `selfhost/parsing/lexer.ivy` + driver `selfhost/parsing/lexdrive.ivy`;
  corpus `selfhost/corpus/lexer/ident/` (10 file). Lệnh:
  `powershell -File selfhost/difftest.ps1`.
- H1.2: cùng port + driver; oracle lọc kind `integer,float`; corpus
  `selfhost/corpus/lexer/num/` (8 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/num -Kinds integer,float`.
- H1.3: cùng port + driver; oracle lọc kind `string,char`; corpus
  `selfhost/corpus/lexer/str/` (7 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/str -Kinds string,char`.
- H1.4: cùng port + driver; oracle `-Kinds all` (so toàn bộ token stream, gồm
  cả `eof`; cần `all` vì kind của toán tử `,` trùng dấu phân cách `-Kinds`);
  port đã phát token `eof`. Corpus `selfhost/corpus/lexer/op/` (5 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/op -Kinds all`.
- H1.5: cùng port + driver; oracle `-Kinds all` (comment/whitespace là trivia,
  không sinh token — kiểm chứng qua token stream + `eof`). Corpus
  `selfhost/corpus/lexer/trivia/` (7 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/trivia -Kinds all`.
- H1.6: cùng port + driver; oracle `-Kinds all`. Port thêm chẩn đoán lỗi lexer
  (unexpected character; invalid hex/bin; unterminated string/char/raw/block
  comment) đúng định dạng oracle `"path":line:col: error: msg` và **chỉ in chẩn
  đoán** (không in token) khi có lỗi, giống `ivy --lex`. Corpus
  `selfhost/corpus/lexer/err/` (9 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/lexer/err -Kinds all`.
- H2.1: port `preprocessor.ivy` (object-like `#define` + macro expansion) +
  driver `ppdrive.ivy` (lex → preprocess → in token stream). Oracle
  `ivy --tokens`, `-Modules token,lexer,preprocessor -Driver ppdrive`. Corpus
  `selfhost/corpus/pp/define_obj/` (10 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/pp/define_obj -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive`.
- H2.2: cùng port + driver; oracle `--tokens`. Port thêm `#define` function-like
  (fixed-arity + variadic `...`/`__VA_ARGS__`): tham số thực được pre-expand
  (mirror `expandFunctionLike`), body sau thay thế được rescan; dấu `,` nối
  `__VA_ARGS__` tổng hợp lấy lexeme từ separator call-site nhưng line/col của
  tên macro. Corpus `selfhost/corpus/pp/define_fn/` (12 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/pp/define_fn -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive`.
- H2.3: cùng port + driver; oracle `--tokens`. Port thêm conditional
  (`#ifdef`/`#ifndef`/`#if`/`#elif`/`#else`/`#endif`) + `#undef`, kèm evaluator
  biểu thức hằng của `#if` (precedence climbing trên `long long`,
  `defined NAME`/`defined(NAME)`, mở rộng macro object-like một cấp, định danh
  không định nghĩa → 0). Directive được xử lý cả khi đang inactive (theo dõi
  nesting); token ngoài directive trong nhánh inactive bị bỏ qua. Corpus
  `selfhost/corpus/pp/cond/` (16 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/pp/cond -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive`.
- H2.4: cùng port + driver; oracle `--tokens`. Port thêm `#` (stringify) và
  `##` (token paste) trong macro body qua `substitute_body` trên hai buffer
  tham số raw + expanded (object- và function-like). Lexeme tổng hợp (đã paste
  / stringify) lưu trong một arena văn bản hợp nhất chứa cả bản sao nguồn, nên
  printer và tra cứu tên macro đọc thống nhất. Corpus `selfhost/corpus/pp/expand/`
  (16 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/pp/expand -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive`.
- H2.5: cùng port + driver; oracle `--tokens`. Predefined macros (`__LINE__`/
  `__FILE__` ngữ cảnh; `__cplusplus`/`__ivy__`/`__DATE__`/`__TIME__` object-like
  tại 0:0), `#pragma ivy cnumber`, và chẩn đoán unknown-pragma / redefine /
  #undef predefined. Body của predefined nằm trong bản sao mở rộng của mảng
  token đầu vào; chẩn đoán do driver in ra và chặn dump token khi lỗi. Corpus
  `selfhost/corpus/pp/predef/` (20 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/pp/predef -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive`.
- H2.6: cùng port + driver; oracle `--tokens`. `#line N ["file"]` (remap dòng
  token phát ra sau đó theo đường emitToken và đổi tên file trong chẩn đoán),
  `#error` (fatal) và `#warning` (không fatal); thông điệp gom bằng join khoảng
  trắng đơn và bỏ nháy chuỗi. Corpus `selfhost/corpus/pp/line/` (22 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/pp/line -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive`.
- H2.7: cùng port + driver; oracle `--tokens`. E4: `#include` bị từ chối trong
  file `.ivy` (chẩn đoán + bỏ qua dòng). Corpus `selfhost/corpus/pp/no_include/`
  (9 file). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/pp/no_include -Kinds all -Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive`.
- Gate tầng H2 (H2.1–H2.7): toàn bộ 7 corpus pp (`define_obj` 10, `define_fn` 12,
  `cond` 16, `expand` 16, `predef` 20, `line` 22, `no_include` 9 — tổng 105 file)
  byte-identical với `ivy --tokens` (gồm `eof`, chẩn đoán, macro paste/stringify,
  predefined, `#line`/`#error`/`#warning`, từ chối `#include`). Tầng preprocessor
  hoàn tất.
- Gate tầng H1 (H1.1–H1.6): port lex byte-identical toàn bộ token stream trên
  98 file thật (`examples/`, `std/`, `selfhost/parsing/`) so với `ivy --lex`,
  gồm cả `eof`; và byte-identical chẩn đoán lỗi trên corpus `err/`. Tầng lexer
  hoàn tất.
- H3.1 (Declaration): port parser + dump AST byte-identical trên
  `selfhost/corpus/parser/decl/` (15 file) so với `ivy --ast` (khai báo hàm:
  định nghĩa `{}`, prototype `;`, `extern "C"` (đơn và block), `constexpr`,
  `fn ... -> T;`, variadic `...`, tham số ẩn danh, `lifetime<...>` (biến thể
  `fn` trailing), kiểu C-style dưới `#pragma ivy cnumber`, trả về con trỏ,
  `unsigned`/`const`; gồm 2 file chẩn đoán C-style type/`char`). Lệnh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/decl -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,parser,ast -Driver astdrive`.
- H3.2 (Expression + precedence): port parser expression + minimal statement
  (compound, expr-stmt, return), dump body AST byte-identical on
  `selfhost/corpus/parser/expr/` (13 file) vs `ivy --ast` (precedence bieu thuc,
  unary/postfix, ternary, call/index/member/assign, sizeof/alignof/static_cast,
  toan tu bit/shift/so sanh). Khong gom khai bao (deferred H3.3). Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/expr -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,parser,ast -Driver astdrive`.
  Ghi chu: mang `const char**` (chan doan) bi loi codegen Ivy khi ghi >=2 phan
  tu, nen `Parser.dmsg_` dung `long*` + `reinterpret_cast<const char*>`.
- H3.3 (Statement / control flow): port parser statement + control flow, dump
  body AST byte-identical on `selfhost/corpus/parser/stmt/` (15 file) vs
  `ivy --ast` (decl + multi-declarator (desugar compound), if/else, if
  constexpr, while, do-while, for (init decl/expr/null, cond, incr), break,
  continue, switch/case/default, nextcase, unsafe, nested block). Con cho
  range-for (can mang/struct). Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/stmt -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,parser,ast -Driver astdrive`.
  Ghi chu: con cua compound/switch dung danh sach moc noi (`AStmt.next`) thay
  cho stack chung, tranh xen ke khi long nhau.

---

## 5. Bảng ánh xạ tiến độ

Mỗi tính năng: `C++ ref` ↔ `selfhost file` ↔ `oracle mode` ↔ `corpus` ↔ `status`.

### H1 — Lexer

| STT | Tính năng | C++ ref | selfhost | corpus | trạng thái |
|-----|-----------|---------|----------|--------|-----------|
| H1.1 | Identifier / keyword | `lexer.cpp` | `parsing/lexer.ivy` | `corpus/lexer/ident` | ✅ byte-identical 10 files |
| H1.2 | Numeric literal (dec/hex/bin/oct/float) | `lexer.cpp` | `parsing/lexer.ivy` | `corpus/lexer/num` | ✅ byte-identical 8 files |
| H1.3 | String / char / raw / prefix | `lexer.cpp` | `parsing/lexer.ivy` | `corpus/lexer/str` | ✅ byte-identical 7 files |
| H1.4 | Operator / punctuation | `lexer.cpp` | `parsing/lexer.ivy` | `corpus/lexer/op` | ✅ byte-identical 5 files (full stream) |
| H1.5 | Comment / whitespace / line | `lexer.cpp` | `parsing/lexer.ivy` | `corpus/lexer/trivia` | ✅ byte-identical 7 files (full stream) |
| H1.6 | Error cases | `lexer.cpp` | `parsing/lexer.ivy` | `corpus/lexer/err` | ✅ byte-identical 9 files (diagnostics only) |

### H2 — Preprocessor (phạm vi 0.5: module-only)

| STT | Tính năng | C++ ref | selfhost | corpus | trạng thái |
|-----|-----------|---------|----------|--------|-----------|
| H2.1 | `#define` object-like | `preprocessor.cpp` | `parsing/preprocessor.ivy` | `corpus/pp/define_obj` | ✅ byte-identical 10 files |
| H2.2 | `#define` function-like + variadic | `preprocessor.cpp` | `parsing/preprocessor.ivy` | `corpus/pp/define_fn` | ✅ byte-identical 12 files |
| H2.3 | Conditional (`#if/#ifdef/#elif/#else`) | `preprocessor.cpp` | `parsing/preprocessor.ivy` | `corpus/pp/cond` | ✅ byte-identical 16 files |
| H2.4 | Macro expansion / paste / stringify | `preprocessor.cpp` | `parsing/preprocessor.ivy` | `corpus/pp/expand` | ✅ byte-identical 16 files |
| H2.5 | `#pragma`, predefined macros | `preprocessor.cpp` | `parsing/preprocessor.ivy` | `corpus/pp/predef` | ✅ byte-identical 20 files |
| H2.6 | `#line`, `#error`, `#warning` | `preprocessor.cpp` | `parsing/preprocessor.ivy` | `corpus/pp/line` | ✅ byte-identical 22 files |
| H2.7 | E4: từ chối `#include` trong `.ivy` | `preprocessor.cpp` | `parsing/preprocessor.ivy` | `corpus/pp/no_include` | ✅ byte-identical 9 files |

### H3 — Parser / AST

| STT | Tính năng | C++ ref | selfhost | corpus | trạng thái |
|-----|-----------|---------|----------|--------|-----------|
| H3.1 | Declaration (var/type/func) | `parser.cpp`, `ast.h` | `parsing/parser.ivy`, `parsing/ast.ivy` | `corpus/parser/decl` | ✅ byte-identical 15 files |
| H3.2 | Expression + precedence | `parser.cpp` | `parsing/parser.ivy` | `corpus/parser/expr` | ✅ byte-identical 13 files |
| H3.3 | Statement / control flow | `parser.cpp` | `parsing/parser.ivy` | `corpus/parser/stmt` | ✅ byte-identical 15 files |
| H3.4 | Struct / enum / class | `parser.cpp` | `parsing/parser.ivy` | `corpus/parser/aggregate` | ☐ |
| H3.5 | Template / concept | `parser.cpp` | `parsing/parser.ivy` | `corpus/parser/template` | ☐ |
| H3.6 | Module / `import` / `import cpp` | `parser.cpp` | `parsing/parser.ivy` | `corpus/parser/module` | ☐ |
| H3.7 | Error cases (E2/E3/E4) | `parser.cpp` | `parsing/parser.ivy` | `corpus/parser/err` | ☐ |

### H4 — HIR

| STT | Tính năng | C++ ref | selfhost | corpus | trạng thái |
|-----|-----------|---------|----------|--------|-----------|
| H4.1 | Name resolution | `hir_builder.cpp`, `hir.h` | `hir/hir_builder.ivy`, `hir/hir.ivy` | `corpus/hir/name` | ☐ |
| H4.2 | Type checking | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/type` | ☐ |
| H4.3 | constexpr folding | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/constexpr` | ☐ |
| H4.4 | Template instantiation | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/template` | ☐ |
| H4.5 | Borrow / lifetime check | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/borrow` | ☐ |
| H4.6 | Move / use-after-move | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/move` | ☐ |

### H5 — MIR + Interpreter

| STT | Tính năng | C++ ref | selfhost | corpus | trạng thái |
|-----|-----------|---------|----------|--------|-----------|
| H5.1 | CFG lowering | `mir_builder.cpp`, `mir.h` | `mir/mir_builder.ivy`, `mir/mir.ivy` | `corpus/mir/cfg` | ☐ |
| H5.2 | Lifetime annotation | `mir_builder.cpp` | `mir/mir_builder.ivy` | `corpus/mir/lifetime` | ☐ |
| H5.3 | Interpreter exec | `interpreter.cpp` | `mir/interpreter.ivy` | `corpus/interp` | ☐ |
| H5.4 | `--run` outputs | `interpreter.cpp` | `mir/interpreter.ivy` | suite `--run` | ☐ |

### H6 — Codegen

| STT | Tính năng | C++ ref | selfhost | corpus | trạng thái |
|-----|-----------|---------|----------|--------|-----------|
| H6.1 | LLVM C API binding (`import c`) | — | `codegen/codegen.ivy` | build smoke | ☐ |
| H6.2 | IR emitter | `codegen.cpp` | `codegen/codegen.ivy` | suite IR diff | ☐ |
| H6.3 | Object emission | `codegen.cpp` | `codegen/codegen.ivy` | link smoke | ☐ |
| H6.4 | Linker orchestration | `codegen.cpp` | `codegen/codegen.ivy` | e2e run | ☐ |

### H7 — Driver

| STT | Tính năng | C++ ref | selfhost | corpus | trạng thái |
|-----|-----------|---------|----------|--------|-----------|
| H7.1 | Arg parsing | `app/main.cpp` | `app/main.ivy` | CLI smoke | ☐ |
| H7.2 | `import` resolve + search path | `app/main.cpp` | `app/main.ivy` | module suite | ☐ |
| H7.3 | `.ivm` read/write | `module_io.cpp` | `parsing/module_io.ivy` | module I/O | ☐ |
| H7.4 | Diagnostics | `diagnostic.h` | `common/diagnostic.ivy` | err suite | ☐ |

### H8/H9 — Bootstrap & Std

| STT | Tính năng | trạng thái |
|-----|-----------|-----------|
| H8.1 | `ivy` biên dịch chính nó (F2) | ☐ |
| H8.2 | CI dogfood — output khớp bản C++-built | ☐ |
| H9.1 | Port std/runtime (`libivyrt` + std) sang Ivy | ☐ |

---

## 6. Cổng kiểm chứng (gates)

1. **Per-feature**: oracle diff xanh trên corpus của tính năng.
2. **Per-layer**: `selfhost/difftest.ps1` end-to-end **byte-identical** với
   baseline C++ trên suite đầy đủ.
3. **Final**: H8 — binary do Ivy-built biên dịch chính nó và pass toàn suite.

---

## 7. Điều kiện tiên quyết & H0

- [x] **H0.1** Gỡ hoàn toàn `bootstrap/` (đã xóa, không lưu trữ)
- [ ] **H0.2** Freeze baseline C++ tại tag `0.5-baseline`
- [x] **H0.3** Oracle cho lexer: tái dùng `ivy --lex` + lọc theo token-kind ở harness (chưa cần `extern "C"` shim)
- [x] **H0.4** Dựng `selfhost/difftest.ps1` + `selfhost/corpus/`
- [ ] **H0.5** Test suite baseline xanh

---

## 8. Liên kết

- [OLD_PLAN1.md](file:///d:/project/Ivy/ivyc/OLD_PLAN1.md) — kế hoạch 0.1 (vừa bị thay)
- [OLD_PLAN0.md](file:///d:/project/Ivy/ivyc/OLD_PLAN0.md) — kế hoạch gốc (Giai đoạn 1–10)
- [spec/ivy05.md](file:///d:/project/Ivy/ivyc/spec/ivy05.md) — spec ngôn ngữ 0.5 (chuẩn port)
- [README.md](file:///d:/project/Ivy/ivyc/README.md)

**Kiến trúc pipeline (không đổi)**:

```
Source (.ivy) → Lexer → Preprocessor → Parser → AST → HIR → MIR → LLVM IR → Native
                                                     ↘ IvyInterpret (--run)
```
