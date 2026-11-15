# H7.2 - `import` resolution + search paths (`imports.ivy`, `main.ivy`)

## 1. Phạm vi

H7.2 port phần đầu của `run(...)` trong `src/app/main.cpp` (dòng 825-1019)
cùng `ivy::resolveModulePath` (`src/parsing/module_io.cpp:478-495`):

1. auto thêm `lib/` vào include search path (`main.cpp:1547-1565`);
2. mở file nguồn — chẩn đoán `ivy: error: cannot open file '<path>'` (exit 2);
3. lexer → preprocessor, in chẩn đoán pp đúng format (`diagFile:line:col:
   error|warning: msg`);
4. pre-scan token **sau pp** tìm `import`, resolve từng module thành `.ivm`,
   báo module thiếu (`ivy: error: module '<name>' not found (searched .ivm
   files in include paths)`, exit 1).

**Không** port `readModuleInterface` (H7.3) và parser trở đi. Vì vậy scan
thành công vẫn dừng ở stub "dispatch not ported" (exit 3), trừ khi bật
`IVY_IMPORT_DUMP=1` (in include path + import đã resolve).

Deliverables:

- `selfhost/app/imports.ivy`: `ImportRec` + `Imports` (resolve, scan, dump,
  auto `lib/`);
- `selfhost/app/main.ivy`: nối parse → auto lib → open → lex → pp → scan;
- `selfhost/app/cli.ivy`: thêm `out_preprocessed()` + `print_path_quoted()`;
- `selfhost/app/build_import_smoke.ps1`: harness H7.2;
- `selfhost/app/build_cli_smoke.ps1`: cập nhật build module + cwd dump + stub;
- `selfhost/app/H72_NOTES.md` (file này);
- `PLAN.md` dòng H7.2.

Không sửa `src/**`, không rebuild oracle (27,031,552 byte, mtime
`2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`).

## 2. Auto include path `lib/`

Oracle (`main.cpp:1550-1565`) lấy `fs::current_path()` rồi thử lần lượt:

```text
<cwd>/lib
<cwd>/../lib
<cwd>/../../lib
```

và **append** path đầu tiên tồn tại vào `includePaths` (sau các `-I` tường
minh). `Imports.auto_lib_dir()` mirror đúng điều đó:

- `cwd()` = `_getcwd(buf, 4096)` (absolute, backslash);
- `exists(p)` = `_access(p, 0) == 0` (đúng cho cả file và thư mục);
- path **không** được normalize: `<cwd>/../lib` cho ra chuỗi literal
  `...\a\..\lib`, y như `std::filesystem::path` (oracle cũng in/giữ `..`).

`main.ivy` gọi `im.auto_lib_dir()` ngay sau `parse()` rồi
`args.push_include(libdir)`. Vì vậy dump H7.1 (`IVY_CLI_DUMP=1`) giờ có thêm
dòng `include=<...>\lib` khi thư mục đó tồn tại; `build_cli_smoke.ps1` chạy
các dump case trong cwd không có `lib/` (kể cả `../lib`, `../../lib`) nên text
kỳ vọng của H7.1 không đổi.

## 3. `resolveModulePath`

`Imports.resolve(name, dirs)` mirror `module_io.cpp:478-495`:

1. từng `-I` theo thứ tự: `dir / (name + ".ivm")`;
2. `<cwd>/<name>.ivm`;
3. `<cwd>/std/<name>.ivm`.

Candidate đầu tiên `exists` thắng; không có ⇒ trả null.

`join(dir, name, ext)` mirror `std::filesystem::path::operator/` trên Windows:

| trường hợp | kết quả |
| --- | --- |
| `name` absolute (`\x`, `/x`, `C:\x`, `C:/x`) | `name + ext` (bỏ `dir`) |
| `dir` rỗng | `name + ext` |
| `dir` kết thúc `\` hoặc `/` | `dir + name + ext` |
| `dir` kết thúc `:` (bare drive) | `dir + name + ext` |
| còn lại | `dir + "\" + name + ext` |

Hệ quả đã test: `-I ""` resolve module `cc` thành `cc.ivm` (relative, không có
dấu `\`), khớp oracle.

## 4. Pre-scan `import`

`Imports.scan` mirror vòng lặp `main.cpp:931-1017` trên token **sau pp**:

- điều kiện vòng: `i + 2 < n` (stream luôn có `eof`);
- chỉ xét token `keyword` với lexeme `import`;
- bỏ qua `import cpp ...` (token kế là keyword `cpp`);
- bỏ qua `import c ...` khi token sau `c` là `<` hoặc `string`;
- `import "<name>";` → lấy lexeme, bỏ 2 ký tự quote đầu/cuối khi `len >= 2`
  (không unescape; `len < 2` giữ nguyên lexeme);
- `import <ident>` → tên là lexeme của identifier;
- không kiểm tra dấu `;`.

Mỗi tên được resolve; thiếu ⇒ in đúng một dòng ra **stderr**, đặt `failed`,
**tiếp tục** scan (báo hết mọi module thiếu), cuối cùng caller exit 1.

Lưu ý đã kiểm chứng: `import nope` **không** có `;` vẫn báo thiếu, vì stream
sau pp còn token `eof` nên `i + 2 < n` vẫn đúng. Điều kiện chỉ bỏ qua khi
`import` là token áp chót trước `eof` (không xảy ra với dạng `import <name>`).

`import c;` (không có `<`/string sau `c`) **không** được bỏ qua — oracle coi
`c` là tên module và báo `module 'c' not found`; port khớp byte.

## 5. Byte-exactness

- `cannot open file`: oracle stream `std::filesystem::path` qua `operator<<`
  (`std::quoted`), nên path được **bọc nháy kép** và escape `\` + `"`:

  ```text
  ivy: error: cannot open file '"missing.ivy"'
  ivy: error: cannot open file '"C:\\...\\x.ivy"'
  ```

  `CliArgs.print_path_quoted()` tạo buffer đã escape rồi in một lần.
- `module '<name>' not found` dùng `std::cerr << std::string`, **không** quote.
- Chẩn đoán preprocessor dùng `diagFile` (`#line` nếu có) dạng `std::string`,
  **không** quote: `path:line:col: error: ...` / `... warning: ...`; chỉ
  warning không làm fail, error (hoặc `#error`) ⇒ exit 1.
- Thứ tự oracle được giữ: mở file → lexer → pp (in diag) → gate
  `--lex`/`-o *.i|*.ii` → gate `--tokens` → pre-scan import.
- `-I` trước, auto `lib/` sau; `cwd`/`std` fallback dùng path absolute.

## 6. `main.ivy` và gating

Luồng H7.2:

```text
parse() -> auto lib -> IVY_CLI_DUMP? -> gate mode chưa port -> open file
        -> lex_all -> pp.run + in diag -> im.scan -> IVY_IMPORT_DUMP? -> stub
```

Gate (exit 3, chưa port): `--lex`, `-o *.i|*.ii` (`out_preprocessed()`),
`--tokens`. Các mode này chạy **trước** pre-scan trong oracle, nên phải gate
trước khi mở file để lỗi module không che mất chúng.

`IVY_IMPORT_DUMP=1` in:

```text
include=<path>          (mỗi include path, theo thứ tự)
imports=<count>
import=<name> -> <path> (mỗi import đã resolve)
```

Không có env đó và scan thành công ⇒ stub:

```text
ivy: error: selfhost driver dispatch is not ported yet (H7.2: import resolution only)
```

## 7. Harness (`build_import_smoke.ps1`)

Build trong TEMP (chỉ clang, không cần LLVM libs): token/lexer/preprocessor/
cli/imports/main bằng oracle (`-c -o <run>/<m>.obj` sinh luôn `<m>.ivm`) rồi
link với `ivyrt.obj`.

Fixture trong `<run>/w`: module `aa` (`a/`), `bb` (`b/`), `cc` (cwd), `ss`
(`std/`), `kk` (`lib/`), `dup` (`a/` + `b/`); các consumer tương ứng. Module
`.ivm` do oracle sinh.

Nhóm test:

- **10 missing**: ident/string, 2 module thiếu, `import cpp`, `import c
  <...>`, `import c;`, `#if 1`, không `;`, import resolve rồi tới missing,
  và auto-lib không tồn tại — so **raw bytes** (exit/stdout/stderr).
- **17 dump**: `-I` order/precedence (`a` vs `b` cho `dup`), cwd/std/lib
  fallback, `../lib`, `../../lib`, deep no-lib, `#if 0`, comment, thứ tự
  import, duplicate, `-I` không tồn tại, `-I ""`, import sau declaration —
  so dump chính xác + chạy oracle `-c` phải exit 0.
- **2 cannot-open**: relative + absolute, byte-identical.
- **1 stub**: file không import, không env ⇒ exit 3, đúng 1 dòng stderr.
- **2 cli-dump**: `IVY_CLI_DUMP=1` từ `<w>` thấy `include=<w>\lib`; từ thư mục
  sâu không có `lib/` thì không có dòng `include=`.

Cuối harness kiểm tra `selfhost/app` không sinh file mới.

## 8. Kết quả (2026-11-15)

```text
build_import_smoke.ps1: PASS 32 / 32 (10 missing, 17 dump, 2 cannot-open,
                                     1 stub, 2 cli-dump)
build_cli_smoke.ps1:    PASS 67 / 67 (40 oracle, 26 dump, 1 stub)
build_smoke.ps1:        PASS (h61 smoke ok)
build_object_smoke.ps1: object PASS 246 / 246, run PASS 8 / 8
build_link_smoke.ps1:   run PASS 8 / 8, negative PASS 2 / 2, space PASS 1 / 1
```

## 9. Hạn chế (H7.3+)

- **Đọc `.ivm`**: pre-scan mới resolve path; `readModuleInterface` và merge
  declaration là H7.3.
- **Lexer diagnostics**: `Lexer::err` của port in ra **stdout** và không escape
  path; oracle in ra stderr với path được quote/escape. Đây là gap của H7.4
  nên harness tránh fixture lỗi lexer. `main.ivy` chỉ dùng `had_error()` rồi
  exit 1.
- **`pp.hasError()`**: port dựa trên danh sách diagnostic (`diag_warn == 0`)
  như `interpdrive.ivy`; oracle còn cờ `hasError()` cho vài lỗi không phát
  diagnostic — thuộc H7.4.
- **Mode chưa port**: `--tokens`, `--lex`, `-o *.i|*.ii`, `--ast/hir/mir`,
  `--run`, `--llvm`, `-c`, link — vẫn stub exit 3.
- **`_access` vs `exists`**: đủ cho file/thư mục thường; không mô phỏng
  permission/error_code của `std::filesystem`.
- Exit 3 của stub là tạm thời, không phải hành vi oracle.

## 10. Trạng thái

- `selfhost/app/{imports.ivy,build_import_smoke.ps1,H72_NOTES.md}` mới;
  `cli.ivy`, `main.ivy`, `build_cli_smoke.ps1` cập nhật; `PLAN.md` H7.2 → ✅.
- Working tree chỉ có `selfhost/app/` + `PLAN.md`; commit/push chờ yêu cầu
  (gợi ý: `H7.2: import resolution and search paths in selfhost driver`).
