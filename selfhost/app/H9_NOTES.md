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
