# H7.1 - CLI argument parsing (`cli.ivy`, `main.ivy`)

## 1. Phạm vi

H7.1 port phần đầu của `main()` trong `src/app/main.cpp` (dòng 1464-1611) sang
selfhost:

1. vòng lặp flag (`--tokens`, `--lex`, `--ast`, `--hir`, `--mir`, `--llvm`,
   `--run`, `-c`, `--crt-static`, `--target <abi>`, `-o <file>`, `-I <dir>`,
   `-Idir`, `-h`/`--help`, unknown option, positional);
2. `printUsage()` byte-identical (29 dòng, 1384 byte);
3. toàn bộ chẩn đoán CLI và exit code của oracle;
4. validate extension `.ivy` (missing / legacy C-C++ / unrecognized);
5. các mode suy ra sau vòng lặp: `-o *.o|*.obj` ⇒ `-c`, `-o *.ll` ⇒ `--llvm`,
   `linkMode` mặc định.

**Không** port `run(...)` (mở file → lexer → preprocessor → parser → HIR → MIR →
codegen → link). Dispatch đó là H7.2+; xem mục 6 và 9.

Deliverables:

- `selfhost/app/cli.ivy`: `CliPath` + `CliArgs` (parse, usage, dump);
- `selfhost/app/main.ivy`: entry point lấy argv từ UCRT rồi gọi `CliArgs`;
- `selfhost/app/build_cli_smoke.ps1`: harness CLI smoke;
- `selfhost/app/H71_NOTES.md` (file này);
- `PLAN.md` dòng H7.1.

Không sửa `src/**`, không rebuild oracle (27,031,552 byte, mtime
`2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`).

## 2. argv trong Ivy

`main()` của chương trình Ivy do oracle sinh trong `CodeGen::emitEntryPoint`
(`src/codegen/codegen.cpp:671-697`):

```llvm
define i32 @main() {
  %r = call i32 @ivy_main()
  ret i32 %r
}
```

Tức là chương trình Ivy **không nhận** `argc`/`argv`. H7.1 lấy chúng từ UCRT:

```ivy
extern "C" int*    __p___argc();
extern "C" char*** __p___argv();
```

(đúng như `__argc`/`__argv` trong `<stdlib.h>` của UCRT). Probe xác nhận
`argc`/`argv` (kể cả argument có dấu cách) đến nguyên vẹn; `char***` khai báo
được, `*__p___argv()` trả `char**`, và mọi thao tác con trỏ nằm trong `unsafe`.

## 3. `cli.ivy`

`CliArgs` giữ đúng các biến local của oracle:

| field | oracle |
| --- | --- |
| `file` | `std::filesystem::path file` / `std::string fileArg` |
| `out_path` | `std::string outPath` |
| `includes` / `includes_tail` / `include_count` | `std::vector<std::filesystem::path> includePaths` |
| `show_tokens` .. `show_mir` | `bool showTokens` .. `bool showMir` |
| `show_llvm` | `bool showLlvm` |
| `do_run` | `bool doRun` |
| `compile_only` | `bool compileOnly` |
| `crt_static` | `bool crtStatic` |
| `link_mode` | `bool linkMode` (tính sau vòng lặp) |
| `target` | `std::optional<CodeGen::Platform>` (0 none / 1 itanium / 2 msvc) |

`parse(argc, argv)` trả `-1` khi parse xong (caller tiếp tục), `0` cho
`-h`/`--help`, `2` cho mọi chẩn đoán CLI — khớp `main()` oracle.

Danh sách `-I` là linked list cấp phát bằng `__ivy_alloc` (16 byte/node:
`char*` + `CliPath*`), không free vì process thoát ngay sau khi parse; chuỗi
trỏ thẳng vào argv.

Helper chuỗi (đều mirror `std::string`/`std::string_view` của oracle):

| helper | oracle |
| --- | --- |
| `streq` | `std::string_view ==` |
| `starts_with` | `arg.starts_with("-I")` |
| `is_option` | `!arg.empty() && arg[0] == '-'` |
| `file_empty` / `out_empty` | `path.empty()` / `outPath.empty()` |
| `ext_off` | `find_last_of('.')` (quét cả path, nên dấu chấm trong tên thư mục cũng tính) |
| `ext_eq` | `hasExtension(path, ext)` (case-sensitive) |
| `ext_ieq` | `getExtension(path)` lowercase rồi so sánh |
| `ext_missing` | `getExtension(...).empty()` (không có dấu chấm, hoặc dấu chấm cuối) |

Một khác biệt ngữ nghĩa phải xử lý: oracle dùng `std::string` (không bao giờ
null), còn port dùng `char*`. `ext_off` trả `-1` khi con trỏ null và
`ext_eq`/`ext_ieq` trả `false` ngay — nếu không, `-o` vắng mặt sẽ dereference
null (lỗi này bị harness dump bắt được trong lúc phát triển).

## 4. Byte-exactness

- `printUsage()` ghi ra **stdout**; nhánh unknown option in chẩn đoán ra
  **stderr** rồi gọi lại `printUsage()` nên usage vẫn ra **stdout** (đính chính
  giả định ban đầu là usage ra stderr — byte-compare mới thấy).
- Usage in qua `__ivy_write_str(fd, ...)` với `fd` tường minh, nên chỉ có **một**
  bản copy 29 dòng trong `cli.ivy` dùng cho cả hai luồng.
- `__ivy_write_str` gọi CRT `_write` ở text mode ⇒ `\n` thành CRLF, giống
  `std::cout`/`std::cerr` (oracle `--help`: 1384 byte, 29 CRLF).
- Chẩn đoán extension in **extension đã lowercase** (`foo.CPP` → `'.cpp'`),
  nên `ext_lower_diag` copy lowercase trước khi in.
- `-o ""` được coi như không có `-o` (`std::string::empty()`), nên không suy ra
  `-c` và không tắt `linkMode`.
- `--target` nhận giá trị kế tiếp kể cả khi nó bắt đầu bằng `-`
  (`--target --help` ⇒ `unknown target ABI '--help'`).
- `-I -c` ⇒ include path `-c` (không bật `compileOnly`); `-o -c` ⇒ out path
  `-c` (không suy ra gì, `linkMode=1`).

Chẩn đoán (tất cả exit 2, byte khớp oracle):

```text
ivy: error: option '--target' requires an ABI name
ivy: error: unknown target ABI '<t>' (expected 'itanium' or 'msvc')
ivy: error: option '-o' requires a file name
ivy: error: option '-I' requires a directory
ivy: error: unknown option '<arg>'                       (+ usage ra stdout)
ivy: error: missing file extension — use the .ivy extension
ivy: error: legacy '.<ext>' files are no longer accepted — use the .ivy extension (rename to .ivy; C++ interop via 'import cpp ...')
ivy: error: unrecognized file extension '.<ext>' — expected .ivy
```

## 5. Mode suy ra

| command line | kết quả |
| --- | --- |
| `f.ivy` | `link_mode=1` |
| `-c f.ivy` | `compile_only=1` |
| `-o out.o` / `-o out.OBJ` / `-o out.obj` | `compile_only=1` |
| `-o out.ll` | `llvm=1` |
| `-o out.LL` | `llvm=0`, `link_mode=1` (hasExtension case-sensitive) |
| `-o out.i` / `-o out.ii` | mọi mode = 0 (preprocessed output) |
| `-o out.exe` | `link_mode=1` |
| `--run` / `--llvm` / `--tokens`... | mode tương ứng, `link_mode=0` |
| `a.ivy b.ivy` | file = `b.ivy` (positional cuối thắng, không báo lỗi) |
| `-o ""` | như không có `-o` |

## 6. Debug dump và stub dispatch

- `IVY_CLI_DUMP=1`: `main.ivy` in kết quả parse (13 dòng cố định + mỗi `-I` một
  dòng `include=`), exit 0. Dùng cho harness; không ảnh hưởng oracle.
- Không có env đó: parse thành công ⇒ in đúng một dòng
  `ivy: error: selfhost driver dispatch is not ported yet (H7.1: argument parsing only)`
  ra stderr và exit 3. Đây là stub tạm thời, sẽ bị thay bởi `run(...)` ở H7.2+.

## 7. Harness (`build_cli_smoke.ps1`)

Build (tất cả trong TEMP, không cần LLVM libs — driver chỉ link `ivyrt.obj`):

1. `cli.ivy -I std -c -o <run>/cli.obj` → sinh `cli.ivm` trong run dir;
2. `main.ivy -I <run> -I std -c -o <run>/main.obj`;
3. `clang -c lib/ivyrt.c -I lib -o <run>/ivyrt.obj`;
4. `clang main.obj cli.obj ivyrt.obj -o <run>/main.exe`.

Ba nhóm test:

- **40 oracle cases**: mọi command line khiến oracle thoát *trong lúc parse*
  (usage, help, unknown option, thiếu giá trị, `--target` sai, extension
  missing/legacy/unrecognized, arg rỗng, positional cuối, flag đứng sau file…).
  So sánh stdout/stderr/exit **raw bytes**.
- **26 dump cases**: parse thành công; so dump với text kỳ vọng, đồng thời chạy
  oracle với cùng command line và yêu cầu oracle đi qua parse rồi chết ở
  `run()` với `ivy: error: cannot open file '"missing.ivy"'` (exit 2) — chứng
  minh hai bên parse giống nhau. Lưu ý `std::filesystem::path` được
  `operator<<` in kèm dấu nháy kép.
- **1 stub case**: không set env ⇒ exit 3, stderr đúng một dòng, stdout rỗng.

`Quote-Arg` quote cả argument rỗng (`""`) để test `-o ""` và arg rỗng đi qua
`CreateProcess` đúng. Cuối harness kiểm tra `selfhost/app` không sinh thêm file
(`.ivm`/`.obj`/`.exe`).

## 8. Kết quả (2026-11-15)

```text
build_cli_smoke.ps1: PASS 67 / 67 (40 oracle, 26 dump, 1 stub)
build_smoke.ps1:        PASS (h61 smoke ok)
build_object_smoke.ps1: object PASS 246 / 246, run PASS 8 / 8
build_link_smoke.ps1:   run PASS 8 / 8, negative PASS 2 / 2, space PASS 1 / 1
```

Ví dụ vài case oracle (exit, stdout, stderr):

| command line | exit | stdout | stderr |
| --- | --- | --- | --- |
| (không arg) | 2 | usage 1384 B | 0 B |
| `-h` / `--help` | 0 | usage 1384 B | 0 B |
| `--bogus` | 2 | usage 1384 B | `unknown option '--bogus'` 38 B |
| `--target bogus` | 2 | 0 B | 71 B |
| `-o` / `-I` | 2 | 0 B | 46 B |
| `foo` / `foo.` | 2 | 0 B | 63 B |
| `foo.txt` / `foo.TxT` | 2 | 0 B | 66 B |
| `foo.cpp` / `foo.CPP` | 2 | 0 B | 134 B |
| `-c` / `--crt-static` | 2 | usage 1384 B | 0 B |

## 9. Hạn chế (H7.2+)

- **Auto include path `lib/`**: oracle thêm `<cwd>/lib`, `<cwd>/../lib`,
  `<cwd>/../../lib` (nếu tồn tại) vào `includePaths` (`main.cpp:1547-1565`).
  H7.1 bỏ qua — cần filesystem/exists + search path của H7.2. Vì vậy dump chỉ
  liệt kê `-I` tường minh.
- **Dispatch `run(...)`**: mở file (`cannot open file`), lexer/preprocessor/
  parser/HIR/MIR/codegen, các dump `--tokens`/`--lex`/`--ast`/`--hir`/`--mir`,
  `--run`, `-c`, `-o *.ll`, `-o *.i/.ii`, link mặc định.
- **`--crt-static`**: chỉ được parse/giữ cờ; chưa ảnh hưởng link (H6.4 cũng
  document out-of-subset).
- Exit 3 của stub là tạm thời, không phải hành vi oracle.

## 10. Trạng thái

- `selfhost/app/{cli.ivy,main.ivy,build_cli_smoke.ps1,H71_NOTES.md}` mới;
  `PLAN.md` H7.1 → ✅.
- Working tree chỉ có `selfhost/app/` (untracked); commit/push chờ yêu cầu
  (gợi ý: `H7.1: CLI argument parsing in selfhost driver`).
