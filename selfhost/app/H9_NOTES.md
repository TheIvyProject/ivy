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