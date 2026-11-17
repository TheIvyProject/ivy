# H8.1 - `ivy` bien dich chinh no (F2)

## 1. Pham vi

H8.1 hoan tat `run(...)` cua oracle (`src/app/main.cpp:825-1450`) va chung
minh **F2**: compiler do Ivy-built bien dich chinh no, cho ra binary giong
het compiler o stage truoc.

Bon phan:

1. **pipeline dung chung** - `selfhost/app/pipeline.ivy`: `struct Cg` (truoc
   day nam trong `cgdrive.ivy`) gom lexer -> preprocessor -> import scan ->
   parser -> `.ivm` -> HIR -> MIR. `compile()`, `unit()`, `diag_file()`,
   `release()`, `import_count()`, `module_object()`;
2. **`-c`** - IR text -> `codegen::ObjEmit` -> object native;
3. **link mode** - IR text -> `codegen::Linker` (+ D1 module objects +
   `libivyrt`) -> executable; unit khong co `main` thi ha xuong object
   (main.cpp:1367-1397);
4. **`--llvm`** - IR ra stdout hoac ra `-o`.

Deliverables:

- `selfhost/app/pipeline.ivy` (moi): `Cg` + helper dung chung;
- `selfhost/app/main.ivy`: driver hop nhat (`struct Driver`);
- `selfhost/codegen/cgdrive.ivy`: driver mong chi con doc stdin + goi `Cg`;
- `selfhost/codegen/codegen.ivy`: `Linker` them `mods_/nmod_/capmod_` +
  `add_module_object()`, chen module objects truoc `-o`;
- `selfhost/mir/mir.ivy`: `MUnit.has_main()`;
- `selfhost/parsing/module_io.ivy`: `escape` ten type goc (xem 6);
- `selfhost/app/bootstrap_smoke.ps1`: harness F2 (stage0 -> stage1 -> stage2);
- `selfhost/app/driver_smoke.ps1`: harness doi chieu driver voi oracle;
- file nay + `PLAN.md` dong H8.1.

Khong sua `src/**`, khong rebuild oracle.

## 2. Bat IR text tu stdout

`Emitter` ghi IR ra stdout cua tien trinh, giong oracle. Hai che do can text
trong bo nho (`-c`, link) hoac trong file nguoi dung dat (`--llvm -o`) nen
driver lam dung viec ma harness H6.3/H6.4 da lam: **chuyen huong fd 1** vao
file tam bang `_open`/`_dup`/`_dup2`, goi `emitter`, roi khoi phuc fd 1 va
doc lai file bang `io::read_file`.

- file tam: `_tempnam(TEMP|TMP, "ivy")` + `.ll` (duy nhat moi lan chay, khong
  de lai file trong repo), xoa bang `remove()` sau khi dung;
- fd 1 duoc khoi phuc **truoc** khi in bat ky chan doan nao, nen
  `ivy: '<path>' generated` van ra stderr binh thuong;
- IR text giu nguyen CRLF, da chung minh object bytes khop oracle.

## 3. `-c`, link, `--llvm`

`-c` (`main.cpp:1340-1365`):

- `obj_path = -o` neu co, nguoc lai `<dir>/<stem>.obj`;
- IR -> `ObjEmit.emit_text()` -> `ivy: '<obj>' generated` (stderr);
- loi: `<diagFile>:0:0: error: [LLVM IR parse error: ivy: |
  LLVM target lookup failed: ]<err>`.

Link (`main.cpp:1367-1425`):

- **D1**: unit khong co `main` (`MUnit.has_main()`) -> chi emit object, khong
  link. `has_main` mirror `mir::Unit::hasMain`: `name == "main"`, co body,
  khong `extern "C"`;
- nguoc lai: IR -> `Linker.link_text()` voi danh sach module object cua moi
  import da resolve (`<dir>/<stem>.obj` neu ton tai) + `libivyrt`;
- `libivyrt` cua selfhost: `IVYRT_LIB_PATH` (env), roi `<dir argv0>/ivyrt.lib`,
  roi `<dir argv0>/libivyrt.a`, roi chuoi rong (link khong runtime);
- chan doan link mirror `CodeGen::linkExecutable` (code 1/2/3).

`--llvm` (`main.cpp:1427-1448`): khong `-o` -> IR ra stdout; co `-o` ->
`ivy: error: cannot open output file '<path>'` (exit 2) hoac ghi file.

## 4. Vi sao driver la `struct Driver`

Oracle HIR builder khong cho **free function** nhan/tra raw pointer ma khong
co lifetime annotation (`hir_builder.cpp:855-886`), trong khi **method cua
struct** thi duoc. Toan bo helper (`slen`, `dir_of`, `temp_ir_path`,
`capture_ir`, `emit_object`, `link_executable`, ...) vi vay la method cua
`struct Driver`. Hai he qua khac cung duoc ghi nho:

- **khong** lay dia chi struct local (`&oe`, `&lk`): HIR builder oracle khong
  ho tro, gay crash/hong ten struct -> `ObjEmit`/`Linker`/`Cg` duoc truyen
  **by value**;
- **khong** dat ten bien local la `tmp`: slot `%tmp.<n>` trung temp counter
  cua oracle emitter -> IR invalid (`multiple definition of local value named
  'tmp.5'`). Da doi thanh `scratch`.

## 5. Stage0 -> stage1 -> stage2 (F2)

`selfhost/app/bootstrap_smoke.ps1`: oracle bien dich 17 module + `main.ivy`
(moi module 1 object), roi **clang + `llvm-config --libs x86` +
`--system-libs`** link thanh `ivy_selfhost.exe` (selfhost compiler khong tu
link duoc vi can LLVM libs - giong `build_object_smoke.ps1`). Sau do stage1 tu
bien dich lai 17 module + `main.ivy`, so sanh:

- **18/18 object byte-identical** (stage1 vs stage2);
- **17/17 `.ivm` byte-identical**;
- **executable byte-identical** -> **F2 PASS**.

Link duoc lam deterministic bang `-Wl,/Brepro` (chi trong harness): khong co
no, hai lan link cung input van khac nhau o 2 field timestamp trong PE header
va debug directory (da kiem chung: chi 2 byte khac nhau, dung 32-bit
`TimeDateStamp`).

## 6. Bug tim ra o stage2: `escape` ten type goc trong `.ivm`

Stage2 that bai o `main.ivy`: stage1 sinh `codegen.ivm` **3192** byte trong khi
oracle sinh **3196**, va stage2 bao `struct 'ObjEmit' has no method 'code'`.

Nguyen nhan: `module_io.cpp` viet `escape(serializeType(p.type))` - tuc la
escape **ca chuoi type**, ke ca ten goc. Type C-style `long long` chua dau
cach, nen oracle ghi `long\slong:unsigned:n`. Port chi escape ten field/tham
so ma **khong** escape ten goc trong `type()` -> ghi `long long:unsigned:n`.

Sua: `type()` -> `type_x(t, 1)` (escape ten goc), them `type_raw()` ->
`type_x(t, 0)` cho **return type cua free function**, vi oracle ghi
`serializeType(f.returnType)` **khong** escape (`module_io.cpp:132`) - day la
quirk phai giu de byte-identical.

Bug nay chi lo ra o `codegen.ivy` (module duy nhat dung `long long`), nen cac
module khac van khop truoc do.

## 7. Ket qua

| Kiem tra | Ket qua |
|---|---|
| `selfhost/app/bootstrap_smoke.ps1` - stage0 -> stage1 -> stage2 | **F2 PASS**: 18/18 object, 17/17 `.ivm`, EXE byte-identical |
| `selfhost/app/driver_smoke.ps1` - driver vs oracle | 17/17 object cua chinh compiler, `--llvm` 249/249, `-c` 249/249, link+run 249/249 |
| `selfhost/codegen/cgdiff.ps1` - corpus codegen qua `cgdrive` | PASS 249/249 |

Hai fixture `138_new_struct_noparen.ivy` (`p->y` chua khoi tao) va
`226_structured_binding_mixed.ivy` (in gia tri khong xac dinh) **khong on
dinh** ngay tren exe cua oracle (moi lan chay mot khac), nen `driver_smoke` chi
so buoc link cho chung.

Trong qua trinh phat trien con dung them 2 harness tam (khong dua vao repo):
so IR 19 module cua `cgdrive` voi oracle (19/19 byte-identical, gom `pipeline`)
va so rieng buoc link; ket qua trung voi 2 harness chinh thuc o tren.

## 8. Con lai (ngoai H8.1)

Driver van stub exit 3 voi chan doan ro rang cho: `--lex`, `--tokens`,
`-o *.i|*.ii`, `--ast`, `--hir`, `--mir`, `--run`. Khong can cho F2; cac
tang dump/interpreter la viec cua cac muc sau.
