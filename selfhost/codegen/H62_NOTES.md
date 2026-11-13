# H6.2a - selfhost LLVM IR text emitter (scalar subset)

## 1. Phạm vi

H6.2a là phần đầu tiên của H6.2 (IR emitter). Thay vì dựng IR qua LLVM C API
như H6.1, stage này **ghi thẳng text IR** ra stdout bằng `io::print`, giống hệt
đường `ivy --llvm <file>` của oracle (`src/codegen/codegen.cpp`). Mục tiêu là
byte-identical trên một tập con scalar nhỏ, có driver và harness riêng.

Đã giao:

- `selfhost/codegen/emitter.ivy` - module emitter (khoảng 1000 dòng);
- `selfhost/codegen/cgdrive.ivy` - driver: frontend selfhost + emitter;
- `selfhost/codegen/cgdiff.ps1` - harness build/so sánh byte;
- `selfhost/corpus/codegen/` - 10 fixture;
- `selfhost/codegen/H62_NOTES.md` (tệp này) và cập nhật `PLAN.md` dòng H6.2.

Ngoài phạm vi H6.2a: struct/aggregate, string/global, call (trừ `ivy_main`/
`main`), io runtime, allocator, bounds check, name mangling, template, virtual
dispatch, số thực. Một cấu trúc ngoài tập con sẽ ghi diagnostic và làm driver
thoát khác 0 thay vì phát IR sai.

Không sửa `src/**`, không rebuild oracle đóng băng. Oracle vẫn là
27,031,552 bytes, mtime 2026-10-21 16:41:44.

## 2. Thiết kế

### 2.1 `VRef` - giá trị đã hạ

Oracle biểu diễn mọi giá trị bằng `std::string`; bản port chỉ cần ba dạng thực
tế xuất hiện trong tập con:

| `kind` | ý nghĩa | in ra |
|---:|---|---|
| 0 | hằng số nguyên | `ival` ở dạng thập phân |
| 1 | temporary `%tmp.N` | `%tmp.<idx>` |
| 2 | slot alloca `%<name>.<N>` (lvalue) | `%<name>.<slot>` |

Nhờ vậy `emit_val` không phải cấp phát chuỗi, và thứ tự cấp phát `%tmp.N` được
kiểm soát chính xác.

### 2.2 Sao chép `MUnit` lên heap

Ivy không cho lấy địa chỉ struct (`&mu` bị cấm), nên `Emitter::setup(MUnit u)`
sao chép toàn bộ 25 trường của `MUnit` vào một khối `__ivy_alloc(256)` rồi giữ
con trỏ `MUnit*`. Đây là cùng idiom với `interpdrive.ivy`.

### 2.3 Bảng biến

Bảng biến dùng hai mảng heap `vnames_` / `vnums_` (`void*`), cấp phát 4096 phần
tử mỗi mảng. **Quan trọng:** Ivy có lỗi với `const char**` (index ghi đè sai
phần tử), nên tên được lưu dưới dạng `long*` và đọc lại bằng
`reinterpret_cast<const char*>(names[i])` - đúng idiom `Parser::tnames_` đã
dùng. Nếu dùng `char**`, từ biến thứ hai trở đi sẽ hỏng con trỏ và crash.

## 3. Ground truth P0

Scratch: `C:\Users\ASUS\AppData\Local\Temp\opencode\h62a\p0fresh\`. Mỗi probe
chạy oracle `ivy --llvm` với `TEMP`/`TMP` mới; cả 10 probe đều exit 0.

| probe | nội dung | byte stdout |
|---|---|---:|
| 01_return_const | `return 42` | 202 |
| 02_int_ops | `1 + 2 * 3` | 333 |
| 03_assign | gán + cộng | 362 |
| 04_if | if/else | 471 |
| 05_while | while | 671 |
| 06_for | for (assign trong increment) | 692 |
| 07_ops | `+ - * / % & | ^ << >>` | 907 |
| 08_unary_cmp | `-`, `!`, `==` | 674 |
| 09_cast | i32 -> i64 -> i32 | 496 |
| 10_void | `void main` + `return;` | 196 |

10 probe này được chép nguyên văn thành `selfhost/corpus/codegen/01..10`.

## 4. Điểm cần khớp của oracle

1. **Dấu gạch ngang trong comment entry point là em dash.** Dòng là
   `; C1: C entry point — calls ivy_main()` với U+2014 (UTF-8 `E2 80 94`),
   không phải `-` ASCII. Đây là khác biệt 2 byte từng làm 01 lệch độ dài.
2. **Header** đúng một dòng `; LLVM IR emitted by ivy`, kết thúc bằng `\n`
   (harness so byte nên CRLF do host sinh ra cũng phải giống nhau).
3. **Đánh số alloca:** tham số trước, rồi toàn bộ alloca của mọi block theo
   thứ tự block/instruction (hoist lên entry block).
4. **Thứ tự `%tmp.N`:** toán tử một ngôi `-`, `!`, `~` cấp phát temp *trước*
   khi hạ toán hạng; binary hạ hai toán hạng, cast, rồi mới cấp phát temp kết
   quả. `%tmp` reset về 0 mỗi function.
5. **Assign dùng như biểu thức** (increment của `for`) là `MExpr` kind 14, được
   hạ thành lvalue + rhs + `store`, không cast, và trả về giá trị đã lưu. Nếu
   chỉ xử lý assign ở mức instruction (kind 1) thì `06_for` sẽ báo
   "expression kind is out of scope".
6. **Terminator bảo đảm:** block rỗng hoặc thiếu terminator in `ret void` /
   `unreachable` theo kiểu trả về, giống `CodeGen::lowerBlock`.
7. **Function `main`** đổi tên thành `@ivy_main`, rồi sinh `@main` gọi lại;
   `void main` dùng `define i32 @main()` + `call void @ivy_main()` + `ret i32 0`.

## 5. Cấu trúc được hỗ trợ

- header module và thân function có entry alloca + `br label %bb0`;
- label `bb<i>` và `condbr` / `br` với đánh số block của oracle;
- `ret <const|ident>` (qua cast nếu cần) và `ret void`;
- khai báo/gán biến cục bộ (alloca + store), `store` từ assignment statement;
- số nguyên: `+ - * / % & | ^ << >>`, so sánh `== != < > <= >=`, một ngôi
  `+ - ! ~`;
- cast số nguyên i32 <-> i64 (và các cặp width khác qua `sext`/`zext`/`trunc`);
- `if`/`else`, `while`, `for` (kể cả assign trong increment);
- entry point C.

## 6. Harness

`selfhost/codegen/cgdiff.ps1` build toàn bộ module selfhost + `emitter` +
`cgdrive` trong một thư mục TEMP mới, rồi với từng fixture chạy riêng:

- oracle: `ivy --llvm <fixture>`;
- driver: `cgdrive.exe` với đường dẫn fixture ghi vào stdin.

stdout, stderr và exit status được so **byte thô**; cả hai bên phải exit 0.

Kết quả lần chạy cuối (2026-11-04):

```text
[1/4] building selfhost LLVM IR emitter driver ...
[2/4] collecting corpus ...
[3/4] running 10 fixtures ...
PASS 01_return_const.ivy
PASS 02_int_ops.ivy
PASS 03_assign.ivy
PASS 04_if.ivy
PASS 05_while.ivy
PASS 06_for.ivy
PASS 07_ops.ivy
PASS 08_unary_cmp.ivy
PASS 09_cast.ivy
PASS 10_void.ivy
[4/4] summary
      PASS 10 / 10
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Lệnh chạy:

```powershell
powershell -NoProfile -File selfhost/codegen/cgdiff.ps1 `
    -TempRoot C:\Users\ASUS\AppData\Local\Temp\opencode\h62a\<fresh>
```

## 7. Hạn chế đã biết

1. Chỉ tập con scalar ở mục 5; mọi thứ khác báo diagnostic
   "… is out of scope for H6.2a" và driver exit 1.
2. Chưa hỗ trợ `bool` literal (`true`/`false`, MExpr kind 4); biểu thức so sánh
   và `!` vẫn cho ra i1 nên các fixture hiện tại không cần.
3. Chưa hỗ trợ khai báo hàm (chỉ định nghĩa), hàm `extern "C"` không tên
   `main`, và name mangling - đúng chủ ý của H6.2a.
4. Emitter ghi trực tiếp ra stdout nên khi gặp lỗi giữa chừng có thể đã in một
   phần IR; đây cũng là hành vi của oracle (cả hai đều exit 1). Harness chỉ
   chấm các fixture thuộc tập con nên không gặp trường hợp này.
5. Chưa có object emission / linker (H6.3, H6.4).

## 8. Trạng thái

Tệp thêm/sửa cho H6.2a:

- `selfhost/codegen/emitter.ivy` (mới);
- `selfhost/codegen/cgdrive.ivy` (mới);
- `selfhost/codegen/cgdiff.ps1` (mới);
- `selfhost/corpus/codegen/01..10_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (tệp này);
- `PLAN.md` dòng H6.2.

Không commit/push. Không có `.obj`, `.ivm`, `.exe` trong cây làm việc; mọi
scratch nằm dưới `C:\Users\ASUS\AppData\Local\Temp\opencode\h62a\`.

---

# H6.2b - tham số, call, khai báo và MSVC mangling

## 9. Phạm vi

H6.2b **mở rộng** emitter của H6.2a, không viết lại. Tập con vẫn là scalar
nguyên, nhưng nay bao gồm:

- tham số hàm được materialize vào alloca slot ở entry block;
- gọi hàm người dùng (kể cả đệ quy, hàm `void`, call lồng nhau làm đối số);
- gọi hàm `extern "C"`;
- khai báo hàm (`has_body = 0`), cả `extern "C"` lẫn không `extern "C"`;
- name mangling MSVC cho host mặc định Windows.

Mọi hành vi và định dạng byte của H6.2a được giữ nguyên; 10 fixture cũ vẫn
PASS. Cấu trúc ngoài tập con vẫn ghi diagnostic `... is out of scope for
H6.2b` và làm driver exit 1 thay vì phát IR sai.

Không sửa `src/**`, không rebuild oracle đóng băng (27,031,552 bytes, SHA-256
`FFB376CF...313E`).

## 10. Ground truth P0

Scratch: `C:\Users\ASUS\AppData\Local\Temp\opencode\h62b\probe\`. Mỗi probe
chạy `ivy.exe --llvm <file>` với `TEMP`/`TMP` mới.

| probe | nội dung | kết quả |
|---|---|---|
| p01_call | `add(int32,int32)` + `main` gọi | exit 0, 532 byte |
| p02_externc | `extern "C" int32 abs(int32);` | exit 0, 288 byte |
| p03_void | hàm `void` có biến cục bộ | exit 0, 465 byte |
| p04_rec | fib đệ quy | exit 0, 772 byte |
| p05_params | 3 tham số + 2 local slot | exit 0, 812 byte |
| p06_nested | call lồng trong đối số | exit 0, 809 byte |
| p07b_types | int8/16/32/64, uint8/16/32/64, bool, void | exit 0, 1666 byte |
| p07c_cnumber | char/short/long với `#pragma ivy cnumber` | exit 0, 663 byte |
| p09_decl_non_c | khai báo không `extern "C"` | exit 0, 280 byte |
| p10_void_noargs | `void tick()` không tham số | exit 0, 299 byte |
| p08_proto_def | prototype + definition cùng tên | **exit 1**: `ambiguous call to overloaded function` |

Hai cấu trúc bị oracle từ chối và **không** đưa vào corpus:

1. prototype + definition cùng một hàm (p08) - frontend báo ambiguous;
2. `char`/`short`/`long` không có `#pragma ivy cnumber` (probe p07 đầu tiên) -
   frontend yêu cầu dùng `int8`/`uint8`/...; khi bật pragma thì hợp lệ và mã
   mangling lần lượt là `D`, `F`, `J`.

## 11. Bảng MSVC mangling đã cài

`mangleFunction` của oracle: `main` -> `ivy_main`; `extern "C"` giữ nguyên
tên; còn lại dùng `?<name>@@Y<ret><params>@Z` (không có scope). Tên chứa
`?`/`@` nên luôn được quote theo `llvmGlobalName`.

| kiểu Ivy | mã | nguồn xác nhận |
|---|---|---|
| `int8_t` | `C` | probe p07b |
| `uint8_t` | `E` | probe p07b |
| `int16_t` | `F` | probe p07b |
| `uint16_t` | `G` | probe p07b |
| `int32_t` | `H` | probe p01/p07b |
| `uint32_t` | `I` | probe p07b |
| `int64_t` | `_J` | probe p07b |
| `uint64_t` | `_K` | probe p07b |
| `bool` | `_N` | probe p07b |
| `char` | `D` | probe p07c |
| `short` / `unsigned short` | `F` / `G` | probe p07c + oracle code |
| `int` / `unsigned int` | `H` / `I` | oracle code |
| `long` / `unsigned long` | `J` / `K` | probe p07c + oracle code |
| `long long` / `unsigned long long` | `_J` / `_K` | oracle code |
| `int128_t` / `uint128_t` | `_J` | oracle code |
| `size_t` / `uptr_t` | `_K` | oracle code |
| `ptrdiff_t` | `J` | oracle code |
| `void` (kiểu trả về) | `X` | probe p03/p10 |
| không có tham số | `X` | probe p10 |

Ví dụ: `int32 add(int32,int32)` -> `?add@@YHHH@Z`;
`void hello(int32)` -> `?hello@@YXH@Z`; `void tick()` -> `?tick@@YXX@Z`.

## 12. Quy tắc định dạng

### 12.1 Tham số và số hiệu slot

- Prologue entry block: với từng tham số theo thứ tự khai báo, in
  `%<name>.<slot> = alloca <ty>` rồi `store <ty> %arg<i>, ptr %<name>.<slot>`.
- Bộ đếm `alloca_` reset về 0 mỗi function: tham số chiếm slot trước, sau đó
  toàn bộ local alloca theo thứ tự block/instruction (hoist lên entry).
- Tham số không tên in `%arg.<slot>` (giống `valueName`: tên rỗng -> `arg`).
- Ví dụ `int32 add(int32 a, int32 b)`:
  `%a.0`, `%b.1`, rồi local đầu tiên là `%x.2`.

### 12.2 Call

- Có giá trị trả về: `%tmp.N = call <ret> @<sym>(<args>)`.
- `void`: `call void @<sym>(<args>)`, **không** cấp `%tmp`.
- Đối số hạ trái sang phải; mỗi đối số được cast về kiểu tham số nếu khác
  (`valueLlvmType`, tức bỏ reference). Temp của kết quả call được cấp **sau**
  khi hạ xong toàn bộ đối số.
- Call lồng nhau: call trong đối số tự cấp temp trước, nên thứ tự `%tmp.N`
  khớp oracle (xem `16_nested_call`: `%tmp.0` inc(1), `%tmp.1` inc(2),
  `%tmp.2` add).
- Đối số được lưu trong scratch dạng stack (`arg_vals_`/`arg_tys_`,
  `arg_sp_`) để call lồng không ghi đè đối số mà call cha đã hạ.

### 12.3 Khai báo

- Thứ tự giống `CodeGen::generate()`: (1) khai báo `extern "C"` không body,
  (2) khai báo không `extern "C"` không body, (3) các definition.
- Dạng: `declare <ret> @<sym>(<kiểu tham số, không tên>)`.
- `extern "C"` không mangle: `declare i32 @abs(i32)`.
- Không `extern "C"` thì mangle: `declare i32 @"?ext@@YHH@Z"(i32)`.
- Không có dòng trống giữa các khai báo, cũng không có dòng trống giữa khai
  báo cuối và definition đầu; mỗi definition kết thúc bằng `}` + một dòng
  trống (giữ nguyên H6.2a).

## 13. Corpus bổ sung

| fixture | nội dung |
|---|---|
| `11_two_functions.ivy` | `add(int32,int32)` + `main` gọi `add(2,3)` |
| `12_recursion.ivy` | fib đệ quy, if/else + hai call trong một biểu thức |
| `13_void_call.ivy` | hàm `void` có local, gọi như statement |
| `14_extern_c.ivy` | khai báo `extern "C" int32 abs(int32)` + call |
| `15_params.ivy` | 3 tham số + 2 local slot (`%d.3`, `%e.4`) |
| `16_nested_call.ivy` | call làm đối số, kiểm tra thứ tự `%tmp.N` |

## 14. Kết quả

`cgdiff.ps1` trên toàn bộ 16 fixture (2026-11-05):

```text
[3/4] running 16 fixtures ...
PASS 01_return_const.ivy
...
PASS 16_nested_call.ivy
[4/4] summary
      PASS 16 / 16
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Regression interpreter (`selfhost/interpdiff.ps1 -Dir selfhost/corpus/interp`):

```text
[3/4] running 23 fixtures ...
[4/4] summary
      PASS 23 / 23
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Lệnh chạy:

```powershell
powershell -NoProfile -File selfhost/codegen/cgdiff.ps1 `
    -TempRoot C:\Users\ASUS\AppData\Local\Temp\opencode\h62b\<fresh>
powershell -NoProfile -File selfhost/interpdiff.ps1 `
    -Dir selfhost/corpus/interp `
    -TempRoot C:\Users\ASUS\AppData\Local\Temp\opencode\h62b\<fresh>
```

## 15. Hạn chế đã biết (H6.2c+)

1. **Itanium mangling** chưa port; host mặc định Windows dùng MSVC. Không có
   mô hình `platform_` nên không thể chọn `_Z...`; đây là gap có chủ ý.
2. **String/global, io runtime, allocator, bounds check** chưa port.
3. **Struct/aggregate, template, virtual dispatch, new/delete** chưa port.
4. **Pointer/reference** trong chữ ký hàm, đối số call và mangling đều báo
   diagnostic; pointer chỉ còn trong các cast scalar của H6.2a.
5. **Float** (float/double/floatN_t/bfloat16) bị từ chối ở chữ ký hàm, đối số
   call và mangling.
6. **Variadic** chưa có trong mô hình MIR (`isVariadic` không được port), nên
   không thể phát `...`; call variadic sẽ không khớp tham số và bị xử lý như
   call thường hoặc báo lỗi tùy cấu trúc.
7. **Overload resolution** không port: `find_fn` khớp theo tên; frontend đã
   từ chối prototype + definition nên tập con không có nhập nhằng.
8. **Namespace/method** (`name` chứa `::`) báo diagnostic thay vì mangle
   scope.
9. Chưa có object emission / linker (H6.3, H6.4).

## 16. Trạng thái H6.2b

Tệp thêm/sửa:

- `selfhost/codegen/emitter.ivy` (mở rộng: cơ chế alloca slot đã có từ H6.2a,
  thêm param slot, call/declaration/MSVC mangling + scratch đối số);
- `selfhost/corpus/codegen/11..16_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 9-16 này);
- `PLAN.md` dòng H6.2.

Không commit/push. Không có `.obj`, `.ivm`, `.exe` trong cây làm việc; scratch
nằm dưới `C:\Users\ASUS\AppData\Local\Temp\opencode\h62b\`.

---

# H6.2c - string literal + io::print builtins

## 17. Phạm vi

H6.2c **mở rộng** `selfhost/codegen/emitter.ivy` của H6.2a/H6.2b, không viết
lại. Tập con scalar và call/khai báo/mangling cũ được giữ nguyên; phần mới:

- pre-pass thu toàn bộ `StringLit` của unit trước khi hạ hàm, dedup và đánh số
  `@.str.N` đúng thứ tự `collectExpr`;
- hạ biểu thức string literal thành `getelementptr i8, ptr @.str.K, i64 0`;
- phát các global `private unnamed_addr constant [len+1 x i8]` ở cuối module;
- định tuyến `io::print`, `io::println`, `io::eprint`, `io::eprintln` theo tên
  callee **trước** generic call lookup;
- format string `{}` với các segment literal được intern riêng;
- sáu runtime declare của io, phát sau entry point.

Mọi fixture 01..16 vẫn giữ nguyên byte; 17..22 là corpus mới.

## 18. Ground truth P0

Scratch: `C:\Users\ASUS\AppData\Local\Temp\opencode\h62c\probe\`. Mỗi probe chạy
oracle `build\Release\ivy.exe --llvm <file>` với `TEMP`/`TMP` mới. Tất cả đều
exit 0 và được dùng làm nguồn byte:

| probe | nội dung | byte stdout |
|---|---|---:|
| p01_print_str | `io::print("hi")` | 639 |
| p02_println_mix | `io::println("hi"); io::print(42)` | 800 |
| p03_print_int_var | biến `int32 x = 42` rồi `io::println(x)` | 664 |
| p04_print_format | `io::println("x={} y={}", a, b)` | 1234 |
| p05_eprint | `io::eprint("e"); io::eprintln("f")` | 886 |
| p06_escape | `a\nb\t\"q\"\\end`, `\x41`, `\101` | 1103 |
| p07_same_twice | cùng literal `"hi"` in hai lần | 733 |
| p08_nonfirst | `io::print(x, "tail")` | 779 |
| p09_kinds | `char`, `bool`, `uint32` | 1021 |
| p10_format_edges | `"{}"`, `"pre{}post"`, `"plain"` | 1549 |

Các probe trùng nghĩa (`p03_int_var`, `p04_format`, ...) cũng PASS 17/17 trong
`cgdiff.ps1` trước khi thêm fixture. Không có probe nào bị oracle từ chối.

## 19. Decode và escape

`decode_string` là port của `CodeGen::decodeStringTyped` cho `width == 1`:

- chấp nhận tiền tố `u8`, bỏ nó trước khi decode;
- từ chối `L`, `u`, `U` (wide string) bằng diagnostic, đúng phạm vi H6.2c;
- raw string `R"(...)"` (và `u8R"(...)"`) lấy nội dung giữa `(` đầu và `)` cuối,
  không decode escape;
- string thường bỏ hai dấu `"`, decode body qua `decode_body`.

`decode_body` port nguyên `CodeGen::decodeBody`:

- các escape một ký tự: `\n \t \r \a \b \f \v \\ \' \" \?`;
- `\x` đọc tối đa 2 chữ số hex, phải có ít nhất 1, nếu không báo
  `invalid \x escape in literal`;
- octal đọc tối đa 3 chữ số **sau chữ số đã bị `switch` tiêu thụ**. Vì vậy
  `"\101"` cho `0x01`, `"\1"` cho `0x00` - đây là quirk của oracle, được giữ
  nguyên byte-for-byte;
- escape lạ hoặc chuỗi kết thúc giữa escape báo diagnostic.

`emit_escaped` port `CodeGen::llvmEscape`: byte `0x20..0x7E` trừ `"` và `\` in
thẳng; mọi byte còn lại thành `\XX` với hex **in hoa**. NUL kết thúc do emitter
thêm bằng `\00` và độ dài global là `len + 1`, không nằm trong buffer decode.

## 20. Quirk global trùng lặp

Oracle có hai cách intern vào cùng `stringList_`:

- pre-pass `collectExpr`: key `"1:" + bytes` (có tiền tố width);
- `internCString` cho segment format: key là raw `bytes`.

Vì key khác nhau nên `io::print("hi")` sinh **hai** global:

```text
@.str.0 = private unnamed_addr constant [3 x i8] c"hi\00"   ; pre-pass, không dùng
@.str.1 = private unnamed_addr constant [3 x i8] c"hi\00"   ; segment, được dùng
```

và biểu thức dùng `@.str.1`. Emitter giữ đúng quirk này bằng hai map riêng:
`pm_*` key `"1:"+bytes`, `sm_*` key raw bytes. Dedup xảy ra **trong từng map**:
in cùng literal hai lần vẫn chỉ có một `@.str.0` pre-pass và một `@.str.1`
segment; hai call cùng trỏ `@.str.1` (probe p07).

Hệ quả khác cũng được giữ: nếu string literal không phải đối số đầu tiên của
`io::print`, nó đi qua `lower_string` và dùng thẳng global pre-pass, **không**
sinh thêm segment. `io::print(x, "tail")` chỉ có `@.str.0` = `"tail"` (p08).
Với format, segment rỗng (ví dụ ngay trước `{}`) không được intern; chỉ segment
có ít nhất một byte mới sinh global raw-bytes.

## 21. Hạ io::print và thứ tự %tmp

`lower_call` kiểm tra `is_io_print_name(e.sval)` **trước** `find_fn`, nên các
builtin không cần MIR entry. `fd = 1` cho `print`/`println`, `2` cho
`eprint`/`eprintln`; newline chỉ với `println`/`eprintln`.

Nếu đối số đầu là `StringLit`:

1. decode format string;
2. duyệt byte, gom segment giữa các `{}`;
3. mỗi segment gặp `{}` hoặc cuối chuỗi: intern qua map raw-bytes, cấp
   `%tmp.N` cho `getelementptr`, rồi phát
   `call void @__ivy_write_str(i32 <fd>, ptr %tmp.N)`;
4. mỗi `{}` tiêu thụ đối số kế tiếp và hạ theo kiểu của nó.

Nếu đối số đầu không phải string literal: hạ **từng** đối số theo thứ tự, cùng
quy tắc kiểu.

Quy tắc kiểu và thứ tự temp (khớp `lowerIoPrint`):

- pointer/string: `lowerExpr` (có thể cấp `%tmp` cho gep/load) rồi
  `write_str(fd, v)`; không cấp thêm temp;
- `char`/`int8_t`/`uint8_t`: `lowerExpr` rồi `write_char(fd, v)`;
- `bool`: `lowerExpr` -> `%tmp = call ptr @__ivy_fmt_bool(i1 v)` ->
  `write_str(fd, %tmp)`;
- integer khác `i64`: `lowerExpr` -> `%tmp = sext|zext <ty> v to i64` ->
  `%tmp = call ptr @__ivy_fmt_int(i64 ext)` -> `write_str(fd, p)`;
- integer đã là `i64`: bỏ bước extend; temp `fmt_int` vẫn được cấp;
- float: **không hạ**; ghi diagnostic
  `floating-point arguments to io::print are out of scope for H6.2c`.

Ví dụ `io::println(7)`:

```text
%tmp.0 = sext i32 7 to i64
%tmp.1 = call ptr @__ivy_fmt_int(i64 %tmp.0)
call void @__ivy_write_str(i32 1, ptr %tmp.1)
call void @__ivy_write_newline(i32 1)
```

Format `"x={} y={}"` với hai biến: segment `"x="` trước, rồi arg `a`, segment
`" y="`, arg `b`; tất cả literal segment nằm sau các global pre-pass.

## 22. Runtime declare

Khi pre-pass thấy bất kỳ call `io::print`/`println`/`eprint`/`eprintln` nào,
sau entry point phát **đủ sáu** dòng, đúng thứ tự, kể cả builtin không dùng:

```text
declare void @__ivy_write_str(i32, ptr)
declare void @__ivy_write_char(i32, i8)
declare ptr @__ivy_fmt_int(i64)
declare ptr @__ivy_fmt_float(double)
declare ptr @__ivy_fmt_bool(i1)
declare void @__ivy_write_newline(i32)
```

Sau sáu declare mới đến các `@.str.N`; thứ tự tổng thể của module là: header,
khai báo extern/non-extern, definition, entry point, io runtime, string global.
Không có `__ivy_panic` trong phạm vi này.

## 23. Corpus bổ sung

| fixture | nội dung |
|---|---|
| `17_print_str.ivy` | `io::print("hi")` - quirk hai global |
| `18_println_mix.ivy` | `println` chuỗi + `print` hằng số |
| `19_print_int_var.ivy` | biến `int32` rồi `println` (load, sext, fmt_int) |
| `20_print_format.ivy` | `"x={} y={}"` với hai biến, segment + newline |
| `21_eprint.ivy` | `eprint`/`eprintln`, fd = 2 |
| `22_string_escape.ivy` | escape điều khiển, `\x41`, octal `\101` |

## 24. Kết quả

`cgdiff.ps1 -Dir selfhost/corpus/codegen` (2026-11-06), scratch
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62c\run4\`:

```text
[3/4] running 22 fixtures ...
PASS 01_return_const.ivy
...
PASS 22_string_escape.ivy
[4/4] summary
      PASS 22 / 22
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Regression interpreter
`interpdiff.ps1 -Dir selfhost/corpus/interp` (scratch `...\h62c\run5\`):

```text
[3/4] running 23 fixtures ...
[4/4] summary
      PASS 23 / 23
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 01..16 của H6.2a/H6.2b vẫn PASS byte-for-byte.

## 25. Chưa hỗ trợ / defer

1. `io::read_line`, `io::read_file`, `io::write_file` và runtime D6 - chưa port.
2. Float argument cho `io::print` (`__ivy_fmt_float`): emit diagnostic, không
   phát IR sai. Float trong chữ ký hàm vẫn bị H6.2b từ chối.
3. Wide string `L"..."`, `u"..."`, `U"..."`: từ chối; `u8"..."` xử lý như
   string width 1.
4. Allocator `new`/`delete` (`__ivy_alloc`/`__ivy_free`), bounds check và
   `__ivy_panic`: để H6.2d.
5. Array/struct/aggregate, variadic call, overload resolution, template,
   virtual dispatch, Itanium mangling: ngoài phạm vi, vẫn diagnostic.

## 26. Trạng thái H6.2c

Tệp thêm/sửa:

- `selfhost/codegen/emitter.ivy` (mở rộng: pre-pass string, decode/escape,
  hai map intern, global constants, io builtin lowering, runtime declare);
- `selfhost/corpus/codegen/17..22_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 17-26 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a+b+c ... 22/22` (không đánh dấu ✅).

Không sửa `src/**`, không rebuild oracle đóng băng (27,031,552 bytes, SHA-256
`FFB376CF...313E`). Không commit/push. Không có `.obj`, `.ivm`, `.exe` trong
cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62c\`.

## 27. H6.2d - mảng một chiều + bounds check

### 27.1 Phạm vi

H6.2d mở rộng emitter (không viết lại) với đúng một nhóm cấu trúc:

- kiểu mảng một chiều `T[N]` với `T` là scalar nguyên (`int32 a[3]`);
- alloca mảng hoist lên entry, zeroinit tại điểm khai báo;
- `a[i]` vừa là lvalue vừa là rvalue;
- bounds check + panic message + `declare void @__ivy_panic(ptr, i32)`;
- bỏ bounds check khi instruction nằm trong `unsafe`.

Mọi thứ khác của H6.2a/b/c giữ nguyên byte-for-byte. Không dùng LLVM C API.

Ghi chú về kind MIR: trong repo này `MExpr` kind **12** là `Index` (base ở
`a`, index ở `b`), còn kind **13** là `Member`. Điều này đúng theo bảng kind
trong `selfhost/mir/mir.ivy` và `MirBuilder::ex_index` (`add_expr(12)`). Mô tả
stage ghi "kind 13 = Index" là nhầm; bản port bám theo mã nguồn và oracle.

### 27.2 Kiểu mảng và zeroinit

`llvm_type` trả `[N x T]` cho `array_size > 0`, với `T` là cùng `AType` nhưng
`array_size = 0` (đúng nhánh đầu của `CodeGen::llvmType`). Chỉ scalar nguyên
được chấp nhận; mảng con trỏ, mảng struct/float bị từ chối bằng diagnostic.
Parser selfhost chỉ cho một hậu tố `[N]`, nên mảng nhiều chiều đã bị chặn từ
trước.

Alloca mảng đi qua vòng hoist sẵn có nên slot vẫn là `%a.<slot>` và thứ tự
slot không đổi. Không có initializer, `lower_inst(Alloca)` phát zeroinit **tại
điểm khai báo** (không phải trong prologue):

```llvm
%a.0 = alloca [3 x i32]
...
store [3 x i32] zeroinitializer, ptr %a.0
```

Vì vậy khai báo trong một block `if`/`while` sẽ đặt store zeroinit trong đúng
block đó, giống oracle. Array initializer list bị từ chối
(`array initializers are out of scope for H6.2d`).

### 27.3 Hạ `a[i]`

`lower_index_addr` dùng chung cho lvalue và rvalue, theo đúng thứ tự
`CodeGen::lowerLValue`/`lowerExpr` nhánh `Index`:

1. hạ biểu thức index trước;
2. lấy `idxTy = llvm_type(index.type)` (giữ nguyên, không drop ref);
3. cấp phát temporary GEP (`gep = newTemp()`);
4. base phải là định danh mảng cục bộ; nó decay thành slot **không** cấp phát
   temporary;
5. bounds check (trừ `unsafe`);
6. phát GEP sau label ok-block;
7. rvalue mới cấp phát temporary load và phát `load <elemTy>, ptr <gep>`.

GEP dùng `i32 0` cho chỉ số mảng ngoài và `idxTy` cho chỉ số phần tử:

```llvm
%tmp.0 = getelementptr [3 x i32], ptr %a.0, i32 0, i32 0
```

Index có kiểu `i64` in `i64 <idx>`; index `i32` in `i32 <idx>`.

### 27.4 Thuật toán bounds check

Bản port sao chép `CodeGen::emitBoundsCheck`, kể cả các quirk:

- nếu `MInst.in_unsafe != 0` thì thoát ngay, không cấp phát temp, không thêm
  chuỗi, không bật `declared_panic_`;
- bật cờ panic;
- **luôn** cấp phát `idx64 = newTemp()`, kể cả khi `idxTy == "i64"`; trường hợp
  i64 temp này không bao giờ được phát, để lại lỗ hổng trong dãy `%tmp.N`;
- cấp phát `oob = newTemp()` **trước** khi phát sext (thứ tự oracle);
- nếu `idxTy != "i64"`: `idx64 = sext idxTy idx to i64`; ngược lại so sánh
  trực tiếp `idx`;
- `oob = icmp uge i64 <idxCasted>, <N>`;
- cấp phát hai block `bb.i<K>` (panic) và `bb.i<K+1>` (ok), bộ đếm reset = 0
  mỗi function;
- phát `br i1 <oob>, label %bb.i<K>, label %bb.i<K+1>`;
- block panic: intern message, cấp phát temp con trỏ message, phát
  `getelementptr i8`, `call void @__ivy_panic(ptr <msg>, i32 <line>)`,
  `unreachable`;
- label ok-block;
- GEP đã cấp phát ở bước 3 mới được phát ở đây.

Ví dụ với index hằng `i32` và `N = 3` (đúng ground truth):

```llvm
%tmp.1 = sext i32 0 to i64
%tmp.2 = icmp uge i64 %tmp.1, 3
br i1 %tmp.2, label %bb.i0, label %bb.i1
bb.i0:
%tmp.3 = getelementptr i8, ptr @.str.0, i64 0
call void @__ivy_panic(ptr %tmp.3, i32 3)
unreachable
bb.i1:
%tmp.0 = getelementptr [3 x i32], ptr %a.0, i32 0, i32 0
```

Với index `i64`, `%tmp` dành cho `idx64` bị bỏ trống:

```llvm
%tmp.1 = load i64, ptr %i.1
%tmp.4 = icmp uge i64 %tmp.1, 2
...
%tmp.2 = getelementptr [2 x i64], ptr %a.0, i32 0, i64 %tmp.1
```

### 27.5 Chuỗi panic và declare

Message `index out of bounds (line <line>)` đi vào **map raw-bytes** (cùng map
với segment của `io::print`), nên:

- hai truy cập cùng một dòng nguồn dùng chung một `@.str.N`;
- các dòng khác nhau nhận global kế tiếp, sau toàn bộ string pre-pass và io
  segment (thứ tự first-seen trong lúc hạ hàm);
- `emit_string_constants()` vẫn chạy cuối module;
- nếu đã dùng panic, sau các global string mới phát
  `declare void @__ivy_panic(ptr, i32)` (đúng thứ tự `generate()` của oracle).
  Các declare runtime io vẫn nằm trước string constants.

### 27.6 `unsafe`

`lower_inst` gán `cur_in_unsafe_ = inst.in_unsafe` ở đầu mỗi instruction, đúng
`CodeGen::lowerInst` (`inUnsafe_ = inst.inUnsafe`). Vì vậy `unsafe { a[5] = 1; }`
không phát sext/icmp/br/panic/string, chỉ còn GEP và store:

```llvm
%tmp.0 = getelementptr [3 x i32], ptr %a.0, i32 0, i32 5
store i32 1, ptr %tmp.0
```

Nếu module không có bounds check nào, không có `declare @__ivy_panic`.

### 27.7 Corpus mới

| fixture | nội dung |
|---|---|
| `23_array_rw.ivy` | ghi/đọc hai index hằng, dedup message cùng dòng |
| `24_array_var_index.ivy` | index biến `int32`, GEP `i32 %tmp` |
| `25_array_loop.ivy` | `while` ghi rồi đọc mảng, block `bb.i` tăng dần |
| `26_array_unsafe.ivy` | truy cập ngoài biên trong `unsafe`, không panic |
| `27_array_compound.ivy` | `a[1] += 2`, hai lần hạ `a[1]` |
| `28_array_types.ivy` | `int8[3]` (trunc) + `int64[2]` (không sext index) |

Tất cả được oracle chấp nhận và so byte với `ivy --llvm`.

### 27.8 Kết quả

`cgdiff.ps1 -Dir selfhost/corpus/codegen` (2026-11-06, scratch dưới
`...\opencode\h62d\runcg\`):

```text
[3/4] running 28 fixtures ...
PASS 01_return_const.ivy
...
PASS 28_array_types.ivy
[4/4] summary
      PASS 28 / 28
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Regression interpreter `interpdiff.ps1 -Dir selfhost/corpus/interp`
(scratch `...\h62d\runinterp\`): **PASS 23 / 23**, exit 0. Toàn bộ 01..22 của
H6.2a/b/c vẫn byte-identical.

### 27.9 Chưa hỗ trợ / defer

1. Struct/aggregate, struct init list, `new`/`delete` (parser selfhost chưa
   hỗ trợ).
2. Array initializer list (`int32 a[3] = {...}`) - oracle từ chối primitive
   array init; emitter cũng từ chối.
3. Array parameter/return/decay ngoài `a[i]` - vẫn diagnostic.
4. Mảng nhiều chiều, mảng con trỏ/struct/float.
5. Float nói chung, wide string, variadic, template/virtual, Itanium mangling.

### 27.10 Trạng thái H6.2d

Tệp thêm/sửa:

- `selfhost/codegen/emitter.ivy` (mở rộng: `llvm_type` mảng, zeroinit, Index
  lvalue/rvalue, bounds check, panic string/declare, `in_unsafe`);
- `selfhost/corpus/codegen/23..28_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 27 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a+b+c+d ... 28/28` (không đánh dấu ✅).

Không sửa `src/**`, không rebuild oracle đóng băng (27,031,552 bytes,
mtime 2026-10-21 16:41:44, SHA-256 `FFB376CF...313E`). Không commit/push.
Không có `.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62d\`.

## 28. H6.2e - struct cục bộ, member, copy và nested

### 28.1 Phạm vi

H6.2e chỉ mở đúng phần struct mà frontend selfhost đã dựng được:

- định nghĩa `struct`/`class` không template, không trùng tên;
- biến cục bộ kiểu struct (kể cả trong block lồng nhau);
- gán/đọc member bằng `.`, kể cả member lồng `o.in.a`;
- copy toàn bộ struct (`Point q = p;` và `q = p;`);
- zeroinit struct chưa khởi tạo;
- in định nghĩa `%struct.Name = type { ... }`, kể cả struct không dùng.

Struct parameter/return bị từ chối rõ ràng (`struct parameters/return types
are out of scope for H6.2e`) vì mangling struct chưa port.  Arrow `->` đã có
code lowering nhưng không có fixture: parser selfhost từ chối cast
`(Point*)0` và `(Point*)__ivy_alloc(...)`, nên không tạo được con trỏ struct.

### 28.2 Pool HIR

`selfhost/hir/hir.ivy` thêm:

```ivy
export struct HField { const char* name; AType type; };
export struct HStruct { const char* name; HField* fields; long nfields; long line; long col; };
```

`HUnit` thêm `structs`/`struct_n`/`struct_cap` và `fields`/`fld_n`/`fld_cap`.
`HirBuilder.setup` cấp phát pool theo `struct_count()` + tổng `nfields` (có
slack); `build_structs` export sau `layout_struct(i)` với kiểu field đã
`resolve_template_struct_type(resolve_alias(...))`; `release` free cả hai pool.
Chỉ declaration được chấp nhận mới vào pool, đúng như oracle bỏ template và
bản trùng.

Lưu ý: `HUnit` đã có `st_n` là **số statement**.  Lần đầu đặt tên struct count
là `st_n` đã ghi đè, làm unit không struct đọc rác (`st_n=7`) và emitter in
`%struct.?2???` rồi crash.  Tên đúng là `struct_n`/`struct_cap`.

### 28.3 Pool MIR

`selfhost/mir/mir.ivy` thêm `MField`/`MStruct` (cùng shape với HIR) và
`MUnit.structs`/`st_n`/`st_cap`, `MUnit.fields`/`fld_n`/`fld_cap`.
`MirBuilder.setup` cấp phát từ `u.struct_n`/`u.fld_n`; `build` copy struct
**trước** vòng function (giữ thứ tự source); `release` free.  `MStruct.fields`
trỏ vào run field liên tục, nên index trong run chính là GEP field index.

### 28.4 Emitter

- `setup` copy thêm 6 field struct vào bản `MUnit` heap (nâng allocation
  256 -> 320 byte).
- `make_struct_type_name` / `find_struct` / `find_field` / `is_struct_type`:
  helper tra cứu; tên `%struct.<name>` cấp phát heap và leak như spelling
  `[N x T]`.
- `emit_struct_defs` in ngay sau header `; LLVM IR emitted by ivy`, trước
  mọi `declare`; mỗi struct một dòng, field ngăn bằng `, `; nếu có ít nhất
  một struct thì in thêm một dòng trống.
- `llvm_type`: struct value -> `%struct.<name>`; struct pointer vẫn là `ptr`
  (nhánh pointer đứng trước).
- `lower_member_addr`: base value đi qua `lower_lvalue`, base arrow đi qua
  `lower_expr` rồi giảm `ptr_depth`; phát
  `getelementptr %struct.X, ptr <base>, i32 0, i32 <field>`.
- `lower_expr` kind 13: member rvalue = địa chỉ + `load` kiểu field.
- `lower_lvalue` kind 13: member lvalue = địa chỉ GEP (nested tự nối tiếp).
- `lower_inst` decl: struct chưa khởi tạo phát
  `store %struct.X zeroinitializer, ptr %slot` tại điểm khai báo (alloca đã
  hoist từ entry block); struct có init đi qua store chung nên thành
  `load %struct.X` + `store %struct.X` (copy toàn bộ).
- `check_fn_signature`: từ chối struct return và struct parameter.

### 28.5 Quirk oracle đã khớp

```llvm
%struct.Point = type { i32, i32 }
%struct.Empty = type {  }
```

- empty struct có **hai space** giữa `{` và `}`;
- một dòng trống sau block struct (khi có struct);
- định nghĩa đứng sau header, trước extern declare, theo thứ tự source;
- struct không dùng vẫn in;
- local struct chưa khởi tạo: `store %struct.X zeroinitializer, ptr %p.0`;
- copy: `%tmp.N = load %struct.X, ptr %src` rồi
  `store %struct.X %tmp.N, ptr %dst`;
- member: GEP `i32 0, i32 <idx>`, field index là số thứ tự trong struct;
- nested member nối GEP liên tiếp trên cùng base;
- `emit_cast` trả nguyên value khi from == to nên struct copy không sinh cast.

### 28.6 Corpus mới

| fixture | nội dung |
|---|---|
| `29_struct_local.ivy` | struct local, ghi/đọc hai field |
| `30_struct_copy.ivy` | copy khởi tạo `Point q = p` + gán `r = q` |
| `31_struct_nested.ivy` | `Outer` chứa `Inner`, member lồng `o.in.a` |
| `32_struct_fields.ivy` | field `bool`/`int8`/`int64`/`uint32` |
| `33_struct_member_copy.ivy` | copy member struct sang local `Inner q = o.in` |
| `34_struct_multi.ivy` | nhiều struct, có `struct Empty {}` |
| `35_struct_inner_block.ivy` | struct local trong block `if` |
| `36_struct_copy_write.ivy` | copy rồi sửa bản copy, bản gốc không đổi |

Tất cả được oracle chấp nhận và so byte với `ivy --llvm`.

### 28.7 Kết quả

`cgdiff.ps1 -Dir selfhost/corpus/codegen` (2026-11-06, scratch dưới
`...\opencode\h62e\run3\`):

```text
[3/4] running 36 fixtures ...
PASS 01_return_const.ivy
...
PASS 36_struct_copy_write.ivy
[4/4] summary
      PASS 36 / 36
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Regression HIR (byte-identical): name 27, type 124, constexpr 70,
template 99, borrow 45, move 60 file.  Regression MIR: cfg 75, lifetime 18
file.  `interpdiff.ps1` **23/23**, `build_smoke.ps1` PASS.

### 28.8 Chưa hỗ trợ / defer

1. Struct parameter/return: emitter từ chối ở H6.2e; **đã mở ở H6.2f** (mục 29).
2. Arrow `p->x` trên con trỏ struct: code có; **đã có fixture ở H6.2g** qua
   `nullptr` (mục 30).  Cast `(Point*)0` / `(Point*)__ivy_alloc(...)` vẫn bị
   parser selfhost từ chối.
3. Struct initializer list / designated init, `new`/`delete`: parser selfhost
   chưa hỗ trợ.
4. Struct template, kế thừa, method, virtual dispatch.
5. Mảng struct, struct chứa mảng, float trong struct.
6. Allocator, pointer/reference trong signature, array parameter/return/decay,
   mảng nhiều chiều, float, wide string, variadic, Itanium mangling.

### 28.9 Trạng thái H6.2e

Tệp thêm/sửa:

- `selfhost/hir/hir.ivy`, `selfhost/hir/hir_builder.ivy` (pool `HStruct`/`HField`);
- `selfhost/mir/mir.ivy`, `selfhost/mir/mir_builder.ivy` (pool `MStruct`/`MField`);
- `selfhost/codegen/emitter.ivy` (struct type, member, copy, zeroinit, reject
  signature);
- `selfhost/corpus/codegen/29..36_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 28 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a+b+c+d+e ... 36/36` (không đánh dấu ✅).

Không sửa `src/**`, không rebuild oracle đóng băng (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không commit/push.
Không có `.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62e\`.

## 29. H6.2f - struct parameter/return + mangling

### 29.1 Phạm vi

H6.2f mở struct **by value** trong signature:

- tham số struct (kể cả struct lồng nhau);
- giá trị trả về kiểu struct;
- hàm vừa nhận vừa trả struct;
- symbol MSVC của hàm có struct dùng đúng fallback của oracle.

Vẫn ngoài phạm vi: pointer/reference/array/float trong signature, struct
template/kế thừa/method, và arrow có fixture (xem 29.7).

### 29.2 Ground truth oracle

```llvm
define i32 @"?sum@@YHH@Z"(%struct.Point %arg0) {
%p.0 = alloca %struct.Point
store %struct.Point %arg0, ptr %p.0
...
}

define %struct.Point @"?make@@YHX@Z"() {
...
%tmp.2 = load %struct.Point, ptr %p.0
ret %struct.Point %tmp.2
}
```

- tham số struct đi thẳng bằng `%struct.Point %argN`, rồi được materialize
  vào alloca + `store` y như scalar;
- call: `load %struct.Point` từ slot rồi truyền value;
- return: `load` rồi `ret %struct.Point`;
- không có `byval`/`sret` attribute trong textual IR của oracle.

### 29.3 Mangling

`CodeGen::msvcMangleType` rơi xuống `return "H";` cho kiểu không nhận ra, nên
struct by value cũng thành `H`:

```text
?sum@@YHH@Z          int32 sum(Point)
?make@@YHX@Z         Point make()
?add@@YHHH@Z         Point add(Point, Point)
```

Đây là hành vi **của oracle**, không phải MSVC struct mangling thật
(`U...`); port giữ nguyên để byte-identical.  Emitter thêm sau nhánh float:

```ivy
if (this.is_struct_type(t)) { this.ps("H"); return; }
```

### 29.4 Emitter

- bỏ hai chốt từ chối struct return/parameter trong `check_fn_signature`;
- `llvm_type` đã map struct value -> `%struct.Name` từ H6.2e;
- `lower_function` (alloca + store `%argN`), `lower_call` (load + truyền) và
  `lower_inst` return (`ret`) vốn đã generic nên không cần nhánh mới;
- `emit_cast` trả nguyên value khi from == to, nên struct param/return không
  sinh cast.

### 29.5 Corpus mới

| fixture | nội dung |
|---|---|
| `37_struct_param.ivy` | `int32 sum(Point p)` |
| `38_struct_return.ivy` | `Point make()` + `Point p = make()` |
| `39_struct_param_return.ivy` | `Point add(Point, Point)` |
| `40_struct_nested_param.ivy` | tham số `Outer` chứa `Inner` |

### 29.6 Kết quả

`cgdiff.ps1 -Dir selfhost/corpus/codegen` (2026-11-07, scratch dưới
`...\opencode\h62f\run5\`):

```text
[3/4] running 40 fixtures ...
PASS 01_return_const.ivy
...
PASS 40_struct_nested_param.ivy
[4/4] summary
      PASS 40 / 40
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 36 fixture H6.2a–e vẫn byte-identical sau khi phục hồi
emitter (xem 29.8).

### 29.7 Chưa hỗ trợ / defer

1. Pointer/reference/array/float trong signature: vẫn diagnostic H6.2b.
2. Arrow `p->x` trên con trỏ struct: code có; **đã có fixture ở H6.2g** qua
   `nullptr` (mục 30).  Cast `(Point*)0` / `(Point*)__ivy_alloc(...)` vẫn bị
   parser selfhost từ chối.
3. Struct initializer list / designated init, `new`/`delete`: parser selfhost
   chưa hỗ trợ.
4. Struct template, kế thừa, method, virtual dispatch.
5. Mảng struct, struct chứa mảng, float trong struct.
6. MSVC struct mangling thật (`U...`): oracle không làm, port cũng không.
7. Allocator, array parameter/return/decay, mảng nhiều chiều, float, wide
   string, variadic, Itanium mangling.

### 29.8 Trạng thái H6.2f

Tệp thêm/sửa:

- `selfhost/codegen/emitter.ivy` (mangling fallback `H`, bỏ reject signature,
  phục hồi H6.2d/e — xem ghi chú sự cố bên dưới);
- `selfhost/corpus/codegen/37..40_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 29 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a+b+c+d+e+f ... 40/40` (không đánh dấu ✅).

Sự cố: commit `2cb9856` (H6.2e) đã ghi nhầm `emitter.ivy` phiên bản H6.2c
(mất H6.2d/e) do trạng thái working tree bị phục hồi ngoài ý muốn trong lúc
phiên bị tạm dừng.  H6.2f khôi phục emitter H6.2d từ `f0684dd`, áp lại toàn
bộ H6.2e, rồi thêm H6.2f; `cgdiff.ps1` 40/40 xác nhận lại cả 36 fixture cũ.

Không sửa `src/**`, không rebuild oracle đóng băng (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Commit/push H6.2f
theo yêu cầu của người dùng. Không có `.obj`, `.ivm`, `.exe` mới trong cây
làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62f\`.

## 30. H6.2g - `nullptr` + arrow `->`

### 30.1 Phạm vi

H6.2g mở đường có fixture cho arrow (phần code H6.2e trước đó chưa được test):

- literal `nullptr` trong `unsafe`, gán cho biến con trỏ struct;
- `p->x` đọc/ghi member qua con trỏ;
- arrow lồng `p->in.a` (GEP nối tiếp);
- biến cục bộ kiểu `Point*` với `alloca ptr` + `store ptr null`.

`&q` vẫn ngoài phạm vi: HIR selfhost báo `struct 'Point' has no matching
operator '&'` (và oracle bị crash ở chính input đó), nên không thể làm
fixture.  Cast `(Point*)0` / `(Point*)__ivy_alloc(...)` vẫn bị parser selfhost
từ chối.

### 30.2 Ground truth oracle

```llvm
%p.0 = alloca ptr
br label %bb0
bb0:
store ptr null, ptr %p.0
%tmp.0 = load ptr, ptr %p.0
%tmp.1 = getelementptr %struct.Point, ptr %tmp.0, i32 0, i32 0
store i32 1, ptr %tmp.1
```

- `nullptr` không sinh temporary: in thẳng `null`;
- `store ptr null` (không phải `inttoptr`);
- arrow = `load ptr` từ slot rồi GEP `%struct.X, ptr <loaded>, i32 0, i32 <field>`;
- nested arrow dùng GEP nối tiếp trên cùng giá trị pointer.

### 30.3 Emitter

- `VRef` thêm kind 4 = `nullptr`; `emit_val` in `null`;
- `lower_expr` kind 5 -> `null_ref()`;
- `emit_cast`: giữ nguyên `null` khi target là `ptr` (mirror oracle
  `if (value == "null" && to == "ptr") return "null";`);
- `llvm_type` map cả `nullptr` (HIR selfhost) và `nullptr_t` (oracle) -> `ptr`;
- nhánh arrow trong `lower_member_addr` (đã viết ở H6.2e) giờ được chạy thật:
  giảm `ptr_depth`, `lower_expr` base, rồi GEP.

### 30.4 Corpus mới

| fixture | nội dung |
|---|---|
| `41_struct_arrow_nullptr.ivy` | `Point* p = nullptr`, ghi/đọc `p->x`, `p->y` |
| `42_struct_arrow_read.ivy` | ghi `p->x` rồi đọc lại qua arrow |
| `43_struct_arrow_nested.ivy` | `Outer* p = nullptr`, `p->tag`, `p->in.a` |

### 30.5 Kết quả

`cgdiff.ps1 -Dir selfhost/corpus/codegen` (2026-11-07, scratch dưới
`...\opencode\h62g\run6\`):

```text
[3/4] running 43 fixtures ...
PASS 01_return_const.ivy
...
PASS 43_struct_arrow_nested.ivy
[4/4] summary
      PASS 43 / 43
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 40 fixture H6.2a–f vẫn byte-identical.

### 30.6 Chưa hỗ trợ / defer

1. `&q` trên struct: HIR selfhost từ chối; oracle crash cùng input.
2. Cast `(Point*)0`, `(Point*)__ivy_alloc(...)`: parser selfhost từ chối.
3. Pointer/array/float trong signature; reference parameter + reference cục bộ
   **đã mở ở H6.2h** (mục 31).
4. Struct initializer list / designated init, `new`/`delete`.
5. Struct template, kế thừa, method, virtual dispatch.
6. Mảng struct, struct chứa mảng, float trong struct.
7. Allocator, array parameter/return/decay, mảng nhiều chiều, float, wide
   string, variadic, Itanium mangling.

### 30.7 Trạng thái H6.2g

Tệp thêm/sửa:

- `selfhost/codegen/emitter.ivy` (`nullptr` kind 4, `llvm_type`, `emit_cast`,
  header);
- `selfhost/corpus/codegen/41..43_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 30 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a+b+c+d+e+f+g ... 43/43` (không đánh dấu ✅).

Không sửa `src/**`, không rebuild oracle đóng băng (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Commit/push H6.2g
theo yêu cầu của người dùng. Không có `.obj`, `.ivm`, `.exe` mới trong cây
làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62g\`.

## 31. H6.2h - tham số reference (`T&`) và reference cục bộ

### 31.1 Phạm vi

H6.2h mở kiểu tham chiếu trong signature và trong khai báo cục bộ:

- tham số `T&` (kể cả `Point&` và nhiều tham số reference);
- truyền địa chỉ lvalue của đối số tại call site;
- đọc/ghi qua tham chiếu (hai load cho rvalue, một load cho lvalue);
- forwarding: tham số reference truyền tiếp cho tham số reference khác;
- khai báo cục bộ `T& r = x;` (alias tới slot của `x`);
- symbol MSVC dùng `AAV` cho kiểu reference.

Vẫn ngoài phạm vi: raw pointer parameter/return (`T*`), reference return
(`T& f()`), array parameter/return/decay, float, struct reference return,
và mọi cấu trúc chưa có trong các tầng trước.

### 31.2 Ground truth oracle

```llvm
define i32 @"?bump@@YHAAV@Z"(ptr %arg0) {
%r.0 = alloca ptr
store ptr %arg0, ptr %r.0
br label %bb0
bb0:
%tmp.0 = load ptr, ptr %r.0          ; lowerLValue(r)
%tmp.1 = load ptr, ptr %r.0          ; lowerExpr(r): địa chỉ
%tmp.2 = load i32, ptr %tmp.1        ; lowerExpr(r): giá trị
%tmp.3 = add i32 %tmp.2, 1
store i32 %tmp.3, ptr %tmp.0
%tmp.4 = load ptr, ptr %r.0
%tmp.5 = load i32, ptr %tmp.4
ret i32 %tmp.5
}

define i32 @ivy_main() {
%x.0 = alloca i32
...
%tmp.0 = call i32 @"?bump@@YHAAV@Z"(ptr %x.0)
ret i32 %tmp.0
}
```

- tham số reference có LLVM type `ptr`, slot là `alloca ptr`, prologue
  `store ptr %argN, ptr %<name>.<slot>`;
- rvalue của biến reference = `load ptr` (lấy địa chỉ) rồi `load <T>`;
- lvalue của biến reference = chỉ `load ptr` (địa chỉ đối tượng);
- call site truyền `ptr <slot lvalue>` (không load giá trị);
- reference cục bộ: slot `alloca ptr`, init `store ptr <lowerLValue(init)>`.

### 31.3 Mangling

`CodeGen::msvcMangleType` kiểm tra pointer/reference trước các kiểu scalar:

```cpp
if (t.pointerDepth > 0 || t.isReference) return t.isReference ? "AAV" : "PAV";
```

nên `int32 bump(int32& r)` thành `?bump@@YHAAV@Z`; `void swap(int32&, int32&)`
thành `?swap@@YXAAVAAV@Z`; `int32 add(int32&, int32)` thành
`?add@@YHAAVH@Z`; `int32 sum(Point&)` cũng là `?sum@@YHAAV@Z` (không phụ
thuộc tên struct).

### 31.4 Emitter

- `Emitter` thêm mảng song song `vrefs_` (mirror `varIsRef_`) và
  `add_var(name, slot, is_ref)` / `is_ref_var(name)`;
- `check_fn_signature` chỉ còn từ chối `ptr_depth > 0`; reference parameter
  được đi qua; reference return vẫn bị từ chối;
- `emit_mangle_type` in `AAV` cho `is_ref`, `PAV` cho pointer;
- `llvm_type` đã map reference (khi `drop_ref == 0`) thành `ptr`, và
  `drop_ref == 1` thành kiểu giá trị;
- `lower_function` đăng ký `p.type.is_ref` vào bảng biến và dùng `ptr` cho
  prologue (đã generic từ H6.2a);
- `lower_ident`: biến reference -> `load ptr` rồi `load <T>`;
- `lower_lvalue` (kind 6): biến reference -> `load ptr`;
- `lower_assign` / `lower_inst` Store: bỏ chốt từ chối; kiểu store dùng
  `llvm_type(type, 1)` (strip reference);
- `lower_inst` Return: bỏ chốt từ chối; `lower_expr` xử lý reference;
- `lower_binary`: chỉ từ chối `ptr_depth`, cho phép toán hạng reference;
- `lower_call`: nếu param là reference thì `lower_lvalue(arg)` + type `ptr`,
  ngược lại giữ nguyên lower/cast như cũ;
- `lower_inst` Alloca: reference cục bộ -> `store ptr <lower_lvalue(init)>,
  ptr slot`;
- `lower_io_arg` vẫn từ chối reference (io::print trên `T&` chưa có fixture).

### 31.5 Corpus mới

| fixture | nội dung |
|---|---|
| `44_ref_param.ivy` | `int32 bump(int32& r)` ghi/đọc qua reference |
| `45_ref_swap.ivy` | `void swap(int32&, int32&)` hai reference |
| `46_ref_forward.ivy` | `outer(int32&)` gọi `inner(int32&)` (forwarding) |
| `47_struct_ref_param.ivy` | `int32 sum(Point& p)` member qua reference |
| `48_ref_local.ivy` | `int32& r = x;` reference cục bộ |
| `49_ref_mixed.ivy` | `int32 add(int32&, int32)` trộn reference + value |

### 31.6 Kết quả

`cgdiff.ps1 -Dir selfhost/corpus/codegen` (2026-11-07, scratch dưới
`...\opencode\h62h\run\`):

```text
[3/4] running 49 fixtures ...
PASS 01_return_const.ivy
...
PASS 49_ref_mixed.ivy
[4/4] summary
      PASS 49 / 49
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 43 fixture H6.2a–g vẫn byte-identical.

### 31.7 Chưa hỗ trợ / defer

1. Raw pointer parameter/return (`T*`): oracle yêu cầu `unsafe` trong
   signature ("raw pointer parameter type requires an unsafe block") và
   parser từ chối `int32* get()`; chưa có cú pháp selfhost/oracle thống nhất.
2. Reference return (`T& f()`): vẫn diagnostic H6.2b.
3. Array parameter/return/decay, mảng nhiều chiều.
4. Float, wide string, variadic, Itanium mangling.
5. `&q` trên struct (HIR selfhost từ chối; oracle crash cùng input), cast
   `(Point*)0` / `(Point*)__ivy_alloc(...)`, initializer list, `new`/`delete`,
   template/kế thừa/method/virtual.
6. io::print trên biểu thức reference.

### 31.8 Trạng thái H6.2h

Tệp thêm/sửa:

- `selfhost/codegen/emitter.ivy` (bảng `vrefs_`, `add_var`/`is_ref_var`,
  `check_fn_signature`, `emit_mangle_type`, `lower_ident`, `lower_lvalue`,
  `lower_assign`, `lower_binary`, `lower_call`, Alloca/Store/Return, header);
- `selfhost/corpus/codegen/44..49_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 31 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a+b+c+d+e+f+g+h ... 49/49` (không đánh dấu ✅).

Không sửa `src/**`, không rebuild oracle đóng băng (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Commit/push H6.2h
theo yêu cầu của người dùng. Không có `.obj`, `.ivm`, `.exe` mới trong cây
làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62h\`.
## 32. H6.2i - reference return (`T& f()`)

### 32.1 Phạm vi

H6.2i mở kiểu trả về reference:

- `T& f(...)` -> `define ptr`, `ret ptr <địa chỉ lvalue>`;
- call site trả `call ptr`; dùng như rvalue thì thêm `load <T>`;
- `T& r = f(...)` (reference cục bộ khởi tạo từ call, dùng raw pointer);
- call reference làm lhs (`f(x) = v`) và làm đối số reference (`g(f(x))`);
- struct reference return (`Point& get(Point&)`) copy by value qua `load`/`store`;
- mangling `AAV` cho kiểu trả về reference.

Ngoài phạm vi: raw pointer return (`T*`), array parameter/return/decay,
float, wide string, variadic, multi-dim, initializer list, `new`/`delete`.

### 32.2 Ground truth oracle

```llvm
define ptr @"?get@@YAAVAAV@Z"(ptr %arg0) {
%r.0 = alloca ptr
store ptr %arg0, ptr %r.0
br label %bb0
bb0:
%tmp.0 = load ptr, ptr %r.0
ret ptr %tmp.0
}

define i32 @ivy_main() {
%x.0 = alloca i32
br label %bb0
bb0:
store i32 1, ptr %x.0
%tmp.0 = call ptr @"?get@@YAAVAAV@Z"(ptr %x.0)
%tmp.1 = load i32, ptr %tmp.0
ret i32 %tmp.1
}
```

Struct reference return (`Point r = get(q);`):

```llvm
%tmp.2 = call ptr @"?get@@YAAVAAV@Z"(ptr %q.0)
%tmp.3 = load %struct.Point, ptr %tmp.2
store %struct.Point %tmp.3, ptr %r.1
```

- `Return` của hàm trả reference dùng `lowerLValue(value)` rồi `ret ptr`;
- `lowerLValue` của call reference dùng raw pointer (`lowerCallPtr`),
  không auto-load; `lowerExpr` của call reference load giá trị;
- tham số của hàm trả reference vẫn là tham số reference (`ptr`).

### 32.3 Emitter

- thêm `cur_ret_ref_` và `suppress_ref_load_` (mirror `suppressRefLoad_`);
- `check_fn_signature` chỉ còn chặn `f.ret.ptr_depth > 0`; reference return
  được đi qua;
- `lower_function` đặt `cur_ret_ref_ = func.ret.is_ref != 0`;
- `lower_inst` Return: nếu `cur_ret_ref_` -> `ret ptr <lower_lvalue>`;
- `lower_call`: sau `call`, nếu `e.type.is_ref` và không suppress ->
  `load <T>, ptr <call result>`;
- thêm `lower_call_ptr` và nhánh `kind == 11` trong `lower_lvalue`;
- `llvm_type`: khi `drop_ref == 1`, tra struct với `is_ref` đã xoá để
  `Point&` -> `%struct.Point` (sửa lỗi cast của `Point r = get(q);`).

### 32.4 Corpus mới

| fixture | nội dung |
|---|---|
| `50_ref_return.ivy` | `int32& get(int32&)` trả reference, dùng làm rvalue |
| `51_ref_local_call.ivy` | `int32& y = get(x);` reference cục bộ từ call |
| `52_ref_call_lhs.ivy` | `get(x) = 5;` call reference làm lhs |
| `53_ref_call_arg.ivy` | `bump(get(x))` call reference làm đối số |
| `54_ref_pick.ivy` | `pick(int32&, int32&)` trả một trong hai reference |
| `55_struct_ref_return.ivy` | `Point& get(Point&)` copy struct by value |
| `56_ref_call_binary.ivy` | `get(x) + 1` call reference trong biểu thức |

### 32.5 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62i\tmp` (2026-11-08):

```text
[3/4] running 56 fixtures ...
...
PASS 56_ref_call_binary.ivy
[4/4] summary
      PASS 56 / 56
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 49 fixture H6.2a-h vẫn byte-identical.

### 32.6 Chưa hỗ trợ / defer

1. Raw pointer parameter/return (`T*`): oracle yêu cầu unsafe trong
   signature; chưa có cú pháp selfhost/oracle thống nhất.
2. Array parameter/return/decay, mảng nhiều chiều.
3. Float (`float32`/`float64`), wide string, variadic, Itanium mangling.
4. `&q` trên struct (HIR selfhost từ chối; oracle crash cùng input), cast
   `(Point*)0` / `(Point*)__ivy_alloc(...)`, initializer list, `new`/`delete`,
   template/kế thừa/method/virtual.
5. io::print trên biểu thức reference.

### 32.7 Trạng thái H6.2i

Tập thêm/sửa:

- `selfhost/codegen/emitter.ivy` (`cur_ret_ref_`, `suppress_ref_load_`,
  `check_fn_signature`, `lower_function`, Return, `lower_call`,
  `lower_call_ptr`, `lower_lvalue` kind 11, `llvm_type`, header);
- `selfhost/corpus/codegen/50..56_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 32 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a+b+c+d+e+f+g+h+i ... 56/56`.

Không sửa `src/**`, không rebuild oracle đúng bang (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Commit/push H6.2i
theo yêu cầu của người dùng. Không có `.obj`, `.ivm`, `.exe` mới trong cây
làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62i\`.

### 33. H6.2j — float scalar (`float`/`double`/`float32_t`/`float64_t`)

#### 33.1 Phạm vi

H6.2j mở rộng emitter sang số thực dấu phẩy động vô hướng, vẫn giữ
nguyên tắc "text IR emission qua `io::print`" để khớp từng byte với
`ivy --llvm`:

- literal float -> `std::to_string(double)` (6 chữ số phần lẻ);
- `+ - * /` -> `fadd`/`fsub`/`fmul`/`fdiv`;
- so sánh -> `fcmp oeq/one/olt/ogt/ole/oge` (operand ép về kiểu lhs);
- `-x` -> `fsub <ty> 0.0, x`; `++`/`--` -> `fadd <ty> <old>, 1.0`
  (oracle dùng `fadd` cho cả `--`);
- cast `sitofp`/`uitofp`, `fptosi` (kể cả unsigned), `fpext`
  float->double, `fptrunc` double->float;
- tham số/return float, mangling MSVC `M` (float/float32_t) và
  `N` (double/float64_t);
- `io::print`/`println` float -> `fpext` lên double nếu cần rồi
  `call ptr @__ivy_fmt_float(double ...)` + `__ivy_write_str`.

#### 33.2 Thiết kế

- `VRef` thêm trường `double fval` (5 field, 40 byte); `arg_vals_` cấp
  `arg_cap_ * 40`.
- `float_lit` mang `kind == 5`; `lower_expr` đọc `e.fval` (MIR kind 1).
- `emit_double_lit(double v)` in `std::to_string`:
  - tách phần nguyên `ip = (long)v` trước, chỉ nhân phần lẻ với 1e6
    (`frac = (long)((v - ip) * 1e6 + 0.5)`), carry nếu `frac >= 1e6`;
  - in `ip`, `.`, rồi 6 chữ số của `frac`;
  - literal `>= 2^63` bị từ chối rõ ràng (`large floating-point literals
    are out of scope for H6.2j`) thay vì emit sai — cần big-integer
    formatting mới xử lý được.
- `is_float_base` nhận cả alias (`float`/`float32`/`float32_t`/
  `double`/`float64`/`float64_t`); `is_fp_ir` mirror `isFpType` của oracle.
- `lower_binary` rẽ nhánh `fadd/fsub/fmul/fdiv` khi lhs là float;
  `lower_unary` phát `fsub 0.0` và `fadd 1.0`.
- `emit_cast` xử lý float<->float và float<->int theo đúng thứ tự
  opcode của oracle.
- `check_fn_signature` cho phép return/param float; `emit_mangle_type`
  ánh xạ `M`/`N`.
- `lower_io_arg` phát `__ivy_fmt_float`; `lower_call` không còn từ chối
  đối số float; nhánh `io::print` không còn `fail`.
- Không khai báo biến `const char*` ngoài `unsafe`; dùng trực tiếp
  `this.llvm_type(...)` trong `this.ps(...)`.

#### 33.3 Corpus mới

| fixture | nội dung |
|---|---|
| `57_float_local.ivy` | `float64` cục bộ + literal |
| `58_float_ops.ivy` | `+ - * /` trên float |
| `59_float_cmp.ivy` | 6 toán tử so sánh float |
| `60_float_cast.ivy` | int<->float, float32<->float64 |
| `61_float_print.ivy` | `io::print` float64 |
| `62_float32.ivy` | biến `float32` + literal |
| `63_float_unary.ivy` | `-x`, `++`, `--` |
| `64_float_literals.ivy` | làm tròn 6 chữ số, 0.000001 |
| `65_float32_sig.ivy` | tham số/return `float32_t` |
| `66_float_ret.ivy` | hàm trả `float64` |
| `67_float_mixed_cmp.ivy` | so sánh float32 với float64 |
| `68_struct_float_param.ivy` | struct chứa float truyền by value |
| `69_struct_float_ret.ivy` | struct chứa float trả by value |
| `70_float_cond.ivy` | điều kiện `if`/`while` trên float |
| `71_float_mangle.ivy` | mangling trộn `N`/`i`/`M` |
| `72_float_compound.ivy` | `x += 2.0` |
| `73_float32_print.ivy` | `io::print` float32 (fpext) |

#### 33.4 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62j\tmp2` (2026-11-09):

```text
[3/4] running 73 fixtures ...
...
PASS 73_float32_print.ivy
[4/4] summary
      PASS 73 / 73
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 56 fixture H6.2a-i vẫn byte-identical. Ngoài corpus còn
11 probe đầu + 11 probe biên (`f12..f28`) chạy tay: tất cả khớp byte,
riêng `f13_frem` khớp lỗi frontend.

#### 33.5 Giới hạn / defer

1. `%` trên float (`frem`): frontend oracle từ chối
   `error: '%' expects integer operands`, nên không có cú pháp nguồn
   hợp lệ; selfhost khớp lỗi.
2. Literal `>= 2^63` (ví dụ `1e20`): oracle in
   `100000000000000000000.000000`; selfhost từ chối có chủ đích vì cần
   big-integer formatting. `1e15`/`1e18` đã PASS.
3. Các mục ngoài phạm vi của H6.2i vẫn giữ nguyên (raw pointer
   signature, array param/decay/multi-dim, wide string, variadic,
   template/virtual, Itanium mangling, initializer list, `new`/`delete`,
   `io::print` trên biểu thức reference).

#### 33.6 Trạng thái H6.2j

Tập thêm/sửa:

- `selfhost/codegen/emitter.ivy` (`VRef.fval`, `float_lit`,
  `emit_double_lit`, `is_float_base`, `is_fp_ir`, `emit_cast`,
  `fcmp_name`, `lower_binary`, `lower_unary`, `check_fn_signature`,
  `emit_mangle_type`, `lower_io_arg`, header);
- `selfhost/corpus/codegen/57..73_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 33 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a..j ... 73/73`.

Không sửa `src/**`, không rebuild oracle đúng bang (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Commit/push
H6.2j theo yêu cầu của người dùng. Không có `.obj`, `.ivm`, `.exe` mới
trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62j\`.

### 34. H6.2k — short-circuit, ternary và switch

#### 34.1 Phạm vi

H6.2k đóng nốt các khoảng trống control-flow của emitter:

- `&&` / `||`: short-circuit bằng hai inline block (`bb.i<N>`) và một
  `phi i1`, nhánh hằng là `false` (`&&`) hoặc `true` (`||`);
- ternary `c ? a : b`: ba inline block (then/else/merge) và
  `phi <ty>` ở merge block;
- `switch` (MIR instruction kind 6): in
  `switch <ty> <cond>, label %<default> [ ... ]` với từng arm
  `    <ty> <value>, label %<bb>`, kể cả khi `default` đứng trước các
  `case` trong nguồn;
- `do/while`, `break`, `continue` vốn đã đi qua CFG jump của MIR từ
  H6.2a; H6.2k thêm fixture regression để khóa hành vi.

Comma operator vẫn ngoài phạm vi: frontend oracle từ chối cú pháp
(`expected ')' to close parenthesized expression`).

#### 34.2 Thiết kế

- Thêm tracking `cur_blk_inline_` / `cur_blk_idx_` mirror `curBlock_`
  của oracle (một cặp thay cho `std::string`):
  - `lower_block` đặt `(0, local_idx)`;
  - `emit_bounds_check` đặt `(1, panic_b)` rồi `(1, ok_b)`;
  - `lower_binary` (`&&`/`||`) và `lower_ternary` cập nhật sau mỗi
    nhánh.
- PHI predecessor được đọc lại **sau khi** lower nhánh, nên ternary
  lồng ternary hoặc `&&` lồng `&&` vẫn trỏ đúng block cuối
  (`rhsEnd`/`thenEnd`/`elseEnd` của oracle).
- Thứ tự cấp inline block khớp oracle:
  - `&&`/`||`: `evalB`, `mergeB`;
  - ternary: `thenB`, `elseB`, `mergeB`.
- `lower_ternary` dùng `llvm_type(e.type, 0)` cho kiểu PHI.
- `lower_inst` kind 6: cond `-1` không in gì; arm value in thập phân;
  block in qua `emit_bb` (global → local).
- Helper mới: `emit_blk_ref` (`%bb<local>` / `%bb.i<N>`) và
  `emit_inline_label`.

#### 34.3 Corpus mới

| fixture | nội dung |
|---|---|
| `74_ternary_basic.ivy` | `c ? 10 : 20` |
| `75_ternary_nested.ivy` | ternary lồng trong cả hai nhánh |
| `76_ternary_and.ivy` | điều kiện ternary là `&&` |
| `77_shortcircuit.ivy` | `&&` và `||` cơ bản |
| `78_shortcircuit_nested.ivy` | `&&` lồng `&&` |
| `79_or_ternary.ivy` | `||` với ternary ở rhs |
| `80_or_chain.ivy` | chuỗi `||` ba toán hạng |
| `81_ternary_float.ivy` | ternary trả `float64` |
| `82_ternary_arg.ivy` | ternary làm đối số call |
| `83_ternary_ret.ivy` | ternary trong `return` |
| `84_ternary_call.ivy` | hai nhánh gọi hàm khác nhau |
| `85_shortcircuit_call.ivy` | `&&` với call hai vế |
| `86_switch_basic.ivy` | `switch` 2 case + default, mỗi case `return` |
| `87_switch_expr.ivy` | cond là biểu thức `x + 1` |
| `88_switch_nodefault.ivy` | không có `default` (nhánh exit) |
| `89_switch_default_first.ivy` | `default` đứng trước các `case` |
| `90_switch_i8.ivy` | `switch` trên `int8` |
| `91_switch_nested.ivy` | `switch` lồng trong case |
| `92_dowhile.ivy` | `do { } while` (regression) |
| `93_loop_break_continue.ivy` | `break`/`continue` trong `for` |
| `94_and_if.ivy` | `&&` trong điều kiện `if` |

#### 34.4 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62k\tmp` (2026-11-09):

```text
[3/4] running 94 fixtures ...
...
PASS 94_and_if.ivy
[4/4] summary
      PASS 94 / 94
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 73 fixture H6.2a-j vẫn byte-identical.

#### 34.5 Giới hạn / defer

1. `switch` dạng `case ...: ... break;` hoặc `nextcase;` bị frontend
   oracle từ chối (`Ivy forbids implicit fallthrough...`), nên fixture
   dùng `return` trong từng case; selfhost khớp đúng lỗi đó.
2. Comma operator: frontend oracle từ chối cú pháp.
3. Các mục ngoài phạm vi của H6.2i/j giữ nguyên (raw pointer signature,
   array param/decay/multi-dim, wide string, variadic, template/virtual,
   Itanium mangling, initializer list, `new`/`delete`, `io::print` trên
   biểu thức reference).

#### 34.6 Trạng thái H6.2k

Tập thêm/sửa:

- `selfhost/codegen/emitter.ivy` (`cur_blk_inline_`, `cur_blk_idx_`,
  `emit_blk_ref`, `emit_inline_label`, tracking trong
  `emit_bounds_check`/`lower_block`, nhánh `&&`/`||` trong
  `lower_binary`, `lower_ternary`, `lower_expr` kind 10,
  `lower_inst` kind 6, header);
- `selfhost/corpus/codegen/74..94_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 34 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a..k ... 94/94`.

Không sửa `src/**`, không rebuild oracle đúng bang (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Commit/push
H6.2k theo yêu cầu của người dùng. Không có `.obj`, `.ivm`, `.exe` mới
trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62k\`.

### 35. H6.2l — mảng float/struct và io::print reference

#### 35.1 Phạm vi

- Mở rộng phần tử mảng một chiều từ chỉ số nguyên sang:
  - số thực (`float32`/`float64`, kể cả alias `float32_t`/`float64_t`);
  - struct (`Point a[2]`).
- `a[i]` với phần tử float/struct: GEP `[N x T]`, load/store đúng kiểu,
  member chain `a[i].x` (kể cả struct lồng nhau), whole-element copy
  `a[1] = a[0]`, float formatting của `a[i]`.
- `io::print` trên biểu thức reference: dùng đúng nhánh của referent
  (char/bool/float/int) như oracle.

#### 35.2 Thiết kế

- `llvm_type` nhánh array: thay điều kiện "chỉ int" bằng
  `elem_ok = int | float | struct`; pointer/reference element vẫn bị
  từ chối (`arrays of pointers are out of scope`).
- Struct element được tra với `is_ref` đã xóa (`is_struct_type(ev)`).
- `make_array_type` không đổi: `[N x <llvm_type(elem)>]` tự sinh
  `[3 x double]`, `[2 x %struct.P]`.
- `lower_index_addr`, `lower_member_addr`, bounds check dùng lại nguyên
  vẹn; chỉ kiểu phần tử thay đổi.
- `lower_io_arg`: bỏ fail `is_ref`. Oracle không special-case reference
  và dùng `llvmType(at)` (trả `ptr` cho reference), nên selfhost in lại
  đúng `sext ptr ...` / `fpext ptr ...` — bug-compatible, IR này không
  hợp lệ cho backend về sau nhưng khớp byte oracle.

#### 35.3 Corpus mới

| fixture | nội dung |
|---|---|
| `95_float_array.ivy` | `float64 a[3]`, gán và in phần tử |
| `96_float32_array.ivy` | `float32 a[2]` |
| `97_float_array_loop.ivy` | `a[i] = a[i] + 0.5` trong `for` |
| `98_struct_array.ivy` | `P a[2]`, `a[i].x`/`a[i].y` |
| `99_struct_array_copy.ivy` | `a[1] = a[0]` whole-struct copy |
| `100_struct_array_nested.ivy` | `Outer a[2]`, `a[0].in.x` |
| `101_struct_array_float.ivy` | struct chứa `float64` trong mảng |
| `102_bool_array.ivy` | mảng `bool` (regression int base) |
| `103_print_ref.ivy` | `io::println("{}", r)` với `int32&` |
| `104_print_ref_float.ivy` | reference `float64&` (bug-compat `fpext ptr`) |
| `105_print_ref_char.ivy` | reference `int8&` → `__ivy_write_char` |

#### 35.4 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62l\tmp` (2026-11-09):

```text
[3/4] running 105 fixtures ...
...
PASS 99_struct_array_copy.ivy
[4/4] summary
      PASS 105 / 105
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 94 fixture H6.2a-k vẫn byte-identical.

#### 35.5 Giới hạn / defer

1. Mảng con trỏ (`int32* a[2]`): parser yêu cầu `unsafe` cho khai báo raw
   pointer, nên chưa có cú pháp nguồn hợp lệ.
2. Mảng là field của struct (`struct S { int32 a[2]; }`): parser từ chối
   (`expected ';' after field declaration`).
3. Enum type trong `llvm_type`/mangling: cần bảng enum xuyên HIR/MIR
   (dự kiến H6.2m).
4. Initializer list, `new`/`delete`, static local, raw pointer
   param/return, array param/decay/multi-dim: vẫn ngoài phạm vi.

#### 35.6 Trạng thái H6.2l

Tập thêm/sửa:

- `selfhost/codegen/emitter.ivy` (nhánh array trong `llvm_type`,
  `lower_io_arg`, header);
- `selfhost/corpus/codegen/95..105_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 35 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a..l ... 105/105`.

Không sửa `src/**`, không rebuild oracle đúng bang (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Commit/push
H6.2l theo yêu cầu của người dùng. Không có `.obj`, `.ivm`, `.exe` mới
trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62l\`.

### 36. H6.2m — enum type resolution

#### 36.1 Phạm vi

- Giá trị enum được resolve về **kiểu số nguyên nền** (underlying) đúng
  như oracle: `Color c = Green;` -> `alloca i32`, `store i32 1`,
  `load i32`; không còn `i8` + `trunc`/`sext`.
- Enum không scoped (`enum Color { ... }`) và scoped (`enum class Mode
  { ... }`, `enum struct`).
- Underlying tường minh: `enum Small : int8`, `enum Big : uint16`,
  `enum Tiny : uint8` (parser oracle từ chối hậu tố `_t`; dùng
  `int8`/`uint16`/`uint8`).
- Enum trong: biến cục bộ, tham số, giá trị trả về, phần tử mảng,
  field struct, `io::print`/`println`, `sizeof`, chỉ số mảng, số học
  (`Green + 1`), giá trị enumerator tường minh/biểu thức.
- Mangling MSVC:
  - unscoped enum -> mã của underlying (`H`, `C`, `G`, ...);
  - scoped enum -> `W4<name>@@` (không có namespace prefix trong
    selfhost subset).
- Enum không dùng vẫn được ghi vào bảng nhưng không sinh IR toàn cục
  (giống oracle).

#### 36.2 Thiết kế

- `selfhost/hir/hir.ivy`: thêm `HEnum { name, is_scoped, underlying }`
  và `HUnit.enums/enum_n/enum_cap`.
- `selfhost/hir/hir_builder.ivy`: cấp phát/free pool enum trong
  `setup`/`release`; `build_enums` export các enum được chấp nhận
  (bỏ qua enum trùng tên như oracle). Parser selfhost để `underlying`
  rỗng khi nguồn không ghi kiểu nền; builder đổi thành `int` (oracle
  mặc định `int` ngay trong parser).
- `selfhost/mir/mir.ivy`: thêm `MEnum { name, is_scoped, underlying }`
  và `MUnit.enums/en_n/en_cap`.
- `selfhost/mir/mir_builder.ivy`: cấp phát/free pool; copy
  `hu.enums` -> `mu.enums` ở đầu `build()`.
- `selfhost/codegen/emitter.ivy`:
  - `find_enum(name)` + `is_enum_type(t)` (mirror `enumTypes_`);
  - `llvm_type` resolve enum qua underlying (đệ quy, giữ `drop_ref`);
    nhánh array chấp nhận phần tử enum;
  - `emit_mangle_type`: unscoped -> đệ quy underlying, scoped ->
    `W4` + tên + `@@`;
  - `Emitter::setup` copy thêm `enums/en_n/en_cap` (xem 36.5 mục 1).
- Không cần đổi `check_fn_signature`: enum là kiểu value, không phải
  array/pointer.

#### 36.3 Corpus mới

| fixture | nội dung |
|---|---|
| `106_enum_local.ivy` | `Color c = Green` -> `alloca i32`, `store i32 1` |
| `107_enum_param_return.ivy` | tham số + return enum, mangling `H` |
| `108_enum_scoped.ivy` | `enum class Mode`, `Mode::B` |
| `109_enum_scoped_param.ivy` | tham số/return scoped, mangling `W4Mode@@` |
| `110_enum_underlying_i8.ivy` | `enum Small : int8` -> `i8` |
| `111_enum_underlying_u16.ivy` | `enum Big : uint16` -> `i16` |
| `112_enum_underlying_u8.ivy` | `enum Tiny : uint8` -> `i8`, mangling `E` |
| `113_enum_array.ivy` | `Color a[2]` -> `[2 x i32]` |
| `114_enum_struct_field.ivy` | `struct S { Color c; int32 n; }` |
| `115_enum_print.ivy` | `io::println("{}", c)` -> `__ivy_fmt_int` |
| `116_enum_explicit_values.ivy` | `A=1, B=2, C=A|B, D=-1` |
| `117_enum_multi_param.ivy` | `mix(Color, Mode, int32)` |
| `118_enum_in_struct_param.ivy` | struct chứa enum truyền by value |
| `119_enum_return.ivy` | hàm trả enum không so sánh |
| `120_enum_scoped_array.ivy` | mảng `Mode a[2]`, `Mode::C` |
| `121_enum_struct_print.ivy` | in field enum + int trong struct |
| `122_enum_scoped_print.ivy` | in giá trị scoped enum |
| `123_enum_struct_return.ivy` | trả struct chứa enum |
| `124_enum_param_i8.ivy` | tham số `Small : int8` |
| `125_enum_unused.ivy` | enum không dùng không sinh IR |
| `126_enum_arith.ivy` | `Green + 1` |
| `127_enum_sizeof.ivy` | `sizeof(Color)` = 4 |
| `128_enum_index.ivy` | `a[Green]` |
| `129_enum_scoped_underlying.ivy` | `enum class M : uint8` |
| `130_enum_scoped_underlying_print.ivy` | `enum class M : int8` + in |
| `131_enum_nested_struct_array.ivy` | `S a[2]; a[1].c` |
| `132_enum_two_params.ivy` | hai enum khác nhau trong một chữ ký |
| `133_enum_float_field.ivy` | struct `{ Color; float64 }` + in |
| `134_enum_expr_values.ivy` | giá trị enumerator `A+B`, `C*2` |

#### 36.4 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62m\tmp` (2026-11-10):

```text
[3/4] running 134 fixtures ...
PASS 106_enum_local.ivy
...
PASS 134_enum_expr_values.ivy
[4/4] summary
      PASS 134 / 134
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 105 fixture H6.2a-l vẫn byte-identical.

#### 36.5 Giới hạn / defer

1. `Emitter::setup` copy `MUnit` từng field thủ công; lần đầu bỏ sót
   `enums/en_n/en_cap` nên `find_enum` luôn trả -1 và emitter rơi về
   `i8`. Đã sửa bằng cách copy thêm ba field (kích thước 320 byte vẫn
   đủ: `MUnit` hiện 272 byte).
2. So sánh enum (`c == Green`) bị frontend oracle từ chối
   (`'==' between incompatible types`), nên không có fixture dương.
3. C-style cast (`(int32)c`) yêu cầu `unsafe`; chẩn đoán selfhost khác
   oracle ở trường hợp này (frontend, ngoài phạm vi H6.2m).
4. Underlying `long` bị frontend từ chối; hậu tố `_t` (`int8_t`, ...)
   đã bị loại khỏi ngôn ngữ.
5. Namespace chưa được selfhost parser hỗ trợ, nên scoped mangling
   không có namespace prefix; Itanium mangling vẫn ngoài phạm vi.
6. Raw pointer param/return, array param/decay/multi-dim, initializer
   list, `new`/`delete`, static local: vẫn defer.

#### 36.6 Trạng thái H6.2m

Tập thêm/sửa:

- `selfhost/hir/hir.ivy` (`HEnum`, `HUnit.enums`);
- `selfhost/hir/hir_builder.ivy` (pool enum + export trong
  `build_enums`);
- `selfhost/mir/mir.ivy` (`MEnum`, `MUnit.enums`);
- `selfhost/mir/mir_builder.ivy` (pool + copy);
- `selfhost/codegen/emitter.ivy` (`find_enum`, `is_enum_type`,
  `llvm_type`, `emit_mangle_type`, `setup`, header);
- `selfhost/corpus/codegen/106..134_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 36 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a..m ... 134/134`.

Không sửa `src/**`, không rebuild oracle đúng bang (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62m\`.

### 37. H6.2n — `new`/`delete` (kèm deref `*p` và address-of `&x`)

#### 37.1 Phạm vi

- `new T`, `new T(args)` với `T` là số nguyên, số thực, enum hoặc struct.
- `delete p` và `delete[] p`.
- Chỉ được dùng trong `unsafe { ... }` (kể cả dạng một câu lệnh
  `unsafe stmt;`), khớp `Parser::inUnsafe_` của oracle.
- Hai toán tử con trỏ tối thiểu để `new`/`delete` dùng được:
  - deref `*p` (rvalue: load pointee; lvalue: chính giá trị con trỏ);
  - address-of `&x` trên biến (`IdentRef`) và trên lvalue khác.

#### 37.2 Thiết kế

- `ast.ivy`: thêm kind 21 = `new` (type trong `e.type`, args trong
  `astart/an`) và 22 = `delete` (`isArray` trong `flag`, toán hạng
  trong `a`).
- `parser.ivy`: field `in_unsafe_`, bật/tắt quanh thân `unsafe`
  (save/restore như oracle), `parse_new`/`parse_delete` gọi từ
  `parse_unary` trước chuỗi postfix; lỗi
  `'new'/'delete' requires an unsafe block in the Ivy subset`.
- `hir_builder.ivy`: `build_new` (kind 20; kiểu nút là `T*`, args build
  qua `abuf_` staging như call) và `build_delete` (kind 21, chẩn đoán
  `'delete' expects a pointer operand` khi toán hạng không phải con trỏ).
- `mir_builder.ivy`: `ex_new` (kind 15, `lt = Unknown`, reserve run args
  trước khi build) và `ex_delete` (kind 16).
- `emitter.ivy`:
  - `uses_alloc_`/`uses_free_` + `collect_expr` kinds 15/16 (chỉ bật
    cờ, KHÔNG đệ quy vào args/toán hạng — đúng như oracle, nên string
    literal/`io::print` nằm trong `new` arg không được pre-collect);
  - `sizeof_type` mirror `CodeGen::sizeofType` (struct = tổng field làm
    tròn lên 8; enum = underlying; bỏ qua `ptr_depth` như oracle);
  - `lower_new`: `%tmp.N = call ptr @__ivy_alloc(i64 sizeof(T))`, sau đó
    nếu có args thì `store T <arg0>, ptr %tmp.N` (bug-compat: oracle chỉ
    dùng args[0] và lưu nguyên kiểu `T`, kể cả struct);
  - `lower_delete`: `call void @__ivy_free(ptr ...)`;
  - emit `declare ptr @__ivy_alloc(i64)` / `declare void @__ivy_free(ptr)`
    sau entry point, trước io runtime;
  - `lower_unary` nhánh `&` (IdentRef -> slot của biến) và `*`
    (load `llvmType(e.type)`), `lower_lvalue` nhánh Unary `*`
    (trả giá trị con trỏ);
  - `lower_string` trả `null` khi literal chưa được pre-collect (oracle:
    `return "null"`), và `VRef` kind 6 = "empty" cho void call / `delete`
    dùng như biểu thức (`emit_val` không in gì, khớp chuỗi `""` của
    oracle).

#### 37.3 Corpus mới

| fixture | nội dung |
|---|---|
| `135_new_scalar.ivy` | `new int32(5)`, `*p`, `delete p` |
| `136_new_no_paren.ivy` | `new int32;` + `*p = 7` |
| `137_new_struct.ivy` | `new P(5)` + `p->x` |
| `138_new_struct_noparen.ivy` | `new P;` + ghi/đọc field |
| `139_new_enum.ivy` | `new Color(2)` -> alloc `i64 4` |
| `140_new_float.ivy` | `new float64(1.5)` + in |
| `141_new_float32.ivy` | `new float32(2.25)` + in |
| `142_new_expr_arg.ivy` | `new int32(v + 1)` |
| `143_new_multiarg.ivy` | `new P(1, 2)` (chỉ args[0] được store) |
| `144_delete_array.ivy` | `delete[] p` |
| `145_new_in_expr.ivy` | `*new int32(9)` |
| `146_new_loop.ivy` | cấp phát/giải phóng trong `while` |
| `147_deref_struct.ivy` | `(*p).x` |
| `148_addr_local.ivy` | `int32* p = &x` + `*p = 5` |
| `149_double_deref.ivy` | `int32** pp = &p`, `**pp` |
| `150_new_single_stmt.ivy` | `unsafe stmt;` chứa `new` |
| `151_new_struct_pad.ivy` | struct 3 field -> `i64 16` |
| `152_new_struct_i8.ivy` | struct `{int8; int32}` -> `i64 8` |
| `153_new_nested_struct.ivy` | struct lồng nhau -> tổng sizeof |

#### 37.4 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62n\run` (2026-11-10):

```text
[3/4] running 153 fixtures ...
PASS 135_new_scalar.ivy
...
PASS 153_new_nested_struct.ivy
[4/4] summary
      PASS 153 / 153
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Exit 0. Toàn bộ 134 fixture H6.2a-m vẫn byte-identical. `interpdiff.ps1`
vẫn `PASS 23 / 23` (parser/HIR dùng chung không đổi hành vi).

Ngoài corpus, 25 probe trong `...\h62n\probe` khớp oracle từng byte, gồm
`m06_new`, `p10_new_noparen`, `p11_new_struct`, `p12_new_outside`
(lỗi unsafe), `n06_delete_nonptr` (chẩn đoán), `n12_addr_member` /
`n13_addr_nonvar` (lỗi `&` không phải biến), `n17..n19` (sizeof struct
có padding/lồng nhau), `n20` (string literal trong `new` arg -> `null`,
không sinh `@.str`) và `n21` (void call trong `new` arg -> `store i32 ,
ptr`, io runtime không được emit — bug-compat).

#### 37.5 Giới hạn / defer

1. `new T[n]` (mảng cấp phát) bị cả hai frontend từ chối
   (`expected ';' after declaration`) — không có fixture dương.
2. `delete` không kiểm tra double-free/leak (chỉ là lowering IR).
3. `new T(args)` nhiều hơn một arg: oracle chỉ store args[0] và bỏ các
   arg còn lại (không sinh IR); selfhost giữ đúng hành vi đó.
4. `&` chỉ hợp lệ trên biến (HIR `oe.kind != 8` -> lỗi
   `'&' is only supported on variables in the Ivy subset`); `&s.x`
   cũng bị từ chối ở HIR, khớp oracle.
5. Struct initializer list, raw pointer param/return, array param/decay/
   multi-dim, static local: vẫn defer.

#### 37.6 Trạng thái H6.2n

Tập thêm/sửa:

- `selfhost/parsing/ast.ivy` (comment kind 21/22);
- `selfhost/parsing/parser.ivy` (`in_unsafe_`, `parse_new`,
  `parse_delete`, dispatch trong `parse_unary`, save/restore trong
  `parse_statement`);
- `selfhost/hir/hir.ivy` (comment kind 19–21);
- `selfhost/hir/hir_builder.ivy` (`build_new`, `build_delete`, dispatch
  AST 21/22);
- `selfhost/mir/mir_builder.ivy` (`ex_new`, `ex_delete`, dispatch HIR
  20/21);
- `selfhost/codegen/emitter.ivy` (`uses_alloc_`/`uses_free_`,
  `collect_expr`, `sizeof_type`, `lower_new`, `lower_delete`, nhánh `*`/
  `&` trong `lower_unary`, nhánh `*` trong `lower_lvalue`, declare
  allocator, header);
- `selfhost/corpus/codegen/135..153_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 37 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a..n ... 153/153`.

Không sửa `src/**`, không rebuild oracle đúng bang (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62n\`.

### 38. H6.2o — struct initializer list (`P p{...}`, `p = {...}`)

#### 38.1 Phạm vi

Đúng theo những gì oracle chấp nhận (khảo sát `i01`–`i15`):

- declaration init: `P p{1, 2};` và `P p = {1, 2};`;
- assignment: `p = {1, 2};` (kể cả `p = {}` và `p = {.y = 3}`);
- designated initializer `.field = value`, trộn positional/designated
  theo luật C++20 (positional sau designator đi tiếp từ field kế tiếp);
- partial `{1}` và empty `{}` (các field còn lại zero-init);
- field kiểu enum / `int8` / `float` / `float32` / struct (chỉ khi
  element là biểu thức có kiểu tương thích).

Oracle **từ chối** (giữ nguyên, không có fixture dương): nested
`{{7}, 2}`, array init `int32 a[3] = {...}`, init-list làm đối số
(`sum({4, 5})`), `return {...}`, array-of-struct init
(`P a[2] = {{1, 2}, {3, 4}}`). Thông điệp tương ứng:
`braced initializer list is only valid in a declaration initializer`,
`braced initializer list can only initialize a struct variable, not 'T'`,
`field 'in' of struct 'Out' expects 'In', got 'int'`.

#### 38.2 Thiết kế

- `ast.ivy`: kind 23 = init-list; field mới `AExpr.dstart` (index đầu
  tiên trong pool designator). `astart/an` giữ run element trong
  call-arg pool như call.
- `parser.ivy`: pool `dpool_` (`long*`, lưu con trỏ interned dưới dạng
  long để tránh bug `const char**`) + `push_desig`/`desigs`; nhánh
  `{ ... }` trong `parse_primary` (element có thể có `.field =`,
  trailing comma được chấp nhận); staging `argn_idx_`/`argn_next_` như
  call để call lồng nhau không xen kẽ argument; stride `epool_`
  168 → 176 byte.
- `hir_builder.ivy`: `build_struct_init` (mirror `buildStructInit`):
  - resolve designator theo tên field, positional cursor;
  - chẩn đoán `has no field named`, `is initialized more than once`,
    `too many initializers`, `expects 'T', got 'U'` (dùng loc của
    element như oracle);
  - list HIR đúng `nfields` phần tử, field không khởi tạo = `-1`;
  - dispatch trong `build_decl` (init kind 23) và `build_assign`
    (`op == "="`, rhs kind 23, type lấy từ lhs);
  - nhánh `a.kind == 23` trong `build_expr` phát chẩn đoán
    `braced initializer list is only valid in a declaration initializer`
    + node dummy (giữ thứ tự lỗi của oracle);
  - scratch `ilres_`/`ilini_` (field của builder) thay cho mảng heap.
- `mir_builder.ivy`: dùng `ex_initlist` sẵn có (kind 17), reserve run
  args trước khi build element.
- `emitter.ivy`:
  - `lower_init_list_into(ilIdx, slot)`: `store %struct.T
    zeroinitializer` rồi GEP + cast + store từng field có element;
  - `lower_expr` kind 17: cấp `%a.N` ẩn, fill, `load` cả struct;
  - `Inst::Alloca` với init kind 17: lower trực tiếp vào slot của biến
    (không tạo alloca tạm);
  - `lower_assign` với rhs kind 17: lower trực tiếp vào lvalue;
  - pre-scan trong `lower_function`: các init-list rvalue (value của
    `Store`, `Eval`, `Ret`, `CondBranch`) được cấp slot `%a.N` **trước**
    khi in `br label %bb0`, đúng vị trí oracle splice
    `;__IVY_ENTRY_ALLOCAS__`; `il_next_` tiêu thụ theo đúng thứ tự
    lowering.

#### 38.3 Corpus mới

| fixture | nội dung |
|---|---|
| `154_struct_brace_init.ivy` | `P p{1, 2}` |
| `155_struct_eq_init.ivy` | `P p = {1, 2}` |
| `156_struct_partial_init.ivy` | `P p = {1}` (3 field) |
| `157_struct_empty_init.ivy` | `P p = {}` |
| `158_struct_float_field_init.ivy` | field `float64` |
| `159_struct_assign_init.ivy` | `p = {1, 2}` (alloca `%a.N` ẩn) |
| `160_struct_designated_init.ivy` | `{.y = 5}` |
| `161_struct_enum_field_init.ivy` | field enum |
| `162_struct_i8_field_init.ivy` | field `int8` (cast `trunc`) |
| `163_struct_desig_then_pos.ivy` | `{.x = 1, 7}` (cursor C++20) |

#### 38.4 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62o\run` (2026-11-11):

```text
[3/4] running 163 fixtures ...
PASS 154_struct_brace_init.ivy
...
PASS 163_struct_desig_then_pos.ivy
[4/4] summary
      PASS 163 / 163
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

`interpdiff.ps1` vẫn `PASS 23 / 23`; `build_smoke.ps1` vẫn PASS.

Ngoài corpus, 15 probe `i01`–`i15` (khảo sát oracle, gồm cả các ca bị
từ chối) và 15 probe mở rộng `e01`–`e15` (designated trùng/không tồn
tại, quá nhiều initializer, assign trong loop/nested block, hai local,
`float32`, `int8`, …) đều khớp oracle từng byte (stdout, stderr, exit).

#### 38.5 Giới hạn / defer

1. Nested brace init, array init, init-list làm argument/return,
   array-of-struct init: oracle từ chối — giữ nguyên.
2. Default member initializer trong struct: không nằm trong phạm vi
   H6.2o (oracle có `defaultInits`, selfhost chưa port).
3. Struct method/`this`, global variable: vẫn defer như H6.2n.

#### 38.6 Trạng thái H6.2o

Tập thêm/sửa:

- `selfhost/parsing/ast.ivy` (kind 23 + `dstart`);
- `selfhost/parsing/parser.ivy` (`dpool_`/`push_desig`/`desigs`,
  nhánh `{...}` trong `parse_primary`, stride 176);
- `selfhost/hir/hir_builder.ivy` (`dp_`, `desig_eq`,
  `db_put_desig`, `build_struct_init`, dispatch decl/assign/expr,
  `ilres_`/`ilini_`);
- `selfhost/codegen/emitter.ivy` (`il_next_`, `lower_init_list_into`,
  kind 17 trong `lower_expr`, Alloca init, `lower_assign`, pre-scan
  alloca ẩn);
- `selfhost/corpus/codegen/154..163_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 38 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a..o ... 163/163`.

Không sửa `src/**`, không rebuild oracle dựng bằng (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62o\`.

### 39. H6.2p — pointer surface & `extern "C"` variadic

#### 39.1 Phạm vi

Đúng theo những gì oracle chấp nhận (khảo sát `t01`–`t10`, `u01`–`u15`,
`v01`–`v09`, `w01`–`w04`):

- khai báo `extern "C"` có tham số con trỏ (`void use(int32* p);`) và
  variadic (`int printf(const char* fmt, ...);`, `int f(...);`);
- gọi hàm với đối số con trỏ: biến `int32*`, `&x`, string literal, và
  array decay khi tham số là `T*`;
- biến con trỏ cục bộ (`int32* p = nullptr;`), gán `p = q`;
- `declaration-only` (không body, kể cả không `extern "C"`) với tham số
  con trỏ / trả về con trỏ: `int32 use(int32* p);`, `int32* get();`,
  `void* alloc(int64 n);`;
- so sánh con trỏ: `p == q`, `p != q`, `p < q`, `p > q`, `p <= q`,
  `p >= q`, `nullptr == p` (vế trái `nullptr`);
- số học con trỏ: `p + n`, `n + p`, `p - n`, `p++`, `p--`, `p[i]`
  (không bounds check), `*p` (đọc/ghi), `!p`, ternary `p ? p : nullptr`;
- `p[i]` với element `int8`, struct; `p + n` với `n` kiểu `int64`.

Oracle **từ chối** (giữ nguyên, đã kiểm khớp từng byte): `p += 1`,
`p * 2`, `p + q`, `p - q`, `p == 0`, `p > nullptr`, `1[p]`,
`int32 a[3]` làm tham số, array decay ở HIR (`argument 1 ... expects
'int32_t*', got 'int32_t'`), `nullptr` ngoài `unsafe`.

`v10` (`P* p = &s; return p->y;`) làm oracle **segfault**
(0xC0000005) nên không emulate.

#### 39.2 Thiết kế

- `ast.ivy`: `AFunction.is_variadic` (cuối struct).
- `parser.ivy`: stride `AFunction` 200 → 208 (2 pool); `f.is_variadic =
  pl.variadic` trong `build_function`, `m.is_variadic = pl.variadic`
  trong `push_method_record`.
- `hir.ivy` / `hir_builder.ivy`: `HFunction.is_variadic`, stride 120 →
  128; `add_function_at` khởi tạo 0; `build_free_signature` copy từ
  `AFunction`.
- `mir.ivy` / `mir_builder.ivy`: `MFunction.is_variadic`, stride 128 →
  136; copy từ `HFunction`.
- `emitter.ivy`:
  - `check_fn_signature` trả `true` sớm khi `has_body == 0` (chỉ
    declaration mới được dùng con trỏ/array; definition vẫn bị HIR chặn
    như oracle);
  - `emit_declaration` in `, ...` / `...` theo `is_variadic`;
  - `lower_call` bỏ chặn pointer/array argument, thêm nhánh array decay
    (giữ nhánh reference);
  - `find_call_target`: mirror vòng chọn callee của oracle
    (`CodeGen::lowerExpr` Call): so khớp tên + signature (kể cả numeric
    widening, float widening, array-to-pointer decay), fallback theo tên
    cho `extern "C"` variadic; sửa luôn bug chọn nhầm overload theo số
    tham số ở call site (fixture 174);
  - `same_type`: so base/unsigned/const/ptr_depth/array_size/tpl_args;
  - `lower_binary`: nhánh con trỏ (`is_ptr`) — so sánh ở `ptr` với
    predicate unsigned (`eq/ne/ult/ugt/ule/uge`), `+`/`-` gọi
    `lower_ptr_arith`;
  - `lower_ptr_arith`: lower lại toán hạng nguyên, `sub 0, idx` cho
    `p - n`, cấp temp GEP **trước** khi lower lại pointer operand (đúng
    thứ tự `%tmp.N` của oracle);
  - `llvm_elem_type`: giảm `ptr_depth`, `void*` → `i8`;
  - `lower_index_addr`: nhánh base là con trỏ (không bounds check,
    element type = `llvm_type(e.type, 0)`);
  - `lower_unary` `++`/`--` con trỏ → `getelementptr <elem>, ptr oldV,
    i64 1/-1` rồi store.

#### 39.3 Corpus mới

| fixture | nội dung |
|---|---|
| `164_printf_basic.ivy` | `extern "C" int32 printf(const char* fmt, ...)` + 1 arg |
| `165_printf_multi.ivy` | variadic nhiều arg (`int32`, string, `float64`) |
| `166_puts.ivy` | `extern "C" int32 puts(const char* s)` |
| `167_extern_ptr_decl.ivy` | `extern "C" void use(int32* p);` không gọi |
| `168_ptr_arg.ivy` | truyền `&x` và biến `int32*` cho tham số con trỏ |
| `169_ptr_cmp.ivy` | `== != < > <= >=` giữa hai con trỏ |
| `170_ptr_ret_void.ivy` | `extern "C" void* alloc(int64 n);` |
| `171_variadic_zero.ivy` | `extern "C" int32 f(...);` gọi 0 arg |
| `172_ptr_arith.ivy` | `p + 1`, `p - 1`, `p++`, `p--`, `p[2]` |
| `173_decl_only_ptr.ivy` | decl-only `int32 use(int32*)` / `int32* get()` |
| `174_overload_count_call.ivy` | overload theo số tham số: call site chọn đúng bản 3 tham số |

#### 39.4 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62p\cgdiff3` (2026-11-11):

```text
[3/4] running 174 fixtures ...
PASS 164_printf_basic.ivy
...
PASS 174_overload_count_call.ivy
[4/4] summary
      PASS 174 / 174
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

`interpdiff.ps1` vẫn `PASS 23 / 23`; `build_smoke.ps1` vẫn PASS.
13 example đích (`test_array`, `test_auto`, `test_b4_null_safe`,
`test_c2_new_delete`, `test_c3_io_print`, `test_default_args`,
`test_directives`, `test_overload`, `test_sizeof_alignof`,
`test_stringify_paste`, `test_switch`, `test_types_a1`, `test_warning`)
khớp oracle từng byte.

Trên 65 example được oracle chấp nhận: **26 MATCH** (trước p: 12).
Các DIFF còn lại thuộc stage sau: `test_cast` (q), `test_print` (r),
`test_move_*`/`test_d4_move` (z), `test_methods`/`test_ctor_dtor`/
`test_raii_break` (s), `test_operator_overloading` (t),
`test_concepts`/`test_template_struct`/`test_variadic` (u/v),
`test_structured_binding` (w), `test_range_for` (x),
`test_inherit*` (y), `test_b5_*`/`test_b6_*`/`test_d1..d7`/
`test_module_math`/`test_ivy_ext` (aa).

#### 39.5 Giới hạn / defer

1. `v10` oracle segfault — không emulate.
2. Array parameter (`int32 a[3]`) và array decay ở HIR: oracle từ chối —
   giữ nguyên.
3. `p += 1`, `p * 2`, `p ± q`, `p == 0`, `p > nullptr`: oracle từ chối —
   giữ nguyên.
4. `test_fn_a3`: selfhost báo lỗi parse (43/46/47) thay vì 4 lỗi pointer
   của oracle do thiếu `namespace` — defer stage ac.
5. Method/`this`, ctor/dtor, global variable, `move()`, template…: vẫn
   defer như các stage sau. (C-style cast đã xong ở H6.2q — xem mục 40.)

#### 39.6 Trạng thái H6.2p

Tập thêm/sửa:

- `selfhost/parsing/ast.ivy` (`AFunction.is_variadic`);
- `selfhost/parsing/parser.ivy` (stride 208, gán `is_variadic` cho
  function + method);
- `selfhost/hir/hir.ivy`, `selfhost/hir/hir_builder.ivy`
  (`HFunction.is_variadic`, stride 128);
- `selfhost/mir/mir.ivy`, `selfhost/mir/mir_builder.ivy`
  (`MFunction.is_variadic`, stride 136);
- `selfhost/codegen/emitter.ivy` (`check_fn_signature`,
  `emit_declaration`, `lower_call`, `find_call_target`, `same_type`,
  `lower_binary`, `lower_ptr_arith`, `llvm_elem_type`,
  `lower_index_addr`, `lower_unary`);
- `selfhost/corpus/codegen/164..174_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 39 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a..p ... 174/174`.

Không sửa `src/**`, không rebuild oracle dựng bằng (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62p\`.

## 40. H6.2q — cast surface (`static_cast` / `reinterpret_cast` / `const_cast` / C-style)

#### 40.1 Phạm vi

`static_cast<T>(x)`, `reinterpret_cast<T>(x)`, `const_cast<T>(x)` đã có từ
trước; C-style `(type)expr` thì chưa — `(int32)x` bị hiểu là biểu thức
trong ngoặc nên selfhost báo lỗi parse. Stage q bổ sung nhánh C-style và
nối flag 3 (CStyle) xuyên HIR/MIR/emitter.

- `selfhost/parsing/parser.ivy`:
  - thêm `lexeme_prefix()` và `is_cstyle_cast_type()` mirror
    `isCStyleTypeKeyword` + danh sách built-in của oracle:
    `unsigned/signed/char/short/int/long/float/double`, `bool`, `void`,
    tiền tố `int`/`uint`/`float`, `bfloat16(_t)`, `size(_t)`, `ptrdiff_t`,
    `iptr`, `uptr`, `nullptr_t`, `max_align_t`, hoặc identifier thuộc
    `token_is_type_name` (struct/enum/alias/template param). `const`
    **không** nằm trong danh sách — `(const int32)x` vẫn là paren-expr và
    lỗi `unexpected token in expression` như oracle.
  - trong `parse_primary`, nhánh `(` kiểm tra cast **trước** paren-expr:
    `next()`, `parse_type()`, expect `)`, operand `parse_unary()` (cast
    bám chặt hơn binary), node kind 20 `flag = 3`, loc = token `(`.
- `selfhost/hir/hir_builder.ivy`: `build_cast` nhận `flag == 1 || flag == 3`,
  `require_unsafe` với `"reinterpret_cast"`/`"C-style cast"`, dùng chung
  rules ptr↔ptr / int→ptr / ptr→int / numeric↔numeric, và lỗi luôn in tiền
  tố `reinterpret_cast: cannot convert from 'X' to 'Y'` (kể cả C-style).
  Operand được build trước khi kiểm tra cast (giữ thứ tự chẩn đoán oracle).
- `selfhost/mir/mir_builder.ivy` (`ex_cast`) và `selfhost/codegen/emitter.ivy`
  (`lower_cast`, `emit_cast`) đã phủ mọi chuyển đổi cần thiết từ trước —
  không sửa.

#### 40.2 Probe

`cmp.ps1` (oracle vs `cgdrive.exe`) trên `...\h62q\probe` (c01–c32),
`...\h62q\probe2` (d01–d16), `...\h62q\probe3` (e01–e10): MATCH toàn bộ
phần cast, gồm:

- numeric: trunc/ext, int↔float, unsigned↔signed, `bool`, nested cast;
- pointer: `(char*)p`, `(int32*)p`, `(void*)p`, `int64←ptr`, `ptr←int`,
  `ptr←0`, `ptr↔ptr` khác base, `ptr[i]` operand, double pointer;
- static_cast: numeric, enum↔int, `nullptr→ptr`, `void*` up/down,
  ptr↔int, float↔int;
- reinterpret_cast: ptr↔ptr, int↔ptr, numeric;
- const_cast: bỏ/thêm const trên pointer và value cùng base;
- precedence: `(int32)a + b` khác `(int32)(a+b)`, `-(int32)x`,
  `(int32)-x`, `(int32)x++`, `(int32)(a?b:c)`, `(int32)*p`, `(int32)p.x`;
- diagnostics: C-style ngoài `unsafe` (`C-style cast requires an unsafe
  block`), `(int)x` không có `#pragma ivy cnumber`, `(const int32)x`,
  `(int32*)nullptr`, enum→int/int→enum, float→ptr, ptr→struct, `(void)x`.

Ngoại lệ duy nhất: `d11`/`e08` lấy `&struct` — oracle segfault
(0xC0000005) nên không emulate (cùng nhóm `v10`).

#### 40.3 Fixture mới (175–184)

| Fixture | Nội dung |
|---|---|
| `175_cstyle_numeric.ivy` | C-style numeric: widen/narrow, int↔float, `bool`, `uint32` |
| `176_cstyle_ptr.ivy` | `(char*)p`, `(void*)p`, `(int32*)v`, ptr→int, int→ptr, `ptr←0` |
| `177_static_cast_numeric.ivy` | `static_cast` int/uint/float/float32 |
| `178_static_cast_enum_ptr.ivy` | enum↔int, `void*`, `nullptr→ptr`, ptr↔int |
| `179_reinterpret_cast.ivy` | ptr↔ptr khác base, int↔ptr, numeric |
| `180_const_cast.ivy` | bỏ/thêm const trên pointer và value |
| `181_cstyle_precedence.ivy` | cast chỉ bám unary: `+`, `*`, unary `-`, nested |
| `182_cstyle_nested.ivy` | cast lồng, `uint32`, `int8` |
| `183_cstyle_control_flow.ivy` | cast trong `while`/`if`/`return`/call arg |
| `184_cstyle_unsigned.ivy` | `zext` unsigned và `trunc` `int8`/`uint16`/`uint8` |

#### 40.4 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62q\cg2` (2026-11-11):

```text
[3/4] running 184 fixtures ...
...
[4/4] summary
      PASS 184 / 184
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

`interpdiff.ps1` vẫn `PASS 23 / 23`; `build_smoke.ps1` vẫn PASS. 13 example
đích của p khớp oracle từng byte.

Trên 65 example được oracle chấp nhận: **29 MATCH** (trước q: 26). Ba file
mới khớp byte là `test_cast`, `test_int_convert`, `test_unsafe_a2`; không có
file nào bị regress. 36 DIFF còn lại thuộc các stage sau: `test_print` (r),
`test_move_*`/`test_d4_move` (z), `test_methods`/`test_ctor_dtor`/
`test_raii_break` (s), `test_operator_overloading` (t),
`test_concepts`/`test_template_struct`/`test_variadic` (u/v),
`test_structured_binding` (w), `test_range_for` (x), `test_inherit*` (y),
`test_b5_*`/`test_b6_*`/`test_d1..d7`/`test_designated_init`/
`test_return_value`/`test_module_math`/`test_ivy_ext` (aa).

#### 40.5 Trạng thái H6.2q

Tập thêm/sửa:

- `selfhost/parsing/parser.ivy` (`lexeme_prefix`, `is_cstyle_cast_type`,
  nhánh C-style trong `parse_primary`);
- `selfhost/hir/hir_builder.ivy` (`build_cast` nhận flag 3);
- `selfhost/corpus/codegen/175..184_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 40 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a..q ... 184/184`.

Không sửa `src/**`, không rebuild oracle dựng bằng (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62q\`.

## 41. H6.2r — `ivy::print*` và io file builtins

#### 41.1 Phạm vi

Stage r phủ bốn nhóm builtin còn lại của `isCodegenBuiltin`:

1. **`ivy::print` / `ivy::println`** — dispatch theo type của **arg đầu**:
   - `pointerDepth > 0` → `str`; `char`/`int8_t`/`uint8_t` → `char`;
     họ float (`float`, `double`, `float32_t`, `float64_t`, `float16_t`,
     `float128_t`, `bfloat16_t`, `pointerDepth == 0`) → `float`; còn lại
     (kể cả `bool` và enum) → `int`.
   - symbol: `@ivy_print_<suffix>` hoặc `@ivy_println_<suffix>`; type của
     tham số là `valueLlvmType` (reference decay), giống oracle.
   - không có arg → oracle trả chuỗi rỗng: **không** emit call và **không**
     cấp `%tmp`; các arg thứ 2 trở đi cũng không được lower (chỉ arg[0]).
2. **8 tên explicit** `ivy::print_int|float|str|char`,
   `ivy::println_int|float|str|char` — đi nhánh builtin generic:
   `::` → `_` nên symbol là `ivy__print_int` (hai dấu gạch dưới,
   bug-compat), không mangling, arg giữ type value, return type lấy từ HIR
   (`builtin_ret`: void cho print*, `int32` cho `io::write_file`, `char*`
   cho `io::read_line`/`io::read_file`).
3. **`io::read_line`** — `%tmp.N = call ptr @__ivy_read_line()`.
4. **`io::write_file`** — lower lần lượt arg 0/1/2 (thiếu thì dùng literal
   `null`/`null`/`0` như oracle), rồi cấp temp kết quả và emit
   `call i32 @__ivy_write_file(ptr, ptr, i64)`.
5. **`io::read_file`** — lower path trước, cấp `%tmp.N` cho outLen và
   splice `%tmp.N = alloca i64` vào entry block (sau các slot param/local,
   trước `br label %bb0`), sau đó
   `%tmp.M = call ptr @__ivy_read_file(ptr path, ptr slot)`.

Declares:

- `usesIvyPrint_` được set khi callee bắt đầu bằng `ivy::` **và** là
  builtin; khi đó emit đủ 8 dòng `declare void @ivy_print_int(i32)` …
  `@ivy_println_char(i8)` — đặt sau `emit_struct_defs()` và **trước** vòng
  `extern "C"` declaration (mirror `CodeGen::emitGlobals`).
- `usesIoReadLine_` → `declare ptr @__ivy_read_line()`;
  `usesIoFile_` (dùng `io::read_file` **hoặc** `io::write_file`) → emit cả
  `declare ptr @__ivy_read_file(ptr, ptr)` và
  `declare i32 @__ivy_write_file(ptr, ptr, i64)`. Hai khối này phát sau
  `emit_io_runtime()`, trước `emit_string_constants()` (mirror
  `emitIoD6Runtime`).

Splice entry alloca: oracle lower function vào `ostringstream` rồi mới in,
nên alloca phát sinh giữa chừng vẫn nằm đúng entry. Emitter buffer
**prologue** (A), **body** (B) và **alloca phát sinh** (X) cho mọi function
rồi flush theo thứ tự A, X, B. Cơ chế bật cho tất cả function
(byte-neutral; đã chứng minh bằng 184 fixture trước r và 191 sau r), và mọi
helper emit chuyển từ `io::print` trực tiếp sang `out_str`/`out_long`/
`out_char` (qua `ps`/`pl`/`nl`) để capture hoạt động.

#### 41.2 Probe

`cmp.ps1` (oracle vs `cgdrive.exe`) trên `...\h62r\probe2`:

```text
MATCH r01_readfile   (io::read_file, splice alloca)
MATCH r02_explicit   (5 tên explicit + no-arg)
MATCH r03_types      (bool/int/float/char/str + literal)
MATCH r04_io         (read_line + write_file + printf)
MATCH r05_noargs     (print/println không arg, nhiều arg)
MATCH r06_order      (extern "C" puts + ivy::print_int)
MATCH r07_wfmissing  (write_file 1 và 0 arg)
```

7/7 MATCH, cả trước lẫn sau refactor capture. Bộ fixture 185–191 dưới
`...\h62r\probe3` cũng 7/7 MATCH trước khi copy vào corpus.

#### 41.3 Fixture mới (185–191)

| Fixture | Nội dung |
|---|---|
| `185_ivy_print_types.ivy` | `print`/`println` cho bool/int/float/char/str, literal, no-arg, nhiều arg |
| `186_ivy_print_explicit.ivy` | đủ 8 tên `ivy::print_*`/`ivy::println_*` (symbol `ivy__*`) |
| `187_io_read_line.ivy` | `io::read_line` → `@__ivy_read_line` |
| `188_io_write_file.ivy` | `io::write_file` 3/2/1/0 arg (literal `null`/`0`) |
| `189_io_read_file.ivy` | hai `io::read_file`, numbering temp + splice entry |
| `190_ivy_print_decl_order.ivy` | 8 declares `ivy_print_*` cạnh `extern "C" puts` |
| `191_ivy_print_enum.ivy` | enum (mặc định và `uint8`) vào `ivy::print*` → suffix `int` |

#### 41.4 Kết quả

`cgdiff.ps1 -TempRoot ...\opencode\h62r\cg3` (2026-11-12):

```text
[3/4] running 191 fixtures ...
...
[4/4] summary
      PASS 191 / 191
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

`interpdiff.ps1` vẫn `PASS 23 / 23`; `build_smoke.ps1` vẫn PASS.

Trên 65 example được oracle chấp nhận: **32 MATCH** (trước r: 29). Ba file
mới khớp byte là `test_print`, `test_designated_init`, `test_d6_io`; không
có file nào bị regress. 33 DIFF còn lại thuộc các stage sau:
`test_b5_*`/`test_b6_*`/`test_concepts`/`test_ctor_dtor`/`test_d1..d7`/
`test_methods`/`test_raii_break`/`test_operator_overloading`/
`test_template_struct`/`test_variadic`/`test_structured_binding`/
`test_range_for`/`test_inherit*`/`test_move_*`/`test_return_value`/
`test_module_math`/`test_ivy_ext`.

#### 41.5 Trạng thái H6.2r

Tập thêm/sửa:

- `selfhost/codegen/emitter.ivy`: fields `uses_ivy_print_`/
  `uses_io_read_line_`/`uses_io_file_`; `is_ivy_print_name`,
  `is_ivy_float_type`, `emit_builtin_symbol`, `emit_globals`,
  `emit_io_d6_runtime`; `lower_ivy_print`, `lower_builtin_explicit`,
  `lower_io_read_line`, `lower_io_write_file`, `lower_io_read_file`;
  capture (`cap_*`, `put_long`, `cap_append`, `out_str`/`out_long`/
  `out_char`, `flush_capture`) và `lower_function`/`emit` cập nhật theo;
- `selfhost/corpus/codegen/185..191_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 41 này);
- `PLAN.md` dòng H6.2 -> `◐ H6.2a..r ... 191/191`.

Không sửa `src/**`, không rebuild oracle dựng bằng (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62r\`.

## 42. H6.2s — method, `this`, constructor/destructor và RAII

#### 42.1 Phạm vi

Stage s phủ bốn nhóm còn thiếu của `examples/test_methods`,
`examples/test_ctor_dtor` và `examples/test_raii_break`:

1. **Method mangling** (`emitter.ivy`) — `emit_mangled_name` tách tên theo
   `::` (helpers `name_part_count`/`name_part_off`/`name_part_len`/
   `name_part_eq`, buffer `mangle_buf_`):
   - method thường `?<name>@<scope>@@Y<ret><params>@Z`
     (vd `?sum@Point@@YHAAV@Z`);
   - ctor `?0<Struct>@@AEX<params>@Z` (vd `?0Logger@@AEXAAVH@Z`);
   - dtor `?1<Struct>@@AEXAAV@Z`.
2. **`this`** — MIR expr kind 7: `lower_expr` load giá trị qua con trỏ,
   `lower_lvalue` trả địa chỉ (`load ptr` từ slot `this`). Emitter cũng bỏ
   điều kiện `e.type.is_ref` khỏi nhánh IdentRef của `lower_lvalue`: chỉ
   `is_ref_var(name)` quyết định (mirror `varIsRef_` của oracle) — nhờ vậy
   `thisArg` (biến value nhưng `isRef = true`) trả về địa chỉ slot thay vì
   load.
3. **Constructor injection** (`hir_builder.ivy`) — `build_decl` chèn
   `T::T(&x, args...)` sau khai báo khi `T` có ctor do người dùng khai báo:
   - `T x(args)` / `T x = T(args)` → dùng chính args của call, decl mất
     initializer;
   - `T x;` → ctor mặc định, 0 arg;
   - `T x = other` → 1 arg (copy-construction);
   - `T x = {..}` (InitList) → **bypass** ctor (aggregate), vẫn record dtor.
   Ctor/dtor được nhận diện từ tên qualified do parser sinh
   (`Struct::Struct` / `Struct::~Struct`) qua `name_is_struct_ctor` /
   `name_is_struct_dtor`, không thêm flag AST/HIR/MIR (giữ stride 3 struct).
   `find_ctor` mirror `resolveCtorOverload`: bỏ param[0] (`this`), so
   `is_assignable`, chọn exact-match, ambiguous → -1.
4. **Member initializer list** (`parser.ivy` + `hir_builder.ivy`) — parser
   giờ ghi lại `: field(expr), ...` vào hai pool song song
   `miname_`/`miarg_`; `AFunction` thêm `minit_start`/`minit_n` (208 → 224
   byte, cập nhật cả `fpool_` và `mpool_`). HIR synth `this.field = arg` ở
   đầu body (prefix stmt `pre_first_`/`pre_last_`/`pre_n_`, tiêu thụ bởi
   compound kế tiếp) — đúng thứ tự oracle (assignment trước body gốc).
5. **RAII / destructor** — stack phẳng `dsnm_`/`dscope_` (mirror
   `dtorStacks_`), `dbase_` (mirror `dtorBaseline_`), `rsnm_` (mirror
   `returnedLocals_`):
   - `build_compound` emit dtor của scope hiện tại (thứ tự ngược khai báo)
     trước `leave_scope`;
   - `build_return` emit dtor từ scope trong cùng tới `dbase_`; NRVO đánh
     dấu local được trả về để bỏ dtor; return không phải plain-local được
     đánh giá vào temp `__ret[N]` **trước** các dtor, rồi `load`/`return`;
   - `break`/`continue`/`nextcase` (`wrap_scope_dtors`) chỉ chạy dtor của
     **scope trong cùng** (`dtorStacks_.back()`), rồi truncate stack —
     bug-compat: `break` trong `if` lồng không chạy dtor của loop body
     (đúng IR oracle của `test_raii_break`).
6. **Ctor call dạng biểu thức** — `build_call` nhánh ident: nếu tên là
   struct có ctor (và không phải function/template) thì resolve ctor, callee
   thành `Struct::Struct`, type là struct (mirror `buildCall` 3097-3117).
   Trước đây nhánh này báo "undeclared identifier 'Logger'".

#### 42.2 Probe

`cmp.ps1` (oracle vs `cgdrive.exe`) trên `...\h62s\probe`:

```text
MATCH test_ctor_dtor  (6552 B)
MATCH test_methods    (4092 B)
MATCH test_raii_break (4922 B)
```

`hirdrive.exe` (selfhost) vs `ivy --hir` trên cùng 3 example: thân HIR khớp
byte (chỉ khác dòng header `=== <path> ===` do hirdrive in).

#### 42.3 Fixture mới (192–198)

| Fixture | Nội dung |
|---|---|
| `192_ctor_dtor.ivy` | ctor member-init list + dtor, hai scope lồng (thứ tự dtor) |
| `193_ctor_overload.ivy` | ctor mặc định + tham số, hai field init list |
| `194_ctor_agg_bypass.ivy` | `Pair p = {7, 8}` bypass ctor, vẫn chạy dtor |
| `195_raii_break.ivy` | `break` trong `if` lồng → dtor scope trong cùng |
| `196_raii_continue.ivy` | `continue` + dtor loop body |
| `197_dtor_return_temp.ivy` | return biểu thức (`g.n + 1`) → temp `__ret` trước dtor |
| `198_dtor_nrvo.ivy` | `return b` local → NRVO, không emit dtor trên đường return |

#### 42.4 Kết quả

`cgdiff.ps1` (2026-11-12):

```text
[3/4] running 198 fixtures ...
[4/4] summary
      PASS 198 / 198
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

`indep_verify.ps1` (build lại từ đầu trong scratch riêng, `...\h62verify`):
`INDEP PASS 198/198`. `interpdiff.ps1`: `PASS 23 / 23`.
`build_smoke.ps1`: PASS (`h61 smoke ok`).

Trên 65 example được oracle chấp nhận: **35 MATCH** (trước s: 32). Ba file
mới khớp byte là `test_methods`, `test_ctor_dtor`, `test_raii_break`;
không file nào regress. 30 DIFF còn lại thuộc stage sau:
`test_b5_*`/`test_b6_*`/`test_d1..d7`/`test_d4_*`/`test_move_*` (z),
`test_operator_overloading` (t), `test_concepts`/`test_template_struct`/
`test_variadic` (u/v), `test_structured_binding` (w), `test_range_for` (x),
`test_inherit*` (y), `test_return_value`/`test_module_math`/`test_ivy_ext`
(aa).

#### 42.5 Trạng thái H6.2s

Tập thêm/sửa:

- `selfhost/parsing/ast.ivy`: `AFunction.minit_start`/`minit_n`;
- `selfhost/parsing/parser.ivy`: pool `miname_`/`miarg_`/`mi_n_`/`mi_cap_`
  (+alloc/free), `parse_ctor_inits` ghi lại entry, ctor record lưu slice,
  `ctor_init_name`/`ctor_init_arg`, `fpool_`/`mpool_` 224 byte;
- `selfhost/hir/hir_builder.ivy`: fields `dsnm_`/`dscope_`/`dbase_`/
  `rsnm_`/`dtor_*`/`pre_*`; `name_is_struct_ctor`/`name_is_struct_dtor`/
  `struct_has_ctor`/`find_dtor`/`find_ctor`/`is_returned_local`/
  `mark_returned_local`/`record_dtor_var`/`build_dtor_stmt`/
  `emit_dtor_calls`/`synth_ret_name`/`build_ctor_call`/
  `build_ctor_member_assign`/`wrap_scope_dtors`; cập nhật `build_body`,
  `build_compound`, `build_decl`, `build_return`, `build_call`, `build_stmt`
  kind 9/10/11;
- `selfhost/codegen/emitter.ivy`: `emit_mangled_name` + helpers `::`,
  ctor/dtor prefix, `this` (kind 7), `lower_lvalue` chỉ dùng `is_ref_var`;
- `selfhost/corpus/codegen/192..198_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 42 này);
- `PLAN.md` dòng H6.2 → `◐ H6.2a..s ... 198/198`.

Không sửa `src/**`, không rebuild oracle (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62s\` (và `h62verify`,
`h62r` cho verify độc lập).

## 43. H6.2t — operator overloading

#### 43.1 Phạm vi

Mục tiêu là `examples/test_operator_overloading.ivy` (struct `Vec3` với
`operator+`, `operator==`, unary `operator-`, `operator[]`, `operator()`).
Port đã có sẵn phần lớn đường HIR từ các stage trước:

- `try_operator_overload` + `find_operator_owner` (kế thừa qua base);
- nhánh Unary, Binary, Index gọi `try_operator_overload` với `+`/`==`/`-`/
  `[]`… khi operand là struct value (`ptr_depth == 0`);
- `err_no_operator` in đúng `struct 'X' has no matching operator 'op'`.

Ba phần còn thiếu của stage t:

1. **`operator()` trong `build_call`** (`hir_builder.ivy`) — mirror
   `hir_builder.cpp:2926-3039`. Probe chạy sau builtin `move()` và trước
   template-id/ctor/overload thường, chỉ khi callee là `IdentRef` và tên
   không phải function/template/struct-có-ctor. Callee được build để lấy
   type; nếu `base` là struct thì:
   - 0/1 arg → `try_operator_overload(callee, "()", rhs)`;
   - >1 arg → dựng Call trực tiếp từ `find_operator_owner` +
     `resolve_overload` trên danh sách `[this, args...]`;
   - không match → `err_no_operator(..., "()")` và giữ call dummy.
   Port chỉ build callee khi tên resolve được (`find_sym` hoặc
   `find_global_enum`), để nhánh sau vẫn in `undeclared identifier` đúng
   một lần như oracle.
2. **Escape tên operator trong mangling** (`emitter.ivy`) —
   `emit_name_part` mirror `CodeGen::escapeLlvmIdent`: `<`, `>`, `,`,
   space, `:`, `&`, `*`, `(`, `)` → `_`. Nhờ vậy `Vec3::operator()`
   mangle thành `?operator__@Vec3@@YHAAV@Z` (đúng byte oracle), còn
   `operator+`/`operator[]` giữ nguyên ký hiệu.
3. **Materialize call trả struct by value khi dùng làm lvalue**
   (`emitter.ivy`, `lower_lvalue` kind 11) — mirror
   `CodeGen::lowerLValue` nhánh Call: nếu call trả struct by value thì
   `lower_expr` lấy value, cấp `%tmp.N = alloca %struct.T` trong entry
   block (phase 3 của capture), `store` value vào slot rồi trả slot làm
   địa chỉ. Nhờ vậy `a + b + c`, `make() + b`, `(a + b)[1]` có `this`
   hợp lệ cho operator ngoài.

#### 43.2 Probe

- `examples/test_operator_overloading.ivy`: oracle `--hir` và `--llvm`
  dump; body HIR của port khớp byte-for-byte sau khi thêm probe.
- `operator[]` trả `T&` rồi gán `a[0] = 42`: oracle nhận, port khớp
  (đi qua `lower_call_ptr` trong `lower_lvalue`).
- `make() + b`, `(a + b)[1]`: oracle nhận, port khớp (materialization).
- `a += b`: **cả hai** báo `compound assignment '+=' expects numeric
  operands` (không hỗ trợ operator compound).
- `(a + b)()`: **cả hai** báo `callee must be a function name` ở parser.

#### 43.3 Fixture mới (199-206)

| Fixture | Nội dung |
| --- | --- |
| `199_operator_binary.ivy` | `operator+` trả struct, `operator==` trả bool, chuỗi `a + b + c` |
| `200_operator_unary.ivy` | unary `operator-` |
| `201_operator_index.ivy` | `operator[](int32)` trả `int32` |
| `202_operator_call.ivy` | `operator()` 0 tham số |
| `203_operator_call_arg.ivy` | `operator()` 1 tham số |
| `204_operator_call_multi.ivy` | `operator()` 2 tham số (nhánh multi-arg) |
| `205_operator_index_ref.ivy` | `operator[]` trả `int32&`, gán `a[0] = 42` |
| `206_operator_call_result.ivy` | call trả struct làm receiver (`make() + b`, `(b + c)[1]`) |

#### 43.4 Kết quả

`cgdiff.ps1` (2026-11-12):

```text
[3/4] running 206 fixtures ...
[4/4] summary
      PASS 206 / 206
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

`indep_verify.ps1` (build lại từ đầu trong scratch riêng):
`INDEP PASS 206/206`. `interpdiff.ps1`: `PASS 23 / 23`.
`build_smoke.ps1`: PASS (`h61 smoke ok`).

Trên 65 example được oracle chấp nhận: **38 MATCH** (trước t: 35). Ba
file mới khớp byte là `test_operator_overloading`, `test_concepts` và
`test_ivy_ext`; không file nào regress. 27 DIFF còn lại thuộc stage sau:
`test_b5_*`/`test_b6_*`/`test_d1..d7`/`test_d4_*`/`test_move_*` (z),
`test_template_struct`/`test_variadic` (u/v), `test_structured_binding`
(w), `test_range_for` (x), `test_inherit*` (y),
`test_return_value`/`test_module_math` (aa).

#### 43.5 Trạng thái H6.2t

Tập thêm/sửa:

- `selfhost/hir/hir_builder.ivy`: probe `operator()` trong `build_call`
  (0/1 arg qua `try_operator_overload`, multi-arg dựng Call trực tiếp);
- `selfhost/codegen/emitter.ivy`: `emit_name_part` escape ký tự LLVM
  không hợp lệ; `lower_lvalue` kind 11 materialize call trả struct
  by value vào entry-block `%tmp.N`;
- `selfhost/corpus/codegen/199..206_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 43 này);
- `PLAN.md` dòng H6.2 → `◐ H6.2a..t ... 206/206`.

Không sửa `src/**`, không rebuild oracle (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62t\` (và `h62verify`,
`h62r` cho verify độc lập).

## 44. H6.2u — template struct

#### 44.1 Phạm vi

Mục tiêu là `examples/test_template_struct.ivy`: `template <typename T>
struct Box` với field `T value` và hai method `T get()` / `void set(T v)`,
dùng `Box<int32>` trong `main` (khai báo, gọi method, đọc field, cộng kết
quả). So với stage t, phần HIR của port **đã khớp byte** oracle (cả `main`
lẫn `Box<int32_t>::get`/`set`) trước khi sửa: parser đã parse template
struct, `instantiate_struct_template` đã thay kiểu field/param/return và
build method body, `resolve_template_struct_type` đã thay template-id bằng
tên specialization. Gap nằm ở MIR/emitter.

#### 44.2 Nguyên nhân

Oracle `instantiateStructTemplate` (`src/hir/hir_builder.cpp:5516`) gọi
`buildStruct(cloned)`, và `buildStruct` append specialization vào
`hir_->structs` (dòng 653) — cùng đường với struct khai báo thường. MIR
builder copy `hir_.structs` theo thứ tự, nên emitter thấy
`%struct.Box<int32_t>` và resolve được member.

Port chỉ ghi `spc_name_`/`spc_rec_` qua `add_spec` mà **không** export vào
`u.structs`; vì vậy `mu->structs` không có specialization, `find_struct`
trả -1 và emitter rơi vào nhánh "member access on a non-struct", đồng thời
`llvm_type` coi local là `i8` (thiếu `%struct.*`, thiếu zeroinit).

#### 44.3 Sửa

1. **Export specialization vào `u.structs`** (`hir_builder.ivy`,
   `instantiate_struct_template`, ngay sau `add_spec`/`layout_spec`) —
   mirror block export của `build_struct`: tên `rec.name` (= tên mangled
   `Box<int32_t>`), field đã substitute (`cf.type`), giữ đúng thứ tự oracle
   (field type được resolve trước, nên specialization lồng nhau vào bảng
   trước specialization ngoài; method body build sau đó). Cache
   `find_spec` vẫn chặn export trùng.
2. **Escape tên struct type** (`emitter.ivy`) — mirror
   `CodeGen::escapeLlvmIdent` ở hai chỗ:
   - `make_struct_type_name` escape `<`, `>`, `,`, space, `:`, `&`, `*`,
     `(`, `)` → `_` (`Box<int32_t>` → `%struct.Box_int32_t_`);
   - `emit_struct_defs` in định nghĩa qua `make_struct_type_name` thay vì
     `"%struct." + s.name` (trước đó chỉ phần thân dùng tên đã escape nên
     định nghĩa vẫn in tên thô).

#### 44.4 Probe

- `examples/test_template_struct.ivy`: `--hir` port khớp oracle (trước và
  sau sửa); `--llvm` khớp byte sau sửa (`%struct.Box_int32_t_ = type { i32
  }`, zeroinit `store %struct.Box_int32_t_ zeroinitializer`, GEP field 0,
  method `?set@Box_int32_t_@@YXAAVH@Z` / `?get@Box_int32_t_@@YHAAV@Z`).
- `Wrap<Box<int32> >` (template struct lồng nhau, cần space trước `>`):
  oracle nhận và port khớp, kể cả `w.outer().value` (method trả struct
  dùng làm receiver của member access).
- `Box<int32> a = { 5 };` (aggregate init specialization): khớp.
- `Box<int64>` với literal `5000000000` + `%lld`: khớp.
- `b = a` (gán toàn struct specialization): khớp.
- `void copy(Box<T> o)` (tham số là template-id của chính template): khớp.
- `Box<int32_t>`/`int64_t` trong source bị cả hai từ chối
  (`_t-suffixed type ... is removed`) — dùng `int32`/`int64`.

#### 44.5 Fixture mới (207-214)

| Fixture | Nội dung |
| --- | --- |
| `207_template_struct_basic.ivy` | `Box<int32>` get/set + field access |
| `208_template_struct_two_params.ivy` | `Pair<int32, int32>` (2 type param), method trả `A`/`B` |
| `209_template_struct_method_call.ivy` | method gọi method khác (`twice()` gọi `get()`), 2 biến |
| `210_template_struct_int64.ivy` | `Box<int64>` + literal lớn + `%lld` |
| `211_template_struct_agg.ivy` | aggregate init `Box<int32> a = { 5 };` |
| `212_template_struct_nested.ivy` | `Wrap<Box<int32> >`, method trả struct lồng, `w.outer().value` |
| `213_template_struct_assign.ivy` | gán toàn struct `b = a` |
| `214_template_struct_param.ivy` | tham số `Box<T>` trong method (`copy(Box<T> o)`) |

#### 44.6 Kết quả

`cgdiff.ps1` (2026-11-13):

```text
[3/4] running 214 fixtures ...
[4/4] summary
      PASS 214 / 214
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

`indep_verify.ps1` (build lại từ đầu trong scratch riêng):
`INDEP PASS 214/214`. `interpdiff.ps1`: `PASS 23 / 23`.
`build_smoke.ps1`: PASS (`h61 smoke ok`).

Trên 65 example được oracle chấp nhận: **39 MATCH** (trước u: 38).
File mới khớp byte là `test_template_struct`; không file nào regress.
26 DIFF còn lại thuộc stage sau: `test_b5_*`/`test_b6_*`/`test_d1..d7`/
`test_d4_*`/`test_move_*` (z), `test_variadic` (v),
`test_structured_binding` (w), `test_range_for` (x), `test_inherit*` (y),
`test_return_value`/`test_module_math` (aa).

#### 44.7 Trạng thái H6.2u

Tập thêm/sửa:

- `selfhost/hir/hir_builder.ivy`: export specialization vào `u.structs`
  trong `instantiate_struct_template`;
- `selfhost/codegen/emitter.ivy`: escape tên struct type trong
  `make_struct_type_name` và dùng nó ở `emit_struct_defs`;
- `selfhost/corpus/codegen/207..214_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 44 này);
- `PLAN.md` dòng H6.2 → `◐ H6.2a..u ... 214/214`.

Không sửa `src/**`, không rebuild oracle (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62u\` (và `h62s`, `h62n`,
`h62r`, `h62verify` cho verify độc lập).

## 45. H6.2v — variadic template

#### 45.1 Phạm vi

Mục tiêu là `examples/test_variadic.ivy`: `template <typename... Args>`
với pack param `Args... args`, `sizeof...(args)`, và bốn dạng fold:
unary right `(args + ...)`, unary left `(... + args)`, binary
`(init + ... + args)`, logic `(args && ...)` / `(args || ...)`, cùng
comma fold `(print_int(args), ...)`.

Trước stage v, parser đã parse template param variadic
(`ATemplateParam.is_variadic`), pack expansion `expr...` (AST kind 17)
và `instantiate_template` đã sinh các param `__args0..`, splice pack
expansion trong danh sách argument của call. Còn thiếu `sizeof...`,
fold expression, cách build HIR của chúng và comma operator ở emitter.

#### 45.2 Nguyên nhân

Oracle parse `sizeof...` thành `Expr::SizeofPack` và fold thành
`Expr::FoldExpr` (`src/parsing/parser.cpp:2920-2932`, `3117-3157`).
HIR builder fold `SizeofPack` thành `IntegerLit` (`int64_t`,
`hir_builder.cpp:3731-3742`) và expand `FoldExpr` thành một chuỗi
`Binary` left-to-right bằng cách clone pattern AST rồi thay ident của
pack bằng tên param phần tử (`hir_builder.cpp:3858-4060`). Port chưa có
ba kind AST tương ứng nên `sizeof...` bị parse như `sizeof(...)`, còn
`(args + ...)` bị parse như biểu thức thường (lỗi cú pháp).

#### 45.3 Sửa

1. **`parser.ivy`** — thêm hai kind AST:
   - kind 24 = `sizeof...(pack)`: `sval` là tên pack, nhận diện bằng
     `kw("sizeof") && kind_eq(peek(1), "...")` **trước** nhánh
     `sizeof(type)`;
   - kind 25 = fold expression: `sval` là toán tử, `a`/`b` là hai toán
     hạng (-1 khi vắng), `flag` = 1 cho unary-left. `fold_operator()`
     mirror `Parser::foldOperator` (đủ `+ - * / % ^ & | << >> && || ==
     != < > <= >= ,`). Nhánh speculative (parse_unary rồi mới biết có
     phải fold) lưu/khôi phục cả `pos_` **và** `dn_` — oracle lưu
     `diagnostics_.size()` để bỏ chẩn đoán của lần parse thử.
2. **`ast.ivy`** — cập nhật comment kind 17 (pack expansion), 24, 25.
3. **`hir_builder.ivy`**:
   - kind 24 → `IntegerLit` kiểu `int64_t`, đếm qua `pack_find` (0 khi
     pack không xác định);
   - kind 25 → `build_fold`: `expr_pack_idx` mirror
     `findPackExpansion` (chỉ trả index khi ident thật sự là pack đã
     biết; call chỉ tìm trong arguments; pack expansion chỉ tìm trong
     pattern nếu pattern là bare ident); pattern được build lại một lần
     cho mỗi phần tử với substitution `fold_pack_name_` /
     `fold_elem_name_` / `fold_elem_type_` (thay vì clone AST);
     `pack_first_` được chụp ngay sau `pack_find` (trước khi build
     init); chain `Binary` left-to-right với type = type phần tử pack
     đầu tiên (bug-compat oracle); empty pack → `true`/`false` cho
     `&&`/`||`, còn lại `int64` 0 — kể cả binary fold có init
     (bug-compat);
   - trạng thái substitution lưu/khôi phục qua stack `AType`
     (`fold_pstack_`/`fold_estack_`/`fold_tstack_`) vì subset không cho
     khai báo `const char*` local ngoài `unsafe` và không hỗ trợ cast
     `long` → pointer; tên pack/phần tử nằm ở field `base` của `AType`.
4. **`emitter.ivy`** — nhánh comma operator: lower lhs (vì side effect)
   rồi trả rhs, mirror `CodeGen` `Binary` nhánh `,`
   (`src/codegen/codegen.cpp:1298-1302`).

#### 45.4 Probe

- `--hir` oracle của `test_variadic`: pack param đổi tên `__args0..`,
  `sizeof...` → IntLit, fold → chuỗi `Binary`; `sum<>` → `int 0`;
  `print_all` → `binary ',' : int` với các call `print_int`.
- `--llvm` của `test_variadic` khớp byte (11,013 bytes), kể cả mangling
  `count_args<>`, `count_args<int,int,int>`, `count_after<int,int>`,
  `sum_from<int,int,int>`.
- Bug-compat đã xác nhận với oracle: `g(args...)` trực tiếp trong call
  → `undeclared identifier 'args'` (exit 1) nhưng vẫn splice; empty
  binary fold có init vẫn trả identity.

#### 45.5 Fixture mới (215-222)

| Fixture | Nội dung |
| --- | --- |
| `215_variadic_sizeof.ivy` | `sizeof...(args)` với pack 0/1/3 phần tử |
| `216_variadic_fixed_pack.ivy` | `typename T` + `typename... Rest`, `sizeof...(rest)` |
| `217_variadic_fold_sum.ivy` | `(args + ...)`, `(... + args)`, `(init + ... + args)` |
| `218_variadic_fold_logic.ivy` | `(args && ...)` / `(args \|\| ...)` |
| `219_variadic_comma.ivy` | comma fold `(print_int(args), ...)` |
| `220_variadic_mixed.ivy` | fold `+` trả `long long` (`%lld`) |
| `221_variadic_empty.ivy` | pack rỗng: `sum()`, `all_true()`, `any_true()` |
| `222_variadic_binary_init.ivy` | binary fold có init, gồm cả pack rỗng |

#### 45.6 Kết quả

`cgdiff.ps1` (2026-11-13):

```text
[3/4] running 222 fixtures ...
[4/4] summary
      PASS 222 / 222
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

`indep_verify.ps1` (build lại từ đầu trong scratch riêng):
`INDEP PASS 222/222`. `interpdiff.ps1`: `PASS 23 / 23`.
`build_smoke.ps1`: PASS (`h61 smoke ok`).

Trên 65 example được oracle chấp nhận: **40 MATCH** (trước v: 39).
File mới khớp byte là `test_variadic`; không file nào regress.
25 DIFF còn lại thuộc stage sau: `test_b5_*`/`test_b6_*`/`test_d1..d7`/
`test_d4_*`/`test_move_*` (z), `test_structured_binding` (w),
`test_range_for` (x), `test_inherit*` (y),
`test_return_value`/`test_module_math` (aa).

#### 45.7 Trạng thái H6.2v

Tập thêm/sửa:

- `selfhost/parsing/parser.ivy`: kind 24 (`sizeof...`), kind 25 (fold),
  `fold_operator()`, lưu/khôi phục `dn_` ở nhánh speculative;
- `selfhost/parsing/ast.ivy`: comment kind 17/24/25;
- `selfhost/hir/hir_builder.ivy`: kind 24/25 trong `build_expr`,
  `build_fold`, `expr_pack_idx`, substitution `fold_*` trong
  `build_ident`, stack lưu trạng thái;
- `selfhost/codegen/emitter.ivy`: nhánh comma operator;
- `selfhost/corpus/codegen/215..222_*.ivy` (mới);
- `selfhost/codegen/H62_NOTES.md` (mục 45 này);
- `PLAN.md` dòng H6.2 → `◐ H6.2a..v ... 222/222`.

Không sửa `src/**`, không rebuild oracle (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h62v\` (và `h62s`, `h62n`,
`h62r`, `h62verify` cho verify độc lập).

