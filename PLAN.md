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
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/decl -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,ast,parser -Driver astdrive`.
- H3.2 (Expression + precedence): port parser expression + minimal statement
  (compound, expr-stmt, return), dump body AST byte-identical on
  `selfhost/corpus/parser/expr/` (13 file) vs `ivy --ast` (precedence bieu thuc,
  unary/postfix, ternary, call/index/member/assign, sizeof/alignof/static_cast,
  toan tu bit/shift/so sanh). Khong gom khai bao (deferred H3.3). Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/expr -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,ast,parser -Driver astdrive`.
  Ghi chu: mang `const char**` (chan doan) bi loi codegen Ivy khi ghi >=2 phan
  tu, nen `Parser.dmsg_` dung `long*` + `reinterpret_cast<const char*>`.
- H3.3 (Statement / control flow): port parser statement + control flow, dump
  body AST byte-identical on `selfhost/corpus/parser/stmt/` (15 file) vs
  `ivy --ast` (decl + multi-declarator (desugar compound), if/else, if
  constexpr, while, do-while, for (init decl/expr/null, cond, incr), break,
  continue, switch/case/default, nextcase, unsafe, nested block). Con cho
  range-for (can mang/struct). Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/stmt -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,ast,parser -Driver astdrive`.
  Ghi chu: con cua compound/switch dung danh sach moc noi (`AStmt.next`) thay
  cho stack chung, tranh xen ke khi long nhau.
- H3.4 (Struct / enum / class): tieu thu/khai bao aggregate + ten kieu nguoi
  dung, dump AST byte-identical on `selfhost/corpus/parser/aggregate/` (14 file)
  vs `ivy --ast` (struct/class field + method + ctor/dtor + operator + virtual/
  override + access specifier, base list, enum + enum class + underlying type +
  enumerator value, `using` alias; kieu nguoi dung trong tham so/tra ve/field,
  member access `p.x`, member call). `--ast` KHONG dump aggregate o top-level,
  nen byte-identical = tieu thu dung + nhan ten kieu de ham con dump giong.
  Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/aggregate -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,ast,parser -Driver astdrive`.
  Ghi chu: them registry ten kieu (`tnames_` luu `long` dia chi intern) +
  `token_is_type_name`/`register_type_name`; `is_type_start`/`parse_type` nhan
  identifier da dang ky. Chua ho tro namespace/template/global var.
- H3.5 (Template / concept): port parser `template<...>` (function/struct/class,
  `constexpr`/`consteval`, trailing `fn ... -> T`, variadic `...`, non-type
  `int32 N`, constrained `Concept T`) va `concept Name = requires(...) { ... };`.
  Ten tham so template luu registry tam thoi rieng (`tpnames_`), tra trong
  `token_is_type_name` nen `T` duoc nhan la kieu; khong dump template param hay
  concept (giong oracle, `--ast` chi liet ke `tu.functions`). Dump body AST
   byte-identical on `selfhost/corpus/parser/template/` (16 file) vs `ivy --ast`.
  Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/template -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,ast,parser -Driver astdrive`.
  Ghi chu: template struct/class tieu thu het + dang ky ten kieu (giu trong
  `tnames_`), con `tpnames_` duoc luu/tra ve quanh moi template.
- H3.6 (Module / `import` / `import cpp`): port parser `module NAME;`,
  `export module NAME;`, `export <decl>` (struct/class/enum/using/template/
  constexpr/function, `fn ... -> T`), va `import` (`cpp <header>`/`cpp "h"`,
  `c <header>`/`c "h"`, `"name"`, bare `name`). Ten module luu `module_name_` +
  `is_module_export_`. `--ast` khong dump module/import nen byte-identical =
  tieu thu dung + dump ham nhu thuong. Oracle ghi `<Name>.ivm` va in
  `ivy: module interface written to '<dir>\<Name>.ivm'` cho `export module`,
  nen `astdrive` in lai dung dong nay (dung separator `\`); `difftest.ps1` them
  `-I selfhost\parsing` cho oracle de bare `import` resolve giong ban port
  build. Corpus `selfhost/corpus/parser/module/` (16 file) byte-identical vs
  `ivy --ast`. Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/module -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,ast,parser -Driver astdrive`.
  Ghi chu: `--ast` chi liet ke `tu.functions`; `.ivm` do oracle sinh trong
  corpus dir bi gitignore (`*.ivm`). Chua ho tro module name co dau `.`
  (oracle cung vay) va `export extern "C"`/`export { }`/`export import`.
- H3.7 (Error cases E2/E3/E4): port chan doan parser:
  E2 `[[ivy::unsafe]]` trong statement in `[[ivy::unsafe]] is removed - use
  'unsafe { }' instead` tai `[[` dau, roi `unknown Ivy attribute 'ivy::unsafe'
  in this position` cho tung attribute; validate attribute o vi tri function
  (`lt_def`/`lt_ret`) va parameter (`lt`). E3 `_t` keyword in
  `_t-suffixed type '<lexeme>' is removed - use '<suggestion>' instead`
  (`ptrdiff_t` -> `iptr`, con lai bo `_t`), chi voi token kind keyword; khong
  them `expected a type`, khong ap dung cho identifier nhu `int128_t`/`uptr_t`.
  E4 `#include` trong `.ivy` da do preprocessor xu ly (H2.7); parser giu
  `unsupported preprocessor directive` cho `#` con lai. `AAttr` them `line`/
  `col`; thong diep dong dung trong `sbuf_` voi `sb_end()` (NUL + tien con
  tro) de khong ghi de thong diep truoc. Corpus `selfhost/corpus/parser/err/`
  (19 file) byte-identical vs `ivy --ast` (ke ca chan doan PP E4). Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/parser/err -Kinds all -Oracle --ast -Modules token,lexer,preprocessor,ast,parser -Driver astdrive`.
  Ghi chu: thu tu build phai la `ast` truoc `parser` (parser `import ast;`);
  cac lenh H3.x da cap nhat theo thu tu nay.
- H4.1 (Name resolution): port buoc phan giai ten cua `hir_builder.cpp` sang
  `hir/hir.ivy`, `hir/hir_builder.ivy`, `hir/hirdrive.ivy`: khai bao scope
  (function/compound/for), shadowing, lookup local/param, enum constant
  (unscoped inject global, `Enum::X`, enum class, explicit values), struct
  field `.`/`->`, method `Struct::method` voi `this`, free/method call,
  using alias, va cac chan doan ten (`undeclared identifier`,
  `redefinition of variable`, `struct ... has no member/method`,
  `enum ... has no enumerator`, `'this' is only valid...`,
  `... is not a struct type`, `'->' requires...`, `call to undeclared`).
  Parser bo sung metadata `AStruct`/`AEnum`/`AAlias`/`line`/`col` va sua
  child-stack call args de nested call khong lan vao run cua call ngoai.
  Corpus `selfhost/corpus/hir/name/` (27 file) byte-identical vs `ivy --hir`.
  Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/hir/name -Kinds all -Oracle --hir -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive`.
  Ghi chu: driver in chan doan HIR theo format oracle va chi dump khi khong
  co chan doan; corpus tranh float literal, sizeof/alignof, switch, ctor/dtor/
  operator (chua thuoc H4.1). `difftest.ps1` them `-ModuleDirs`/`-DriverDir`
  (mac dinh giu nguyen `selfhost/parsing`).
- H4.2 (Type checking): port phep kiem tra kieu cua `hir_builder.cpp` sang
  `hir/hir_builder.ivy` (va `hir/hir.ivy`): he thong kieu (bang kieu primitive
  + `const`/`unsigned`/`&`/con tro, `cnumber`, struct/class, enum/enum class,
  using-alias, `void`), kieu cua literal (int/float/char/bool/string/nullptr/
  nullopt), integer promotion + usual arithmetic conversion (`promoteTypes`),
  phep toan nhan (`+ - * / %`, `& | ^`, `<< >>`, so sanh, `&& ||`, unary
  `+ - ! ~ * & ++ --`), gan/initialization (`isAssignable`), chuyen doi ngam
  (int<->float, enum->int, alias->underlying, derived->base qua reference),
  kiem tra tham so va gia tri tra ve cua ham (`checkCall`), chon overload
  (`resolveOverload`), dieu kien dieu khien (`checkCondition`), `return` va
  "may reach the end without returning a value", con tro/mang (`&`, `*`,
  pointer arithmetic, so sanh con tro, `nullptr`), va tap khop chan doan
  `file:line:col: error: <message>` theo thu tu build (file co chan doan thi
  khong dump HIR). Bo sung `AType.array_size` + `AStruct.bases` (ke thua),
  `HExpr.line/col`, va float literal trong parser (render `%g` 6 chu so
  nhu `ostream << double`). Corpus `selfhost/corpus/hir/type/` (120 file)
  byte-identical vs `ivy --hir`. Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/hir/type -Kinds all -Oracle --hir -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive`.
  Ghi chu: oracle crash khi toan nhan tren struct (dang ky dung `lt` sau
  `std::move`) nen corpus tranh case do; `sizeof`/`alignof` (fold hang so),
  `if constexpr`, template/lambda/new/delete, aggregate init `{...}` va
  constructor injection de cho H4.3-H4.4 (hoac cac milestone sau; `sizeof`/
  `alignof` xong o H4.3, template xong o H4.4).
- H4.2 (giai chinh va xac minh lap lai): kiem tra lai bang fixture rieng
  (ngoai corpus) cho mo ra cac lech doi oracle, va sua port:
  (1) `isAssignable` thieu khoi B5/B6 (`expected<T,E>` / `optional<T>`) —
  bo sung `is_expected_struct`/`is_optional_struct` + wrap thanh `init-list`
  (node HIR moi, kind 19) cho ca khai bao va gan;
  (2) thieu kiem tra khai bao trong `buildEnum`/`buildStruct`:
  `redefinition of struct|enum|enumerator`, `enumerator ... must be a
  constant expression`, `enumerator ... references undefined constant`;
  (3) parser bo sung default argument (`= expr`) cho tham so, `checkCall`
  dien gia tri mac dinh tai call site va `rank_overload` chap nhan ung vien
  co default;
  (4) parser giu ten toan tu (`operator+` thay vi `operator`) va HIR builder
  viet lai `base op rhs` thanh goi `Struct::operator<op>` (unary/binary/index,
  ke ca ke thua); method/operator cua base class cung duoc tim thay;
  (5) parser tu choi kieu `signed` don le (oracle: "expected a type"),
  dung nhu `unsigned`/`const` don le.
   Corpus bo sung 9 file (120 file tong). Con lai: aggregate init, goi
   constructor/destructor (constructor injection; phan *khai bao* cua
   constructor da port), `new`/`delete`, lambda, range-for, move-tracking,
   namespace van chua port (de milestone sau; `sizeof`/`alignof` xong o H4.3,
   template xong o H4.4, ha muc `__ret` + phat sinh destructor/RAII + NRVO +
   struct construction xong o milestone RAII/DTOR, kiem tra *borrow* (MIR)
   xong o H5.1/H5.2).
- H4.3 (Constexpr folding): port phan fold hang so cua `hir_builder.cpp`
  sang `hir/hir_builder.ivy` (va `hir/hir.ivy`):
  (1) `sizeof`/`alignof` fold thanh integer literal (`typeSize`/
  `typeAlign`): bang kieu kich thuoc cua codegen (pointer/ref = 8 byte,
  `bool`/`char`/`int8_t`/`uint8_t` = 1, `short`/`int16_t`/`uint16_t`/
  `float16_t`/`bfloat16_t` = 2, `int`/`uint`/`float` = 4, `size_t`/
  `ptrdiff_t`/`uptr_t` = 8, `long`/`double` = 8, kieu 128 bit = 16,
  fallback 4) + layout struct (vptr 8 byte neu polymorphic, cac base
  subobject, cac field; moi thanh duoc can vao alignment cua no, size lam
  tron len alignment cua struct) + mang (`T[N]` = sizeof(T) * N) + alias;
  (2) goi ham `constexpr` fold thanh literal (`tryEvalConstexprCall`): chi
  fold khi co argument (khong co argument thi khong fold), argument phai la
  literal (int/float/bool), body chi dung duoc literal + unary/binary/
  ternary, tham so duoc the bang gia tri cua argument, ket qua la integer
  literal hoac float literal (in bang `%g` 6 chu so nhu `ostream << double`);
  (3) `if constexpr`: dieu kien duoc fold truoc, chi build nhanh duoc chon
  (nhanh bi bo qua khong duoc resolve hay kiem tra kieu);
  (4) gia tri chinh xac cua float literal (`strtod`, lam tron dung nhu
  `std::stod`) de fold dung, va `switch` statement (dieu kien phai la kieu
  integral, toi da mot `default`, moi case phai ket thuc bang `break`/
  `return`/`continue`/`nextcase`).
  Corpus `selfhost/corpus/hir/constexpr/` (70 file) byte-identical vs
  `ivy --hir`. Lenh:
  `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/hir/constexpr -Kinds all -Oracle --hir -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive`.
- H4.3 (giai chinh va xac minh lap lai): xac minh lai toan bo 23 suite
  (H1.1-H4.3, 475 file) va them fixture rieng (ngoai corpus) de kiem tra
  oracle parity byte-for-byte (sizeof/alignof moi kieu, constexpr call,
  `if constexpr`, switch, loi):
  (1) fold `-0.0` ra `-0`: codegen ha unary `-` tren float thanh `0.0 - x`
  nen dau cua zero bi mat — dung `x * -1.0` (phep IEEE) va `fmt_double` kiem
  tra dau cua zero bang `1.0 / v`;
  (2) pool van ban float bi cat im lang khi tran (trai ve chuoi rong, sai
  gia tri folded) — doi thanh pool chia chunk (chunk khong bao gio di chuyen
  nen con tro cua literal da fold van hieu luc);
  (3) `switch` chua duoc build (HIR in ra `null`) — port them
  `build_switch` + 3 chan doan cua oracle (dieu kien integral, `default`
  trung, khong cho fallthrough);
  (4) buffer float literal cua parser bi cat ngam (gia tri sai khi literal
  dai > 254 ky tu) — buffer co kich thuoc theo do dai source;
  (5) dem layout cua struct doc bo nho chua khoi tao (`__ivy_alloc` khong
  zero) — khoi tao bang 0; truong `HExpr.fval` cua node HIR khac kind 2
  cung chua duoc khoi tao — khoi tao bang 0.0;
  (6) gia tri hang cua oracle la mot `ConstValue` duoc dung lai: khi ket
  qua la mot so thuc thi truong integer giu nguyen gia tri cu (khong duoc
  reset), va `!`, `~`, dieu kien cua toan tu dieu kien va toan hang `&&`/
  `||` doc truong do — port theo dung quy tac do (truoc do port reset
  truong integer, nen `!x` tren float cho ket qua khac oracle);
  (7) corpus: 5 file khong test duoc dung quy tac (khong parse duoc: `int*`,
  `double`, `char` thieu `#pragma ivy cnumber`; ten ham `not` la keyword;
  `1 + 2` la mot enumerator value hop le nen khong co loi) — sua lai
  (doi ten 3 file) va them 8 file: regression cho `-0`, gia tri hang cua
  float, `switch` (2 file), pool float va 3 loi cua `switch`.
  Ghi chu: oracle crash (access violation) khi body cua ham `constexpr` co
  goi toi ham `constexpr` khac nam trong bieu thuc nhi phan (vd
  `return sq(x) * x;`) — `substituteParams` tra null roi `evalConstExpr`
  dereference null; corpus tranh case do.
   Con lai (de milestone sau): aggregate init `{...}`, goi
   constructor/destructor (constructor injection; phan *khai bao* cua
   constructor da port), `new`/`delete`, lambda, range-for, structured
   binding, move-tracking, namespace (template xong o H4.4; ha muc `__ret` +
   phat sinh destructor/RAII + NRVO + struct construction xong o milestone
   RAII/DTOR, kiem tra *borrow* (MIR) xong o H5.1/H5.2).
- H4.4 (Template instantiation): port phan khoi tao mau (instantiate) cua
  `hir_builder.cpp` sang `hir/hir_builder.ivy` (va `hir/hir.ivy` khong doi
  gi - ten mau chi la mot `const char*`):
  (1) function template: registry ten -> AST function; khoi tao khi co
  template-id call (`f<T>(x)`) hoac khi suy luan ngam (`f(x)`, chi khi
  khong co overload thuong cung ten). Chi *chu ky* (kieu tra ve + kieu tham
  so) duoc thay the; than giu trong co mang. Ten mau = `ten<arg0,arg1>`
  (arg = base + " unsigned"? + " const"? + "*"*ptrDepth, phan tach bang dau
  phay KHONG khoang trang); pack rong -> `ten<>`. Thu tu dump: tat ca ham
  thuong truoc, cac ban khoi tao theo thu tu khoi tao dau tien, mot ban
  khoi tao chi xuat hien mot lan (dedup theo ten mau);
  (2) suy luan (deduce): chi truong hop "kieu tham so DUNG la tham so kieu";
  bien -> `int32_t`, literal so nguyen -> `int` (khong phai `int32_t`),
  literal char -> `char`, literal float -> `double`, bool -> `bool`, con tro
  -> kieu pointee, mang -> kieu phan tu, cau truc -> ten cau truc;
  reference bi bo qua, `const T` bo const cua tham so; cung mot tham so
  kieu suy luan hai lan voi hai kieu khac nhau -> suy luan that bai; tham
  so kieu khong phai (non-type) khong bao gio duoc suy luan;
  (3) variadic pack: `Args... args` no ra mot tham so cho moi phan tu
  (`__args0`, `__args1`), tham so co dinh truoc pack giu ten;
  (4) struct/class template: ten bien thanh ten mau, thay the kieu field /
  kieu tham so / kieu tra ve, method duoc dat ten `<Mau>::<method>` va
  `this` co kieu `<Mau>&`, constructor giu ten goc
  (`Holder<int32_t>::Holder`); khoi tao theo kieu template-id (kieu field,
  kieu tham so, kieu tra ve, alias, kieu trong method);
  (5) concept: kiem tra LUC KHOI TAO, chi khi tham so la tham so kieu co
  rang buoc; yeu cau co toan tu nhi phan -> kieu phai la so hoc (ke `bool`),
  con cau truc phai co `operator<op>` kiem duoc (oracle khong co free
  `operator+` nen moi kieu cau truc deu khong thoa man); concept la khong
  biet duoc coi la thoa man; yeu cau khong co toan tu (member, sizeof) khong
  duoc kiem tra.
   Corpus `selfhost/corpus/hir/template/` (99 file: 16 file type-parameter +
   suy luan, 5 file explicit argument, 5 file variadic pack, 14 file overload/
   recursion/prototype/constexpr/thu tu, 31 file class template, 9 file
   concept, 19 file error) byte-identical vs `ivy --hir`. Lenh:
   `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/hir/template -Kinds all -Oracle --hir -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive`.
  Loi (bug) cua oracle duoc tai hien nguyen ban, KHONG sua: kieu `T` cua
  bien cuc bo trong function template KHONG duoc thay the (`currentTypeMapping_`
  chi duoc bat khi khoi tao struct template); template goi template khac
  bang template-id thi tham so cung khong duoc thay the; kieu const/con tro
  cua kieu suy luan bi mat khi thay the vao tham so `T`; literal so nguyen
  suy luan ra `int` chu khong phai `int32_t`; tham so template thua duoc
  chap nhan va xuat hien trong ten mau; hai template trung ten -> template
  thu hai thay the template dau (khong bao loi); khoi tao struct template
  LAM MAT danh sach base; `sizeof`/`alignof` cua mot template-id = 4 (gia
  tri fallback); concept la khong biet duoc coi la thoa man; default
  argument cua template bi mat; alias khong duoc resolve trong template
  argument.
   Con lai (de milestone sau): `sizeof...(pack)` va fold expression
   `(args + ...)` (parser chua ho tro), namespace (`ns::id<int32>` - port chua
   co namespace), goi constructor cua mot specialization (`Box<int32>(1)` -
   port chua co constructor resolution/injection; phai goi constructor cua
   mot struct thuong (`W(1)`) cung chua ho tro, nen day la tinh nang
   constructor chung chu phai rieng cho template), ha muc `__ret` + phat
   sinh destructor/RAII + NRVO + struct construction (milestone RAII/DTOR),
   kiem tra *borrow* + suy dien lifetime (MIR) (H5.1/H5.2).
- H4.4 (giai chinh va xac minh lap lai - phase 6/6): xac minh lai toan bo
   24 suite (574 file) + 120 fixture rieng (ngoai corpus) de kiem tra oracle
   parity byte-for-byte, va sua port:
   (1) template-id call KHONG tra cuu registry cua struct template (oracle
   tra `lookupStructTemplate` TRUOC `lookupTemplate`): ten vua la struct
   template vua la function template bi resolve sai, va mot template-id call
   tren struct template bao loi sai ("'X' is not a template"). Port sua:
   tra cuu struct template truoc, khoi tao specialization voi argument
   explicit, va giu call chua resolve (callee = ten goc, type = dummy) -
   dung nhu oracle khi specialization khong co constructor;
   (2) buffer chuoi chua doan (dien bao) bi cat im lang khi tran: port xay
   message trong mot buffer cu dinh roi diagnostic giu con tro vao buffer do,
   nen message bi cat (hoac rong) khi buffer day. Port sua: copy message vao
   mot cap phinh on dinh va tai su dung buffer lam scratch;
   (3) layout cua mot struct specialization khong bao gio tinh polymorphic
   (`layout_spec` duyet method cua specialization, ma specialization khong
   co method) -> vptr khong duoc dem. Port sua: tinh tu method cua template.
   Corpus bo sung 4 file regression (3 file `05_` + 1 file `e_`) -> 99 file.
   Ghi chu: `get_stmt` co dong gan `s.line`/`s.col` cho `HStmt` (struct nay
   khong co field do) nhung do la dead code sau `return` nen khong bien
   dich; khong doi.
- H4.5 (Borrow / lifetime check - phan *annotation* o tang HIR): port phan
   ghi nhan + kiem tra *chu thich* lifetime cua `hir_builder.cpp` sang
   `hir/hir_builder.ivy` (va `hir/hir.ivy`, `hir/hirdrive.ivy`; parser/ast giu
   lifetime trong AST):
   (1) cu phap **native**: `lifetime<$a, $b>` (prefix khai bao danh sach),
   `$a` tren tham so (`const int32& $a x`, `int32* $a p`), `$a` sau kieu
   tra ve (`-> const int32& $a`) va dang leading (`const int32& $a f(...)`);
   `lifetime<>` (danh sach rong) hop le;
   (2) cu phap **legacy**: `[[ivy::lt_def(a, b)]]` (khai bao danh sach),
   `[[ivy::lt_ret(a)]]` (lifetime tra ve), `[[ivy::lt(a)]]` (tham so, phai
   viet sau ten tham so);
   (3) thu tu uu tien - theo **code** oracle (`hir_builder.cpp:217-305`),
   khong theo comment cua no: danh sach khai bao = ten native truoc, ten
   legacy `lt_def` sau; lifetime **tra ve**: legacy `lt_ret` **thang** native
   `$a`; lifetime **tham so**: native `$a` **thang** legacy `lt`; cung mot
   ten o ca hai dang -> `duplicate lifetime` (van phien legacy);
   (4) 10 chan doan HIR-stage (dung chuoi va thu tu, dung ca 3 loai vi
   tri: token kieu tham so / token bat dau khai bao ham / vi tri cua
   attribute):
   `lifetime '$b' is not declared in lifetime<...>`,
   `return lifetime '$b' is not declared in lifetime<...>`,
   `lifetime annotation requires a reference or pointer parameter`,
   `[[ivy::lt(b)]]: lifetime 'b' is not declared via [[ivy::lt_def(...)]]`,
   `[[ivy::lt]] expects exactly one lifetime argument`,
   `[[ivy::lt_ret]] expects exactly one lifetime argument`,
   `[[ivy::lt]] requires a reference or pointer parameter`,
   `duplicate lifetime 'a'`, `duplicate lifetime '$a'`,
   `function 'f' returns a pointer but has no return lifetime - use
   `lifetime<$a>` and annotate the return type with $a` (co em dash U+2014);
   (5) dong dump HIR moi: `  lifetimes: a b` (chi khi danh sach khong rong),
   `  return lifetime: X` (chi khi ham co lifetime tra ve), va hau to
   ` [lt:X]` ngay sau ten tham so (KHONG co khoang trong sau `lt:` - MIR
   dump dung ` [lt: X]`).
   Corpus `selfhost/corpus/hir/borrow/` (45 file: 28 file nhom `01_`-`06_` +
   17 file `e_` chan doan) byte-identical vs `ivy --hir`. Lenh:
   `powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/hir/borrow -Kinds all -Oracle --hir -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive`.
   Ghi chu: corpus phai **MIR-clean** - `ivy --hir` chay ca pipeline, nen mot
   chan doan MIR-stage in ra SAU HIR dump (exit 1) va khong bao gio doi
   chieu duoc voi port chi co HIR; vi du bi loai khoi corpus: `syntax/x27`
   va `p02` (MIR lifetime mismatch), `stage/s1,s4,s5,s6,s7,s11,s15` (borrow
   error), `ret/r01..r22` (`__ret` + RAII/destructor).
   `syntax/x10` (`lifetime<...>` + `template<...>`) **khong con bi loai** nua:
   xem H4.5 (giai chinh) ben duoi - port da reproduce dung 3 chan doan cua
   oracle (bug oracle: `parser.cpp:789` consume '<' hai lan).
   Con lai (de H5 - MIR): borrow checker (`checkBorrow` aliasing/mutability,
   `checkReturn` dangling + khop lifetime, `checkStore` dangling store), suy
   dien lifetime (`mir::Expr::lifetime` Named/Static/Local/Unknown) va MIR
   dump (` [lt: X]`, `block N`, `alloca`/`eval`/`ret`) - xem H5.1/H5.2;
   ha muc `__ret` (danh gia bieu thuc tra ve truoc destructor) + phat sinh
   destructor/RAII + NRVO (`returnedLocals_`) + struct construction (can
   khoi tao mau `Res` co destructor) - canh RAII/DTOR milestone (hoac H5).
   (Khong con gi ve *chu thich* lifetime trong danh sach "chua port" - H4.5
   da port xong phan annotation; phan kiem tra *borrow* thi thuoc MIR.)
- H4.5 (giai chinh va xac minh lap lai - phase 5/5): xac minh lai toan bo
   25 suite (620 file) + 52 fixture rieng (ngoai corpus) de kiem tra oracle
   parity byte-for-byte, va sua port:
   (1) **harness `difftest.ps1` so sanh byte-exact**: ban cu ghi ca hai ben
   bang encoding ASCII, nen moi byte khong ASCII (em dash U+2014 trong chan
   doan lifetime) bi fold thanh '?' o CA HAI ben - lech byte bi giu an.
   Da doi sang: doc raw byte -> tach dong o muc byte -> so sanh mang byte
   (chi chuan hoa CRLF -> LF). Chung minh blind spot: doi em dash thanh en
   dash trong port, harness CU PASS (ASCII), harness MO FAIL (byte-exact).
   (2) **parser: `lifetime<...> template<...>`** - port thieu nhanh XAY
   (oracle `parser.cpp:785-809`); port bao "expected a function declaration
   after lifetime<...>" thay vi 3 chan doan cua oracle. Da them nhanh
   (mirror oracle, ke ca bug consume '<' hai lan + khong register ten
   template param) + regression `corpus/hir/borrow/e_13_lifetime_then_template.ivy`.
   (3) **parser: `parse_template_params`** - oracle `expect(Lt, ...)` consume
   token loi (de parse luon tien), port bao loi roi return (1 chan doan thay
   vi 3). Da sua + regression `corpus/parser/template/16_template_no_lt.ivy`.
   (4) **parser: arena intern** - `sb_cap_` = `src_len + 8192` khong du vi
   H4.5 intern lai text cua `[[ivy::lt_def(...)]]` (de tach ten lifetime)
   -> tran arena, port in chan doan rong/SAI ( lech oracle). Da doi
   `sb_cap_` = `src_len * 4 + 65536` (4 lan: ten bien + text attribute + ten
   lifetime + tieu de bien).
   (5) **pool stride**: da re-derive moi kich thuoc pool cua `parser.ivy` /
   `hir_builder.ivy` / `hirdrive.ivy` tu layout cua struct trong `ast.ivy` /
   `hir.ivy` (x86-64: pointer 8, `long` 8, `bool` 1, align 8) - khong co sai
   lech nao; xac nhan them bang `sizeof` cua chinh compiler Ivy (AType 48,
   AAttr 40, ALifetime 8, ALifetimeList 16, ALegacyLifetime 40,
   ALegacyLifetimeList 16, AParam 128, ATemplateParam 96, AField 72,
   AEnumerator 32, AStruct 96, AFunction 200, AEnum 96, AAlias 56,
   AConcept 48, AExpr 168, AStmt 136, ACase 24, HExpr 144, HStmt 120,
   HCase 24, HParam 88, HFunction 120).
   (6) **MIR-clean**: 45/45 file corpus `hir/borrow` khong co chan doan
   MIR-stage (stdout/stderr tach rieng: 0 file co stderr sau HIR dump).
   Con lai: xem "Con lai (de H5 - MIR)" o H4.5 ben tren.


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
| H3.4 | Struct / enum / class | `parser.cpp` | `parsing/parser.ivy` | `corpus/parser/aggregate` | ✅ byte-identical 14 files |
| H3.5 | Template / concept | `parser.cpp` | `parsing/parser.ivy` | `corpus/parser/template` | ✅ byte-identical 16 files |
| H3.6 | Module / `import` / `import cpp` | `parser.cpp` | `parsing/parser.ivy` | `corpus/parser/module` | ✅ byte-identical 16 files |
| H3.7 | Error cases (E2/E3/E4) | `parser.cpp` | `parsing/parser.ivy`, `parsing/ast.ivy` | `corpus/parser/err` | ✅ byte-identical 19 files |

### H4 — HIR

| STT | Tính năng | C++ ref | selfhost | corpus | trạng thái |
|-----|-----------|---------|----------|--------|-----------|
| H4.1 | Name resolution | `hir_builder.cpp`, `hir.h` | `hir/hir_builder.ivy`, `hir/hir.ivy` | `corpus/hir/name` | ✅ byte-identical 27 files |
| H4.2 | Type checking | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/type` | ✅ byte-identical 120 files |
| H4.3 | constexpr folding | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/constexpr` | ✅ byte-identical 70 files |
| H4.4 | Template instantiation | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/template` | ✅ byte-identical 99 files |
| H4.5 | Borrow / lifetime check | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/borrow` | ✅ byte-identical 45 files |
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
