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
- `-Stage1File`: ghi duong dan stage 1 vua link ra file de gate dung lai.

## 4. Workflow

`.github/workflows/selfhost-dogfood.yml` (`windows-latest`, timeout 120 phut):

1. checkout;
2. `choco install llvm` -> `C:\Program Files\LLVM`, them `bin` vao `PATH`
   (oracle goi `clang++` khi link) va dat `LLVM_ROOT`;
3. CMake `-G "Visual Studio 17 2022" -A x64 -DLLVM_DIR=...` build oracle
   Release (`build/Release/ivy.exe` khong duoc commit);
4. `selfhost/dogfood.ps1 -TempRoot $env:RUNNER_TEMP\h82`;
5. khi fail: upload `${{ runner.temp }}/h82/**/*.err` lam artifact.

Workflow chua chay duoc o local; co the phai chinh sau lan CI dau (phien ban
VS/LLVM tren runner, duong dan LLVM). Phan gate thi da chay that o local.

## 5. Ket qua

| Kiem tra | Ket qua |
|---|---|
| `selfhost/app/bootstrap_smoke.ps1` - stage0 -> stage1 -> stage2 | **F2 PASS**: 18/18 object, 17/17 `.ivm`, EXE byte-identical |
| `driver_smoke.ps1 -SkipOwn -FullCorpus` - `--llvm` | 1052 / 1052 match |
| `driver_smoke.ps1 -SkipOwn -FullCorpus` - `-c` | 1052 / 1052 match |
| `driver_smoke.ps1 -SkipOwn -FullCorpus` - link + run | 249 / 249 match (2 fixture UB chi so buoc link) |
| `selfhost/dogfood.ps1 -SkipBootstrap -Stage1 <stage1>` | **DOGFOOD PASS** |

## 6. Con lai

- Workflow chi duoc kiem chung khi chay that tren GitHub Actions.
- Gate chua gom cac mode dump/interpreter (`--lex`, `--ast`, `--hir`, `--mir`,
  `--run`) - chung van la stub exit 3 cua driver, se vao gate khi cac tang
  tuong ung duoc port.
