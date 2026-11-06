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
