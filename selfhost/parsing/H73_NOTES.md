# H7.3 - `.ivm` read/write (`module_io.ivy`, `ivmdrive.ivy`)

## 1. Phạm vi

Port `src/parsing/module_io.cpp` sang `selfhost/parsing/module_io.ivy`:

1. `writeModuleInterface` - serialize translation unit thành file `.ivm`
   (dòng 115-221);
2. `serializeType` / `escape` / `unescape` (dòng 25-111);
3. `readModuleInterface` - đọc `.ivm` thành `TranslationUnit` (dòng 223-476).

Không sửa `src/**`, không rebuild oracle (27,031,552 byte, mtime
`2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`).

Phần parser phải bổ sung để writer có dữ liệu như oracle:

- cờ `isExported` cho function/struct/enum (`parse_export` đánh dấu đúng
  declaration vừa parse, mirror `parser.cpp:1858-1923`): 3 pool song song
  `long* fexp_/sexp_/eexp_` + `mark_*_exported` / `*_exported(i)`;
- `AFunction.is_consteval` (oracle serialize **cả** `isConstexpr` và
  `isConsteval`; `consteval` set cả hai);
- enum không ghi underlying type mặc định `int` trong AST, nên `parse_enum`
  set `underlying.base = "int"` (oracle `parseEnum` làm việc này).

Deliverables:

- `selfhost/parsing/module_io.ivy` - records, `IvmBuf`, writer, reader, dump;
- `selfhost/parsing/ivmdrive.ivy` - driver `--write` / `--dump`;
- `selfhost/parsing/build_ivm_smoke.ps1` - harness H7.3;
- `selfhost/parsing/parser.ivy`, `selfhost/parsing/ast.ivy` - export flags +
  `is_consteval` + enum `int`;
- `selfhost/parsing/H73_NOTES.md` (file này);
- `PLAN.md` dòng H7.3.

## 2. Định dạng `.ivm`

Header: `IVYMOD` + byte `1` + `\n`, sau đó `module <name>\n`, rồi mỗi
declaration một dòng (chỉ LF, mở file binary):

```text
func <name> <ret> <nparams> <type:name>... <externC> <constexpr> <consteval> <operator> [sym]
struct <name> <nfields> <type:name>... <nbases> <type>... <isClass> \
       <ntpl> [<name> <isTypename> <isVariadic>]... \
       <nmethods> [<name> <ret> <nparams> <type:name>... <ctor> <dtor> <operator> [sym]]...
enum <name> <scoped> <underlying> <n> <name>=<value>...
using <name> <target>
concept <name> <param> <nreq> <requirement>...
```

Type: `base[:unsigned][:const][:ref][:ptr<depth>][:arr<size>][:tpl{<type>}...]`
(`serializeType`). `escape` chỉ đổi newline -> hai ký tự `\n`, CR -> `\r`,
space -> `\s`; **không** escape `\` (mirror oracle, kể cả khi điều đó làm
`unescape` không đối xứng với `\s`).

`IvmBuf` là buffer byte tăng trưởng (`add/ch/num/esc/type/param/func/method/
struct_decl/enum_decl/using_decl`), dùng **một chỗ** cho cả `write` và `dump`
nên hai đường không thể lệch nhau.

## 3. Writer (`ModuleUnit.from_parser` + `write`)

`from_parser(pa)` gom đúng các declaration được export, theo thứ tự oracle:
functions (exported, kể cả `extern "C"`), structs, enums, usings, concepts.
Oracle ghi **mọi** `usingDecls` và `concepts` (không có cờ export), nên port
cũng ghi tất cả.

- Function: `name`, `ret`, params, `is_extern_c`, `is_constexpr`,
  `is_consteval`, `is_operator` (luôn 0 - xem §8), `op_symbol`.
- Struct: fields, bases, `is_class`, template params (`name`, `is_typename`,
  `is_variadic`), methods. Method flags (`isCtor`/`isDtor`/`isOperator`) suy
  từ **tên đã qualified** (`Struct::name`, `Struct::~name`,
  `Struct::operator<sym>`) vì selfhost AST không giữ các cờ này.
- Enum: `is_scoped`, underlying type, enumerators (`name`, value). Value chỉ
  là `-1` khi initializer không phải integer literal (giống oracle).
- Concept: `name`, `param_name`, danh sách requirement text (đã unescape;
  writer escape lại khi ghi).

Pool của `ModuleUnit` cấp phát một lần theo số declaration (params/fields/
bases/tpls/methods/enumerators/requirements/types) rồi `release()` giải
phóng. Record size ghi bằng **hằng số literal** vì `sizeof` của oracle fold
sai khi field là struct:

| record | bytes | | record | bytes |
| --- | --- | --- | --- | --- |
| `ModParam` | 56 | | `AField` | 72 |
| `ModFunc` | 112 | | `AType` | 48 |
| `ModMethod` | 104 | | `ATemplateParam` | 96 |
| `ModStruct` | 80 | | `AEnumerator` | 32 |
| `ModEnum` | 80 | | `ModConcept` | 32 |
| `ModUsing` | 56 | | `AFunction` | 248 |

## 4. Reader (`read_module`)

Vì Ivy không có `std::getline`/`std::istringstream`, reader tự cắt dòng trên
buffer `io::read_file`:

- magic phải đúng `IVYMOD\x01`; file rỗng hoặc magic sai -> in
  `ivy: error: invalid module interface file '<path>' (bad magic)`, `ok = 0`;
- **không có dòng thứ hai** (file chỉ có magic) -> fail **im lặng** như
  `std::getline` thất bại trong oracle (không in gì, `ok = 0`); đây là khác
  biệt đã sửa so với vòng lặp `pos <= n` ban đầu;
- dòng 2: `module <name>` (nếu không khớp thì tên module rỗng, và dòng đó bị
  "nuốt" y như oracle);
- các dòng sau: tokenize tại chỗ (`ModLine.tok` thay separator bằng NUL), bỏ
  dòng rỗng, dispatch theo từ đầu (`func`/`struct`/`enum`/`using`/`concept`).

Từng reader:

- `read_param`: `type:name`, tên lấy sau **dấu `:` cuối cùng** (`rfind`, để
  chịu được `T:ptr1:ptr`), rồi `deser_type` phần trước.
- `read_func`: đọc params, 4 cờ, symbol nếu `is_operator`; sau đó **qualify**
  tên: `module::name` khi `is_extern_c == 0`, tên chưa có `::`, module name
  khác rỗng (mirror `module_io.cpp:264-300`).
- `read_method`: tên đã qualified trong file nên **không** prepend lần nữa;
  flags suy từ tên qua `method_flags` (`ctor`/`dtor`/`operator<sym>`).
- `read_struct`: fields, bases, `is_class`, tpl params, methods.
- `read_enum`: `is_scoped`, underlying, enumerators. Oracle **bỏ** value khi
  đọc (`Enumerator.value` để trống), nên reader đặt `value = -1`; dump vì
  thế in `name=-1` cho mọi enumerator.
- `read_using`, `read_concept` (requirement text nằm trong pool riêng).
- `deser_type`: mirror `deserializeType`, gồm `tpl{...}` lồng nhau (đệ quy
  với stack `tstack`/`types`), `ptrN`, `arrN`, `unsigned`, `const`, `ref`.
- Chuỗi kết quả (tên, type base, requirement) được copy vào **arena** của
  reader (`arena_dup`/`arena_qual`); arena cạn thì fallback `heap_dup`
  (đường chỉ dùng cho input lạ, chấp nhận leak).

`dump()` in lại đúng các dòng `write()` sinh ra (không có header), nhờ dùng
chung `IvmBuf`; đó là bề mặt kiểm tra reader trong harness.

## 5. Quy ước Ivy đã gặp

- **Struct truyền by value**: hàm nhận `ModParam`/`ModMethod`/`ModLine` mà
  sửa field thì caller không thấy. Vì vậy:
  - `ModuleUnit` giữ dòng hiện tại trong field `ModLine cur` (truyền
    `ModLine*` hoặc gọi method trên con trỏ làm oracle crash 0xC0000005);
  - `method_flags` trả kết quả qua field `method_flag`/`method_op`;
  - mọi "mutate" record đi qua field của `ModuleUnit` hoặc biến local rồi
    `push_*`.
- **Lifetime**: `char*` parameter không thể `return` nếu không có
  `lifetime<$a>` -> `unesc` đổi thành `void` (sửa tại chỗ, 18 call site bỏ
  gán).
- Raw pointer/cast/index chỉ trong `unsafe { }`: toàn bộ thân method của
  `IvmBuf`/`ModuleUnit` bọc `unsafe`; `sizeof` không dùng được cho pool nên
  dùng literal (§3).
- Ivy từ chối `char` nếu thiếu `#pragma ivy cnumber` (fixture dùng pragma).

## 6. Driver (`ivmdrive.ivy`)

`export struct IvmDrive` (helper phải là method vì free function có raw
pointer param cần `unsafe`):

- `--write <src.ivy> <out.ivm>`: `io::read_file` -> `Lexer` -> `Preprocessor`
  -> in diagnostic pp/parser đúng format oracle -> `Parser.parse` -> kiểm tra
  `is_module_export()` -> `ModuleUnit.from_parser` -> `write`;
- `--dump <in.ivm>`: `read_module` -> `dump`;
- exit 0 thành công, 1 lỗi (kèm chẩn đoán), 2 sai cú pháp lệnh;
- argv lấy qua UCRT `__p___argc` / `__p___argv` (giống `main.ivy` của H7.1).

## 7. Harness (`build_ivm_smoke.ps1`)

Build trong TEMP (chỉ clang, không cần LLVM libs): token/lexer/preprocessor/
ast/parser/module_io/ivmdrive bằng oracle (`-c -o <run>/<m>.obj` sinh luôn
`<m>.ivm`) rồi link với `ivyrt.obj`.

8 fixture module: `f_empty`, `f_funcs` (extern "C" decl, variadic,
constexpr, consteval, hàm không export), `f_structs` (base, ctor/dtor/operator,
field con trỏ), `f_enums` (scoped, underlying `uint8`/`uint64`/`int32`, value
âm), `f_tpl` (template struct/function), `f_alias` (using + concept),
`f_mixed`, `f_rich` (tổng hợp). Mỗi fixture 3 case:

1. **write**: `ivmdrive --write` phải **byte-identical** với `.ivm` oracle;
2. **dump**: `ivmdrive --dump` trên `.ivm` oracle phải khớp text kỳ vọng =
   file với 2 chuẩn hoá của reader (qualify func, `=N` -> `=-1`);
3. **consumer**: oracle compile một file `import <module>;` với `.ivm` do
   selfhost ghi (exit 0) - chứng minh file dùng được, không chỉ trùng byte.

Thêm 12 interface thật (`std/*.ivm`, `examples/**/*.ivm`) chạy cùng kiểm tra
dump (bao phủ `tpl{...}`, `ref`, `const`, `ptrN`, operator `==`/`!=`/`+`/
`[]`/`*`, template copy-ctor).

Lỗi: file rỗng / chỉ có magic / sai version byte / garbage được so **raw
bytes** (exit/stdout/stderr) với oracle (oracle fail khi import chính file
đó); file thiếu, output path không ghi được, source không phải module - so
với message trong `module_io.cpp:227`/`write`/driver.

Cuối harness kiểm tra `selfhost/parsing` và repo root không sinh file mới.

## 8. Kết quả (2026-11-15)

```text
build_ivm_smoke.ps1:    PASS 43 / 43 (8 fixture x write/dump/consumer,
                                     12 interface thật, 4 bad-interface,
                                     3 driver error)
build_import_smoke.ps1: PASS 32 / 32
build_cli_smoke.ps1:    PASS 67 / 67
build_object_smoke.ps1: object PASS 246 / 246, run PASS 8 / 8
build_link_smoke.ps1:   run PASS 8 / 8, negative PASS 2 / 2, space PASS 1 / 1
build_smoke.ps1:        PASS (h61 smoke ok)
```

## 9. Hạn chế (H7.4+)

- **Free `operator` function**: parser selfhost chưa parse được khai báo
  operator ngoài struct, nên writer luôn ghi `is_operator = 0` cho function.
  Oracle có thể ghi 1 (fixture/harness tránh dạng này).
- **Template base**: AST selfhost chỉ giữ bare name cho base, mất
  `:tpl{...}` (oracle giữ). Vì vậy fixture tránh `struct X : Base<T>`.
- **Array field**: oracle từ chối `int32 data[4];` trong struct body, nên
  `:arrN` chỉ xuất hiện qua type của param/return; harness không có case
  riêng cho `arrN`.
- **`sizeof` fold**: oracle tính `sizeof(struct)` = 8 khi struct là field,
  nên pool size trong port dùng literal (§3); nếu record đổi layout phải
  cập nhật bảng đó.
- **Method call qua con trỏ** (`p->m()`) làm oracle crash; `ModuleUnit` dùng
  field `cur` thay vì `ModLine*` cho đường đọc.
- **Lexer diagnostics**: vẫn in ra stdout/không quote path (gap H7.4), nên
  harness chỉ dùng fixture hợp lệ về lexer.
- **Reader arena**: input khổng lồ có thể rơi vào `heap_dup` (leak có chủ
  đích); `.ivm` thật nhỏ hơn nhiều so với ngưỡng.

## 10. Trạng thái

- `selfhost/parsing/{module_io.ivy,ivmdrive.ivy,build_ivm_smoke.ps1,
  H73_NOTES.md}` mới; `parser.ivy`, `ast.ivy` cập nhật; `PLAN.md` H7.3 -> ✅.
- Working tree chỉ có `selfhost/parsing/` + `PLAN.md`; commit/push chờ yêu
  cầu (gợi ý: `H7.3: .ivm module interface read/write in selfhost`).
