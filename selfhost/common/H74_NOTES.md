# H7.4 - Diagnostics (`common/diagnostic.ivy`)

## 1. Phạm vi

Port `src/common/diagnostic.h` (`Diagnostic`) + cách `src/app/main.cpp` in
chẩn đoán ở từng tầng sang `selfhost/common/diagnostic.ivy`, rồi chuyển
lexer / preprocessor / parser / các driver sang dùng chung một collection:

- `Lexer::err` không còn `io::println` trực tiếp: chỉ ghi vào `Diags`;
- `Preprocessor` bỏ pool `dtxt_/dline_/dcol_/dmsg_/dn_/dcap_`, dùng `Diags`,
  đồng thời bổ sung **toàn bộ** diagnostic của `preprocessor.cpp` còn thiếu;
- `Parser` bỏ pool `dline_/dcol_/dmsg_/dn_/dcap_`, dùng `Diags`;
- driver in đúng stream (stderr cho `ivy`, stdout cho driver differential),
  đúng quoting (lexer dùng `std::quoted`, pp/parser dùng plain `diagFile`).

Không sửa `src/**`, không rebuild oracle (27,031,552 byte, mtime
`2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`).

Deliverables:

- `selfhost/common/diagnostic.ivy` - `Diag`, `Diags`, 4 printer;
- `selfhost/common/build_diag_smoke.ps1` - harness H7.4 (52 case);
- `selfhost/parsing/{lexer,preprocessor,parser}.ivy` - dùng `Diags`;
- `selfhost/parsing/{lexdrive,ppdrive,astdrive,ivmdrive}.ivy`,
  `selfhost/app/main.ivy` - in chẩn đoán;
- `selfhost/{difftest,interpdiff}.ps1`,
  `selfhost/app/build_{cli,import}_smoke.ps1`,
  `selfhost/parsing/build_ivm_smoke.ps1`,
  `selfhost/codegen/{cgdiff,build_object_smoke,build_link_smoke}.ps1` - build
  thêm module `diagnostic`;
- các `*drive.ivy` - thêm `import diagnostic;` trực tiếp (xem §8);
- `selfhost/common/H74_NOTES.md` (file này);
- `PLAN.md` dòng H7.4.

## 2. `common/diagnostic.ivy`

```text
Diag  { long line; long col; long warn; char* msg; }   // 32 bytes
Diags { Diag* items_; long n_; long cap_;
        char* txt_;   long txt_n_; long txt_cap_; }
```

- `init(cap)` cấp `items_` (>= 8 slot) + arena text 65536 byte; gọi **một
  lần** cho mỗi instance của tầng (gọi lần hai sẽ leak buffer cũ).
- `add(line, col, msg, warn)` copy chuỗi vào arena; `add_slice(line, col,
  prefix, src, aoff, alen, warn)` copy `prefix` + slice (aoff âm = không có
  suffix); cả hai tăng trưởng động (`ensure_txt`/`ensure_items`), nên không
  còn hard cap làm rơi chẩn đoán cuối như pool cũ.
- `count/line/col/warn/msg`, `has_error()` (có ít nhất một record
  `warn == 0`), `truncate(n)` (parser rollback parse thử), `reset()`,
  `release()`.
- 4 printer, cùng một hàm `emit`:
  - `print(file)` / `print_quoted(file)` -> **stderr** (đường oracle);
  - `print_out(file)` / `print_out_quoted(file)` -> **stdout** (đường driver
    differential, vì harness merge `2>&1`).
- `print_quoted` mirror `std::filesystem::path` operator<< = `std::quoted`:
  bọc `"` và escape `\` / `"`. Chỉ **lexer** dùng dạng này; pp/parser/HIR/MIR
  dùng `diagFile` plain.

Định dạng một dòng (giống oracle):

```text
<file>:<line>:<col>: error: <message>
<file>:<line>:<col>: warning: <message>
```

`#line` remap: `main.cpp` đổi `diagFile` sang tên trong `#line` nhưng **không**
đổi số dòng (số dòng là dòng vật lý), nên `virt.ivy:2:1` cho `#error` ở dòng
vật lý 2.

## 3. Lexer (`lexer.ivy`)

- thêm field `Diags diags_`, `import diagnostic;`;
- `setup` gọi `diags_.init(16)`; `release` gọi `diags_.release()`;
- `err(line, col, msg)` chỉ `diags_.add(..., false)` và set `had_error_`;
- accessor `diag_count()`, `print_diags()` (stderr, quoted),
  `print_diags_out()` (stdout, quoted). Path lấy từ field `path_` của lexer.

Vì `init` chỉ được gọi một lần/instance, `lexdrive` dùng **hai `Lexer` riêng**
cho hai pass (pass quiet để lấy chẩn đoán + pass token để dump), thay vì
`setup` hai lần trên cùng instance.

## 4. Preprocessor (`preprocessor.ivy`)

### 4.1. Chuyển sang `Diags`

- bỏ `char* dtxt_` + `long* dline_/dcol_/dmsg_` + `dn_/dcap_`; thêm
  `Diags diags_`;
- `diag_add(line, col, prefix, aoff, alen, warn)` -> `diags_.add_slice`;
- giữ accessor `diag_count/diag_line/diag_col/diag_warn/diag_msg` cho harness
  cũ, thêm `diag_failed()` = `diags_.has_error()` (1/0) và
  `print_diags(fname)` / `print_diags_out(fname)`.

`has_error_` còn lại trong port là **flag recovery nội bộ** (ví dụ paste hỏng
vẫn emit token), **không** phải `Preprocessor::hasError()` của oracle.
`diag_failed()` là tương đương đúng vì mọi lỗi pp đều ghi một error diag
(`#error` luôn kèm error), nên driver dùng nó thay cho `hasError()`.

### 4.2. Diagnostic bổ sung (mirror `preprocessor.cpp`)

Helper mới (tất cả là method của `Preprocessor`, thân trong `unsafe`):

| helper | dùng cho |
| --- | --- |
| `put_cstr(out, n, s)` | nối C string |
| `put_slice(out, n, off, len)` | nối slice từ `src_` (mọi lexeme, kể cả synthetic/pasted, đều nằm trong `src_` qua `syn_puts`) |
| `put_num(out, n, v)` | số thập phân (buffer tên `num_buf`, **không** `tmp` - xem §10) |
| `diag_slice2` | `p1 + slice + p2` |
| `diag_arity` | `p1 + slice + p2 + want + p3 + got` |
| `diag_paste` | `p1 + left + p2 + right + p3` |
| `ex_err` | `#if expression: <msg>` tại `ex_line_/ex_col_` |

Field mới `ex_line_`/`ex_col_` (vị trí directive `#if`, đặt trong
`eval_expr`; `eval_expr` đổi signature sang `(long es, long ee, long line,
long col)` để bỏ cast `(long)` trong thân).

Message mới, đúng vị trí + text oracle:

- `#elif without #if`, `#elif after #else`;
- `#else without #if`, `duplicate #else`;
- `#endif without #if`;
- `#define expects a macro name`;
- `#<ifdef|ifndef> expects a macro name` (`#` + tên directive + suffix);
- `unterminated #ifdef/#if block` (0:0);
- `macro NAME expects N argument(s), got M`;
- `variadic macro NAME expects at least N argument(s), got M`;
- `unterminated argument list invoking NAME`;
- `unterminated macro parameter list` (tại tên macro);
- `invalid token in macro parameter list` (chỉ khi token cùng dòng `#define`);
- `##: pasting 'L' and 'R' does not produce a valid token`;
- `##: pasting 'L' and 'R' produces multiple tokens`;
- `##: no left operand for token pasting`;
- `macro expansion too deep: NAME` (object-like và function-like);
- `#if: 'defined' expects a macro name`;
- `#if: 'defined(NAME' missing ')'`;
- `#if: trailing tokens in expression`;
- `#if expression: expected ':' in ternary`;
- `#if expression: expected ')' in #if expression`;
- `#if expression: unexpected end of #if expression`;
- `#if expression: unexpected token in #if expression: 'TOK'`;
- `#if expression: division by zero in #if`;
- `#if expression: modulo by zero in #if`.

`##: pasting ...` lấy line/col của token **trái**; `macro ... expects`
lấy line/col của **tên macro**; các lỗi directive lấy line/col của token
`#`.

## 5. Parser (`parser.ivy`)

- `import diagnostic;`, field `Diags diags_` thay 5 field pool cũ;
- `setup` gọi `diags_.init(in_count + 16)` (chỉ là capacity ban đầu, giờ
  tăng trưởng được), `release` gọi `diags_.release()`;
- `error(t, msg)` / `error_at_ll(line, col, msg)` -> `diags_.add(...)`;
- rollback parse thử trong ngoặc tròn: `this.dn_ = fdiag` ->
  `diags_.truncate(fdiag)` (text arena không tua lại, giống `std::string` bị
  bỏ tham chiếu);
- accessor `diag_count/line/col/msg` giữ nguyên tên cho driver;
- `print_diags(fname)` (stderr) / `print_diags_out(fname)` (stdout), plain.

## 6. Driver và chọn stream

| driver | in gì | stream |
| --- | --- | --- |
| `app/main.ivy` (giống `ivy`) | lexer + pp | stderr, lexer quoted |
| `lexdrive` | lexer | stdout, quoted |
| `ppdrive` | lexer (nếu lỗi) + pp | stdout, plain |
| `astdrive` | lexer (nếu lỗi) + pp + parser | stdout, plain |
| `ivmdrive` | lexer + pp + parser | stderr, plain |

Quy tắc: **lỗi lexer chặn mọi tầng sau** - khi `had_error` thì driver in
chẩn đoán lexer rồi dừng, không dump token/AST (giống oracle). `main.ivy`
dùng `print_diags()` cho lexer (quoted) và `pp.print_diags(fname)` cho pp;
`diag_failed()` thay `hasError()`.

## 7. Harness `build_diag_smoke.ps1` (52 case)

Build trong TEMP (chỉ clang): `diagnostic`, `token`, `lexer`, `preprocessor`,
`ast`, `parser`, `cli`, `imports`, `main`, `lexdrive`, `ppdrive`, `astdrive`
+ `ivyrt.obj`, rồi link 4 exe.

1. **[5/7] app-stderr** - `main.exe <file>` vs `ivy <file>`, so **raw bytes**
   stdout/stderr/exit:
   - 9 file corpus `lexer/err` + 1 đường dẫn tuyệt đối;
   - 33 fixture pp (mỗi message ở §4.2 một fixture, kể cả `#line` remap,
     nhiều lỗi, warning + error);
   - 4 case quoting: path có space, path non-ASCII (`lỗi.ivy`), path lồng
     sâu, file không tồn tại (`exit 2`).
2. **[6/7] stage-stdout** - driver differential vs oracle merge
   (`cmd /c "... > out 2>&1"`), 5 nhóm: `lex-err` (9 file), `pp-diag`
   (35 file: 33 fixture lỗi + 2 fixture warning, gồm token dump sau warning),
   `pp-lexerror`, `ast-err` (19 file), `ast-lexerror`. Chỉ chuẩn hoá CRLF -> LF
   (message có UTF-8, ví dụ em dash trong `#include is removed...`).
3. **[7/7]** tổng kết + guard: `selfhost/{common,parsing,app}` không sinh
   file mới.

## 8. Wiring harness + `import diagnostic;` trực tiếp

- `difftest.ps1` tự thêm `selfhost/common` vào `$mds` và prepend `diagnostic`
  vào `$Modules` nếu thiếu;
- `interpdiff.ps1`, `codegen/{cgdiff,build_object_smoke,build_link_smoke}.ps1`
  thêm `selfhost/common` vào `$moduleDirs` + `@("selfhost\common",
  "diagnostic")` đầu `$modules`;
- `app/build_{cli,import}_smoke.ps1`, `parsing/build_ivm_smoke.ps1` thêm
  `Build-Mod diagnostic` + `diagnostic.obj` vào link.

**Quan trọng:** oracle chỉ link object của các module mà **file chính**
`import` trực tiếp (`src/app/main.cpp:929-1016`, `moduleObjPaths`). Vì
`lexer.obj`/`preprocessor.obj`/`parser.obj` tham chiếu symbol của
`diagnostic.obj`, mọi driver được build bằng `ivy <driver>.ivy -o <exe>` phải
có `import diagnostic;` **trực tiếp**: `cgdrive`, `lexdrive`, `ppdrive`,
`astdrive`, `ivmdrive`, `hirdrive`, `mirdrive`, `interpdrive`, `app/main`.
(Harness nào link thủ công bằng clang thì không bắt buộc, nhưng thêm cho
đồng bộ.)

## 9. Kết quả (2026-11-16)

```text
build_diag_smoke.ps1:     PASS 52 / 52
                          (10 lexer + 33 pp + 4 quoting + 5 stage-stdout
                           + guard source tree)
corpus regression (TEMP): 20 / 20 nhóm byte-identical
                          lexer x6, pp x7, parser x7
difftest-style link:      6 / 6 (lexdrive/ppdrive/astdrive build bằng
                          `ivy -o`, corpus lexer/pp/parser)
build_import_smoke.ps1:   PASS 32 / 32
build_cli_smoke.ps1:      PASS 67 / 67
build_ivm_smoke.ps1:      PASS 43 / 43
build_object_smoke.ps1:   object PASS 246 / 246, run PASS 8 / 8
build_link_smoke.ps1:     run PASS 8 / 8, negative PASS 2 / 2, space PASS 1 / 1
build_smoke.ps1:          PASS (h61 smoke ok)
interpdiff.ps1:           PASS 23 / 23
```

## 10. Quy ước / bẫy đã gặp

- **Không đặt tên biến local là `tmp`** trong file Ivy: oracle codegen sinh
  `%tmp.N` cho mọi giá trị tạm, trùng tên alloca -> LLVM báo
  `multiple definition of local value named 'tmp.4'`. Đã đổi buffer số trong
  `put_num` thành `num_buf`.
- Cast C-style `(long)`/`(int)`/`(char*)` chỉ hợp lệ trong `unsafe`. Vì vậy
  `eval_expr` nhận `long` thay vì `int` (bỏ cast), còn `diag_line/col` của pp
  giữ `(int)` trong `unsafe`.
- `char*` trả về từ method khác (arena của `Diags`) cần
  `unsafe { return this.diags_.msg(i); }`.
- Ivy từ chối `(const char*)0`; so sánh null qua `char* p = (char*) s; if
  (p == (char*) 0)`.
- `common/diagnostic.ivy` **không** `import io;` - `io` là prelude của Ivy,
  và thêm import sẽ tạo phụ thuộc vòng khi `io` được build trước.
- `#define F(a` + dòng sau `F(1)` **không** báo lỗi (tham số bị nuốt qua
  dòng, tìm thấy `)`); chỉ báo `unterminated macro parameter list` khi hết
  file. Fixture harness dùng `#define F(a` (không có dòng sau).
- Oracle stderr dùng CRLF; `io::eprintln` cũng CRLF. So sánh stage-stdout
  phải chuẩn hoá CRLF -> LF vì driver in ra stdout bằng `\n`.

## 11. Hạn chế

- `Diags` cấp arena 65536 byte/instance và không giải phóng từng message;
  `release()` chỉ giải phóng toàn bộ (đúng vòng đời từng tầng).
- `truncate` không tua lại `txt_n_` (text bị bỏ không được tái sử dụng) -
  chấp nhận được vì rollback parse thử rất nhỏ.
- Diagnostic của HIR/MIR/H8 (`hir_builder`, `mir_builder`, interpreter) vẫn
  dùng cơ chế riêng của chúng; `Diags` mới chỉ được nối vào
  lexer/pp/parser/driver. Việc chuyển các tầng sau sang `Diags` để H8.

## 12. Trạng thái

- `selfhost/common/{diagnostic.ivy,build_diag_smoke.ps1,H74_NOTES.md}` mới;
  `lexer.ivy`, `preprocessor.ivy`, `parser.ivy`, các driver + harness cập
  nhật; `PLAN.md` H7.4 -> ✅.
- Working tree chỉ có `selfhost/**` + `PLAN.md`; commit/push chờ yêu cầu
  (gợi ý: `H7.4: shared diagnostics for lexer, preprocessor and parser`).
