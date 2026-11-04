# H6.1 - LLVM C API binding (`import c`)

## 1. Scope

H6.1 establishes the selfhost-side LLVM binding used by the later IR emitter
(H6.2) and object emission (H6.3). The C++ oracle (`src/codegen/codegen.h`,
`src/codegen/codegen.cpp`) builds IR with the LLVM C++ API; this port binds the
LLVM **C** API instead, because the Ivy CLI can only link C symbols through
`extern "C"`.

Delivered:

- `selfhost/codegen/codegen.ivy` - 132 `extern "C"` LLVM declarations plus the
  exported `LlvmC` pass-through facade (18 methods);
- `selfhost/codegen/smoke.ivy` - end-to-end smoke driver;
- `selfhost/codegen/build_smoke.ps1` - isolated build/link/run harness;
- `PLAN.md` H6.1 row.

Out of scope: IR lowering (H6.2), object emission (H6.3), linker orchestration
(H6.4). No codegen policy lives in the binding module.

No `src/**` file was modified and the frozen oracle was not rebuilt. Oracle
identity rechecked after the work: 27,031,552 bytes, mtime 2026-10-21 16:41:44.

## 2. Toolchain identity

| item | value |
|---|---|
| LLVM / clang | 21.1.6 |
| clang target | `x86_64-pc-windows-msvc` |
| root | `D:\Cpp\Clang` |
| headers | `D:\Cpp\Clang\include` (`llvm-c/Core.h`, `Analysis.h`, `Target.h`, `TargetMachine.h`) |
| libraries | `D:\Cpp\Clang\lib` |
| linker | MSVC `link.exe` 14.51.36231 (found through `LIB`) |

## 3. P0 feasibility probe

Scratch: `C:\Users\ASUS\AppData\Local\Temp\opencode\h61\`. Every candidate was
compiled with `build\Release\ivy.exe <file> -c -o <scratch>\<name>.obj` under a
fresh `TEMP`/`TMP`. No probe file was written into the repository.

### 3.1 Frontend acceptance

| probe | construct | result |
|---|---|---|
| A | `import c <llvm-c/Core.h>;` + `extern "C" void* LLVMContextCreate();` | accepted, `-c` exit 0 |
| A | `void*` local handle in `unsafe` | accepted |
| B | `int8*` / `char*` handles, `char*` parameter | accepted |
| C | named opaque struct pointer (`struct Ctx { long unused; }; Ctx*`) | accepted |
| D | raw pointer parameter on an ordinary file-scope function | **rejected**: `raw pointer parameter type requires an unsafe block` |
| E | `LLVMModuleCreateWithNameInContext`, `LLVMPrintModuleToString`, `LLVMDisposeMessage` | accepted, linked, ran |
| F | `void**` in an `extern "C"` parameter (`LLVMFunctionType`) | accepted |
| G | `extern "C" { ... }` block form, `unsigned long long`, `double` | accepted |
| I | `char* err; ... &err` as a `char**` out-parameter | accepted |
| J | cast a raw allocation to `void**`, index and pass it | accepted |
| K | raw pointer parameter/return on a **struct method** | accepted |
| L | calling an imported module's plain `extern "C"` declaration | **rejected**: not visible |
| L2 | `export extern "C" ...` | **rejected**: `expected a declaration after 'export'` |

Rejected spellings:

- array parameter syntax `void* params[]` - parse error;
- `&ps[0]` - `'&' is only supported on variables in the Ivy subset`;
- passing `void* ps[2]` where `void**` is expected - Ivy decays the array to
  `void*`, not `void**`.

### 3.2 Keywords found during the probe

`fn` and `size` are Ivy keywords, so they cannot be C parameter names
(`LLVMCountParams(void* fn)` and `LLVMBuildMemCpy(..., void* size)` failed to
parse). The binding uses `func` and `nbytes` instead. This is a spelling-only
change; the ABI is unchanged.

### 3.3 Link probe

A minimal program calling `LLVMContextCreate` / `LLVMContextDispose` linked and
ran with exit 0. `LLVMContextCreate` + module create + `ret void` + verify +
`LLVMPrintModuleToString` also linked and ran with exit 0.

Two link details:

1. `llvm-config --libs core target irreader` prints 25 absolute `.lib` paths;
   those are passed through unchanged.
2. `llvm-config --system-libs` prints bare `psapi.lib shell32.lib ...`. The
   clang GNU driver treats a bare `.lib` argument as a **file input** and fails
   with `no such file or directory`. Rewriting each name to `-l<name>` makes
   the MSVC linker search `LIB` and link succeeds.

`libxml2s.lib` is listed by `--system-libs` but is **not installed** in this
toolchain (`D:\Cpp\Clang\lib\libxml2s.lib` does not exist). The link succeeds
without it; the harness detects and skips missing system libs explicitly.

`clang-cl.exe` also links successfully, but the planned `clang` command is used
because it matches the requested form.

### 3.4 Other API findings

- `LLVMVerifyModule` lives in `llvm-c/Analysis.h` (not `Core.h`) in LLVM 21.
- `LLVMInitializeNativeTarget` is a `static inline` helper in `Target.h`, not an
  exported symbol. The binding declares the concrete
  `LLVMInitializeX86TargetInfo/Target/TargetMC/AsmPrinter/AsmParser` entry
  points instead.
- `import c <header>` is declaration-only: compiling with `-c` succeeds without
  `-I D:\Cpp\Clang\include`, confirming the header is never opened.

## 4. Binding coverage

`selfhost/codegen/codegen.ivy` declares 132 LLVM functions (plus the two Ivy
runtime helpers `__ivy_alloc` / `__ivy_free`), grouped as:

| group | count | examples |
|---|---:|---|
| context / module / messages | 12 | `LLVMContextCreate`, `LLVMModuleCreateWithNameInContext`, `LLVMPrintModuleToString`, `LLVMDisposeMessage` |
| types | 18 | `LLVMInt32TypeInContext`, `LLVMPointerTypeInContext`, `LLVMFunctionType`, `LLVMStructTypeInContext`, `LLVMArrayType2` |
| constants | 9 | `LLVMConstInt`, `LLVMConstReal`, `LLVMConstStringInContext`, `LLVMConstStructInContext` |
| globals | 7 | `LLVMAddGlobal`, `LLVMSetInitializer`, `LLVMSetLinkage`, `LLVMSetAlignment` |
| functions / parameters | 6 | `LLVMAddFunction`, `LLVMGetParam`, `LLVMSetFunctionCallConv` |
| blocks / builder | 6 | `LLVMAppendBasicBlockInContext`, `LLVMCreateBuilderInContext`, `LLVMPositionBuilderAtEnd` |
| instructions | 57 | `LLVMBuildRet`, `LLVMBuildCondBr`, `LLVMBuildCall2`, `LLVMBuildLoad2`, `LLVMBuildStore`, `LLVMBuildGEP2`, `LLVMBuildAlloca`, integer/float binops, `LLVMBuildICmp`/`FCmp`, casts, `LLVMBuildPhi`/`LLVMAddIncoming`, `LLVMBuildMemSet`/`MemCpy` |
| verification | 2 | `LLVMVerifyModule`, `LLVMVerifyFunction` |
| target / object emission | 15 | `LLVMGetDefaultTargetTriple`, `LLVMCreateTargetMachine`, `LLVMCreateTargetDataLayout`, `LLVMTargetMachineEmitToFile`, X86 initializers |

`LlvmC` (exported struct) exposes 18 methods; the smoke driver uses 16 of them:
context create/dispose, module create/dispose/print/verify, message dispose,
void/i32 types, function type, add function, append block, builder
create/position/dispose, `ret`/`ret void`, and `const int`. All forward to the
C API; `module_verify` additionally disposes the verifier error message.

## 5. Pointer arrays (`void**`) calling convention

Ivy has no `T[] -> T**` decay and rejects `&array[0]`, so a C `LLVMTypeRef *`
argument cannot be built from a local `void*` array directly. The supported
idiom is:

```ivy
void* raw = __ivy_alloc(count * 8);
void** items = (void**) raw;
items[0] = first;
items[1] = second;
// ... call the C function with `items` ...
__ivy_free(raw);
```

This was verified in probe J. The smoke driver avoids the allocation by passing
`(void**) 0` with a parameter count of 0. H6.2 will use the allocation idiom for
`LLVMFunctionType`, `LLVMBuildCall2`, `LLVMBuildGEP2`, and `LLVMAddIncoming`.

## 6. Build and link command

The Ivy CLI has no `-l`/`-L` flags, so the two objects are linked manually.
This is the link form used by `build_smoke.ps1` (the script derives the archive
list dynamically from `llvm-config`):

```powershell
# compile (module interface codegen.ivm is written into the scratch -I dir)
build\Release\ivy.exe selfhost\codegen\codegen.ivy -c `
    -I <scratch> -I selfhost\codegen -o <scratch>\codegen.obj
build\Release\ivy.exe selfhost\codegen\smoke.ivy -c `
    -I <scratch> -I selfhost\codegen -o <scratch>\smoke.obj

# link
D:\Cpp\Clang\bin\clang.exe <scratch>\smoke.obj <scratch>\codegen.obj `
    (D:\Cpp\Clang\bin\llvm-config.exe --libs core target irreader) `
    -lpsapi -lshell32 -lole32 -luuid -ladvapi32 -lws2_32 -lntdll `
    -o <scratch>\h61_smoke.exe
```

`llvm-config --libs core target irreader` resolves to these 25 archives:
`LLVMTarget, LLVMAnalysis, LLVMProfileData, LLVMSymbolize, LLVMDebugInfoBTF,
LLVMDebugInfoPDB, LLVMDebugInfoMSF, LLVMDebugInfoCodeView, LLVMDebugInfoGSYM,
LLVMDebugInfoDWARF, LLVMDebugInfoDWARFLowLevel, LLVMObject, LLVMTextAPI,
LLVMMCParser, LLVMIRReader, LLVMAsmParser, LLVMMC, LLVMBitReader, LLVMCore,
LLVMRemarks, LLVMBitstreamReader, LLVMBinaryFormat, LLVMTargetParser,
LLVMSupport, LLVMDemangle` (all under `D:\Cpp\Clang\lib`).

Run from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File selfhost/codegen/build_smoke.ps1 `
    -TempRoot C:\Users\ASUS\AppData\Local\Temp\opencode\h61\p3root
```

## 7. Smoke result

The smoke module builds `i32 @h61_answer()` with `ret i32 42`, verifies it with
`LLVMReturnStatusAction` (2), prints it with `LLVMPrintModuleToString`, disposes
everything, then writes the marker.

Final run (2026-11-03):

```text
[1/4] compiling binding module ...
[2/4] compiling smoke driver ...
[3/4] linking smoke executable ...
      note: skipping missing system lib libxml2s.lib
[4/4] running smoke executable ...
      scratch: C:\Users\ASUS\AppData\Local\Temp\opencode\h61\p3root\h61smoke_20261103_213841_016_12820
      stdout:  h61 smoke ok
      stderr:
      exit:    0
PASS: h61 smoke ok (LLVM C API binding build + run)
```

stdout bytes: `68 36 31 20 73 6D 6F 6B 65 20 6F 6B 0D 0A`
(`h61 smoke ok\r\n`); stderr is a zero-length file; exit status 0.

The harness accepts a single trailing CRLF or LF around the marker and fails on
any additional stdout bytes or non-empty stderr. The marker comparison is
case-sensitive (`-cne`).

## 8. Known gaps

1. **Extern declarations are module-private.** Ivy does not export `extern "C"`
   declarations and rejects `export extern "C"`, so importers must use the
   `LlvmC` facade. New C functions needed by other modules require new facade
   methods.
2. **No array-to-`T**` decay.** Pointer-array arguments need the
   `__ivy_alloc`/cast idiom (section 5).
3. **Raw pointer parameters/returns on free functions.** These are rejected
   outside `unsafe`, and there is no unsafe-function declaration form. The
   facade is therefore a struct (methods accept pointer parameters and return
   pointers); future emitter helpers should follow the same shape.
4. **Host-specific target initializers.** `LLVMInitializeNativeTarget` is
   inline, so the binding names the X86 entry points. H6.3 must link the X86
   target libraries (not part of `--libs core target irreader`).
5. **Missing `libxml2s.lib`.** Listed by `llvm-config --system-libs` but absent
   from this installation; the harness skips it and the link succeeds.
6. **Coverage is binding-only.** Only 16 of the 18 facade methods are exercised
   by the smoke; the remaining declarations are compile/link-checked but not
   called until H6.2/H6.3.
7. **No IR golden comparison.** H6.1 only proves the binding builds, links,
   verifies, and prints a module; IR text parity is H6.2.

## 9. State

Files added/modified for H6.1:

- `selfhost/codegen/codegen.ivy` (new);
- `selfhost/codegen/smoke.ivy` (new);
- `selfhost/codegen/build_smoke.ps1` (new);
- `selfhost/codegen/H61_NOTES.md` (this file);
- `PLAN.md` H6.1 row.

Not committed; the working tree is left for the user. No `.obj`, `.ivm`, or
`.exe` was written into the repository.
