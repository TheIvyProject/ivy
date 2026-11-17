# H8.2 - CI dogfood: compiler Ivy-built khop ban C++-built

## 1. Pham vi

H8.1 chung minh F2 va driver khop oracle tren corpus codegen. H8.2 bien dieu
do thanh **gate CI**: compiler do Ivy tu bien dich (`ivy_selfhost.exe`, stage 1)
phai cho ra **cung output** voi ban C++-built (oracle) tren **toan bo corpus**
selfhost, khong chi 249 fixture codegen.

Hai deliverable:

1. `selfhost/dogfood.ps1` - gate chay duoc o local va tren CI;
2. `.github/workflows/selfhost-dogfood.yml` - job `windows-latest` build oracle
   tu source roi chay gate.

Gate gom 2 buoc:

1. `selfhost/app/bootstrap_smoke.ps1`: oracle (stage 0) bien dich 17 module +
   `main.ivy` thanh stage 1, stage 1 tu bien dich lai thanh stage 2 va so sanh
   byte (F2);
2. `selfhost/app/driver_smoke.ps1 -SkipOwn -FullCorpus`: stage 1 chay tren moi
   thu muc fixture cua `selfhost/corpus` voi `--llvm` va `-c` (1052 fixture /
   30 thu muc), va link + chay tren corpus codegen (249 fixture).

Chi cac mode da port moi vao gate: `--llvm`, `-c`, link mode. `--lex`,
`--tokens`, `--ast`, `--hir`, `--mir`, `--run` van la stub exit 3 (viec cua cac
tang sau).

## 2. Cac mismatch tim ra va da sua

Chay stage 1 tren toan corpus lo ra 6 nhom lech (12 fixture), tat ca deu nam
trong `selfhost/codegen/emitter.ivy` tru hai cho:

- `pp/define_obj` (10/10): `parser.ivy` parse chain C-style type phai **mutually
  exclusive** nhu oracle (`else if`), va chi vao chain khi `t.base` con rong.
  Ban port cu chay mot day `if` nen `int long` an ca hai keyword va in them mot
  loi cnumber ma oracle khong in.
- `hir/constexpr` (70/70): oracle phan biet `consteval` voi `constexpr` - ham
  `consteval` khong co runtime representation nen bi bo qua o ca vong declare
  (`codegen.cpp:2674`) lan vong definition (`:2695`). Them field
  `is_consteval` xuyen `hir.ivy` HFunction -> `hir_builder.ivy`
  (`add_function`/`add_function_at` + 5 call site) -> `mir.ivy` MFunction ->
  `mir_builder.ivy`; `emitter.ivy` skip declare bodyless non-extern va
  definition.
- `hir/move` (60/60): `move(x)` chi la pass-through khi callee **khong co** MIR
  entry (`codegen.cpp:1539`), nen overload `move` do nguoi dung dinh nghia phai
  la call that. Dong thoi `-0.0` phai giu dau trong `emit_double_lit`
  (`1.0 / v < 0.0`).
- `mir/cfg` (75/75): jump target null (vi du `continue` trong `switch`) khong co
  trong `blockNames_` cua oracle, nen oracle in `br label %` rong - `emit_bb`
  in `%` khi index < 0.
- `hir/template` (99/99): bo guard array param trong `check_fn_signature`;
  array-to-pointer decay **vo dieu kien** (ca nhanh co param type lan fallback,
  khong cast); `emit_mangle_type` array mangle theo element type; call khong co
  MIR entry -> `emit_unresolved_symbol` (`?<name>@@YHX@Z`) + co `has_fn`.
- `hir/type` (124/124): `llvm_type("nullptr")` -> `i8` (chi `nullptr_t` la
  `ptr`); `find_call_target` phan loai float bang **spelling base**
  (`is_fp_ir(at.base)`), khong phai LLVM spelling, nen `double` khong khop
  `float64_t`; nhanh lvalue fallthrough bao dung message
  `expression is not assignable [nodeIdx=<kind> type.base=<base> isRef=<n>]` va
  tra `null`; `emit()` **tiep tuc** emit cac function sau khi loi (oracle chi
  set `hadError_`).

Hai fixture cuoi chi lo ra khi mo rong ra toan corpus:

- `hir/template/01_deduce_array.ivy`: oracle `msvcMangleType` khong co nhanh
  array nen `int32_t[4]` mangle theo element (`H`);
- `hir/type/64_overload_resolution.ivy`: cung ly do `isFpType(a.base)` o tren -
  `double` khong khop `float64_t`, fallback name-only chon overload dau tien.

## 3. Gate script

`selfhost/dogfood.ps1`:

- `-TempRoot` (moi thu ghi vao day, cung la TEMP/TMP cua process con),
  `-LlvmRoot`, `-TimeoutSec`;
- `-SkipBootstrap -Stage1 <exe>`: dung lai stage 1 da build (vong lap sua
  mismatch nhanh hon nhieu so voi bootstrap lai tu dau);
- goi `bootstrap_smoke.ps1` qua process con (`-Stage1File` de lay duong dan
  stage 1 vua link), roi `driver_smoke.ps1 -SkipOwn -FullCorpus`;
- in `--- DOGFOOD PASS ---`, exit 0 chi khi ca hai buoc dat.

`selfhost/app/driver_smoke.ps1` duoc mo rong:

- `-Corpus` / `-LinkCorpus`: them thu muc fixture cho `--llvm` + `-c` va cho
  link+run;
- `-FullCorpus`: moi thu muc co `.ivy` duoi `selfhost/corpus` (codegen luon co
  mat, link+run van chi tren codegen);
- `-SkipOwn`: bo section 1 (driver tu build module cua chinh no) vi gate da
  phu bang F2;
- moi thu muc la mot `-I` rieng, ten fixture in tuong doi so `selfhost/corpus`
  (hai thu muc deu co `01_x.ivy` van phan biet duoc);
- stderr cua `-c` duoc chuan hoa ca duong dan object **va** `.ivm` di kem
  (`-c` luon ghi module interface canh object), neu khong hai fixture
  `export module` bao DIFF gia.

`selfhost/app/bootstrap_smoke.ps1` duoc mo rong:

- auto-detect LLVM: `-LlvmRoot` -> `$env:LLVM_ROOT` -> `D:\Cpp\Clang` ->
  `llvm-config` tren PATH (CI chi can dat `LLVM_ROOT`);
- `-Stage1File`: ghi duong dan stage 1 vua link ra file de gate dung lai;
- khi link fail: in ca stderr, stdout (linker MSVC bao symbol thieu o stdout) va
  command line, de log CI du de chan doan.

## 4. Workflow

`.github/workflows/selfhost-dogfood.yml` (`windows-latest`, timeout 120 phut):

1. checkout;
2. cai **LLVM day du**: ban LLVM co san tren runner (goi choco `llvm`, tuc la
   `LLVM-<ver>-win64.exe`) chi co CLI tools - khong headers, khong libs,
   khong `llvm-config`, khong CMake package
   (actions/runner-images#12565), nen job tai archive
   `clang+llvm-21.1.6-x86_64-pc-windows-msvc.tar.xz` (899 MB) - dung phien ban
   LLVM ma source dang viet (`Module::setTargetTriple(Triple)` chi co o 21.x,
   khong co o 20.x) - giai nen vao `C:\LLVM`, them `bin` vao `PATH` (oracle goi
   `clang++` khi link) va dat `LLVM_ROOT`; va **patch DIA SDK**: package
   Windows cua LLVM ghi cung duong dan `.../Visual Studio/2019/Professional/DIA
   SDK/lib/amd64/diaguids.lib` vao `LLVMExports.cmake` (link se bao LNK1181),
   nen job tro no ve DIA SDK cua runner (giong patch thu cong ma CMakeLists.txt
   ghi cho may dev);
3. CMake (generator mac dinh cho VS cai san - runner da chuyen sang VS 2026)
   `-A x64 -DLLVM_DIR=...` build oracle Release (`build/Release/ivy.exe` khong
   duoc commit);
4. `selfhost/dogfood.ps1 -TempRoot $env:RUNNER_TEMP\h82`;
5. khi fail: upload `${{ runner.temp }}/h82/**/*.err` **va** `*.out` lam artifact
   (linker MSVC in symbol thieu ra stdout, khong phai stderr).

Cac vong sua workflow (moi vong mot commit, chi sua workflow + notes):

1. that bai o buoc cai LLVM: ban LLVM co san chi co CLI tools (muc 2) -> tai
   archive `clang+llvm` day du;
2. that bai o buoc CMake: `windows-latest` da chuyen sang VS 2026, generator
   `Visual Studio 17 2022` khong con -> bo `-G`, de CMake tu chon;
3. that bai khi compile oracle: source can LLVM 21
   (`Module::setTargetTriple(Triple)`) -> dung archive 21.1.6;
4. that bai khi link oracle: `LLVMExports.cmake` cua package ghi cung duong dan
   DIA SDK -> patch (muc 2);
5. that bai khi link stage 1 (F2): `LNK2019: unresolved external symbol
   __imp_RtlGetLastNtStatus` (can `ntdll.lib`). Gate loc `--system-libs` bang
   cach kiem tra file co trong `lib` cua LLVM hoac trong `$env:LIB`; runner
   khong set `LIB` (chay ngoai developer prompt) nen `ntdll.lib` bi bo, trong
   khi may dev co `LIB` nen khong lo ra. Sua: them danh sach lib cua Windows
   SDK/MSVC (`$sdkLibs`) duoc giu lai du khong tim thay file - clang tu tim
   chung qua search path no dat cho linker; lib khong ship cua LLVM
   (`libxml2s.lib`, chi `LLVMWindowsManifest` dung) van bi bo. Sua o ca
   `bootstrap_smoke.ps1` va `build_object_smoke.ps1`.

Tat ca cac loi tren chi xay ra o moi truong CI: gate chay day du o local
(`F2 PASS` + parity toan corpus) ca khi `LIB` duoc set lan khi khong.

## 5. Ket qua

| Kiem tra | Ket qua |
|---|---|
| `selfhost/app/bootstrap_smoke.ps1` - stage0 -> stage1 -> stage2 | **F2 PASS**: 18/18 object, 17/17 `.ivm`, EXE byte-identical |
| `driver_smoke.ps1 -SkipOwn -FullCorpus` - `--llvm` | 1052 / 1052 match |
| `driver_smoke.ps1 -SkipOwn -FullCorpus` - `-c` | 1052 / 1052 match |
| `driver_smoke.ps1 -SkipOwn -FullCorpus` - link + run | 249 / 249 match (2 fixture UB chi so buoc link) |
| `selfhost/dogfood.ps1 -SkipBootstrap -Stage1 <stage1>` | **DOGFOOD PASS** |

GitHub Actions (`selfhost dogfood`, run `44128908682`, commit `1788353`,
`windows-latest`, 6m14s): `--- F2 PASS ---`, `1052 / 1052 match` cho ca `--llvm`
va `-c`, `249 / 249 match` cho link + run, `--- DOGFOOD PASS ---`.

## 6. Con lai

- Job CI tai lai archive LLVM 21.1.6 (~900 MB) moi lan chay; chua cache.
- Gate chua gom cac mode dump/interpreter (`--lex`, `--ast`, `--hir`, `--mir`,
  `--run`) - chung van la stub exit 3 cua driver, se vao gate khi cac tang
  tuong ung duoc port.
