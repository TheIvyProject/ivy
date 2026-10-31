# H5.3 Notes — MIR Interpreter Execution (Research & Specification)

## 0. Ground Truth & Environment

- **Repository root**: `D:\project\Ivy\ivyc`
- **HEAD at review**: `918c1db` (`selfhost: fix H5.2 doc counts and citations`). The tree was clean at this HEAD **before this untracked draft was created**; current status includes `?? selfhost/mir/H53_NOTES.md`.
- **C++ oracle**: `build\Release\ivy.exe`, frozen at 27,031,552 bytes, mtime `2026-10-21 16:41:44`. Do not rebuild it.
- **Designated scratch path**: `C:\Users\ASUS\AppData\Local\Temp\opencode\h53specfix\`.
- **Scope**: This document specifies H5.3 and the H5.4 integration boundary. This review changes no source or oracle files and creates no commit.

---

## 1. Oracle Architecture & Runtime Model

The C++ interpreter is implemented in `src/mir/interp_value.h`, `src/mir/interp_memory.h`, `src/mir/interp_machine.h`, and `src/mir/interpreter.{h,cpp}`.

### 1.1 Values, cells, and memory hooks

- `ivy::mir::Value` is an ordinary **flat/hybrid tagged struct**, not a C++ union. It contains `kind`, integer and floating payloads, pointer and struct payloads, and string storage (`src/mir/interp_value.h:29-50`). `kind` selects the meaningful payload. The exact MSVC ABI size is not established and is not a specification invariant.
- Kinds are `Void`, `Int`, `Float`, `Ptr`, `Struct`, and `Str` (`interp_value.h:30-50`). Integer and float payloads are `long long` and `double`; pointers contain a shared `Cell`, `Provenance`, and null flag; struct values contain a type name and named field cells; strings hold decoded bytes.
- `Cell` is `std::shared_ptr<Value>`; references and pointers can share a mutable value slot (`interp_value.h:22-28`). `Provenance` records a kind, allocation id, and lifetime name (`interp_value.h:14-20`), but it does not mean every access is validated.
- `Memory`/`Allocation` declare raw-byte storage, initialization masks, and liveness (`src/mir/interp_memory.h:11-35`). `Machine` declares memory and call/return hooks (`src/mir/interp_machine.h:13-33`). Neither is used for ordinary interpreter pointer reads/writes. Dereference checks pointer type and null only, then reads its cell (`src/mir/interpreter.cpp:618-623`). Do not claim initialized-memory, provenance, or use-after-free diagnostics.

### 1.2 Interpreter API and lifetime

- Public API: `Interpreter(const TranslationUnit&, Machine*)`, `callMain()`, and `call(name,args)` (`src/mir/interpreter.h:41-56`). The caller owns the `TranslationUnit` and MIR storage for the full call/use (`interpreter.h:70-74`).
- `callMain()` calls `main` with zero arguments (`src/mir/interpreter.cpp:44-49`). `call()` selects a body and invokes it (`interpreter.cpp:51-92`). Its arity matcher checks argument counts (extra arguments are allowed for `extern "C"` functions), not argument types (`interpreter.cpp:51-65`). MIR call-target resolution is performed by the MIR builder (`src/mir/mir_builder.cpp:1244-1286`).
- The API has output setters and defaults to `std::cout`/`std::cerr` (`src/mir/interpreter.h:52-56`, `src/mir/interpreter.cpp:35-38`). The CLI `--run` path does not call those setters; it prints diagnostics itself (`src/app/main.cpp:1327-1337`).

---

## 2. Value, Alias, and Operation Semantics

- Integers and floats use the `long long`/`double` payloads; bool and char values are represented as integers. String literals are decoded to byte strings (`src/mir/interp_value.h:30-50`, `src/mir/interpreter.cpp:313-325`).
- Reference declarations bind the target lvalue cell; assignment writes through aliases (`src/mir/interpreter.cpp:195-210,235-244,663-670`). Local arrays are pseudo-structs named `__array`, with decimal element names and zero-valued cells (`interpreter.cpp:180-193`). Array indexing is bounds checked. Pointer indexing is unsupported and reports `IvyInterpret v0.2: pointer index not yet supported` (`interpreter.cpp:383-403`).
- Integer binary operations include arithmetic, comparisons, bitwise operations, and shifts; division/modulo by zero report runtime errors. Float operations include arithmetic and comparisons. String binary operations support equality/inequality only (`interpreter.cpp:520-597`, especially `581-585`).
- Pointer comparisons compare **nullness**, not cell identity (`interpreter.cpp:587-593`); do not test or promise pointer identity ordering/arithmetic.
- Unary operations include address-of, dereference, integer increment/decrement, sign, logical-not, bitwise-not, and unary-plus (`interpreter.cpp:603-657`). Assignment and compound-assignment operators are handled in `interpreter.cpp:663-715`.
- MIR instructions are `Alloca`, `Store`, `Eval`, `Ret`, `CondBranch`, `Jump`, and `Switch` (`src/mir/interpreter.cpp:158-300`). They walk blocks/instructions and update the current block/index.
- Call arguments are evaluated left-to-right (`interpreter.cpp:905-911`). Functions bind parameters into frame cells and pop the frame on return (`interpreter.cpp:124-155`). Recursive execution therefore requires independent frame records and stable cells across nested calls.

---

## 3. Builtins, I/O, and Runtime Diagnostics

### 3.1 I/O routes

- `io::print`, `io::println`, `io::eprint`, and `io::eprintln` write to distinct output/error streams; `{}` placeholders substitute subsequent arguments (`src/mir/interpreter.cpp:913-962`). When the first argument is a string literal, only `{}` consumes extra arguments; if it has no `{}`, extras are ignored. Without a literal format string, all arguments are printed sequentially (`:924-959`).
- `ivy::print`/`ivy::println` use type-aware formatting (`interpreter.cpp:964-984`). `printf`, `puts`, and `putchar` are interpreter builtins (`interpreter.cpp:1053-1073,1159-1244`); `printf` implements a limited format parser, not a general libc implementation.
- Arbitrary `extern` calls are not dynamically executed. `io::read_line`, `io::read_file`, and `io::write_file` are described as compiler-lowered routines in `std/io.ivy:10-13,30-38`, but are not interpreter builtins (`interpreter.cpp:1053-1073`). Do not claim interpreter support for them.

### 3.2 Allocation and termination

- For an **interpreted program**, `__ivy_alloc(n)`/`malloc(n)` returns a pointer to one cell holding integer zero; `n` only clamps to at least one and does not create an `n`-byte allocation (`src/mir/interpreter.cpp:1131-1147`). `__ivy_free(p)`/`free(p)` is null-safe and marks the pointee `Value.kind = Void` (`:1148-1157`). Dereference has no use-after-free check.
- This differs from the **compiled selfhost driver's own pool storage**: `lib/ivyrt.c:32-41` implements `__ivy_alloc`/`__ivy_free` with native `malloc`/`free`. Pool backing allocations must not be confused with builtin behavior of a program being interpreted.
- `exit(code)` calls `std::exit` with the integer argument cast to `int` (zero if absent/non-integer); `abort()` calls `std::abort` (`interpreter.cpp:1245-1248`). The selfhost engine must represent interpreted termination so the driver can preserve the intended result without prematurely killing a host that still needs to report the fixture. Exact `abort()` status/output and the selfhost termination-record design are open probes in §9.

### 3.3 Result, status, and diagnostics

- CLI `ivy --run <file>` already exists (`src/app/main.cpp:1493-1494`). It calls `callMain()`. On interpreter failure, diagnostics are printed to stderr as `<diagFile>:<line>:<col>: error: <message>\n` and status is 1; otherwise an integer `main` result is cast to process `int`, and a non-integer result maps to 0 (`main.cpp:1327-1337`).
- Runtime errors include division/modulo by zero, null/non-pointer dereference, and array out-of-bounds (`src/mir/interpreter.cpp:552-553,618-623,391-395`). Preserve diagnostic line/column/message and failure status. Exact process stream/status comparison is the H5.4 gate (§10).

---

## 4. CLI and Interpreter Contract

The oracle command is `ivy --run <file.ivy>`; this is existing behavior, not a proposed new switch (`src/app/main.cpp:1493-1494`). The frontend builds MIR before the run path; the interpreter is constructed over that live MIR and calls `main` with no arguments (`main.cpp:1325-1329`, `src/mir/interpreter.cpp:44-49`). On success, process status follows integer `main` return or zero for non-integer return; interpreter diagnostics force status 1 (`main.cpp:1330-1337`).

For H5.3, define an engine result containing the interpreted return value/status, separate internal stdout and stderr byte buffers, and runtime diagnostic records (source line, column, message). Give diagnostic formatting a driver-supplied source filename; the oracle CLI format is specified in §3.3. The caller must retain MIR/unit storage for the entire execution.

---

## 5. Driver & Transport Boundary

- `selfhost/mir/mirdrive.ivy` is present and dumps MIR; it reads source paths, runs the frontend/builders, and prints a MIR dump or stage diagnostics (`selfhost/mir/mirdrive.ivy:1-17`). **No `interpreter.ivy`, `interpdrive.ivy`, or `selfhost/corpus/interp/` exists.**
- The `difftest.ps1` `-Oracle` parameter passes through `--run`; therefore `-Oracle --run` is a valid oracle binding (`selfhost/difftest.ps1:155-163,282-292`). The oracle's `--run` option is already implemented.
- Current `difftest.ps1` is not an interpreter comparison harness: it merges stdout/stderr with `2>&1`, does not compare process exit codes, drops empty lines, and reconstructs output lines/terminators (`difftest.ps1:232-250,255-262,273-279,288,298`). It cannot prove distinct streams, process status, or raw program bytes.
- H5.3 ends at execution over live selfhost `MUnit`, a result/status, separate internal output/error byte buffers, supported expression/instruction behavior, and runtime diagnostics. H5.4 adds the selfhost driver, connects those buffers to host stdout/stderr, maps process status, and upgrades the harness to compare stdout, stderr, and status independently without data loss. Run each fixture in its own process or preserve per-fixture stream/status boundaries.

---

## 6. Proposed Self-Host Representation

Use pool-backed tagged values, cells, frames, strings, and metadata. Pool arrays are an established selfhost pattern (`selfhost/mir/mir.ivy:4-7,131-161`); integer handles avoid native pointer ownership complexity.

- **`IValue` pool**: kind plus payload/handle fields for integer, float, pointer, struct, and string values. The field list and record layout are implementation choices. Any Ivy record-size estimates are **provisional**, must be measured from actual Ivy type layout when implemented, and are never spec invariants. Do not assert C++ `sizeof(Value)` or any C++ ABI size.
- **Stable `ICell` handles**: a pointer/reference contains a cell-pool index and null state (plus provenance fields if parity is claimed). Names, references, and pointers that alias must use the same cell index. Do not recycle a cell handle during an interpreter run; retain cells long enough for aliases to remain stable across nested frames and returned pointers. Include pointer provenance if claiming parity with the oracle's value model; it does not imply new safety diagnostics.
- **Strings**: store decoded bytes plus explicit length in a byte pool. Length is required for embedded zero bytes; do not treat a string as a NUL-terminated host pointer.
- **Frames**: frame pool records identify the function, caller/continuation, current block/instruction, return state/value, and local-name-to-cell mappings. Frame pop removes local bindings but must not invalidate a cell still referenced elsewhere during the run.
- **Function/call metadata prerequisite**: `MExpr` calls currently carry callee/method names and argument runs, not a resolved target index (`selfhost/mir/mir.ivy:41-62`). Add a stable target/function index or define signature-aware lookup over `MFunction`/`MParam`; a name-only fallback is ambiguous with overloads. Use the MIR builder target-resolution behavior as the reference (`src/mir/mir_builder.cpp:1254-1286`).
- **Aggregate metadata prerequisite**: `MUnit` currently has no struct definitions, fields, bases, or vtables (`selfhost/mir/mir.ivy:131-161`). Member/struct/virtual parity requires adding metadata and defining lookup/dispatch, or excluding those scenarios. Expression variants in the enum are not evidence that the selfhost frontend can construct or execute them.
- Keep the interpreted-program `__ivy_alloc`/`__ivy_free` semantics (§3.2) separate from native allocations used by the compiled selfhost driver's pools (`lib/ivyrt.c:32-41`).

---

## 7. Staged H5.3/H5.4 Plan and Gates

Each gate uses the per-case assertions in §8 and oracle fixtures run with the frozen `ivy --run` binary. Exclusions are deliberate until the named prerequisite is complete.

1. **P0 — Metadata, lookup, and pools**: define MUnit function lookup/target metadata, value/cell pools, stable alias handles, byte strings, and frame/call-record schema. **Gate**: pool round trips for every value kind; alias writes observed through every alias; nested frame/cell handles remain stable. **Exclude**: executable fixture checkpoint, aggregates/virtual dispatch.
2. **P1 — Scalar expressions and lvalues**: literals, identifiers, supported unary/binary/assignment/cast behavior, references, dereference, and array index. **Gate**: evaluate representative V01-V08 expressions against a prepared test frame/value pool; assert values and alias writes. Full fixture execution waits for P3. Explicitly check oracle string/pointer limits. **Exclude**: pointer indexing/arithmetic, struct members, and unsupported expression variants.
3. **P2 — Calls and frames**: target resolution, left-to-right argument evaluation, arity behavior, parameter binding, return values, frame lifetime, and recursion. Implement the minimal Ret/CondBranch/Jump traversal needed to call and recurse. **Gate**: V09-V10 and V13 plus nested-frame alias assertions, including correct return results and frame cleanup. **An executable checkpoint requires this phase and the minimal CFG paths**; expression evaluation alone is not an executable interpreter. **Exclude**: unresolved/ambiguous overloads until metadata lookup is proven.
4. **P3 — Complete CFG instructions**: Alloca/Store/Eval/Ret/CondBranch/Jump/Switch and block transitions, completing nonrecursive fixture execution paths. **Gate**: full V01-V12 fixture assertions for branch/loop/switch results and function-return values. **Exclude**: constructs unreachable in the current MIR frontend.
5. **P4 — Reachable builtins, diagnostics, and results**: array cells/bounds, supported `io::*`, `ivy::*`, C output builtins, interpreted allocation semantics, diagnostic records, and interpreter termination/result records. **Gate**: V14-V20 and V23-V24, including separate internal stdout/stderr and exact diagnostic tuple/status assertions. V21-V22 process-exit mapping is finalized at P6. **Exclude**: arbitrary extern calls, std file/line I/O, post-free safety diagnostics, unsupported aggregates.
6. **P5 — Aggregate/feature subphases**: only after adding required MUnit metadata and closing the corresponding G1-G6 frontend gap. Add feature-specific MIR reachability and tests before claiming support; see §8.2. **Gate**: each feature's dedicated oracle fixture and metadata lookup/dispatch assertions. **Exclude**: all unclosed G1-G6 features.
7. **P6 — H5.4 integration**: implement `interpdrive.ivy`, host stream/status mapping, and the harness changes in §10. **Gate**: every §8 supported fixture compares separate stdout, stderr, and process status while preserving exact bytes and fixture boundaries; V21/V22 verify integer/non-integer `main` process mapping.

---

## 8. Corpus and Explicit Case Matrix

The filenames below are a proposed test plan, not existing files. Expected values are assertions, not a claim that any case has already run. Unless a row says otherwise, a passing non-diagnostic fixture has empty stderr and process status 0; stdout is empty unless explicitly given. Encode no-output assertions in the fixture by returning 0 when checks pass and 1 when they fail; output fixtures also compare their exact stream bytes. Execute each case in its own process, or preserve per-fixture result/status boundaries. Process-status mapping cases are called out separately.

### 8.1 Initially reachable through the current frontend

“Reachable” means the current parser/HIR/MIR pipeline can be asked to lower the construct; runtime execution awaits the interpreter. For function cases, target-index/overload metadata is still a P0 prerequisite. Validate each proposed fixture through the frontend before adding it to the corpus.

| ID / proposed fixture | Behavior and explicit assertion | Frontend reachability |
|---|---|---|
| V01 `01_int_ops.ivy` | Assert `(9 + 3) * 2 == 24`; stdout `int=24\n`, status 0. | Reachable: scalar literals, binary expressions, and print calls. |
| V02 `02_assign_unary.ivy` | Prefix/postfix increment and compound assignment: assert final value 7 and postfix old value 4; status 0. | Reachable: unary/assignment MIR variants. |
| V03 `03_float.ivy` | Assert `1.5 * 2.0 == 3.0` and `3.0 > 2.0`; status 0. | Reachable: float literals and binary expressions. |
| V04 `04_short_circuit.ivy` | Assert short-circuit `&&`/`||` skips the appropriate side effect and the final counter is exactly 1; no output, status 0. | Reachable: binary expressions and calls. |
| V05 `05_strings.ivy` | Assert equal strings compare true and unequal strings false; no output, status 0. Do not assert ordering or concatenation. | Reachable: string literals and equality/inequality. |
| V06 `06_refs.ivy` | Bind a reference to a local, write through it, and assert the original local is 9; no output, status 0. | Reachable: reference Alloca/Store MIR. |
| V07 `07_pointers.ivy` | Address/dereference a local, write through the pointer, assert updated value; compare a pointer with null only; no output, status 0. | Reachable for address/dereference; pointer indexing is unsupported. |
| V08 `08_arrays.ivy` | Read/write valid indexes 0 and 1; assert exact stored values and zero initialization; no output, status 0. | Reachable: array-size type data and Index MIR. |
| V09 `09_calls.ivy` | `f(2,3)` returns 5; assert result; no output, status 0. | Reachable: functions, parameters, and calls. Needs P0 target metadata and P2 execution. |
| V10 `10_recursion.ivy` | Recursive factorial of 5 returns 120; assert result; no output, status 0. | Reachable: ordinary function/call/CFG shapes. Needs P2 independent frames. |
| V11 `11_cfg.ivy` | Conditional/loop CFG sums 1..4 to 10; assert `sum=10\n`, status 0. | Reachable: branch/jump loop CFG. |
| V12 `12_switch.ivy` | `switch(2)` selects a case yielding 20 and `switch(9)` selects default yielding 90; assert both results. Test fall-through/`nextcase` only after a frontend MIR probe confirms the emitted control flow. | Basic Switch is reachable; `nextcase` status is unverified for this fixture plan. |
| V13 `13_call_order.ivy` | Side-effecting arguments append markers left-to-right; assert sequence `123`. | Reachable: call argument runs. Oracle evaluation is left-to-right (`interpreter.cpp:905-911`). |
| V14 `14_io_streams.ivy` | Assert internal stdout is exactly `out=7\n`, stderr exactly `err=8\n`, with no cross-stream bytes. | Reachable builtin calls; H5.3 checks buffers, H5.4 checks process streams. |
| V15 `15_io_format.ivy` | Assert `io::println("value={}", 7)` emits `value=7\n`; `io::println("literal", 7)` emits only `literal\n` (extra ignored). | Reachable. Confirms literal format/no-placeholder behavior (`interpreter.cpp:924-960`). |
| V16 `16_c_output.ivy` | `puts("p")` returns 2 and `putchar('!')` returns 33; assert combined stdout bytes are exactly `p\n!`. Restrict `printf` checks to explicitly supported format specifiers. | Reachable only if frontend call lowering accepts the declarations; confirm with MIR probe before corpus inclusion. |
| V17 `17_alloc_cell.ivy` | Call interpreted `__ivy_alloc`, assert dereferencing the returned pointer yields integer zero; no output, status 0. Do not infer allocation size from `n`. | Reachable only if frontend accepts the builtin declaration/call; confirm with MIR probe before corpus inclusion. |
| V18 `18_div_zero.ivy` | Divide by zero; assert exact stderr `<file>:<line>:<col>: error: division by zero\n`, status 1; separately assert stdout is empty. | Reachable runtime diagnostic case; verify fixture compiles through MIR. |
| V19 `19_null_deref.ivy` | Dereference null; assert exact null-deref diagnostic location/message, empty stdout, and status 1. | Reachable if frontend emits the dereference; verify MIR before inclusion. |
| V20 `20_array_bounds.ivy` | Index one past end; assert exact `index out of bounds: index N out of range [0, S)` diagnostic, empty stdout, and status 1. | Reachable for array indexing; verify source expression reaches interpreter and capture exact `N`,`S`. |
| V21 `21_int_status.ivy` | Return integer 7 from `main`; assert empty stdout/stderr and process status 7. | Reachable if frontend lowers the ordinary `main` return; confirm with `ivy --mir`. Oracle mapping is `main.cpp:1335-1337`. |
| V22 `22_nonint_status.ivy` | Return non-integer (e.g. `void`) from `main`; assert empty stdout/stderr and process status 0. | Reachable if frontend accepts the chosen `main` signature; confirm with `ivy --mir`. Oracle mapping is `main.cpp:1335-1337`. |
| V23 `23_expr_misc.ivy` | Assert `(6 & 3)==2`, `(4 | 1)==5`, `(7 ^ 3)==4`, `(2 << 2)==8`, `(8 >> 2)==2`, `!0 == 1`, `~0 == -1`, `(1 ? 8 : 9) == 8`, integer-to-float `3 -> 3.0`, and float-to-integer `3.9 -> 3`; no output, status 0. | MIR kinds/operations are represented; verify exact fixture syntax and casts through `ivy --mir` before inclusion. |
| V24 `24_exit.ivy` | Print `before\n`, call `exit(7)`; assert stdout exactly `before\n`, empty stderr, process status 7. | Reachable if frontend lowers the builtin call; verify with `ivy --mir`. Oracle calls `std::exit(int)` (`interpreter.cpp:1245-1248`); selfhost engine needs a termination result rather than host termination. |

### 8.2 Blocked or gated feature families

Do not include these as passing H5.3 assertions until the indicated frontend/MUnit work is complete. Interpreter expression variants alone do not make a test reachable.

| Feature | Candidate assertion after unblock | Current blocker / required subphase |
|---|---|---|
| Struct values/member access | Initialize/read/write named field; assert field value and alias behavior. | MUnit lacks struct definitions/fields/bases/vtables (`selfhost/mir/mir.ivy:131-161`). Add aggregate metadata and field lookup; otherwise exclude. |
| G1 virtual calls | Base-typed receiver dispatches to derived override; assert override result. | Requires struct inheritance/vtable/virtual metadata and dynamic target lookup; MExpr call target metadata is not resolved index (`mir.ivy:41-62`). Add metadata and lookup/dispatch or exclude. |
| G2 `new`/`delete` | Construct supported scalar/object, delete, assert only behavior actually specified by oracle. | Parser/HIR/MUnit reachability gap; `Delete`/`New` expression tags are not sufficient. Add frontend representation and heap/target metadata or exclude. Do not expect use-after-free diagnostics. |
| G3 direct construction | `S s(args)` calls the selected constructor with `this`; assert constructor effect. | Direct construction is a parser/HIR-builder gap; needs resolved constructor target and object/field metadata. Separate from the existence of `MExpr::Call`. |
| G4 destructor injection | Scope exit/return emits destructor effects in oracle order; assert reverse destruction and synthesized return handling where applicable. | Destructor calls and `__ret` are HIR-builder injection behavior; interpreter `Delete` alone does not implement injection. Requires HIR-builder/MIR inputs and metadata. |
| G5 braced initialization | `S s = {a,b}` initializes named fields as oracle; assert each field. | Braced-init frontend reachability gap plus missing MUnit field metadata. Add both or exclude. |
| G6 namespaces | Resolve qualified function call `ns::f`; assert result. | Namespace frontend/name lookup gap; requires qualified function identity and target lookup. Add support or exclude. |
| Arbitrary extern / file I/O | Never add as builtin expectation without implementation evidence. | Oracle interpreter does not dispatch arbitrary externs and does not list `io::read_line/read_file/write_file` as builtins (`src/mir/interpreter.cpp:1053-1073`, `std/io.ivy:10-13,30-38`). |

Every diagnostic fixture records **separate stderr bytes and process status**, plus stdout bytes; do not reduce it to a merged text expectation.

---

## 9. Gaps, Limits, and Open Questions

### 9.1 HIR/MIR frontend gaps G1-G7

The G labels refer to the HIR/MIR port gap inventory in `selfhost/mir/MIR_NOTES.md:1085-1128`. Track each in the H5.3 interpreter corpus as a reachability/metadata prerequisite; do not infer support from the C++ interpreter visitor's variants.

| Gap | Correct H5.3 treatment |
|---|---|
| **G1 — Virtual dispatch** | Exclude until virtual-call metadata, dynamic type/field/base/vtable metadata, and target lookup are present in selfhost MIR. The interpreter's C++ virtual-call implementation is not evidence the port frontend can provide that input. |
| **G2 — `new` / `delete`** | Exclude until parser/HIR/MIR can reach these expressions and MIR supplies needed type/constructor metadata. C++ interpreter variants do not close the frontend gap. Allocator builtins remain separately testable with their one-cell semantics. |
| **G3 — Direct struct construction** | Exclude until direct initialization lowers to a constructor call with correct `this`, resolved target, and object metadata. |
| **G4 — Destructor injection / `__ret`** | Exclude until HIR builder injects destructor calls and any synthesized return temporary in oracle order. `Delete` evaluation alone does not provide scope-exit injection. |
| **G5 — Braced init list** | Exclude until frontend can lower braced initializers and MUnit has fields/type metadata for initialization. |
| **G6 — Namespaces** | Exclude until qualified names survive frontend lowering and lookup resolves them unambiguously. Do not assume C++ interpreter name fallback substitutes for missing port metadata. |
| **G7 — Statement source location** | **Closed in H5.2** (`selfhost/mir/H52_NOTES.md`, §5). Do not list it as an H5.3 prerequisite. H5.3 still must preserve source locations carried by MIR expressions/instructions for runtime diagnostics. |

### 9.2 Oracle/runtime limitations

- Raw `Memory`/`Machine` safety hooks are declarative and not on ordinary pointer access paths; dereference validates pointer type and null only (`src/mir/interp_memory.h:11-35`, `interp_machine.h:13-33`, `interpreter.cpp:618-623`). `delete` and `__ivy_free` mark the target `Value.kind = Void` (`interpreter.cpp:471-477,1148-1157`) but this is not a use-after-free diagnostic.
- Interpreted `__ivy_alloc(n)` is one zero-valued cell with a minimum clamp of one (`interpreter.cpp:1131-1147`), unlike native runtime allocation for the selfhost host pools (`lib/ivyrt.c:32-41`).
- String binary operations are equality/inequality only; pointer comparison tests nullness; pointer indexing is unsupported (`interpreter.cpp:581-593,383-403`). String-first `io::print` formatting ignores extras without placeholders (`:924-960`).
- `callMain()` supplies zero args; `call()` checks arity but not argument types (`interpreter.cpp:44-65`). Call evaluation is left-to-right (`:905-911`). MIR builder is the normal call-target resolver (`src/mir/mir_builder.cpp:1254-1286`).
- No arbitrary extern dispatch or `io::read_line/read_file/write_file` builtin exists in the interpreter (`interpreter.cpp:1053-1073`, `std/io.ivy:10-13,30-38`).

### 9.3 Open questions and precise probes

1. **`exit`/`abort` termination contract**: create fixtures for `exit(7)` after a byte write and `abort()` after a byte write; run the frozen oracle as separate processes and capture stdout bytes, stderr bytes, and process status independently. Record platform/build identity. This settles exact bytes/status and confirms the termination behavior the engine result must carry; do not infer a portable abort status from `std::abort`.
2. **Frontend reachability of proposed V16/V17/V19/V20/V22/V24 and switch forms**: run each candidate source through existing `ivy --mir` and `mirdrive` using the current MIR corpus invocation pattern. Record whether parsing/building succeeds and inspect the emitted instruction/expression shape; move any failing candidate to blocked/excluded, then finalize its exact output/status by running `ivy --run` only when MIR construction succeeds.
3. **Exact runtime diagnostic bytes for V18-V20**: for each fixture accepted by `--mir`, run the frozen `ivy --run` in a fresh process and record separate stderr bytes, stdout bytes, status, and source location. The documented message formats identify expected fields but not the complete fixture-specific filename/line/column bytes.
4. **MUnit pointer/index and string-length representation details**: before implementation, inspect the actual HIR/MIR fields and compile minimal Ivy record probes for any proposed record layout; measure the Ivy record sizes with the compiler. This settles implementation layout only; sizes remain provisional, not spec invariants.

---

## 10. H5.4 Harness Requirements

`-Oracle --run` is a valid binding: `difftest.ps1` accepts `-Oracle` as a string argument and interpolates it into the oracle command (`selfhost/difftest.ps1:155-163,282-292`). The CLI already supports `ivy --run <file>` (`src/app/main.cpp:1493-1494`). However, the current harness comparison implementation is insufficient for interpreter conformance.

H5.4 must:

1. Add `interpdrive.ivy` to run each source through the selfhost frontend, MIR builder, and interpreter, preserving one result boundary per fixture.
2. Capture oracle and selfhost **stdout and stderr separately** as bytes; never redirect both streams into one file.
3. Capture and compare each child process exit status, including nonzero main returns, `exit(code)`, and runtime failures. Model `exit`/`abort` as interpreter termination results so the host driver can finish capturing the fixture.
4. Compare program output as exact byte arrays: do not drop empty lines, reconstruct text lines, transcode, or normalize line endings. Preserve embedded zero bytes and missing/extra final newlines.
5. Run fixtures separately, or frame each fixture's stdout, stderr, and status so there is no loss of fixture boundaries, including empty output and missing final newlines.
6. For diagnostic fixtures, assert the exact stderr diagnostic bytes and status 1 separately from stdout. For ordinary fixtures, assert output buffers/host streams and status independently.

The current code merges streams with `2>&1` at the oracle and port invocations (`difftest.ps1:288,298`), reconstructs line output in `Get-ByteLines`/`Join-ByteLines` (`:232-250,273-279`), filters empty lines (`:255-262`), and never compares `$LASTEXITCODE` for run processes. It therefore cannot validate separate streams, process status, raw bytes, embedded zero bytes, or exact final-newline behavior. A passing current-harness result is not an H5.3/H5.4 interpreter conformance gate.

---

## 11. Implementation P0 — Metadata and Pools

Recorded 2026-10-25 against frozen HEAD `918c1db`. This section records only the P0 data model, storage, alias, lookup, frame-schema, result-schema, and function-name-resolution foundation. No expression/instruction evaluator, output routing, status mapping, or driver was implemented.

### 11.1 Build evidence

Compiler: the frozen repository oracle `build\Release\ivy.exe` (verified 27,031,552 bytes; mtime 2026-10-21 16:41:44). Scratch and private `TEMP`/`TMP`: `C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c\`. The scratch module import interface was placed on `-I` before the repo module directories. From repository root, exact compile commands were:

```powershell
$env:TEMP = 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c'
$env:TMP = $env:TEMP
& '.\build\Release\ivy.exe' 'selfhost\mir\mir.ivy' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c\mir.obj'
& '.\build\Release\ivy.exe' 'selfhost\mir\interpreter.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c\interpreter.obj'
```

Both commands exited 0; compiler output reported `mir.obj` and `interpreter.obj generated`, with `.ivm` interfaces in scratch. Imports use the actual repository `mir.ivy`; no source build products or driver were created. The compiler accepted the query as `resolve_function(IFunctionQuery query)` (struct-by-value raw-pointer carrier), and `sizeof` / `unsafe` syntax in the probes below compiled.

### 11.2 Measured Ivy record layout

Sizes came from runtime `sizeof(Type)` in the compiled Ivy probe. Every field offset was derived by allocating the actual record, writing a distinct sentinel into each field, and locating the sentinel’s byte pattern from the allocated record base. Pointer offsets used the pointer-bit pattern; float payload uses its 8-byte `double` pattern. Probe source: `C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c\offsets.ivy`; command:

```powershell
& '.\build\Release\ivy.exe' 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c\offsets.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c' -I 'selfhost\parsing' -I 'selfhost\mir' -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c\offsets.exe'
& 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0c\offsets.exe'
```

Both exited 0. The probe reported `sizeof(long) = 8` and `sizeof(char*) = 8` on this port. All measured field offsets are bytes from record base:

| Record | `sizeof` | Field offsets (declaration order) |
|---|---:|---|
| `InterpreterControl` | 16 | `released=0`, `ref_count=8` |
| `InterpreterModel` | 232 | `values=0`, `value_count=8`, `value_capacity=16`, `cells=24`, `cell_count=32`, `cell_capacity=40`, `structs=48`, `struct_count=56`, `struct_capacity=64`, `fields=72`, `field_count=80`, `field_capacity=88`, `frames=96`, `frame_count=104`, `frame_capacity=112`, `locals=120`, `local_count=128`, `local_capacity=136`, `string_buffers=144`, `string_buffer_count=152`, `string_buffer_capacity=160`, `current_frame=168`, `setup_done=176`, `released=184`, `allocation_failed=192`, `result=200` (`state=200`, `value_index=208`, `status=216`), `control=224` |
| `IValue` | 88 | `kind=0`, `int_value=8`, `float_value=16`, `cell_index=24`, `pointer_is_null=32`, `provenance_kind=40`, `provenance_id=48`, `provenance_name=56`, `struct_index=64`, `string_bytes=72`, `string_length=80` |
| `ICell` | 8 | `value_index=0` |
| `IStruct` | 16 | `name=0`, `field_count=8` |
| `IField` | 32 | `struct_index=0`, `order=8`, `name=16`, `cell_index=24` |
| `IFrame` | 72 | `function_index=0`, `caller_index=8`, `continuation_block=16`, `continuation_inst=24`, `block_index=32`, `inst_index=40`, `return_state=48`, `return_value=56`, `active=64` |
| `ILocal` | 24 | `frame_index=0`, `name=8`, `cell_index=16` |
| `IResult` | 24 | `state=0`, `value_index=8`, `status=16` |
| `IFunctionQuery` | 40 | `functions=0`, `function_count=8`, `params=16`, `parameter_count=24`, `name=32` |
| `IFunctionResolution` | 24 | `state=0`, `function_index=8`, `candidate_count=16` |

### 11.3 Allocator strides and ownership

`setup()` starts each append-only pool at 4 records. Initial allocation and every growth use `sizeof` for its actual type: `IValue=88`, `ICell=8`, `IStruct=16`, `IField=32`, `IFrame=72`, and `ILocal=24` bytes per element. The string-buffer pointer table uses `sizeof(char*)=8` per slot, measured on this target. Each separately owned byte string uses `max(length,1)` bytes (empty string still owns one byte); logical contents are the exact `length` bytes, not NUL-terminated text.

The string-buffer table stores the 64-bit pointer bits in `long` slots to follow the compiler’s established pointer-as-`long` pool convention; its allocation stride remains the measured `sizeof(char*)`. Moving a pool copies records and then frees only its old backing allocation; all public indices and string allocations stay stable. String payload buffers are separately allocated and are never moved when that table grows. Names/provenance pointers and `MFunction`/`MParam` inputs are borrowed; the caller must keep their source storage alive while using the model/query. `release()` frees every owned string buffer once, then the final string table and each current pool once. Earlier pool backing arrays are freed immediately on replacement; replaced array allocations are not retained for final release. Release is idempotent; `setup()` is idempotent while active and rejects reinitialization after release. Partial setup unwinds any successful allocations and reports failure. Native allocation failure is latched and allocating/mutating methods return failure rather than dereferencing null or silently using an invalid slot. Growth arithmetic is checked against the actual signed 64-bit `long` limit before doubling/multiplying.

#### Ownership & copy semantics (Blocker fix)

Ivy structs are plain value types lacking copy constructors or automatic destructor injection. Copying an `InterpreterModel` (e.g. `InterpreterModel b = a;`) performs a shallow bitwise copy of the struct fields. If pool pointers and the `released` flag were strictly inline, `a.release()` would free all backing arrays while `b.released` remained 0, causing `b.release()` to attempt a double free and crash with heap corruption `0xC0000374`.

To resolve this, `InterpreterModel` delegates its release state to a heap-allocated `InterpreterControl` block (`sizeof=16`, `released=0, ref_count=1`). When `b = a` copies the struct, `b.control` points to the identical heap control block. On the first `release()` call (on either `a` or `b`), `control->released` is set to 1, and all pool arrays and string byte buffers are freed. Any subsequent `release()` or mutating method on any copied instance invokes `is_released()`, detects `control->released != 0`, and safely returns without touching or double-freeing the freed memory. The small control block remains allocated as a synchronization tombstone to prevent use-after-free crashes across copies.

### 11.4 Executable probe

Source: `C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\finalprobe.ivy` and `C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\probe_p0_fixes.ivy`. Compile/run commands:

```powershell
$env:TEMP = 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix'
$env:TMP = $env:TEMP
& '.\build\Release\ivy.exe' 'selfhost\mir\mir.ivy' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\mir.obj'
& '.\build\Release\ivy.exe' 'selfhost\mir\interpreter.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\interpreter.obj'
& '.\build\Release\ivy.exe' 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\finalprobe.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix' -I 'selfhost\parsing' -I 'selfhost\mir' -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\finalprobe.exe'
& 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\finalprobe.exe'
& '.\build\Release\ivy.exe' 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\probe_p0_fixes.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix' -I 'selfhost\parsing' -I 'selfhost\mir' -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\probe_p0_fixes.exe'
& 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p0fix\probe_p0_fixes.exe'
```

All commands exited 0. `finalprobe.exe` stdout:

```text
P0 probe PASS; counts values=49 cells=22 structs=7 fields=9 frames=16 locals=11 string buffers=9
capacities values=64 cells=32 structs=8 fields=16 frames=16 locals=16 string buffers=16
```

`probe_p0_fixes.exe` stdout:

```text
Probe 1 PASS: double-release on copied InterpreterModel safe
Probe 2 PASS: tag validation rejects invalid tags, accepts 0..5
Probe 3 PASS: non-allocating pop_frame unwinds during allocation_failed
ALL P0 VERIFICATION FIXES VERIFIED SUCCESSFULLY
```

Assertions covered:

- Round-trip tags/payloads for void, signed integer (including `2147483648` proving 64-bit `long` payload), float, non-null pointer/provenance, null pointer, struct handle, and string; tag values are 0–5 as documented in the module.
- Strict tag validation: `append_value(kind)` rejects invalid tags `kind < 0 || kind > 5` with `-1` (verified with `-1`, `6`, `99`, `-100`) and accepts valid tags 0..5.
- Shallow-copy double-free safety: `InterpreterModel b = a; a.release(); b.release();` as well as reverse release `b.release(); a.release();` and multi-copy release do not crash or corrupt the Windows heap (`0xC0000374`).
- Non-allocating frame unwinding: `pop_frame()` intentionally permits stack unwinding even when `allocation_failed` is latched, verifying clean frame unwinding during error/OOM handling.
- Two shared cell aliases and pointer read/write observe each other’s writes. Null pointers return no cell.
- String bytes/length are `[65, 0, 255, 90]` / 4; empty string has length 0 and byte lookup rejects index 0. The test compares byte integers, not PowerShell text capture.
- Interleaved fields across two structs retain per-struct order and exact lookup; duplicate and prefix-only field names are rejected/not matched.
- Nested frame push/pop restores caller, local shadowing selects the latest binding in the requested active frame, an inactive frame rejects lookup, and cells remain usable after pop. Frame position/return-schema setters accept valid state and reject an out-of-range return state.
- Name-only resolution returns unique (`state=1,index=0,count=1`), ambiguous (`state=2,index=-1,count=2`), unresolved exact/prefix mismatch, null query name, and null candidate-name outcomes. It deliberately does not select overloads; parameter data is retained for a later phase.
- Result-record assignment validates state/value indices. Repeated active `setup()` and repeated `release()` are safe; post-release mutation/push fail.

Initial capacities were 4 each. Maximum exercised counts/capacities were values 49/64, cells 22/32, structs 7/8, fields 9/16, frames 16/16, locals 11/16, and string buffers 9/16. Thus value/cell/field/frame/local/string-buffer pools crossed the first doubling threshold; structs crossed it too (capacity 8 after 7 live records). Stable pre-growth value/cell/field/string handles were rechecked after the growth loops. Frame records grew beyond 4; locals grew to 11 and shadow/local lookup was rechecked after growth.

### 11.5 P0 review fixes and limits

- `make_string()` reserves both pool capacities before allocation, handles empty strings, checks null only when length is nonzero, copies exact bytes, and frees its just-created byte buffer if value append fails. Buffer registration occurs only after append succeeds.
- Every raw pointer comparison, dereference, pointer-table operation, string byte access, and C-string scan is inside `unsafe`; `same_name()` handles either null before scanning and stops when either string ends.
- `setup()` tracks partial allocations, checks each result, and unwinds; repeated release is a no-op and released handles are never recycled. Pool growth checks allocation results before touching old storage, copies records before freeing old backing arrays, and latches failure. Pool allocation sizes use the measured actual record sizes, and pointer-table allocations use `sizeof(char*)`.
- Shallow-copy double-free safety: `InterpreterModel` incorporates `InterpreterControl* control` (`sizeof=16`). Shallow copy shares the control block; `release()` latches `control->released = 1` and frees pools; subsequent calls on copies check `is_released()` and return safely without double-freeing or corrupting the heap.
- `append_value(kind)` enforces strict tag bounds `0 <= kind <= 5`, returning `-1` for invalid tags.
- Frame pop marks only that frame inactive, restores its caller, and leaves cell/local records allocated. Non-allocating frame unwinding is permitted during latched `allocation_failed` so that call stacks can unwind cleanly during OOM/runtime errors. Lookup first rejects inactive frames and searches latest binding first, allowing correct same-frame shadowing and explicit nested-frame lookup.
- Resolution compares exact names safely, including null candidate names, and distinguishes unique/ambiguous/unresolved without claiming signature or overload resolution.
- Allocation-failure branches were audited and compiled; simulated OOM was probed to verify non-allocating stack unwinding. Borrowed name/query storage lifetime is caller-owned. Aggregate metadata here is local descriptive registration only; no MUnit aggregate integration or value/aggregate execution exists. Function and result records are schemas only; no function execution, expression/instruction behavior, output, diagnostic/status mapping, or driver is claimed.

---

## 12. Implementation P1 — Scalar expressions and lvalues

Recorded 2026-10-30 against frozen HEAD `918c1db`. This phase edited only `selfhost/mir/interpreter.ivy` and appended this section. No `src/**` change, no oracle rebuild, no commit.

### 12.1 Build evidence

Scratch and private `TEMP`/`TMP`: `C:\Users\ASUS\AppData\Local\Temp\opencode\h53p1c\`. From repository root, exact commands:

```powershell
$env:TEMP = 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p1c'
$env:TMP = $env:TEMP
& '.\build\Release\ivy.exe' 'selfhost\mir\mir.ivy' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p1c\mir.obj'
& '.\build\Release\ivy.exe' 'selfhost\mir\interpreter.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p1c' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p1c\interpreter.obj'
```

Both exited 0 and wrote `.ivm`/`.obj` in scratch only. Oracle identity rechecked after the phase: 27,031,552 bytes, mtime `2026-10-21 16:41:44`.

### 12.2 Implemented and audited (P1)

Oracle reference: `src/mir/interpreter.cpp:308-789` (expressions/lvalues/assignment/cast) and `src/mir/mir_builder.cpp:41-137` (literal decoding).

| Area | Self-host location | Behavior |
|---|---|---|
| Literal decode | `decode_body` (`:119`), `decode_string_literal` (`:248`), `decode_char_literal` (`:306`) | Char/string escapes, `u8`/`L`/`u`/`U` prefixes, raw strings; see §12.3 |
| `IValue` accessors | `:1078-1205`, `make_int`/`make_float`/`make_null_pointer`/`make_cell_pointer`/`make_string` (`:962-1076`) | Values are append-only; every operator result is a fresh value index |
| `eval_expr` | `:2007` | Kinds 0,1,2,3,4,5,6,7,8,9,10,12,14,19. Kinds 11,13,15,16,17,18 are out of P1 and report the unsupported-expression diagnostic |
| `lvalue_cell` | `:1929` | IdentRef, This, Unary `*`, Index; reference cells dereference to the aliased cell |
| Unary | `:2115-2193` | `&`, `*`, prefix/postfix `++`/`--`, `-`, `!`/`not`, `~`, `+` |
| Binary | `:2194-2294` | `&&`/`\|\|` short-circuit (rhs not evaluated), comma, int arithmetic/bitwise/shift/compare, float arithmetic/compare with int promotion, string `==`/`!=`, pointer nullness `==`/`!=` |
| Assignment | `:2333-2390` | `=` plus int/float compound `+= -= *= /= %= &= \|= ^= <<= >>=`; write-through for pointer/reference cells |
| Cast | `:2391-2419` | int↔float, int→int, ptr→ptr pass-through, ptr→int nullness (0/1) |
| Diagnostics | `error` (`:878`), `intern_diagnostic_msg` (`:851`), accessors (`:895-930`), builders (`:1814-1887`) | Append-only `(line, col, interned message)`; `release()` frees messages with the string-buffer pool |
| Array values | `make_array` (`:1362`) | See §12.6 |

Audit fixes in this phase:

- `error_undefined_var` / `error_unsupported_unary` / `error_unsupported_binary` used a fixed 256-byte scratch buffer sized independently of the message. A long identifier or operator could overrun it. They now allocate `diagnostic_scratch_size(part) = 64 + slen(part)` (`:1814-1867`), and the bounds builder uses a 128-byte scratch (`:1869`), which is provably sufficient for two decimal `long` values.
- The rest of the interrupted P1 code was verified against the oracle branch by branch and needed no semantic rewrite: literal kind order, identifier/array/pointer/reference load order, `this` handling, prefix vs postfix result values, short-circuit truthiness rules, comma, div/mod-by-zero return values, int/float/string/pointer operator partitions, pointer nullness comparison, lazy ternary, assignment target selection, compound fall-through texts, and `is_float_base`/`is_integer_base` member lists all match.

### 12.3 Decode rules (oracle parity, byte-level)

- Plain escapes: `\n \t \r \a \b \f \v \\ \' \" \?` map to `10 9 13 7 8 12 11 92 39 34 63`; any other escaped character is copied verbatim.
- Hex: `\x` consumes at most two hex digits. Mirrored oracle quirk: when exactly two digits are consumed, the oracle's `while (++i < size && count < 2)` advances `i` once more and the outer loop advances again, so the byte after the second digit is **skipped**. Self-host reproduces this with a `maxed` flag.
- Octal: `\` + 1–3 octal digits; the same post-max-digit skip applies after three digits.
- Prefixes: `u8` strips two characters; `L`, `u`, `U` strip one (width is ignored, as in the oracle). Raw string detection happens after prefix stripping.
- Raw string: bytes between the first `(` and the last `)` are copied verbatim, no escape processing. The self-host decoder additionally rejects `close <= open` (malformed input only); the oracle has no such guard.
- Char literal: strips `L`/`u`/`U`, decodes the body, and takes `bytes[0]` as an unsigned byte; empty/undecodable bodies yield 0.
- String values carry explicit length, so embedded NUL bytes are preserved. A failed decode yields the empty string (the oracle may keep partially decoded bytes on malformed input; unreachable from valid source).

Oracle cross-check (`ivy --run`, separate process, stdout bytes as integers) on `decode.ivy`:

```text
A={"\x41Z"}      -> 65
B={"\101Z"}      -> 65
C={"\x41\x42"}   -> 65,120,52,50
D={"\101\102"}   -> 65,49,48,50
R={R"(a\nb)"}    -> 97,92,110,98
E={"a\tb"}       -> 97,9,98
N={"a\0b"}       -> 97,0,98
```

All seven byte sequences match the P1 probe expectations.

### 12.4 Diagnostic texts and positions

Messages are byte-identical to the oracle interpreter. Position is the MIR expression `line`/`col` (the expression node whose evaluation failed), which the driver later renders as `<file>:<line>:<col>: error: <message>`.

| Trigger | Exact message |
|---|---|
| int `/` with 0 rhs | `division by zero` |
| int `%` with 0 rhs | `modulo by zero` |
| unary `*` on non-pointer | `dereference of non-pointer` |
| unary `*` on null pointer | `null pointer dereference` |
| unknown identifier | `undefined variable '<name>'` |
| unary op not in the supported set | `IvyInterpret v0.2: unsupported unary op '<op>'` |
| binary op not in any supported partition | `IvyInterpret v0.2: unsupported binary op '<op>'` |
| unary `&` with no lvalue cell | `cannot take address` |
| assignment with no lvalue cell | `cannot assign to expression` |
| `This` with no `this` local | `'this' is only valid inside a method` |
| index of a non-`__array` base | `IvyInterpret v0.2: pointer index not yet supported` |
| array index outside `[0, size)` | `index out of bounds: index <N> out of range [0, <S>)` |
| compound op outside the supported set | `unsupported compound op` |
| unsupported float compound op | `unsupported compound float op` |
| compound with unsupported operand kinds | `unsupported compound assignment` |

Oracle confirmations (frozen `ivy --run`, one process per fixture, stdout empty in all three, status 1):

```text
d_bounds.ivy:6:13: error: index out of bounds: index 5 out of range [0, 2)
d_div.ivy:5:14: error: division by zero
d_null3.ivy:4:16: error: dereference of non-pointer
```

`d_null3.ivy` is `unsafe { return *((long*) 0); }`. Note that `*nullptr` is rejected by the HIR builder (`'*' expects a pointer operand`), and `long* p = (long*) 0; *p` returns 0 because the cast produces an Int and the alloca binds it as a cell, so the interpreter's `null pointer dereference` path is only reachable from MIR that still carries a real null Ptr value.

Stability: `error()` interns a copy of every message into the owned string-buffer pool, so diagnostics survive both diagnostic-pool growth (initial capacity 4, doubled as needed) and string-buffer growth. `release()` frees each message exactly once with the buffer pool; `diag_count()`/`diag_line()`/`diag_col()`/`diag_msg()` return `0`/`0`/`0`/`""` after release, and `error()` is a no-op after release. A shallow copy shares the P0 `InterpreterControl` tombstone, so releasing one copy makes all copies observe the released state without double-free.

### 12.5 Probe matrix

Module-only probe built over a hand-built `MUnit`/`MExpr` array (no repository corpus, no function execution): `C:\Users\ASUS\AppData\Local\Temp\opencode\h53p1c\p1probe.ivy`. Frame locals and cells are created with `push_frame`/`new_cell`/`bind_local`; every assertion uses `IValue` accessors and compares string bytes as integers.

| Group | Return codes | Assertions | Result |
|---|---|---|---|
| setup/allocation | 1–3 | model setup, expression buffer, heap `MUnit` | PASS |
| V01 int arithmetic | 10–16 | `(9+3)*2==24`, `9-4`, `20/6`, `20%6`, `<`, `>=`, `!=` | PASS |
| V02/V06 inc-dec/assign/refs | 20–30 | postfix old 4, prefix 6, postfix `--`, `+=` → 7, reference read, write-through → 9 observed through `x` | PASS |
| V03 floats | 40–43 | `1.5*2.0==3.0`, `3.0>2.0`, mixed `1+2.5`, float equality | PASS |
| V04 short-circuit/comma | 50–58 | `0&&(y=99)` leaves y 0, `1||(y=99)` leaves y 0, no diagnostics, eager rhs runs, `0||(y=5)`, comma writes lhs and returns rhs | PASS |
| V05 strings/chars | 60–81 | escapes, equal/unequal, embedded NUL, `u8`/`L`/`U`, raw string, hex/octal skip parity, char literals, bool/null literals | PASS |
| V07 pointers | 90–100 | `p` cell identity, deref read/write, `==`/`!=` null only, `&x`, ptr→int 1/0, ptr→ptr pass-through | PASS |
| V08 arrays | 110–114 | zero init, write/read index 0, index 1 zero | PASS |
| V23 misc | 120–135 | `&`, `\|`, `^`, `<<`, `>>`, `!0`, `~0`, unary `-`/`+`, lazy ternary, ternary side effects, int→float, float→int, int→int | PASS |
| ref wrapper/compound/deref | 200–204 | Ptr-valued reference cell read/write-through, `rp+=2`, `*p+=1` | PASS |
| truncation/compound failures | 205–212 | `-7/2=-3`, `-7%2=-1`, `/=0` leaves value and reports `unsupported compound op`, float `%=` reports `unsupported compound float op`, `++` on float reports unsupported unary | PASS |
| short-circuit quirk/null/trunc | 213–220 | float `0.0&&` and float `1.0||` still evaluate rhs (oracle only short-circuits Int lhs), int lhs short-circuits, null vs integer 0, `-3.9` → `-3` | PASS |
| empty array/mixed compound/&elem | 221–228 | `[0,0)` bounds text, `<<=`, `>>=`, int cell `+=` float → float 20.5, `&a[1]`, `*(&a[0])` | PASS |
| diagnostics | 140–167 | all texts above, positions, int div/mod return 0, float `/0` yields a float and no diagnostic, two diagnostics for out-of-bounds assignment (bounds then `cannot assign`), `result.state == 2`, stability across 12 further diagnostics, out-of-range diag accessors | PASS |
| frame/copy-safety | 170–173 | frame pop, shared-control release observed by both copies, released mutation returns -1 | PASS |

Probe stdout/status:

```text
P1 probe PASS diags=31
run=0
```

Layout probe: `C:\Users\ASUS\AppData\Local\Temp\opencode\h53p1c\measure.ivy` (heap-allocated model; sentinel scan of every 8-byte slot), exit 0.

### 12.6 Array/index representation decision

- One array value owns one interpreter-local `IStruct` named `__array`, registered per array instance (never shared between arrays).
- Element fields are positional: `order` 0..size-1, `name` is the empty string (the interpreter never formats decimal field names). Each element is a fresh cell holding integer 0, so elements are zero-initialized and independently writable.
- `make_array(count)` returns the index of a **cell** that holds the array struct value; this matches the oracle's `declareCell(a.var, makeCell(arr))` for array allocas. `count < 0` is rejected; `count == 0` registers a struct with no fields, so any index reports `[0, 0)`.
- Array size for bounds is `IStruct.field_count` of that instance, equivalent to the oracle's `strct.fields.size()`.
- Read path (`eval_expr` kind 12) evaluates base then index, then looks up the field by order; write path (`lvalue_cell` kind 12) resolves the base cell, evaluates the index, bounds-checks, and returns the element cell. A failed element lookup on a valid array yields integer 0, matching the oracle.
- Non-`__array` bases (including pointer bases) report the pointer-index message; assignment through such a base reports the bounds/pointer message plus `cannot assign to expression`, matching oracle order.

### 12.7 Measured layout

Measured with `sizeof` and per-field sentinel scans on this target (`sizeof(long) = 8`, `sizeof(char*) = 8`):

| Record | `sizeof` | Note |
|---|---:|---|
| `IValue` | 88 | unchanged from P0 |
| `ICell` | 8 | unchanged |
| `IStruct` | 16 | unchanged |
| `IField` | 32 | unchanged |
| `IFrame` | 72 | unchanged |
| `ILocal` | 24 | unchanged |
| `IResult` | 24 | unchanged |
| `IFunctionQuery` | 40 | unchanged |
| `IFunctionResolution` | 24 | unchanged |
| `InterpreterControl` | 16 | unchanged |
| `InterpreterModel` | 272 | was 232 in P0; +40 for the diagnostic fields |

Diagnostic fields are three `long*` tables plus two `long` counters, inserted after `string_buffer_capacity`:

| Field | Offset |
|---|---:|
| `string_buffers` | 144 |
| `string_buffer_count` | 152 |
| `string_buffer_capacity` | 160 |
| `dline_` | 168 |
| `dcol_` | 176 |
| `dmsg_` | 184 |
| `diag_count_` | 192 |
| `diag_capacity_` | 200 |
| `current_frame` | 208 |
| `setup_done` | 216 |
| `released` | 224 |
| `allocation_failed` | 232 |
| `result.state` | 240 |
| `result.value_index` | 248 |
| `result.status` | 256 |
| `control` | 264 |

Pool strides used by allocations remain `sizeof(IValue)=88`, `sizeof(ICell)=8`, `sizeof(IStruct)=16`, `sizeof(IField)=32`, `sizeof(IFrame)=72`, `sizeof(ILocal)=24`, `sizeof(char*)=8` for the string-pointer table, and `sizeof(long)=8` for each of the three diagnostic tables. Initial capacities are 4; growth doubles with the P0 overflow checks.

Compiler `sizeof` caution (observed, not introduced by P1): `sizeof` under-reports a struct that embeds an imported nested struct by 40 bytes — `sizeof(MExpr)=136` while the real stride is the documented 176, and `sizeof(MInst)=96` while the real stride is 136. The under-report only appears when the translation unit imports `mir` without `ast`; adding `import ast` to the same probe makes `sizeof(MExpr)=176` and `sizeof(MInst)=136`, i.e. the visible value depends on whether the nested `AType` definition was already resolved in that unit. A sentinel scan shows the actual field offsets and the 176/136 strides are correct (`AType` really occupies 48 bytes; e.g. `MInst.line` sits at offset 120 and the next record starts at 136). `MirBuilder` already allocates those pools with the explicit 176/136 constants, so it is unaffected. P1 code does not allocate `MExpr`/`MInst`; test probes must not use `sizeof(MExpr)`/`sizeof(MInst)` as an element stride.

### 12.8 Gaps and limits

- P1 evaluates expressions over live `MUnit`/`MExpr` only. It does not execute instructions, allocate locals from `Alloca`, bind call parameters, or run control flow; frames/cells here come from the caller (P3/P4) or from probes.
- Excluded expression kinds: 11 Call, 13 Member, 15 New, 16 Delete, 17 InitList, 18 Lambda. They report `IvyInterpret v0.2: unsupported expression`; the oracle supports them but P1's gate excludes them.
- `&lambda` is not special-cased (the oracle returns a pointer to a closure cell); `&call` (reference-returning call lvalue) is also absent until P2.
- Pointer identity/ordering and pointer arithmetic are not modeled; only nullness `==`/`!=` is defined. Provenance fields remain descriptive, with no lifetime/use-after-free diagnostics.
- Pointer-valued lvalues dereference to the pointee cell on assignment (`lvalue_cell`), which is deliberately bug-compatible with `src/mir/interpreter.cpp:737-742`.
- Shift counts outside `[0, 63]`, signed shift right of negatives, and signed overflow keep the host compiler's behavior in both implementations; no normalization is claimed.
- Malformed literals diverge: the self-host decoder returns the empty string/0 where the oracle may expose partially decoded bytes.
- Under latched `allocation_failed`, `error()` may drop a diagnostic because the tables cannot grow; the P0 OOM policy intentionally favors safe failure over diagnostic fidelity.
- Shallow model copies share pool pointers, so pool growth through one copy can invalidate another copy's pointers before release. P1 keeps the P0 contract (release safety via `InterpreterControl`) and does not claim shared-read safety for live copies.
- `format_long` remains as an unused helper; diagnostics use `append_long`/`append_to_msg`.

---

## 13. Implementation P2 — Calls, Frames, Recursion

Recorded 2026-10-31 against HEAD `fd508da`. This phase edited only `selfhost/mir/interpreter.ivy` and appended this section. No `src/**` change, no oracle rebuild, no commit. The working tree contains the phase diff (not committed).

### 13.1 Build evidence

Scratch and private `TEMP`/`TMP`: `C:\Users\ASUS\AppData\Local\Temp\opencode\h53p2i\`. From repository root, exact commands:

```powershell
$env:TEMP = 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p2i\tmp'
$env:TMP = $env:TEMP
& '.\build\Release\ivy.exe' 'selfhost\mir\mir.ivy' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p2i\src\mir.obj'
& '.\build\Release\ivy.exe' 'selfhost\mir\interpreter.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p2i\src' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p2i\src\interpreter.obj'
```

Both exited 0 and wrote `.ivm`/`.obj` in scratch only. Oracle identity was rechecked after the phase: 27,031,552 bytes, mtime `2026-10-21 16:41:44`.

Probes were compiled with the same frozen oracle, importing the scratch `interpreter.ivm` first and the repository `mir`/parsing/HIR modules:

```powershell
& '.\build\Release\ivy.exe' 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p2i\p2probe.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p2i\src' -I 'selfhost\parsing' -I 'selfhost\hir' -I 'selfhost\mir' -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p2i\p2probe.exe'
& 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p2i\p2probe.exe'
```

### 13.2 Call resolution and its gaps

`resolve_call` (`interpreter.ivy:2549`) mirrors `Interpreter::call` (`src/mir/interpreter.cpp:51-92`):

1. exact `MFunction.name` match with `has_body` and a signature match;
2. `::`-suffix match (`qualified_suffix_matches`, `:2474`) with `has_body` and a signature match;
3. legacy first same-name function with `has_body`, ignoring arity.

`signature_matches` (`:2520`) implements the oracle's `matchSig`: reject when `pn > argc`; a non-extern-C function needs `pn == argc`; an `extern "C"` function may have fewer fixed parameters than arguments. Functions without a body are never selected.

The port stores the already-qualified `MFunction.name` (`"ns::f"`), while the oracle stores an unqualified `name` plus `namespacePrefix`. Therefore the oracle's first pass (unqualified `f->name == name`) corresponds to the port's exact pass for unqualified callees, and the oracle's second pass (`prefix + name == requested`) to the port's `::`-suffix pass. Outcomes agree for both `f` and `ns::f` spellings.

**Gap — no target pointer**: `MExpr` Call has `sval`/`astart`/`an`/`type` only, so the port cannot reproduce the oracle's `c.target` fast path or the MIR builder's type-directed overload choice (`src/mir/mir_builder.cpp:1254-1286`). Overloads that differ only in parameter types are therefore resolved by the first three name-based passes, which can pick the wrong overload. Type-based disambiguation requires adding a resolved target index/function handle to `MExpr` in a later phase; it is not approximated here.

`eval_call` (`:2692`) evaluates arguments first (oracle `interpreter.cpp:905-911`), then handles `move` as a pass-through, then gates the P4 builtin families (`io::*`, `ivy::*`, `printf`, `puts`, `putchar`, `exit`, `abort`, `__ivy_alloc`, `__ivy_free`, `malloc`, `free`) with `IvyInterpret v0.2: unsupported expression`. User-defined functions with a body always resolve, so recursion and mutual recursion work without special cases. An unresolved callee produces `IvyInterpret v0.2: function 'X' not found` at `0:0` (`error_function_not_found`, `:2610`) and returns void.

### 13.3 Argument evaluation

`evaluate_arg_run` (`:2650`) walks the contiguous `MUnit.args[astart .. astart+an)` run strictly left to right, evaluating every argument exactly once, even when the callee is missing or gated. Argument values are kept in a short-lived heap run (`alloc_arg_values`, `:2631`) and freed after the call; the `IValue` records themselves stay in the append-only value pool. Missing/out-of-range argument indices produce a void value for that slot.

Two probes confirm the order and the once-only rule:
- three arguments `1/0`, `1%0`, `2/0` produce exactly three diagnostics in that order (`division by zero`, `modulo by zero`, `division by zero`);
- two `1/0` arguments produce exactly two diagnostics.

### 13.4 Frames and runner

`run_function` (`:2934`) is the `execFunction`/`execBlock` equivalent:

- `push_frame` (`:1475`) records `function_index` and sets `caller_index = current_frame`, so nested calls form a real caller chain; `pop_frame` (`:1502`) marks only that frame inactive and restores the caller. Frame records and their cells/locals are append-only and never recycled.
- Parameters are bound after the push (see §13.5), starting at the function's `pstart`.
- Execution starts at the function's `block_start`; the runner walks the block's instruction chain via `MBlock.first` and `MInst.next`. It stops the block when an instruction changes `IFrame.block_index` (CondBranch/Jump) or the frame returns, switches to the new block, and treats falling off a non-terminated block as an implicit return. This matches `execBlock` (`interpreter.cpp:158-175`), including the oracle quirk that a Jump to the current block advances by `next` rather than restarting.
- The return value is read from `IFrame.return_state`/`return_value` before `pop_frame`; a missing/void return yields a void value. A missing or already-popped frame cannot be indexed: `exec_inst` validates the frame/instruction ranges and returns abandon, and `pop_frame` returns `-1` safely.

`lookup_local_cell` (`:1917`) now starts at `current_frame` and walks the `caller_index` chain, not the whole local pool. That reproduces the oracle's `lookupCell` (`interpreter.cpp:108-114`), which scans **every** live frame: a callee therefore *does* see the locals of its callers (probe: a nested call reads an outer local and gets `41`, matching `--run`). What it cannot see is a local that was never bound on its caller chain, e.g. a popped frame or a popped sibling; those report `undefined variable '<name>'`. Locals are found by the latest binding in the requested frame (`find_local`, `:1555`), so same-frame shadowing works, and `find_local`/`bind_local` reject inactive frames.

### 13.5 Parameter binding

Binding follows `execFunction` (`interpreter.cpp:129-142`) exactly: parameters are considered up to `min(pn, argc)`; an empty parameter name is skipped. A value parameter always gets a new cell containing the argument value. A reference or pointer parameter shares the argument's cell only when the argument value is a non-null `IValue` Ptr; otherwise the value is wrapped in a new cell.

Oracle parity note (probe-confirmed): the Ivy HIR lowers `f(y)` for a parameter declared `T&` as a plain `ident y`, whose evaluation yields the value, not a pointer. The oracle therefore wraps a copy, and mutating the reference parameter does **not** change the caller's `y` (`y*100+z` is 506, not 606). Cell sharing becomes observable when the argument value is a pointer, as for a pointer parameter or an address-valued MIR argument. Probe 6 builds that shape from a real unit by retyping the call argument as pointer depth 1 and the callee's first parameter as a pointer; the callee's mutation then changes the caller's local (606). Raw pointer parameter *definitions* cannot be built through the current frontend (`raw pointer parameter type requires an unsafe block` at HIR build), so the pointer-parameter path is probed at MIR level.

### 13.6 Instruction coverage

`exec_inst` (`:2747`) implements:

| Kind | Behavior |
|---|---|
| 0 Alloca | Arrays become a `__array` cell with one int-0 field cell per element (`make_array`); reference allocas bind `lvalue_cell` and never copy; a reference alloca with no initializer binds a null pointer cell silently (oracle `:195-209`); an initializer uses `eval_expr`; pointer depth > 0 shares a non-null Ptr cell, else wraps; defaults are null for pointers and int 0 otherwise. Struct default construction remains a P5 gap because `MUnit` has no aggregate table. |
| 1 Store | `lvalue_cell` target; write-through to the pointee when the target cell holds a non-null Ptr; otherwise write the target cell; no lvalue reports `cannot assign to expression`. |
| 2 Eval | Evaluate the expression and discard its value. |
| 3 Ret | Evaluate the value; a reference-returning function converts the lvalue cell to a Ptr (`make_cell_pointer`) so the caller can write through it; missing value is a void return. |
| 4 CondBranch | Truthiness is int != 0 or float != 0.0, else false; sets the then/else unit-level block index. |
| 5 Jump | Sets the unit-level target block index. |
| 6 Switch | Still gated with `IvyInterpret v0.2: unsupported instruction` at the instruction location; implementation is P3. |

`lvalue_cell` (`:1934`) also handles Call expressions (`:1982`): it evaluates the call and returns the cell of a non-null Ptr result, which is required for reference-returning call store targets (oracle `interpreter.cpp:722-730`). Probe 16 confirms the oracle's resulting behavior: with a bare reference argument the returned pointer addresses the callee's copied parameter cell, so `pick(x,y)=5` leaves `x` unchanged (result 12).

### 13.7 Public API and result state

- `call_by_name(MUnit* u, const char* name, long arg_start, long arg_count)` (`:3091`) evaluates the explicit `MUnit.args` run (`arg_start` may be `-1` when `arg_count == 0`), resolves, runs, and returns an `IResult`.
- `call_main(MUnit* u)` (`:3146`) calls `main` with no arguments; `run_unit(MUnit* u)` (`:3150`) is an alias used as the unit entry point.
- `IResult.state`: 0 pending/setup failure, 1 returned, 2 failed, 3 reserved for termination. `error()` latches `failed_` (`:403` field, body at `:882`) and sets `result.state = 2`; `call_by_name` clears `failed_` on entry and maps it to state 2. `has_failed()` exposes the latch.
- `release()` frees every pool, diagnostic table, and owned string buffer once, resets counts, and keeps the shared `InterpreterControl` tombstone. Shallow copies remain double-release safe in both copy/release orders (probe 22/23).

### 13.8 Measured layout

Measured with `sizeof` (scratch `measure.ivy`; `sizeof(long) = 8`, `sizeof(char*) = 8`):

| Record | `sizeof` | Change |
|---|---:|---|
| `InterpreterModel` | 280 | was 272 in P1 (+8 for `failed_` at offset 240) |
| `IFrame` | 72 | unchanged |
| `IValue` / `ICell` / `IStruct` / `IField` / `ILocal` / `IResult` / `InterpreterControl` | 88 / 8 / 16 / 32 / 24 / 24 / 16 | unchanged |

`failed_` sits between `allocation_failed` (232) and `result.state` (248); `control` moves to offset 272. All allocation strides still use `sizeof` of the actual record; the P1 `sizeof(MExpr)`/`sizeof(MInst)` import caveat is unaffected because P2 does not allocate those pools.

### 13.9 Probe matrix

Probe `p2probe.ivy` builds every unit through the ported lexer/preprocessor/parser/HIR/MIR pipeline (`MirBuilder`) and runs the resulting live `MUnit`. Values are the `main` return; all checks also require zero diagnostics and zero active frames unless stated. Oracle ground truth was captured with the frozen `ivy --run` on the same sources.

| # | Case | Assertion | Result |
|---|---|---|---|
| 1 | V09 two-parameter call | `add(2,3)` = 5; 2 frames | PASS |
| 2 | nested call argument | `add(1, add(2,3))` = 6; 3 frames | PASS |
| 3 | left-to-right, once each | three distinct diagnostics in `division`, `modulo`, `division` order | PASS |
| 4 | value parameter copy | callee mutation invisible: 506 | PASS |
| 5 | reference parameter, bare lvalue arg | oracle copies: 506 | PASS |
| 6 | pointer-shaped argument + pointer parameter | shared cell, mutation visible: 606 | PASS |
| 7 | recursion `fact(6)` | 720; 7 frames | PASS |
| 8 | recursion `fib(10)` | 55 | PASS |
| 9 | mutual recursion even/odd(9) | `is_even(9)*10+is_odd(9)` = 1 | PASS |
| 10 | 200 sequential calls + `sum(200)` | 200200; 402 frames; all inactive; inc callers are main | PASS |
| 11 | implicit return (`void f` falls off the block) | void, no diagnostics | PASS |
| 12 | call to that function from `main` | 7 | PASS |
| 13 | early return inside `if` | `pick(5)*10+pick(-5)` = 12 | PASS |
| 14 | array alloca inside a callee | read/write `__array`: 7 | PASS |
| 15 | store through a pointer local | `*p = 9` visible in the local: 9 | PASS |
| 16 | reference-returning call lvalue | `pick(x,y)=5`; oracle writes the copied param cell: 12, no diagnostics | PASS |
| 17 | cross-frame shadowing (callee's own binding wins over a caller's) | 941 | PASS |
| 18 | frame caller chain | main→g→f caller indices `-1,0,1`, function indices `2,1,0` | PASS |
| 19 | missing callee through a rewritten real Call | state 2, one diagnostic, exact text at `0:0`, void, frames popped | PASS |
| 20 | argument diagnostics once each | two `division by zero`, state 2 | PASS |
| 21 | arity mismatch via `call_by_name` | legacy first-name fallback runs; `undefined variable 'a'`, more unbound-parameter diagnostics, state 2 | PASS |
| 22 | copy/release, `a.release(); b.release()` | no crash; result returned | PASS |
| 23 | copy/release, reverse order | no crash; result returned | PASS |
| 24 | recursion depth 420 | 420; 422 frames | PASS |
| 25 | `call_by_name(name, arg_start, arg_count)` | explicit args run `add(4,5)` = 9 | PASS |
| 26 | `call_by_name` missing name | state 2, exact not-found text at `0:0` | PASS |

Probe output:

```text
PASS 1 .. PASS 26
P2 probe PASS
```

Oracle cross-check values (separate frozen `ivy --run` processes): 5, 6, 7, 720, 55, 200200, 7, 12, 941, 9, 506, 12; the three-argument diagnostic order is `division by zero`, `modulo by zero`, `division by zero`.

### 13.10 Recursion depth observed

`sum(n)` recursion was exercised without an interpreter-side depth cap:

| Depth | Result |
|---:|---|
| 200 | PASS (test 10; 402 frames including the 200 sequential `inc` calls) |
| 420 | PASS (422 frames) |
| 450 / 500 / 600 / 800 / 1000 | host stack overflow, exit code `0xC00000FD` |

The limiting factor is the host's native stack: each interpreted Ivy call nests `eval_expr → eval_call → run_function → exec_inst → eval_expr`, so one Ivy frame costs several native frames. The practical limit is therefore host-stack dependent (about 420 nested calls on the default Windows stack for this build); a worker thread with a larger stack or an explicit trampoline/depth budget is the eventual fix, and no depth guard is added in P2.

### 13.11 Remaining limits

- **P3**: Switch remains gated with `IvyInterpret v0.2: unsupported instruction`; only CondBranch/Jump/Ret traversal is implemented.
- **P4**: `io::*`, `ivy::*`, `printf`/`puts`/`putchar`, `exit`/`abort`, and allocator builtins are gated with `IvyInterpret v0.2: unsupported expression`; the hook (`is_gated_builtin`, `:2670`) is where P4 slots in.
- **P5**: struct default construction, members, `new`/`delete`, init lists, lambdas, and virtual dispatch remain unsupported; `MUnit` still has no aggregate/vtable metadata.
- **Overloads**: no `MExpr` target pointer, so parameter-type overload resolution is name/count-only (§13.2).
- **Reference parameter aliasing**: observable only for Ptr-valued arguments; the frontend lowers `f(y)` for `T&` by value, matching the oracle's bug-compatible behavior. Raw pointer parameter definitions are rejected by the current HIR (`requires an unsafe block`), so that shape requires MIR-level construction.
- `allocation_failed` drops further diagnostics/dynamic growth (P0 policy); shallow live copies still share pool pointers before release.

## 14. Implementation P3 — Complete CFG Instruction Set

Recorded 2026-10-31 against HEAD `2df6a9b`. This phase edited only `selfhost/mir/interpreter.ivy` and appended this section. No `src/**` change, no oracle rebuild, no commit, no corpus file added. The working tree contains the phase diff (not committed).

### 14.1 Build evidence

Scratch and private `TEMP`/`TMP`: `C:\Users\ASUS\AppData\Local\Temp\opencode\h53p3i\`. From repository root, exact commands:

```powershell
$env:TEMP = 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p3i\tmp'
$env:TMP = $env:TEMP
& '.\build\Release\ivy.exe' 'selfhost\mir\mir.ivy' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p3i\src\mir.obj'
& '.\build\Release\ivy.exe' 'selfhost\mir\interpreter.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p3i\src' -I 'selfhost\parsing' -I 'selfhost\mir' -c -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p3i\src\interpreter.obj'
```

Both exited 0 (only `.ivm`/`.obj` written in scratch). Oracle identity was rechecked after the phase: 27,031,552 bytes, mtime `2026-10-21 16:41:44`.

The probes were compiled with the same frozen binary, importing the scratch `interpreter.ivm` first and the repository HIR/MIR modules:

```powershell
& '.\build\Release\ivy.exe' 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p3i\p3probe.ivy' -I 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p3i\src' -I 'selfhost\parsing' -I 'selfhost\hir' -I 'selfhost\mir' -o 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p3i\p3probe.exe'
& 'C:\Users\ASUS\AppData\Local\Temp\opencode\h53p3i\p3probe.exe'
```

### 14.2 Switch semantics

`exec_inst` implements kind 6 (`interpreter.ivy:2965-2999`) exactly as `Interpreter::execInst`'s `Switch` (`src/mir/interpreter.cpp:286-300`):

1. Evaluate `a` when present; the condition is `read_int(value)`, which is 0 for every non-int kind (float, string, pointer, void) and for an absent expression; booleans are kind `Int` (`BoolLit` becomes `makeInt`), so `true` stays `1` exactly as in the oracle (`interpreter.cpp:286-292`, fixture 33: oracle `2`, port `2`). The oracle's `cv.isInt() ? cv.asInt() : 0` is therefore reproduced without an extra type test.
2. Start from the default target `b`. The oracle initialises `dest = sw.defaultBlock`, which is null when the frontend emitted no default; the port keeps the `-1`/invalid index and normalizes it as a null edge in the runner.
3. Walk `arm_val`/`arm_blk` from `astart` for at most `an` entries, bounded by `arm_n`, and stop at the first value equal to the condition. First match wins, including when arm values are unsorted or duplicated.
4. Record the destination as a CFG edge (self-edge handling in §14.3).

No arm-run arithmetic overflows: the walk uses an entry counter (`k < an`) plus `i >= 0 && i < arm_n` rather than `astart + an`. A missing, negative, or partly out-of-range arm run therefore reads no arm outside the pools and falls back to the default. **No switch shape is gated any more**: there is no switch-specific diagnostic. A corrupt arm target, like any other invalid target, becomes a null edge (§14.3), and an unknown instruction kind still reports `IvyInterpret v0.2: unsupported instruction`.

### 14.3 Block-edge hardening

The oracle stores `Block*` in `FrameCtx.curBlock`; the port stores unit-level block indices, so every index must be validated against the owning function. The rules, with their oracle basis:

| Rule | Port behaviour | Oracle basis |
|---|---|---|
| Target `-1` or outside `[block_start, block_start + block_n)` or `>= bl_n` | Null edge: the function ends with its current `return_state` (usually an implicit void return) | `execFunction` loops `while (!returned && curBlock)`; a null `curBlock` ends the function (`interpreter.cpp:144-156`) |
| Target is a block of another function | Treated as the same null edge; the foreign block is never read | Undefined in the oracle (raw `Block*` cannot express it); port-specific normalization, documented as such |
| `first == -1` (empty block) | Implicit return | `execBlock`: empty instruction vector falls through, then `!returned && curBlock == blk` sets returned (`:158-175`) |
| `last < first` or `last >= in_n` | Implicit return; no instruction executed | No oracle analogue (`Block` is just a vector); corrupt metadata is stopped instead of read |
| `next` chain ends before `last`, leaves `[first,last]`, or is non-monotonic (`next <= inst`) | Stop the block; falling off means implicit return | The oracle iterates the block's vector exactly once; a port chain that disagrees is not a valid vector |
| Self-edge (dest == current block) | Resume at the block's **second** instruction; a one-instruction self-jump block falls off and returns | `execInst` sets `curInst = 0`, then `execBlock` increments it to 1 (`:162-171`); probe-confirmed, including the resulting infinite loop for a self-jump that is itself instruction 1 |
| `Ret` followed by dead instructions | `Ret` returns 1 and stops the block; the trailing chain is never executed | `execInst` `Ret` sets `returned`; `execBlock` breaks (`:250-270,168`) |
| Loop that never executes / 1000 iterations | Ordinary edges; no iteration cap in the interpreter | No cap in the oracle |

Implementation points: `block_in_function` (`:2759-2781`) validates function ownership; `set_frame_edge` (`:1591-1607`) records the self-edge marker (`inst_index = 1`); the runner (`:3095-3177`) validates the current block, bounds each block to `first..last`, validates every new edge before reading it, and enforces strictly increasing `next` links. `exec_inst` still validates the frame and instruction index before touching pools.

### 14.4 Fixture matrix (actual results vs oracle)

Twenty-three scratch sources were run through the ported lexer → preprocessor → parser → HIR → MIR → interpreter (`p3files.exe`, which embeds the exact file bytes) and, separately, through the frozen oracle `build\Release\ivy.exe --run <file>` with one process per file, its own `TEMP`/`TMP`, and separate stdout/stderr capture. Every fixture has an `int main` whose value becomes the oracle process status; the port compares the `IResult` int value. All oracle runs had empty stdout and stderr except `13_call_order.ivy`.

| File | Port result | Oracle status/stderr | Verdict |
|---|---|---|---|
| `01_int_ops.ivy` | int 24 | 24 | PASS |
| `02_assign_unary.ivy` | int 74 | 74 | PASS |
| `03_float.ivy` | int 2 | 2 | PASS |
| `04_short_circuit.ivy` | int 1 | 1 | PASS |
| `05_strings.ivy` | int 1 | 1 | PASS |
| `06_refs.ivy` | int 9 | 9 | PASS |
| `07_pointers.ivy` | int 9 | 9 | PASS |
| `08_arrays.ivy` | int 57 | 57 | PASS |
| `09_calls.ivy` | int 5, 2 frames | 5 | PASS |
| `10_recursion.ivy` | int 120, 6 frames | 120 | PASS |
| `11_cfg.ivy` | int 10 | 10 | PASS |
| `12_switch.ivy` | int 1, 4 frames | 1 | PASS |
| `13_call_order.ivy` | state 2; `division by zero`, `modulo by zero`, `division by zero` | status 1; same three lines at `6:19`, `6:26`, `6:33` (301 bytes of stderr) | PASS (text order and count; exact line/col offsets are source-layout dependent) |
| `14_loops.ivy` | int 75 | 75 | PASS |
| `20_empty_fn.ivy` | int 1 | 1 | PASS |
| `21_ret_both.ivy` | int 12 | 12 | PASS |
| `22_switch_loop_break.ivy` | int 14 | 14 | PASS |
| `23_loop_never.ivy` | int 7 | 7 | PASS |
| `24_loop_1000.ivy` | int 247 | 247 | PASS |
| `25_rec_switch.ivy` | int 15, 7 frames | 15 | PASS |
| `26_term_only.ivy` | int 3 | 3 | PASS |
| `27_dead_after_ret.ivy` | int 5, 0 diagnostics | 5 | PASS |
| `30_switch_shapes.ivy` | int 9 | 9 | PASS |

`30_switch_shapes.ivy` covers default-only, no-match-without-default, an empty `switch`, unsorted first-match cases, and `nextcase` fall-through (`fall(1)=3`, `fall(2)=2`, `fall(9)=10`). `14_loops.ivy` covers `while`, `for`, `do/while`, an `if/else if/else` chain, nested loops, and `break`/`continue`. `22_switch_loop_break.ivy` covers a `switch` inside a loop with `break`. `25_rec_switch.ivy` covers recursion through a `switch`.

**Divergences.** The only one is V01's documented stdout `int=24\n`: the port still gates `io::*` with `IvyInterpret v0.2: unsupported expression` (P4), so stdout assertions cannot run yet. The same staged gap applies to any §8 row that asserts printed bytes. No value, diagnostic, or frame divergence was observed on the 23 fixtures. Fixtures were written to return their assertion through `main` precisely so P3 can compare them without the P4 builtins.

### 14.5 Probe matrix (44 assertions)

`p3probe.ivy` embeds the fixture shapes and adds MIR-corruption probes (mutating a freshly built unit). All 44 PASS. Ids 1–25 are the fixture/edge checks above; the remainder:

| # | Case | Assertion | Result |
|---|---|---|---|
| 26 | switch arm target `-1` | null edge, implicit void return | PASS |
| 27 | arm run fully outside `arm_n` | no arm read, default taken, int 2 | PASS |
| 28 | arm run starts at the last arm and overruns by 100000 | reads only the real arm, int 2 | PASS |
| 29 | switch condition rewritten to a float literal | non-int condition acts as 0, selects `case 0`, int 5 | PASS |
| 30 | switch condition expression removed (`a = -1`) | absent condition acts as 0, int 5 | PASS |
| 31 | switch default target `-1`, no matching arm | null edge, implicit void return | PASS |
| 32 | arm target in another function's block | invalid edge, implicit void return, foreign `ret` not read | PASS |
| 33 | `CondBranch` then/else both `-1` | null edge, implicit void return | PASS |
| 34 | `CondBranch` targets another function's block | invalid edge, implicit void return, foreign `ret` not read | PASS |
| 35 | `blocks[i].last = first` (chain truncated) | only the declared instruction runs, implicit void return | PASS |
| 36 | `blocks[i].first = -1` (stale `last`) | empty block, implicit void return | PASS |
| 37 | `next` link points at itself (non-monotonic) | guard stops the block, no runaway, implicit void return | PASS |
| 38 | single-instruction block converted to a self-jump | oracle quirk: falls off, implicit void return | PASS |
| 39 | block `[jump(self), ret 3]` | oracle quirk: resumes at instruction 1, returns 3 | PASS |
| 40 | copy/release after 1000-iteration loop + switch, forward order | no crash, state 1 | PASS |
| 41 | same, reverse release order | no crash, state 1 | PASS |
| 42 | release after `1 / 0` failure, then `call_main` on the released model | failure latch observed; post-release call is a state-0 no-op | PASS |
| 43 | `io::println` call | still `IvyInterpret v0.2: unsupported expression`, state 2 | PASS |
| 44 | instruction kind rewritten to 7 | `IvyInterpret v0.2: unsupported instruction`, state 2 | PASS |

Probe output:

```text
PASS 1 .. PASS 39
PASS 43
PASS 44
PASS 40 .. PASS 42
P3 probe PASS
```

(The ids 43/44 appear before 40–42 because the unsupported-kind checks were inserted before the lifecycle block; the embedded file matrix prints `PASS 1 .. PASS 23` and `P3 file matrix PASS 23`.)

### 14.6 Remaining limits

- **P3 boundary closed**: every `MInst` kind 0–6 now executes. The only remaining `unsupported instruction` case is an out-of-range kind tag, which is not emitted by the frontend (probe 44).
- **P4**: `io::*`, `ivy::*`, `printf`/`puts`/`putchar`, `exit`/`abort`, and the allocator builtins remain gated with `IvyInterpret v0.2: unsupported expression` (probe 43). This is the only reason the V01 stdout assertion cannot be checked yet.
- **P5**: aggregates, members, `new`/`delete`, init lists, lambdas, virtual dispatch, and struct default construction are unchanged.
- **P6**: the fixtures are **not** added to `corpus/interp` in P3. Decision: add them at P6, when `interpdrive.ivy` can run a source through the frontend + interpreter and emit stdout/stderr/status separately, so the harness can compare them against `--run` one process per file. Adding them now would leave files that no committed driver consumes. The exact scratch sources are recorded in the P3 probe (`p3probe.ivy`/`p3files.ivy` in the scratch directory, not the repository); at P6 they can be copied verbatim to `selfhost/corpus/interp/` and driven by the same `main`-return convention (nonzero small values 0–254 fit both the oracle process status and the `IResult` int).
- **Overloads**: still name/count-only (§13.2); no `MExpr` target pointer.
- **Recursion depth**: the host-stack limit from §13.10 still applies (about 420 nested calls); P3 adds no depth guard.
- **Self-jump loops**: the port reproduces the oracle quirk, including the fact that a self-jump that is itself the block's second instruction loops forever, exactly as the oracle does. P3 does not add a timeout or iteration cap.
- **Foreign-function block targets**: undefined in the oracle; the port normalizes them to null edges and ends the function. This is the only intentionally port-specific edge rule.

### 14.7 Measured layout

No record changed. Scratch `measure.ivy` against the P3 module:

| Record | `sizeof` |
|---|---:|
| `InterpreterModel` | 280 |
| `IFrame` | 72 |
| `IValue` / `ICell` / `IStruct` / `IField` / `ILocal` / `IResult` / `InterpreterControl` | 88 / 8 / 16 / 32 / 24 / 24 / 16 |

The new helpers add no fields; `inst_index` already existed in `IFrame` and now carries the self-edge marker (`1`) or `-1`.
