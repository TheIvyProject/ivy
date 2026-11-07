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

1. Struct parameter/return: emitter từ chối; mangling struct chưa port.
2. Arrow `p->x` trên con trỏ struct: code có, nhưng parser selfhost từ chối
   `(Point*)0` / `(Point*)__ivy_alloc(...)` nên chưa có fixture.
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
