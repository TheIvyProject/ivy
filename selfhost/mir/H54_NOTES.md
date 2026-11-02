# H5.4 - `--run` outputs: differential corpus and harness

## 1. Scope

H5.4 closes the `--run` conformance gap left by H5.3/P6: the frozen C++ oracle
`build\Release\ivy.exe --run` and the selfhost pipeline (lexer ->
preprocessor -> parser -> HIR -> MIR -> `InterpreterModel`) are compared on the
V01-V24 fixture plan of `H53_NOTES.md` section 8.1. Each fixture runs in its own
process pair; stdout bytes, stderr bytes, and process status are compared
separately.

Out of scope (documented gaps, not forced): `printf` width > 200 / precision >
17, named struct member access / designated init, `new`/`delete`, virtual
dispatch, namespaces, arbitrary extern dispatch, and any other G1-G6 family of
`H53_NOTES.md` section 9.1.

No `src/**` file was modified and the oracle was not rebuilt. Oracle identity
rechecked after the work: 27,031,552 bytes, mtime 2026-10-21 16:41:44.

## 2. P0 reachability probe

Scratch directory (outside the repository):
`C:\Users\ASUS\AppData\Local\Temp\opencode\h54probe\`. Each candidate was
written to scratch, then:

1. `ivy --mir <file>` checked frontend acceptance;
2. `ivy --run <file>` ran the oracle in its own process;
3. `interpdrive.exe` ran the selfhost pipeline in its own process;
4. stdout/stderr went to separate files and were compared as raw bytes, along
   with the process exit status.

| ID | fixture | `--mir` | oracle status | driver status | raw streams | disposition |
|---|---|---:|---:|---:|---|---|
| V01 | `01_int_ops.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V02 | `02_assign_unary.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V03 | `03_float.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V04 | `04_short_circuit.ivy` | 0 | 0 | 0 | match | REACHABLE (division-skip form, see 2.1) |
| V05 | `05_strings.ivy` | 0 | 0 | 0 | match | REACHABLE (literal `==`/`!=` form, see 2.1) |
| V06 | `06_refs.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V07 | `07_pointers.ivy` | 0 | 0 | 0 | match | REACHABLE, reduced (address/deref only; see 2.1) |
| V08 | `08_arrays.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V09 | `09_calls.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V10 | `10_recursion.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V11 | `11_cfg.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V12 | `12_switch.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V13 | `13_call_order.ivy` | 0 | 0 | 0 | match | REACHABLE (stdout markers, see 2.1) |
| V14 | `14_io_streams.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V15 | `15_io_format.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V16 | `16_c_output.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V17 | `17_alloc_cell.ivy` | 0 | 0 | 0 | match | REACHABLE (`static_cast`, see 2.1) |
| V18 | `18_div_zero.ivy` | 0 | 1 | 1 | match | REACHABLE (runtime diagnostic) |
| V19 | `19_null_deref.ivy` | 0 | 0 | 0 | match | **BLOCKED** (oracle emits no null-deref diagnostic) |
| V20 | `20_array_bounds.ivy` | 0 | 1 | 1 | match | REACHABLE (runtime diagnostic) |
| V21 | `21_int_status.ivy` | 0 | 7 | 7 | match | REACHABLE |
| V22 | `22_nonint_status.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V23 | `23_expr_misc.ivy` | 0 | 0 | 0 | match | REACHABLE |
| V24 | `24_exit.ivy` | 0 | 7 | 7 | match | REACHABLE |

Result: 22 fully reachable, V07 reachable with a reduced assertion, V19 blocked.
All 24 candidates were probed; only V19 is absent from the corpus.

### 2.1 Probe findings that shaped the fixtures

- **C-style types need `#pragma ivy cnumber`.** `char`, `int`,
  `unsigned long long`, and C extern declarations are rejected otherwise. Used
  in V16, V17, and V24 (and in a rejected earlier V05 variant). The final V05
  uses direct string literals and needs no pragma.
- **Assignment expressions are not accepted as subexpressions.** V04 could not
  use `false && (x = x + 1)`. The fixture instead uses
  `false && ((10 / z) == 0)` and `true || ((10 / z) == 0)`; evaluating a skipped
  right side would raise division-by-zero, while the taken branch leaves the
  counter at 1. stdout stays empty as specified.
- **Reference parameter writes do not propagate in the oracle.** In V13,
  `m1(int32& a, ...)` updates only the callee copy. The fixture therefore uses
  `mark(v)` calls that print their argument: V13 asserts stdout `123` and the
  returned value 123, which still proves left-to-right argument evaluation.
- **`const char*` variables are compared as pointers (nullness only).** For
  `const char* a = "same"; const char* c = "diff";`, the oracle returns
  `a == c` true and `a != c` false because both pointers are non-null. Direct
  string-literal operands (`"same" == "same"`, `"same" != "diff"`) do use
  content comparison, so V05 uses direct literals.
- **Pointer null comparison is broken in the oracle.** `int32* q = nullptr;`
  and `static_cast<int32*>(0)` both make `q == nullptr` false. The selfhost
  interpreter matches that behavior. V07 therefore tests address/deref writes
  only; the null-comparison sub-assertion is documented, not forced.
- **Null dereference is not diagnosed.** `int32 x = *p;` with a null `p`
  produces the string value `nullptr` and status 0 in the oracle, with empty
  streams. This is the V19 blocker.
- **Selfhost parser gap: C-style pointer casts.** `(int32*)expr` is accepted by
  the oracle but rejected by the selfhost parser (`unexpected token in
  expression`). V17 uses `static_cast<int32*>(raw)` instead; no frontend fix was
  made for H5.4.
- **Driver fixes required by P0.** The P6 driver mapped every ordinary return
  to 0 and did not print interpreter diagnostics. It now forwards an integer
  `main` result (`src/app/main.cpp:1335-1337`: integer -> status, otherwise 0)
  and prints runtime diagnostics as `file:line:col: error: message`. This was a
  selfhost driver fix, not an oracle or `src/**` change.

## 3. Corpus

`selfhost/corpus/interp/` contains the 23 reachable fixtures, named as in
`H53_NOTES.md` section 8.1. V19 is intentionally absent.

| fixture | expected stdout | expected stderr | status |
|---|---|---|---|
| `01_int_ops.ivy` | `int=24\n` | empty | 0 |
| `02_assign_unary.ivy` | empty | empty | 0 |
| `03_float.ivy` | empty | empty | 0 |
| `04_short_circuit.ivy` | empty | empty | 0 |
| `05_strings.ivy` | empty | empty | 0 |
| `06_refs.ivy` | empty | empty | 0 |
| `07_pointers.ivy` | empty | empty | 0 |
| `08_arrays.ivy` | empty | empty | 0 |
| `09_calls.ivy` | empty | empty | 0 |
| `10_recursion.ivy` | empty | empty | 0 |
| `11_cfg.ivy` | `sum=10\n` | empty | 0 |
| `12_switch.ivy` | empty | empty | 0 |
| `13_call_order.ivy` | `123` | empty | 0 |
| `14_io_streams.ivy` | `out=7\n` | `err=8\n` | 0 |
| `15_io_format.ivy` | `value=7\nliteral\n` | empty | 0 |
| `16_c_output.ivy` | `p\n!` | empty | 0 |
| `17_alloc_cell.ivy` | empty | empty | 0 |
| `18_div_zero.ivy` | empty | `.../18_div_zero.ivy:3:18: error: division by zero\n` | 1 |
| `20_array_bounds.ivy` | empty | `.../20_array_bounds.ivy:4:16: error: index out of bounds: index 3 out of range [0, 3)\n` | 1 |
| `21_int_status.ivy` | empty | empty | 7 |
| `22_nonint_status.ivy` | empty | empty | 0 |
| `23_expr_misc.ivy` | empty | empty | 0 |
| `24_exit.ivy` | `before\n` | empty | 7 |

Diagnostic paths use the same fixture argument for oracle and driver, so the
absolute/relative prefix is identical on both sides. Line endings are whatever
the two processes write; the harness compares them verbatim.

## 4. Harness design

`selfhost/interpdiff.ps1` is a new harness; `selfhost/difftest.ps1` is untouched.

- Parameters: `-Dir` (default `selfhost/corpus/interp`) and `-TempRoot` (parent
  for a fresh per-run scratch directory; platform temp when empty).
- Every invocation creates `h54interp_<timestamp>_<pid>` and sets the script's
  `TEMP`/`TMP` to it before any compiler or fixture process starts. All child
  processes inherit the isolated temp directory.
- Builds `io` and every selfhost module into that scratch directory with `-I`
  pointing at scratch first, then the repository source directories, and links
  `selfhost/mir/interpdrive.ivy`. No `.obj`/`.ivm` is written into the
  repository working tree.
- One oracle process and one driver process per fixture:
  - oracle: `ivy.exe --run <relative-fixture-path>`;
  - driver: the same path written to its stdin.
  Both share the repository as working directory, so source paths in
  diagnostics are identical.
- stdout and stderr use separate `System.Diagnostics.Process` redirections.
  The stream `BaseStream` is copied byte-for-byte to separate files; there is
  no `2>&1`, no text decode, no line splitting, no empty-line filtering, and no
  CR/LF normalization.
- Each child's exit status is captured with `Process.ExitCode` (the same value
  PowerShell exposes as `$LASTEXITCODE` for a native command) and compared
  exactly.
- The harness compares stdout bytes, stderr bytes, and status independently,
  prints PASS/FAIL per fixture, prints first-difference offsets and hex dumps
  on failure, then a summary. It exits 0 iff every fixture passes.
- Negative sanity check: a scratch fixture using the known `printf` width-201
  gap (outside the corpus) is reported as
  `FAIL printf201.ivy` with oracle status 0, driver status 1, oracle stdout
  non-empty space padding (63 bytes observed on 2026-11-02; the exact count is
  not part of the gate), driver stdout empty, and the harness exits 1.

Run from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File selfhost/interpdiff.ps1 -Dir selfhost/corpus/interp -TempRoot C:\Users\ASUS\AppData\Local\Temp\opencode\h54h
```

## 5. Run results

Final run (2026-11-02) over the 23 included fixtures:

```text
[1/4] building selfhost interpreter driver ...
[2/4] collecting corpus ...
[3/4] running 23 fixtures ...
PASS 01_int_ops.ivy
PASS 02_assign_unary.ivy
PASS 03_float.ivy
PASS 04_short_circuit.ivy
PASS 05_strings.ivy
PASS 06_refs.ivy
PASS 07_pointers.ivy
PASS 08_arrays.ivy
PASS 09_calls.ivy
PASS 10_recursion.ivy
PASS 11_cfg.ivy
PASS 12_switch.ivy
PASS 13_call_order.ivy
PASS 14_io_streams.ivy
PASS 15_io_format.ivy
PASS 16_c_output.ivy
PASS 17_alloc_cell.ivy
PASS 18_div_zero.ivy
PASS 20_array_bounds.ivy
PASS 21_int_status.ivy
PASS 22_nonint_status.ivy
PASS 23_expr_misc.ivy
PASS 24_exit.ivy
[4/4] summary
      PASS 23 / 23
PASS: stdout bytes, stderr bytes, and exit status match for every fixture
```

Scratch evidence from that run:
`C:\Users\ASUS\AppData\Local\Temp\opencode\h54h\h54interp_20261102_172103_413_12460`.

### 5.1 Exact byte evidence

Both sides produced identical bytes, including the platform CR LF written by
stdout/stderr. Empty streams are zero-length files, not an empty line.

| fixture | stream | hex bytes |
|---|---|---|
| `01_int_ops.ivy` | stdout | `69-6E-74-3D-32-34-0D-0A` (`int=24\r\n`) |
| `11_cfg.ivy` | stdout | `73-75-6D-3D-31-30-0D-0A` (`sum=10\r\n`) |
| `13_call_order.ivy` | stdout | `31-32-33` (`123`, no final newline) |
| `16_c_output.ivy` | stdout | `70-0D-0A-21` (`p\r\n!`, no trailing newline after `!`) |
| `18_div_zero.ivy` | stderr | `selfhost/corpus/interp/18_div_zero.ivy:3:18: error: division by zero\r\n` |
| `20_array_bounds.ivy` | stderr | `selfhost/corpus/interp/20_array_bounds.ivy:4:16: error: index out of bounds: index 3 out of range [0, 3)\r\n` |
| `24_exit.ivy` | stdout | `62-65-66-6F-72-65-0D-0A` (`before\r\n`) |

## 6. Known gaps and blockers

1. **V19 null-deref diagnostic (blocked).** Frozen oracle `--run` does not
   report a null dereference: it yields a `nullptr` value and status 0. The
   V19 fixture would have no diagnostic bytes to compare, so it is excluded.
2. **V07 null comparison (reduced).** `q == nullptr` is false in the oracle for
   a null pointer; the selfhost interpreter matches. Address/deref behavior is
   tested; null equality is not asserted as true.
3. **Pointer variable equality is nullness equality.** `const char*` string
   variables compare as pointers (all non-null pointers equal), so V05 uses
   direct literals for content equality/inequality.
4. **Reference parameter propagation.** Writes through `T&` parameters do not
   reach the caller in the oracle; V13 observes order through stdout.
5. **C-style pointer casts in the selfhost parser.** `(T*)expr` is a frontend
   gap; V17 uses `static_cast`. This is a parser-port issue, not an interpreter
   result difference.
6. **`printf` width/precision.** Width > 200 or precision > 17 is a P4
   interpreter limit and is not part of the reachable corpus.
7. **Aggregate member access.** Named struct members and designated
   initialization remain the P5 data-model gap; no such fixture is included.
8. **`exit`/`abort`.** V24 covers `exit(7)` (stdout preserved, status 7). The
   same driver result path maps `abort` to `-1073740791` (0xC0000409), already
   cross-checked against the oracle in `H53_NOTES.md` section 17.2; no separate
   abort fixture is part of V01-V24.

## 7. State

Files added/modified for H5.4:

- `selfhost/mir/interpdrive.ivy` (diagnostics + integer-`main` status mapping);
- `selfhost/corpus/interp/*.ivy` (23 fixtures);
- `selfhost/interpdiff.ps1` (new harness);
- `selfhost/mir/H54_NOTES.md` (this file);
- `PLAN.md` H5.4 row.

Not committed; the working tree is left for the user.
