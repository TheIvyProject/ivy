# H9.1 - Runtime `lib/ivyrt.c` -> `std/rt.ivy`

## 1. Pham vi

H9.1 thay **runtime C** (`lib/ivyrt.c`, 327 dong, 41 export `__ivy_*`) bang
`std/rt.ivy`, mot translation unit Ivy goi thang UCRT qua `extern "C"`.
Object sinh ra (`rt.obj`) thay the archive `libivyrt` trong moi duong link:

- `CMakeLists.txt`: bo `add_library(ivyrt STATIC lib/ivyrt.c)`, them custom
  command/target `ivyrt` chay `ivy -c std/rt.ivy -I std -o build/rt.obj`;
  `IVYRT_LIB_PATH` gio tro **object** do (khong con la archive);
- `selfhost/app/main.ivy` `find_ivyrt()`: `IVYRT_LIB_PATH`, roi `rt.obj` canh
  `argv[0]`; bo hai nhanh `ivyrt.lib` / `libivyrt.a`;
- 8 harness (`bootstrap_smoke`, `build_smoke`, `build_object_smoke`,
  `build_link_smoke`, `build_cli_smoke`, `build_import_smoke`,
  `build_diag_smoke`, `build_ivm_smoke`) build `rt.obj` bang `ivy -c` thay vi
  `clang -c lib/ivyrt.c`; `driver_smoke` dung `build/rt.obj` do CMake target
  `ivyrt` sinh. Ba harness `build_cli_smoke`/`build_import_smoke`/
  `build_diag_smoke` da hong tu H8.1 (xem muc 8) nen o do chi buoc build
  runtime duoc cap nhat, chua chay lai duoc;
- file nay + `PLAN.md` dong H9.1 + `selfhost/ORACLE_BUGS.md` (R1/R2/R3, B-R2).

Khong sua `src/**`, khong rebuild oracle.

## 2. Giu nguyen ABI

Ten symbol, thu tu tham so va layout `struct __ivy_file` giu y nguyen
`lib/ivyrt.h`, nen IR da emit (`declare void @__ivy_write_str(i32, ptr)`,
`ptr @__ivy_fmt_int(i64)`, `i1` cho `fmt_bool`, `i8` cho `write_char`, ...)
link khong doi. `struct __ivy_file` van la mot `uint64 handle` (that chat la
`FILE*`), kich thuoc 8 byte nhu ban C.

**ABI pun bat buoc**: Ivy subset tu choi raw pointer o tham so/return cua
**ham co body** (`raw pointer parameter type requires an unsafe block`), va
khong co cu phap `unsafe` cho signature (`unsafe extern "C"`,
`extern "C" { unsafe ... }`, `[[ivy::unsafe]]` deu bi tu choi);
`export extern "C"` cung bi tu choi. Vi vay moi slot con tro trong **dinh
nghia** `__ivy_*` duoc khai bao `uint64`; chi **declaration** `extern "C"`
moi dung `void*`/`char*` that (declaration khong bi rule nay). Tren Win64
pointer va `uint64` la cung mot thanh ghi 8 byte -> ABI may khong doi. Cac
`(uint64)` / `(char*)` trong file la noi duy nhat biet ve quy uoc nay.

He qua khac cua subset: khong file-scope/`static` bien, khong `export module`
(neu khai bao se sinh `std/rt.ivm` vao repo), khong `sizeof` cho struct C.

## 3. Cac phan da port

| Nhom | Ghi chu |
|---|---|
| `alloc`/`free` | `malloc`/`free` cua UCRT; `__ivy_free(0)` van null-safe |
| write | `__ivy_write_str`/`_char`/`_newline` -> `_write(fd, ...)`, khong `printf` |
| `fmt_*` | `fmt_int` viet tay vong chia 10 (tranh varargs, xu ly `INT64_MIN` bang wrap unsigned); `fmt_float` dung `_gcvt(v, 15, buf)` de giu quirk `"2."`; `fmt_bool` tra `"true"`/`"false"` |
| `panic` | `puts(msg)` (neu khac null) + `abort()` |
| string | `strlen`/`memcpy`/`memset`/`strcmp`/`strdup` giu nguyen semantics C (memcpy/memset la byte loop; `strcmp` null -> `""`) |
| `read_line` | `fgets` tren `__acrt_iob_func(0)` (xem 4) |
| file | `fopen(path, "rb"/"wb")`, `fread`/`fwrite`, growth loop 4096/`cap*2` y nguyen ban C, `buf[len] = 0`, `outLen` ghi `uint64` |
| math | 19 wrapper 1 dong goi `sqrt`/`pow`/... cua UCRT |

## 4. Ba deviation co chu y

1. **`fmt_int`/`fmt_float` cap buffer moi moi lan goi** thay vi static buffer
   dung chung. Subset khong co bien file-scope lan `static` local. Caller
   luon `_write` ngay sau khi format nen output khong doi; khac biet chi lo
   khi giu >= 2 ket qua cung luc, hoac khi format trong vong lap dai (moi lan
   goi cap 32/64 byte va **khong duoc free** - codegen truyen thang ket qua
   cho `_write`). Ghi thanh `B-R2` trong `selfhost/ORACLE_BUGS.md`; follow-up
   (arena/scratch buffer) de H10.
2. **`read_line` cap 1024 byte bang heap** thay vi `char buf[1024]` tren
   stack: subset khong cho array decay thanh pointer o call site
   (`argument 1 of 'fgets' expects 'char*', got 'char'`), `&buf[0]`/`&buf`
   cung khong hop le. Buffer duoc `__ivy_free` o **moi** return path.
3. **`stdin` -> `__acrt_iob_func(0)`**: `stdin` la bien global cua UCRT,
   subset khong khai bao duoc.

## 5. Ket qua

| Kiem tra | Ket qua |
|---|---|
| `ivy -c std/rt.ivy -I std` (oracle) | exit 0, `rt.obj` 8 711 byte |
| `dumpbin /symbols rt.obj` | du 41 `__ivy_*`, ten khong mangled, moi symbol 1 SECT |
| `rt.obj` do oracle vs do stage1 (selfhost) | **byte-identical** (cung la buoc 2b cua `bootstrap_smoke.ps1`) |
| So output/exit khi link bang `rt.obj` vs `ivyrt.lib` | giong het (fmt int/float/bool/char, eprintln, read/write_file, read_line, 19 math, panic -> exit `-1073740791`, string ops) |
| `selfhost/codegen/build_link_smoke.ps1` | PASS 8/8 link+run, 2/2 negative, 1/1 space-path |
| `selfhost/dogfood.ps1` | **DOGFOOD PASS** (xem 6) |

## 6. Dogfood gate (`selfhost/dogfood.ps1`)

Chay lai toan bo gate H8.2 voi runtime moi:

- **F2 PASS**: stage0 (oracle) build stage1, stage1 build stage2 -
  18/18 object, 17/17 `.ivm` va executable **byte-identical**; them buoc 2b:
  stage1 tu build `std/rt.ivy` -> `rt.obj` va object do **byte-identical**
  voi `rt.obj` oracle build o dau script;
- `--llvm` **1052/1052** fixture khop oracle;
- `-c` **1052/1052** object khop oracle;
- link + run **249/249** fixture codegen khop (exit/stdout/stderr); 2 fixture
  UB (`138_new_struct_noparen`, `226_structured_binding_mixed`) van chi so
  buoc link nhu H8.1.

Trong 249 fixture do co **5 fixture co tinh khong link duoc** (goi extern
khong ton tai / `ivy::print*` chua co runtime / pure virtual thieu dinh
nghia): `168_ptr_arg`, `171_variadic_zero`, `185_ivy_print_types`,
`191_ivy_print_enum`, `236_inherit_pure_virtual`. Ca hai compiler deu that bai
giong nhau; chi khac **duong dan runtime** trong chan doan - xem muc 7.

## 7. Chuan hoa duong dan runtime trong chan doan link

Oracle da duoc build tu truoc H9.1 nen `IVYRT_LIB_PATH` **bake san** trong
binary la `build/Release/ivyrt.lib`; no **khong doc** bien moi truong. Ban
selfhost thi doc `IVYRT_LIB_PATH` (gio la `build/rt.obj`). Voi cac fixture
link that bai, driver in nguyen dong lenh linker ra stderr, nen hai ben khac
nhau dung o duong dan runtime.

`driver_smoke.ps1` va `build_link_smoke.ps1` vi vay chuan hoa **ca hai**
spelling (`<repo>/build/Release/ivyrt.lib` cua oracle va `build/rt.obj` cua
selfhost) thanh `<RT>` truoc khi so sanh - cung cach chung da chuan hoa
duong dan exe thanh `<OUT>`. Khong mat tin hieu: moi phan con lai cua chan
doan van so byte-exact.

## 8. Con lai (ngoai H9.1)

- `lib/ivyrt.c` / `lib/ivyrt.h` **van con** lam tai lieu tham chieu ABI; chi
  xoa o H9.9 (cung `src/**`, `CMakeLists.txt`);
- H9.4: `Linker::link_text` chua passthrough `-l`/`-L`/`-Wl,` nen compiler
  chua tu link chinh no (M3) - khong thuoc H9.1;
- B-R2 (static buffer -> buffer moi) la deviation co chu y, sua/khong sua o
  H10 khi golden da dong bang;
- ba harness H7.x `build_cli_smoke.ps1`, `build_import_smoke.ps1`,
  `build_diag_smoke.ps1` **da hong tu H8.1**, khong phai do H9.1: chung chi
  build 6 module cua driver H7 (diagnostic/token/lexer/preprocessor/cli/
  imports) trong khi `main.ivy` tu H8.1 import ca pipeline (ast/parser/
  module_io/hir/hir_builder/mir/mir_builder/pipeline/emitter/codegen), nen
  buoc `[1/6] building modules` that bai voi `module 'ast' not found` truoc
  khi cham runtime. Da kiem chung bang `git worktree` tai `b238ee1` (HEAD
  truoc H9.1): loi y nguyen. Chuc nang chung (assert "dispatch not ported
  yet", exit 3) cung da bi H8.1 thay the. H9.1 chi sua buoc build runtime
  trong 3 file do (`ivy -c std/rt.ivy` thay `clang lib/ivyrt.c`) de khong con
  tham chieu file se bi xoa o H9.9; re-base (hoac xoa) chung thuoc H9.3/H9.6.

# H9.2 - Driver: noi cac mode dump va interpreter

## 1. Pham vi

`selfhost/app/main.ivy` truoc H9.2 tra `3` (kem chan doan "dispatch is not
ported yet") cho `--lex`, `--tokens`, `-o *.i|*.ii`, `--ast`, `--hir`,
`--mir` va `--run`. H9.2 noi tat ca vao driver that, dung **thu tu stage cua
oracle** (`src/app/main.cpp`):

| Thu tu | Stage | Mode | Nguon in |
|---|---|---|---|
| 1 | lexer | `--lex` | `TokenPrinter` tren `Lexer.tokens()` |
| 2 | preprocessor | `-o *.i\|*.ii` | `Preprocessor.reconstruct_source()` |
| 3 | preprocessor | `--tokens` | `TokenPrinter` tren `Preprocessor.output()` |
| 4 | import scan + parser + merge + splice + `.ivm` | | |
| 5 | (truoc HIR) | `--ast` | `AstDumper` (`ast.ivy`) |
| 6 | HIR | | |
| 7 | (truoc MIR) | `--hir` | `HirDumper` (`hir.ivy`) |
| 8 | MIR | | |
| 9 | (sau MIR) | `--mir`, `--run` | `MirDumper` (`mir.ivy`), `InterpRunner` (`interpreter.ivy`) |

Diem quan trong: `--ast` phai in **truoc** khi build HIR, `--hir` truoc khi
build MIR; neu tang sau co chan doan thi dump cua tang truoc **van phai in
roi moi** `exit 1`. `Cg.compile()` cu return 1 ngay sau chan doan HIR/MIR nen
khong tai hien duoc thu tu nay - vi vay H9.2 tach no thanh tung stage.

## 2. Khong tao module moi

Bon printer va runner duoc dat ngay trong module model tuong ung
(`ast.ivy`, `hir.ivy`, `mir.ivy`, `preprocessor.ivy`, `interpreter.ivy`) thay
vi module moi, de:

- khong phai sua danh sach module cua 7 harness;
- khong nhan doi code: cac `*drive.ivy` gio chi con phan doc file roi goi
  cung struct ma driver that dung.

Moi printer la `export struct` vi subset **cam raw pointer o tham so/return
cua free function co body**, nhung **method cua struct duoc phep**; cac field
giu con tro toi pool cua parser/HIR/MIR.

## 3. Thay doi theo file

| File | Thay doi |
|---|---|
| `parsing/ast.ivy` | `export struct AstDumper` (nguyen van `struct Dumper` cua `astdrive.ivy`) |
| `hir/hir.ivy` | `export struct HirDumper` (nguyen van `HDumper`) |
| `mir/mir.ivy` | `export struct MirDumper` (nguyen van `MirDumper`) |
| `parsing/preprocessor.ivy` | `reconstruct_source()` + `export struct TokenPrinter` (thay `Printer`/`kind_is_eof` cua `ppdrive`) |
| `mir/interpreter.ivy` | `export struct InterpRunner` (`exec(MUnit, char*)` -> exit code, `setup_failed`) |
| `app/pipeline.ivy` | `Cg.lex_stage/pp_stage/parse_stage/hir_stage/mir_stage`; `compile()` = chuoi stage (giu chu ky cho `cgdrive`); `dump_lex/dump_tokens/dump_ast/dump_hir/dump_mir/write_preprocessed`; field `content_` |
| `app/main.ivy` | dispatch theo bang o muc 1; bo hai stub `exit 3` |
| `parsing/ppdrive.ivy` | dung `TokenPrinter`, them `println` sau moi file (xem 4.3) |
| `parsing/astdrive.ivy`, `hir/hirdrive.ivy`, `mir/mirdrive.ivy`, `mir/interpdrive.ivy` | xoa struct da chuyen, doi ten, goi struct cua module |
| `app/bootstrap_smoke.ps1`, `app/driver_smoke.ps1` | them `interpreter` vao danh sach module (`main.ivy` import no) |

## 4. Chi tiet can luu y

1. **`--lex` khong chay preprocessor**; `--tokens` va `-o *.i|*.ii` chay
   preprocessor nhung **khong** import scan/parse; `-o *.i` dung truoc
   `--tokens` (oracle `main.cpp:894-913`).
2. **eof khong co newline**: oracle in `line:col<TAB>eof` roi `break` truoc
   `newline`. `TokenPrinter.show()` vi vay `return` ngay tai eof. Ban cu
   (`Lexer.emit_eof`/`Printer`) in them `\n`; difftest so theo dong nen khong
   bat, nhung `ivy --lex`/`--tokens` that phai khong co newline (da kiem
   chung byte-exact).
3. **`ppdrive` can newline giua cac file**: khi chay nhieu file qua stdin,
   `ppdrive` in them `io::println("")` sau `show()` de khong dinh dong cua
   file sau vao `eof` cua file truoc; CLI that khong in them.
4. **`-o *.i|*.ii` la CRLF**: oracle ghi bang `std::ofstream` o text mode,
   Windows doi moi `\n` thanh `\r\n`. `reconstruct_source()` tra LF (giong
   chuoi oracle), `Cg.write_preprocessed()` doi sang CRLF truoc
   `io::write_file`. `reconstruct_source()` chen `\n` toi `t.line`, mot space
   giua token cung dong, va mot `\n` cuoi.
5. **`--run`**: `InterpRunner.exec()` sao chep `MUnit` len heap 256 byte
   (`&mu` bi tu choi) roi `call_by_name(muptr, "main", -1, 0)`, in
   stdout/stderr cua chuong trinh, in chan doan runtime (`fname:line:col:
   error: ...`), roi tra `r.status` / `1` / `read_int` / `0` nhu
   `main.cpp:1325-1338`. `setup()` that bai khong co trong oracle: driver in
   `ivy: error: cannot initialize the interpreter` va tra 1 (`setup_failed`).
6. **Thu tu in cua `--run`**: oracle in stdout/stderr ngay khi chuong trinh
   chay, chan doan sau khi ket thuc; ban port buffer roi in theo dung thu tu
   do (stdout, stderr, chan doan).

## 5. Harness

- `selfhost/difftest.ps1`:
  - `-DriverExe <path>`: bo buoc build module/driver, chay **chinh**
    `ivy_selfhost.exe` moi file mot process (cung cach thu voi oracle) va so
    **ca exit code**;
  - `-Recurse`: lay fixture ca trong thu muc con (corpus duoc chia nhom);
  - `-TempRoot`: thu muc scratch (module `.obj`/`.ivm` build vao day, khong
    con ghi vao repo);
  - `-Kinds all` giu nguyen nghia "so toan bo dong".
- `selfhost/interpdiff.ps1`: `-DriverExe <path>` chay `--run <file>` moi
  fixture, khong build `interpdrive`.
- `selfhost/dogfood.ps1`: `-Dumps` them buoc 3/3 chay 5 lan `difftest
  -DriverExe` (lexer/pp/parser/hir/mir) va `interpdiff -DriverExe`.
- Bug harness da gap: PowerShell **khong phan biet hoa/thuong** ten bien, nen
  `$DriverExe` (param) va `$driverExe` (duong dan interpdrive) la **cung mot
  bien**; doi bien cuc bo thanh `$port`.

## 6. Ket qua

| Kiem tra | Ket qua |
|---|---|
| `difftest -DriverExe --lex` (46 fixture lexer) | PASS 46/46, stdout+stderr+exit byte-exact |
| `difftest -DriverExe --tokens` (105 fixture pp) | PASS 105/105 |
| `-o *.i` tren 105 fixture pp | PASS 105/105 file byte-identical (ke ca CRLF); `.ii` kiem tra rieng: identical |
| `difftest -DriverExe --ast` (111 fixture parser) | PASS 111/111 |
| `difftest -DriverExe --hir` (425 fixture hir) | PASS 425/425 |
| `difftest -DriverExe --mir` (93 fixture mir) | PASS 93/93 |
| `interpdiff -DriverExe` (23 fixture interp) | PASS 23/23 (stdout/stderr/exit byte-exact) |
| 6 feature driver cu (lexdrive/ppdrive/astdrive/hirdrive/mirdrive/interpdrive) | build lai OK; chay lai harness goc: 10/10 pp, 13/13 ast, 124/124 hir, 75/75 mir, 10/10 lex, interpdiff 23/23 |
| `selfhost/dogfood.ps1 -Dumps` | **DOGFOOD PASS**: F2 19/19 object + 18/18 `.ivm` + `rt.obj` + EXE identical (them module `interpreter`); `--llvm` 1052/1052, `-c` 1052/1052, link+run 249/249; dump/interp: 46/46 + 105/105 + 111/111 + 425/425 + 93/93 + 23/23 |

## 7. Con lai

- `--run` setup failure la **deviation** (oracle khong co duong nay); ghi o
  day, khong phai bug-compat.
- `difftest`/`interpdiff` van can oracle cho che do so truc tiep; H9.3 thay
  bang `selfhost/golden/` de gate khong can `build/Release/ivy.exe`.
- Cac harness H7.x da hong tu H8.1 (xem H9.1 muc 8) van chua duoc re-base.

---

# H9.3 - Dong bang baseline + golden (`selfhost/golden/`)

## 1. Pham vi

H9.2 chung minh driver khop oracle khi **chay oracle cung luc**. H9.3 dong bang
hanh vi do thanh du lieu co the commit, de tu day cac gate chay **khong can**
`build/Release/ivy.exe`. Tag `0.5-baseline` (H0.2) la moc cuoi cua `src/**`;
golden duoc sinh tu chinh `build/Release/ivy.exe` cua moc do.

Hai script:

| Script | Vai tro |
|---|---|
| `selfhost/freeze_golden.ps1` | chay oracle tren toan corpus, ghi `selfhost/golden/` |
| `selfhost/check_golden.ps1` | chay `ivy_selfhost.exe`, so voi golden, **khong** goi oracle |

## 2. Layout golden

20 file, 3 974 260 byte (2026-11-21):

| File | Noi dung |
|---|---|
| `corpus.txt` | danh sach 1052 fixture (repo-relative, `/`) |
| `manifest.txt` | `format=1`, so fixture, modes, SHA-256/size oracle + `rt.obj` |
| `dump/{lex,tokens,ast,hir,mir,llvm}.txt` | 6 mode dump, moi fixture mot record |
| `compile/c.txt`, `compile/objects.tsv` | `-c`: output + SHA-256/size `.obj` va `.ivm` |
| `compile/o.txt`, `compile/preproc.tsv` | `-o *.i`: output + SHA-256/size file |
| `interp/{stdout,stderr}.txt` | `--run` 23 fixture interp (byte-exact) |
| `link/{diag,stdout,stderr,sideeffects}.txt` | 249 fixture link + executable |
| `link/nondet.txt` | 2 fixture UB chi so link (`138`, `226`) |
| `exceptions.txt` | record oracle **crash** (H27), xem muc 6 |

Record co do dai tuong minh vi `--lex`/`--tokens` ket thuc bang `eof` **khong**
newline:

```
@@ <path> <nbytes> <exit>\n<dung nbytes byte tho>
```

Khong nen: dump text ~2.8 MB, object chi luu hash.

## 3. Chuan hoa

- 6 mode dump + output `-c`/`-o`/link: CRLF -> LF, dung **mot** newline cuoi
  (giong `difftest.ps1`); `--run`, stdout/stderr executable, object va `.i` la
  **byte-exact**.
- `<OUT>`: thu muc scratch (ca hai kieu slash) trong diagnostics.
- `<RT>`: `build/Release/ivyrt.lib` (oracle bake) va `build/rt.obj` (selfhost
  doc qua `IVYRT_LIB_PATH`) duoc thay bang cung mot token.
- `<CLANG>`: duong dan `clang++` ma driver in trong chan doan
  `linker failed (...)`, thay bang mot token (them o H9.6) — neu khong, golden
  chi dung tren may co LLVM o cung duong dan.
- Lenh moi fixture dung `-I <thu muc fixture>` (repo-relative, `/`) giong
  `driver_smoke.ps1`.

## 4. Cach dung

```
powershell -NoProfile -File selfhost/freeze_golden.ps1
powershell -NoProfile -File selfhost/check_golden.ps1 -Driver <stage1>\ivy_selfhost.exe
powershell -NoProfile -File selfhost/check_golden.ps1 -Driver ... -Modes ast,hir,mir
powershell -NoProfile -File selfhost/check_golden.ps1 -Driver ... -Filter 135_*
```

`check_golden.ps1` tu set `IVYRT_LIB_PATH=build/rt.obj` neu chua co va canh bao
neu SHA-256 cua `rt.obj` khac manifest. Moi lan chay dung TEMP/TMP rieng.

## 5. Parity gap ma gate mo rong bat duoc

Lan chay dau voi stage1: **74 failure / 12337 check**. Day la **loi that cua
port** (truoc do H9.2 chi chay `--ast` tren `parser/`, `--hir` tren `hir/`,
`--mir` tren `mir/` nen bo sot `codegen/` va `hir/borrow`, `hir/move`):

| File | Gap | Fix |
|---|---|---|
| `parsing/ast.ivy` | thieu kind 21 `new`, 22 `delete`/`delete[]`, 23 `init-list`, 24 `sizeof...`, 25 `fold` | them vao `AstDumper.dump_expr` dung `src/app/main.cpp:145-197` |
| `hir/hir.ivy` | thieu kind 20 `new : T*`, 21 `delete` | them vao `HirDumper.dump_expr` dung `main.cpp:428-437` |
| `mir/mir.ivy` + `mir_builder.ivy` | `virtual_call ''` mat ten method | copy `HExpr.msval` vao `MExpr.sval2` |
| `mir_builder.ivy` | `return lifetime:` in ca tren function **khong co body** | oracle copy trong `buildFunction`; port chi copy khi co body |
| `mir_builder.ivy` | `move(x)` in `[lt: ...]` sai | bug-compat H28: oracle gan `out->lifetime` **sau** `std::move(arg)` nen `arg` null -> call giu `None`; port bo copy lifetime (xem `ORACLE_BUGS.md`) |

Quirk AST giu nguyen: leaf node **khong** co newline (vi vay
`new int32_t` + `int 5` dinh vao dong ke tiep) va `<zero-init>`/`<null lhs>` dung
`pad(depth)` chu khong phai `pad(depth+1)`.

## 6. Ngoai le oracle crash (H27)

`--hir` va `--mir` cua oracle crash `0xC0000005` (`exit -1073741819`) tren
`156_struct_partial_init`, `157_struct_empty_init`, `160_struct_designated_init`
(init-list co `<zero-init>`). `freeze_golden.ps1` tu ghi `exceptions.txt` cho
moi record dump co exit am; `check_golden.ps1` bo so byte cho 6 record nay
nhung **bat buoc port exit 0** (khong tai hien crash).

## 7. Ket qua

| Kiem tra | Ket qua |
|---|---|
| `bootstrap_smoke.ps1` (F2) | **PASS**: 19 object + 18 `.ivm` + `rt.obj` + EXE identical |
| `check_golden.ps1 -Driver <stage1>` (ca 10 mode) | **PASS 12337 check, 0 failure** (1052 fixture) |
| `check_golden.ps1 -Modes ast,hir,mir` | PASS 3156/3156 |
| Gate khi **khong co** `build/Release/ivy.exe` | dung golden, khong goi oracle |

Luu y: gate golden **thay the** nhu cau chay `difftest`/`interpdiff` truc tiep
trong CI tu H9.3; hai script cu van giu de debug khi con oracle.

## 8. Con lai

- Tag `0.5-baseline` **chua tao** (cho phep cua user).
- `exceptions.txt` la du lieu duy nhat do nguoi viet them; neu freeze lai ma
  oracle het crash thi file se rong va gate lai so byte binh thuong.
- 3 harness H7.x van hong (re-base o H9.6).

---

# H9.4 - Self-link (compiler tu link chinh no)

## 1. Pham vi

Truoc H9.4 `Linker::link_text` chi biet `"<linker>" "<obj>" -o "<exe>"
["ivyrt"]`, nen selfhost chi link duoc chuong trinh nguoi dung (khong import
LLVM). H9.4 cho `ivy_selfhost.exe` tu link **chinh no**: cung mot command line
ma `selfhost/app/bootstrap_smoke.ps1` dung (`clang++` + toan bo module object +
`rt.obj` + `LLVM*.lib` + system libs + `/Brepro`).

Day la feature **moi cua selfhost**, khong phai parity: oracle `src/**` khong
co `--self-link` va usage cua oracle khong doi.

## 2. Thiet ke

`selfhost/codegen/codegen.ivy` - `Linker`:

| Thanh phan | Vai tro |
|---|---|
| `self_link_` + `set_self_link()` | bat nhanh command line kieu harness |
| `extra_`/`nextra_`/`capextra_` + `add_extra()` | hang doi fragment **da render**: path thi quote san, flag (`-lfoo`, `-Wl,/Brepro`) de tran |
| `collect_self_link()` | do `LLVM_ROOT` -> `D:\Cpp\Clang` -> `llvm-config` tren PATH; chay `llvm-config --libs x86` va `--system-libs` |
| `link_text()` | nhanh self-link: `<objs> <rt.obj> <llvm libs> <system libs> -Wl,/Brepro -o <exe>` (khac nhanh thuong: `-o` dung truoc `rt.obj`) |

Loc system libs y het `bootstrap_smoke.ps1`: ten khong nam trong `$sdkLibs` bi
bo (thuc te `libxml2s`), ten SDK doi thanh `-l<name>`. `llvm-config --libs x86`
tra full path tung `.lib`, duoc quote.

`selfhost/app/cli.ivy`: co `self_link` (init 0), nhan `--self-link` va ep
`link_mode = 1`. `selfhost/app/main.ivy`:
`link_executable(..., long self_link)` goi `set_self_link(1)` +
`collect_self_link()` **truoc** khi them module object (thu tu fragment tren
command line la co dinh).

## 3. Thu tu object quyet dinh byte cua PE

`bootstrap_smoke.ps1` dung hai danh sach khac nhau:

- `$modules` - thu tu **build** (`io` dau tien vi cac module khac import no);
- `$linkOrder` - thu tu **`import`** cua `main.ivy` (diagnostic, token, lexer,
  preprocessor, ast, parser, module_io, imports, cli, hir, hir_builder, mir,
  mir_builder, interpreter, pipeline, emitter, codegen, io).

Link phai dung `$linkOrder`, neu khong PE khac byte du object giong het. Ten
file object khong anh huong, chi thu tu. `--self-link` tai hien dung thu tu nay
tu `Cg::module_object(i)` (pipeline duyet theo import cua `main.ivy`).

## 4. Bug oracle H29 (workaround bat buoc)

Ghi qua phan tu `T**` (`p[i] = x`) emit `getelementptr i8` **khong scale**,
trong khi doc `p[i]` emit `getelementptr ptr` dung. `Linker` giu `char** mods_`
/ `char** extra_` dung pattern nay, nen `add_module_object()`/`add_extra()`
**khong** duoc viet `this.mods_[i] = p`; phai copy 8 byte bang `__ivy_memcpy`:

```ivy
char* v = p;
__ivy_memcpy((void*) ((char*) this.mods_ + (this.nmod_ * 8)), (void*) &v, 8);
```

Chi tiet + repro: `selfhost/ORACLE_BUGS.md` muc 1.3 (H29) va 2.6.

## 5. Cach chay

```powershell
# gate day du, gom buoc [self-link]
powershell -NoProfile -File selfhost\app\bootstrap_smoke.ps1 `
    -TempRoot "$env:TEMP\h94" -SelfLink

# chi build stage1 tu stage0, roi tu link bang chinh no
& .\ivy_selfhost.exe --self-link -I std -I selfhost\app ... selfhost\app\main.ivy
```

`--self-link` can `LLVM_ROOT` (hoac `D:\Cpp\Clang`) + `IVYRT_LIB_PATH` tro
`rt.obj`; thieu linker thi exit 1 kem diagnostic `linker failed (exit code 1)`
va **nguyen command line** de debug.

## 6. Ket qua (2026-11-21)

| Kiem tra | Ket qua |
|---|---|
| `bootstrap_smoke.ps1 -SelfLink` | **PASS**: 18/18 object + 17/17 `.ivm` + `rt.obj` + stage1/stage2 EXE identical |
| Buoc `[self-link]` | **EXE self-link identical** (27 130 368 byte) |
| `check_golden.ps1 -Driver <stage1>` | **PASS 12337 check, 0 failure** |

## 7. Con lai

- Stage0 / bootstrap khong can oracle: da xong o H9.5 (muc duoi).
- `import cpp` parity (H9.8) van chua port; `--self-link` khong dung den.
- Bug H29 chi duoc **ne**, chua fix: sua `lowerLValue` (dung `llvmType` nhu
  duong doc) o H10, kem freeze lai golden.

---

# H9.5 - Bootstrap khong can oracle (`selfhost/build.ps1`)

## 1. Pham vi

H8.1/H9.4 chung minh compiler Ivy tu build + tu link duoc, nhung stage 0 van la
oracle C++ (`build\Release\ivy.exe`). H9.5 thay stage 0 bang mot binary Ivy
**dong bang** trong repo, nen mot checkout chi can LLVM + MSVC CRT/SDK la build
duoc, khong doc `src/**`, khong CMake, khong oracle.

Chuoi moi:

```
selfhost/bootstrap/ivy-stage0.exe  ->  stage1  ->  stage2
                                         |            |
                                         +-- F2: OBJ/IVM/rt.obj/EXE byte-identical
```

## 2. Stage 0 (seed)

| | |
|---|---|
| file | `selfhost/bootstrap/ivy-stage0.exe` |
| size | 27 130 368 byte |
| SHA-256 | `C5DF31991234D0AD4C9637DA882D8C4DEB9C35ECCBD961307056EAA89EF46008` |
| sinh tu | commit `d8252ba` (H9.4), stage 1 do oracle build |
| kiem tra | `ivy-stage0.sha256` (dinh dang `sha256sum`), verify truoc khi dung |

`selfhost/bootstrap/README.md` ghi nguon goc, ly do commit binary, va cach tai
sinh. `.gitignore` co `*.exe` nen them ngoai le `!selfhost/bootstrap/ivy-stage0.exe`;
`.gitattributes` them `-text` de git khong doi byte.

Seed **khong** phai build output: no chi can doi khi compiler khong con build
duoc source hien tai (feature moi dung truoc khi seed biet) hoac khi mot fix
phai co mat o the he dau.

## 3. `selfhost/build.ps1`

Tham so: `-TempRoot`, `-LlvmRoot`, `-Stage0`, `-Stage1File`, `-SelfLink`,
`-Golden`, `-SkipHashCheck`.

1. verify SHA-256 cua seed (bo qua voi `-SkipHashCheck`, va voi `-Stage0`
   duong dan tuy y);
2. do LLVM nhu cac harness (`-LlvmRoot` -> `LLVM_ROOT` -> `D:\Cpp\Clang` ->
   `llvm-config` PATH); moi run co `TEMP`/`TMP` rieng;
3. stage 0 build `rt.obj` tu `std/rt.ivy`, dat `IVYRT_LIB_PATH`;
4. stage 0 build 18 module + `main.ivy` -> stage 1, harness link (`$linkOrder`
   nhu H9.4);
5. stage 1 build lai tat ca -> stage 2; so **19 object + 18 `.ivm`** byte-exact;
6. stage 1 build lai `rt.obj`, so voi ban cua stage 0;
7. link stage 2, so EXE stage1/stage2;
8. `-SelfLink`: stage 1 tu link stage 2 bang `--self-link`, so voi harness;
9. `-Golden`: chay `check_golden.ps1 -Driver stage1 -Runtime <rt.obj>`.

Tat ca file trung gian nam trong `-TempRoot`; repo khong bi ghi.

## 4. `check_golden.ps1 -Runtime`

Gate cu chi chuan hoa `<RT>` cho `build\Release\ivyrt.lib` va `build\rt.obj`.
Khi `build.ps1` link bang `rt.obj` trong TEMP (dung, va la duong **bat buoc**
sau khi xoa `src/**`), 5 fixture `link` hong vi stderr con duong dan that. Them
tham so `-Runtime` (mac dinh `$env:IVYRT_LIB_PATH`, roi `build\rt.obj`) va
chuan hoa `<RT>` theo runtime **dang dung**. Chi doi cach in chan doan, khong
doi byte output cua compiler.

## 5. Ket qua (2026-11-21)

| Kiem tra | Ket qua |
|---|---|
| `build.ps1 -SelfLink -Golden` | **PASS**: seed hash dung, F2 19 OBJ + 18 IVM + `rt.obj`, EXE stage1/stage2 identical, EXE self-link identical |
| golden (khong oracle) | **PASS 12337 check, 0 failure** |
| **copy repo khong co `build/`** roi chay `build.ps1 -SelfLink -Golden` | **PASS y het** — khong oracle, khong `build\rt.obj`, khong `src/**` |
| `check_golden.ps1 -Modes link` kieu cu (`build\rt.obj`) | PASS 719/719 |
| oracle `build\Release\ivy.exe` | khong bi doc/ghi: size 27 031 552, SHA-256 `FFB376CF…313E` khong doi |

Bang chung truc tiep cho tieu chi H9.5: mot cay chi co source + seed + golden
van build va kiem duoc. Chi con thieu buoc "khong co `src/**`" (H9.9) va CI
dung seed (H9.6).

## 6. Con lai

- CI chi dung Ivy: H9.6 (thay oracle + CMake trong workflow bang seed +
  `build.ps1 -Golden`).
- Reproducible build (`/Brepro` cho moi link, khong duong dan tuyet doi, 2 thu
  muc khac nhau ra cung exe): H9.7.
- Seed hien duoc build tren may nay (duong dan LLVM nam trong PE neu co); H9.7
  moi cam ket reproducible.

---

# H9.6 - CI chi dung Ivy

## 1. Pham vi

H9.5 chung minh mot cay chi co `selfhost/` + seed van build duoc, nhung CI van
chay `dogfood.ps1` tren oracle C++ (`build\Release\ivy.exe`, CMake, `src/**`).
H9.6 doi workflow sang seed:

```
actions/checkout -> LLVM 21.1.6 (cache) -> build.ps1 -SelfLink -Golden
```

Khong CMake, khong `src/**`, khong `build/Release`.

## 2. Workflow `.github/workflows/selfhost-dogfood.yml`

| Buoc | Noi dung |
|---|---|
| Checkout | `actions/checkout@v4` |
| Cache LLVM | `C:\LLVM`, key `llvm-21.1.6-full-${{ runner.os }}-v1` (tang `-v1` khi layout archive/patch DIA doi) |
| Install LLVM | tai `clang+llvm-21.1.6-x86_64-pc-windows-msvc.tar.xz` (~900 MB) khi cache miss; `Test-FullLlvm` doi hoi `clang.exe`, `clang++.exe`, `llvm-config.exe`, `LLVMConfig.cmake`; patch DIA SDK **idempotent** (chi khi file con duong dan cu); them `bin` vao `GITHUB_PATH`, `LLVM_ROOT` + `IVY_LINKER` vao `GITHUB_ENV` |
| Bootstrap | `pwsh -File selfhost\build.ps1 -LlvmRoot $env:LLVM_ROOT -TempRoot $env:RUNNER_TEMP\h96 -SelfLink -Golden` |
| Failure logs | upload `*.err`/`*.out` khi fail |

LLVM cua runner: goi `llvm` cua chocolatey chi co command line tools (khong
header/lib/`llvm-config`), phai dung ban full. `IVY_LINKER` ghim dung cay dang
cache; `windows-latest` con co `clang++` khac tren PATH.

## 3. `<CLANG>` trong golden

5 fixture (`168`, `171`, `185`, `191`, `236`) in nguyen command line link vao
chan doan `linker failed (exit code 1120)`, nen golden cu chua
`D:\Cpp\Clang\bin\clang++.exe`. `freeze_golden.ps1` va `check_golden.ps1`
them `-Linker` (mac dinh `$env:IVY_LINKER`, roi `clang++` tren PATH - dung thu
tu driver tim linker) va chuan hoa ca 2 kieu slash thanh `<CLANG>` trong
`Normalize-Paths`. Chi doi `link/diag.txt` (5 record, -21 B/record). Ten
`clang++` tran khong bi thay (tranh doi output cua fixture).

## 4. Record dai bi lech `nbytes`

Ba file record tho (`interp/stderr.txt`, `link/stdout.txt`, `link/stderr.txt`)
duoc commit tu H9.3 truoc khi `.gitattributes` co `-text`: luc `git add`,
autocrlf bo `\r` khoi than record nhung header van giu `nbytes` cu, nen
`Read-Records` doc sai header ke tiep (`ef\n` 3 B trong khi header ghi 4 B).
Ban freeze moi ghi lai byte tho (`ef\r\n`); moi record khop `nbytes`.

## 5. Corpus phai la LF

Runner checkout bang `core.autocrlf=true` nen fixture thanh CRLF, trong khi
golden freeze tren cay LF. Chi `lexer/str/raw.ivy` lo ra: raw string cua no
trai 3 dong, token text in nguyen byte xuong dong nen `--lex`/`--tokens` lech
+3 B va `-o` lech +3 B (CI: 23 failure). `.gitattributes` them
`selfhost/corpus/** text eol=lf` va `std/** text eol=lf`; 155 fixture dang CRLF
trong working tree (blob da LF tu truoc) duoc chuan hoa lai. Golden khong doi
byte.

## 6. std `.ivm` phai co

Fixture `247_std_math`, `248_std_unique_ptr`, `249_std_vector` dung
`import math|unique_ptr|vector;`, va oracle chi tim `.ivm`
(`<cwd>/std/<name>.ivm`), khong compile `std/*.ivy`. `std/*.ivm` la build
product bi ignore nen checkout sach khong co -> CI bao `module 'vector' not
found` (21 failure). `build.ps1 -Golden` prebuild 3 interface do bang stage 1
vao `$runDir\std-ivm` va truyen `-IvmDir`; `check_golden.ps1`/
`freeze_golden.ps1` nhan `-IvmDir` va noi them `-I <dir>` sau `-I <fixture
dir>`. Byte interface do stage 1 sinh == ban oracle sinh == ban tung nam trong
`std/` (da so hash), nen baseline khong doi.

## 7. Ket qua (2026-11-22)

| Kiem tra | Ket qua |
|---|---|
| freeze lai golden (oracle, `-IvmDir`) | 1052 fixture, 3 974 156 B - **giong het** ban dang commit |
| **checkout sach khong oracle** (`git archive` LF, khong `std/*.ivm`, khong `build/`) + `build.ps1 -Golden` | **PASS**: F2 19 OBJ + 18 IVM + `rt.obj`, EXE identical, **12337 check, 0 failure** |
| junction `mklink /J` toi LLVM khac + prepend PATH (khong `IVY_LINKER`) | `-Modes link` PASS 719/719 nho `<CLANG>` |
| CI run `44926968286` (truoc fix) | **do**: 23 failure, dung 2 loai loi o muc 5 va 6 |
| CI run `44931247724` (commit `be7fdce`) | **xanh 7m53s**: Install LLVM 97s (cache cold), Bootstrap 339s, Post Cache 24s; baseline oracle cu 6m10s |
| CI run `44932030326` (cache am) | **xanh 6m21s**: Cache LLVM 45s, Install LLVM 1s, Bootstrap 323s; baseline 6m10s (+11s) |