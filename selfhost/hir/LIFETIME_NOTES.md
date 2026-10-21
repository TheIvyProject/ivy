# H4.5 - Borrow / lifetime check: reconnaissance notes

**Milestone:** H4.5 (Borrow / lifetime check), phase 1/5 (reconnaissance).
**Repo:** `D:\project\Ivy\ivyc`  (drive **D:**; there is no `C:\project` on this machine).
**Oracle:** `build\Release\ivy.exe`, built 2026-10-21 16:41, i.e. it **includes** commit
`757ad75` "src: evaluate return expression before running local destructors" (the `__ret`
temp change).  It is *current* - do not rebuild it.
**HEAD at time of writing:** `17c68cf` (on top of `757ad75`, `4bba6c3`).
**Production code changed by this phase:** none.  This file is the only artifact created.

Fixtures (98 `.ivy` files = 95 probes + 3 encoding experiments) + the full 5-mode oracle
transcript live in

```
C:\Users\ASUS\AppData\Local\Temp\opencode\h45recon\v2\
    stage\    s01..s18   - stage ownership (HIR vs MIR)
    syntax\   x01..x44   - lifetime syntax surface
    ret\      r01..r22   - the __ret lowering
    bug\      b01..b09   - malformed lifetime input
    order\    o01..o03   - multi-diagnostic ordering
    enc\      emdash / gammanai / hyphen - the em-dash encoding experiment (3.4)
    TRANSCRIPT.txt        - every fixture x {--lex,--tokens,--ast,--hir,--mir}, stdout
                            and stderr separately, plus the exit code
    SYNTAX.txt / RET.txt / STAGE.txt / X3.txt / X4.txt / R1.txt / R2.txt / S1.txt
                            - the extracted --ast/--hir/--mir dumps
    GAP_hir.txt / GAP_ast.txt
                            - oracle-vs-selfhost differential (hirdrive.exe / astdrive.exe)
```

All oracle outputs quoted below are **verbatim** from `TRANSCRIPT.txt`.

---

## 0. Path check (done first)

```
PS> Get-Location
    Path
    ----
D:\project\Ivy\ivyc

PS> git rev-parse --show-toplevel
D:/project/Ivy/ivyc
```

Both point at the repository on drive **D:**.  All shell commands were run with
`workdir = D:\project\Ivy\ivyc`; no `cd` was used.  `C:` was used only for scratch
fixtures under `C:\Users\ASUS\AppData\Local\Temp\opencode\h45recon\`.

---

## 1. TL;DR - the 10 rules

1. **The borrow checker and the lifetime verifier are NOT in HIR.**  They live in
   `src/mir/mir_builder.cpp`: `checkReturn()` (line 228), `checkStore()` (line 298),
   `checkBorrow()` (line 315).  HIR only *records and validates lifetime annotations*
   (`src/hir/hir_builder.cpp`: `lowerLifetimeAttributes()` line 217,
   `lowerParamAttribute()` line 264, pointer-return rule line 882).
2. **`ivy --hir` runs the WHOLE pipeline** (parse -> HIR -> MIR), see `src/app/main.cpp`
   lines 1303-1323.  So a **MIR**-stage diagnostic still appears in
   `--hir` mode, on stderr, **after** the HIR dump, exit code 1.  The selfhost HIR driver
   stops after HIR and can therefore never reproduce it.
   => **The H4.5 corpus must contain no MIR-stage diagnostic.**
3. A **HIR**-stage lifetime diagnostic prints **diagnostics only, no HIR dump**, exit 1 -
   exactly like any other HIR error.  (`src/app/main.cpp`: `if (failed) return 1;` sits
   between the HIR build and `dumpHir`.)
4. The **AST dump is lossy for lifetimes**: native `$a` syntax is *completely invisible*;
   only the legacy `[[ivy::lt_def(a)]]` / `[[ivy::lt_ret(a)]]` / `[[ivy::lt(a)]]`
   attributes show up.  (`dumpAst`, `src/app/main.cpp:331`.)
5. The HIR dump shows lifetimes in exactly **three** places: `  lifetimes: a b`,
   `  return lifetime: X`, and the per-parameter suffix ` [lt:X]` (**no space** after the
   colon - the MIR dump uses ` [lt: X]`, with a space).
6. `hir::Type` has **no** lifetime field and `dumpHirType()` prints none.  The HIR builder
   computes **no** per-expression lifetime: an `ident` that refers to a lifetime-carrying
   parameter carries no lifetime information in HIR.  All lifetime *inference* is
   `mir::Expr::lifetime` (MIR stage).
7. The new `__ret` temp is created **iff**: (a) at least one local with a *user-declared
   destructor* is live at the `return`, (b) the `return` has a value, (c) the function's
   return type is by-value (not a reference, not a pointer, not `void`), (d) the return
   expression is **not** a bare `IdentRef` whose (non-reference) type base equals the
   return type's base.  Name: `__ret`, or `__ret0`, `__ret1`, ... on collision.
8. The `__ret` temp's **declared type is the type of the return *expression***, not the
   function's return type.  The temp `decl` is the **first** statement of the wrapping
   compound (before the destructor calls); the original `return`'s value is replaced by
   `ident __ret`.
9. The `__ret` lowering needs the **entire RAII/destructor machinery** (dtor tracking,
   dtor emission, NRVO/`returnedLocals_`, struct-ctor member-init lowering) and
   **struct construction** - **none of which exists in the selfhost port today** (the port
   even rejects `Res r(1);` with "undeclared identifier 'Res'").  It is therefore *not* an
   H4.5-sized job.
10. **Recommended split:** **H4.5 = lifetime *annotation* lowering only** (a `--hir` corpus,
    ~16 core files).  **The borrow checker + lifetime inference + the `__ret`/RAII
    machinery are H5 (MIR)** - which is exactly how `PLAN.md` already splits them
    (H4.5 "Borrow / lifetime check" / `corpus/hir/borrow` vs. H5.2 "Lifetime annotation" /
    `corpus/mir/lifetime`).

---

## 2. Stage ownership: HIR vs MIR (the key scoping question)

### 2.1 The pipeline (source of truth)

`src/app/main.cpp` lines 1303-1323, in order:

```
parse  ->  if (failed) return 1;                       // parser diagnostics, no dump
if (showAst && tu) dumpAst(*tu, std::cout);            // line 1303 - AST dump, BEFORE the HIR build!
ivy::HirBuilder builder(*tu); hir = builder.build();    // line 1305
         -> print HIR diagnostics to stderr; if (failed) return 1;   // line 1311 - NO HIR DUMP
if (showHir && hir) dumpHir(*hir, std::cout);           // line 1313
ivy::MirBuilder mirBuilder(*hir); mir = mirBuilder.build();  // line 1315
         -> print MIR diagnostics to stderr; if (failed) return 1;   // line 1321 - HIR dump ALREADY printed
if (showMir && mir) dumpMir(*mir, std::cout);            // line 1323
```

Consequences (all verified):

* `--ast` prints the **AST dump first**, then any HIR/MIR diagnostic.
* `--hir` prints **no dump** when the HIR build failed; it prints the **HIR dump then the
  diagnostic** when the MIR build failed.
* `--mir` prints **no dump** at all when either build failed (the MIR dump is after the
  failure check).

### 2.2 Which stage owns which diagnostic (measured)

| Situation | Fixture | Stage | Mode(s) that show it | Message | Exit |
|-----------|---------|-------|----------------------|---------|------|
| returning a reference to a local | `stage/s1_ret_local_ref.ivy` | **MIR** | `--ast`, `--hir`, `--mir` | `returning a reference to a local variable (dangling reference)` | 1 |
| returned lifetime != declared return lifetime | `stage/s7_lt_mismatch.ivy` | **MIR** | `--ast`, `--hir`, `--mir` | `returned reference has lifetime '$b' but the function declares lifetime '$a'` | 1 |
| returned reference with unknown lifetime | `stage/s11_unknown_lt_ret.ivy`, `stage/s15_return_call_result.ivy` | **MIR** | `--ast`, `--hir`, `--mir` | `returned reference has unknown lifetime; annotate the function with lifetime<$a> and tie the return to a parameter` | 1 |
| mutable ref while immutably borrowed | `stage/s4_mut_vs_immut.ivy` | **MIR** | `--ast`, `--hir`, `--mir` | `cannot create a mutable reference to 'a' <EMDASH> it is already borrowed immutably` | 1 |
| second mutable ref | `stage/s5_two_mut.ivy` | **MIR** | `--ast`, `--hir`, `--mir` | `cannot create a second mutable reference to 'a' <EMDASH> only one mutable borrow at a time` | 1 |
| immutable ref while mutably borrowed | `stage/s6_immut_vs_mut.ivy` | **MIR** | `--ast`, `--hir`, `--mir` | `cannot create an immutable reference to 'a' <EMDASH> it is already borrowed mutably` | 1 |
| lifetime `$b` not declared (parameter) | `syntax/x17_undeclared_param_lt.ivy` | **HIR** | `--ast`, `--hir`, `--mir` | `lifetime '$b' is not declared in lifetime<...>` | 1 |
| return lifetime `$b` not declared | `syntax/x18_undeclared_ret_lt.ivy` | **HIR** | `--ast`, `--hir`, `--mir` | `return lifetime '$b' is not declared in lifetime<...>` | 1 |
| lifetime on a by-value parameter | `syntax/x16_lt_on_byvalue_param.ivy` | **HIR** | `--ast`, `--hir`, `--mir` | `lifetime annotation requires a reference or pointer parameter` | 1 |
| legacy `[[ivy::lt(b)]]` with undeclared `b` | `syntax/x19_legacy_undeclared.ivy` | **HIR** | `--ast`, `--hir`, `--mir` | `[[ivy::lt(b)]]: lifetime 'b' is not declared via [[ivy::lt_def(...)]]` | 1 |
| legacy `[[ivy::lt]]` with != 1 argument | `syntax/x21_legacy_lt_arity.ivy`, `x35` | **HIR** | `--ast`, `--hir`, `--mir` | `[[ivy::lt]] expects exactly one lifetime argument` | 1 |
| legacy `[[ivy::lt_ret]]` with != 1 argument | `syntax/x33_legacy_lt_ret_arity2.ivy`, `x34` | **HIR** | `--ast`, `--hir`, `--mir` | `[[ivy::lt_ret]] expects exactly one lifetime argument` | 1 |
| legacy `[[ivy::lt(a)]]` on a by-value parameter | `syntax/x36_legacy_lt_on_byvalue_param.ivy` | **HIR** | `--ast`, `--hir`, `--mir` | `[[ivy::lt]] requires a reference or pointer parameter` | 1 |
| duplicate legacy `lt_def` | `syntax/x20_dup_legacy_ltdef.ivy` | **HIR** | `--ast`, `--hir`, `--mir` | `duplicate lifetime 'a'` | 1 |
| native `$a` + legacy `lt_def(a)` | `syntax/x30_dup_legacy_vs_native.ivy` | **HIR** | `--ast`, `--hir`, `--mir` | `duplicate lifetime 'a'` | 1 |
| duplicate native `lifetime<$a, $a>` | `syntax/x12_dup_native.ivy` | **PARSER** | `--lex`, `--tokens`, `--ast`, `--hir`, `--mir` | `duplicate lifetime '$a'` | 1 |
| pointer return without a return lifetime | `syntax/x22_ptr_ret_no_lt.ivy` | **HIR** | `--ast`, `--hir`, `--mir` | `function 'f' returns a pointer but has no return lifetime <EMDASH> use \`lifetime<$a>\` and annotate the return type with $a` | 1 |

`<EMDASH>` = the 3 characters `U+0253 U+00C7 U+00F6` ("`ΓÇö`"); see section 3.4.

**Not diagnosed at all** (verified, exit 0, no diagnostic):

| Situation | Fixture | Result |
|-----------|---------|--------|
| storing a pointer to a local into a longer-lived object | `stage/s16_check_store_dangling.ivy` | only `address-of operator '&' taking raw pointer requires an unsafe block` (an `unsafe` rule, not a lifetime rule) |
| using a value after its owner is destroyed | `stage/s8_use_after_destroy.ivy` | exit 0, nothing |
| binding a reference to a temporary | `stage/s9_ref_to_temp.ivy` | exit 0, nothing |
| assigning through a variable while it is borrowed | `stage/s14_assign_through_borrow.ivy` | exit 0, nothing |
| the dangling-store check (`checkStore`) | - | practically unreachable: every way of making a `Local`-lifetime value needs `unsafe`, and `checkStore` returns early when `unsafeDepth_ > 0` |

### 2.3 Verbatim proof that the borrow errors are MIR-stage

`stage/s1_ret_local_ref.ivy`:

```
fn dangling_ref() -> const int32& {
    int32 x = 42;
    return x;
}
```

`ivy --hir` (exit 1) - **the HIR dump IS printed, the error comes after it**:

```
function dangling_ref
  return const int32_t&()
  block
    decl int32_t x =
      int 42
    return
      ident x
```
(stderr)
```
...stage\s1_ret_local_ref.ivy:3:5: error: returning a reference to a local variable (dangling reference)
```

`ivy --mir` (exit 1) - **no dump at all**, only the diagnostic (the MIR build failed
before `dumpMir`).

`stage/s7_lt_mismatch.ivy` (`ivy --hir`, exit 1):

```
function pick
  return const int32_t&(const int32_t& x [lt:a], const int32_t& y [lt:b])
  lifetimes: a b
  return lifetime: a
  block
    return
      ident y
```
(stderr)
```
...stage\s7_lt_mismatch.ivy:3:5: error: returned reference has lifetime '$b' but the function declares lifetime '$a'
```

### 2.4 Verbatim proof that the lifetime-annotation errors are HIR-stage

`syntax/x17_undeclared_param_lt.ivy` (`ivy --hir`, exit 1) - **diagnostics only, no dump**:

```
...syntax\x17_undeclared_param_lt.ivy:2:8: error: lifetime '$b' is not declared in lifetime<...>
```

The same file in `--ast` mode (exit 1) prints the AST dump *and then* the diagnostic:

```
function 
  return const int32_t bad(const int32_t x)
  block
    return
      ident x
```
(stderr) `...syntax\x17_undeclared_param_lt.ivy:2:8: error: lifetime '$b' is not declared in lifetime<...>`

### 2.5 Consequence for the harness

`selfhost/difftest.ps1` compares the **combined stdout+stderr** of `ivy --hir <file>`
(CRLF normalised to LF) against the selfhost driver, which stops after the HIR stage.
Therefore:

* a corpus file that triggers a **MIR** diagnostic produces `HIR dump + MIR diagnostic`
  from the oracle, but only `HIR dump` from the port -> **permanent mismatch**;
* a corpus file that triggers a **HIR** diagnostic produces `diagnostic only` from both
  -> **match** (the port's `hirdrive.ivy` prints the diagnostics and only dumps the HIR
  when `hd == 0`, `selfhost/hir/hirdrive.ivy:486`).

**H4.5 corpus files must therefore be MIR-clean.**

---

## 3. The oracle's CLI surface

`ivy --help` (`src/app/main.cpp`, `printUsage`):

```
usage: ivy [options] <file.ivy>

Source files:
  .ivy        Ivy source (only accepted extension)

options:
  --tokens   print the token stream after preprocessing (debug)
  --lex      print the raw lexer token stream, before preprocessing (debug)
  --ast      print the parsed AST (debug)
  --hir      print the analyzed HIR (debug)
  --mir      print the lowered MIR with lifetime annotations (debug)
  --llvm     emit LLVM IR to stdout
  --run      interpret the program via IvyInterpret v0.2 (MIR-based)
  -c         compile to native object file (.o / .obj), no linking
  (default)  compile and link to a native executable
  --crt-static  link the C runtime statically (like Rust crt-static)
  --target <abi>  set the C++ ABI for name mangling: itanium (POSIX) or msvc (Windows)
  -o <file>  write output to <file> (.ll / .i / .ii / .o / .obj / .exe)
  -I <dir>   add <dir> to the #include <...> search path (repeatable)
  -h, --help show this help
```

Notes:

* **Every** mode runs the full parse->HIR->MIR pipeline, so *any* of them can surface a
  HIR- or MIR-stage lifetime diagnostic.  The only difference is which debug dump is
  printed and *when*.
* `--mir` is the only mode that prints per-expression lifetime information
  (` [lt: a]`, ` [lt: unknown]`, `block N`, `alloca`, `eval`, `ret`).  It needs a MIR
  driver in the selfhost port -> **that is H5, not H4.5**.
* `--lex` / `--tokens` only cover the *lexer* level: the `$a` token.  The lifetime
  *feature* (the `lifetime` keyword) is a keyword, and `$a` is a token of kind
  `lifetime`; `selfhost/corpus/lexer/ident/lifetimes.ivy` already covers that (H1.1).
* `-o <file>.ll` emits LLVM IR, `-o <file>.i` emits the expanded C++ source - neither is
  useful for a lifetime corpus.

### 3.4 IMPORTANT: the em dash is mojibake in the oracle's output

Four diagnostics contain a **U+2014 EM DASH** in the C++ source
(`src/hir/hir_builder.cpp:884`, `src/mir/mir_builder.cpp:343`, `:348`, `:356`).
The source file is UTF-8 (`E2 80 94`), but the MSVC toolchain reads the source as
**CP437**, so the literal ends up in the binary as the three characters
`Γ Ç ö` (U+0253, U+00C7, U+00F6) and is written to stderr as the UTF-8 bytes

```
CE 93 C3 87 C3 B6
```

Measured with `ivy --hir syntax/x22_ptr_ret_no_lt.ivy` (stderr hex dump, the only
non-ASCII run in the line):

```
... 20 6E 6F 20 72 65 74 75 72 6E 20 6C 69 66 65 74 69 6D 65 20 CE 93 C3 87 C3 B6 20 75 73 65 ...
```

**Good news:** the selfhost port is compiled by the same toolchain through the same
`.ivy -> .cpp -> .exe` path, so writing the **em dash** (U+2014) literally into the
`.ivy` source reproduces the *exact same bytes*.  Measured proof
(`enc/emdash.ivy` vs `enc/gammanai.ivy`):

| `.ivy` source bytes | program output bytes |
|---------------------|----------------------|
| `41 E2 80 94 42` (em dash) | `41 CE 93 C3 87 C3 B6 42`  <- **matches the oracle** |
| `41 CE 93 C3 87 C3 B6 42` (literal `ΓÇö`) | `41 E2 95 AC C3 B4 E2 94 9C C3 A7 E2 94 9C E2 95 A2 42` <- does NOT match |

=> **Instruction for the implementer:** put the real em dash `—` (UTF-8, no BOM) in the
`.ivy` source of the diagnostic message; do **not** paste the mojibake.  The existing
`selfhost/*.ivy` files are already UTF-8 with a few non-ASCII bytes in comments, so this
is consistent with the repo.  Verify with `difftest.ps1`, which compares raw bytes
(after CRLF normalisation).

The oracle writes stderr with **CRLF** line endings on Windows (`0D 0A`); `difftest.ps1`
normalises `\r\n` -> `\n`, so this does not matter for the harness.

---

## 4. Lifetime syntax - the accepted surface

### 4.1 Syntax table (syntax -> accepted? -> AST dump -> HIR dump)

| Syntax | Accepted? | `--ast` dump | `--hir` dump | Fixture |
|--------|-----------|--------------|--------------|---------|
| `lifetime<$a>` prefix before a `fn` | **yes** | invisible | `  lifetimes: a` | `syntax/x24_lt_trailing_fn.ivy` |
| `lifetime<$a>` prefix before a C-style leading return | **yes** | invisible | `  lifetimes: a` | `syntax/x01_lt_prefix_leading_ret.ivy` |
| `lifetime<$a, $b>` prefix | **yes** | invisible | `  lifetimes: a b` | `syntax/x02_lt_two_trailing_fn.ivy` |
| `lifetime<>` (empty list) | **yes** (no-op) | invisible | *nothing* | `syntax/x03_lt_empty.ivy` |
| `lifetime<$a, $b, $c, $d>` | **yes** | invisible | `  lifetimes: a b c d` | `bug/b07_lt_deep_nesting.ivy` |
| `lifetime<$a> constexpr fn f() ...` | **yes** | invisible | `  lifetimes: a` | `syntax/x23_lt_constexpr.ivy` |
| `lifetime<$a> consteval fn f() ...` | **yes** (same path as constexpr) | invisible | `  lifetimes: a` | UNKNOWN (not probed separately) |
| `lifetime<$a> [[ivy::lt_ret(a)]] fn f(...) ...` | **yes** | `function [[ivy::lt_ret(a)]]` | `  lifetimes: a` + `  return lifetime: a` | `syntax/x32_lt_before_attrs.ivy` |
| `lifetime<$a> template<typename T> fn f(...)` | **NO - oracle bug**, see 8.4 | - | - | `syntax/x10_lt_on_template.ivy` |
| `lifetime<$a> struct S { ... }` | **no** | - | parse error | `syntax/x09_lt_on_struct.ivy` |
| `lifetime<$a> enum E { A }` | **no** | - | parse error | `syntax/x28_lt_on_enum.ivy` |
| `lifetime<$a>` inside `extern "C" { ... }` | **no** | - | parse error | `syntax/x43_extern_c_with_lt.ivy` |
| `lifetime<$a>` followed by nothing | **no** | - | parse error | `bug/b05_lt_then_nothing.ivy` |
| parameter lifetime `const int32& $a x` | **yes** | invisible | `x [lt:a]` | `syntax/x01` |
| parameter lifetime `int32* $a p` | **yes** (but a raw pointer param needs `unsafe`) | invisible | `p [lt:a]` (plus an `unsafe` error) | `syntax/x29_lt_param_ptr.ivy` |
| parameter lifetime on a **by-value** param `int32 $a x` | **no** | - | HIR error | `syntax/x16_lt_on_byvalue_param.ivy` |
| return lifetime `-> const int32& $a` (trailing form) | **yes** | invisible | `  return lifetime: a` | `syntax/x05_ret_lt_trailing.ivy` |
| return lifetime `const int32& $a f(...)` (leading form) | **yes** | invisible | `  return lifetime: a` | `syntax/x06_ret_lt_leading.ivy` |
| return lifetime on a **by-value** return type `-> int32 $a` | **yes** (no check!) | invisible | `  return lifetime: a` | `syntax/x38_lt_ret_on_byvalue_return.ivy` |
| `'a` (Rust style) | **no** - lexes as an unterminated char literal | - | lexer error | `bug/b03_apos_a.ivy` |
| `'static` | **no** | - | lexer error | `bug/b04_apos_static.ivy` |
| `lifetime<'a>` | **no** | - | lexer error | `syntax/x08_lt_apos_a.ivy` |
| `lifetime<'static>` | **no** | - | lexer error | `syntax/x07_static_kw.ivy` |
| legacy `[[ivy::lt_def(a)]]` on a function | **yes** | `function [[ivy::lt_def(a)]]` | `  lifetimes: a` | `syntax/x11_legacy_all.ivy` |
| legacy `[[ivy::lt_def(a, b)]]` on a function | **yes** (multi-arg!) | `function [[ivy::lt_def(a, b)]]` | `  lifetimes: a b` | `syntax/x37_legacy_lt_def_two_args.ivy` |
| legacy `[[ivy::lt_ret(a)]]` on a function | **yes** | `function .[[ivy::lt_ret(a)]]` | `  return lifetime: a` | `syntax/x11_legacy_all.ivy` |
| legacy `[[ivy::lt(a)]]` on a **parameter** (after the name) | **yes** | `f(const int32_t x [[ivy::lt(a)]])` | `x [lt:a]` | `syntax/x11_legacy_all.ivy` |
| legacy `[[ivy::lt(a)]]` on a **function** | **no** | - | parse error `unknown Ivy attribute 'ivy::lt' in this position` | `syntax/x25_legacy_lt_on_fn.ivy` |
| legacy `[[ivy::lt_def(a)]]` on a **parameter** | **no** | - | parse error `unknown Ivy attribute 'ivy::lt_def' in this position` | `syntax/x26_legacy_ltdef_on_param.ivy` |

Legacy attribute position rules (`src/parsing/parser.cpp`):
* on a function the allow-list is `{"lt_def","lt_ret"}` (lines 2038 and 2122);
* on a parameter the allow-list is `{"lt"}` (line 2243) and the attribute must come
  **after** the parameter name (line 2240: `attribute must follow the parameter name`);
* `[[ivy::lt(a)]]` on an **unnamed** parameter -> `error: attribute must follow the
  parameter name` (parser.cpp:2240).  **UNKNOWN**: not probed as a separate fixture.

### 4.2 Native vs legacy precedence (measured, `src/hir/hir_builder.cpp:217-262`)

* **Declared list**: native names first, then the legacy `lt_def` names appended.
  `syntax/x27_native_and_legacy.ivy` (`lifetime<$a>` + `lt_def(b)`) -> `  lifetimes: a b`.
* **Return lifetime**: the **legacy `[[ivy::lt_ret(x)]]` WINS** over the native `$a`.
  The code sets `fn.returnLifetime` from the legacy attribute inside the attribute loop
  (line 246) and only falls back to `af.returnLifetime` when it is still empty
  (line 252).  **The source comment at line 244 claims the opposite - the code is
  authoritative.**  Measured: `syntax/x27_native_and_legacy.ivy` -> `  return lifetime: b`
  (not `a`), and the MIR stage then reports
  `returned reference has lifetime '$a' but the function declares lifetime '$b'`.
* **Parameter lifetime**: the **native `$a` WINS** over the legacy `[[ivy::lt(x)]]`
  (line 266 short-circuits the legacy path).  Measured: `syntax/x40_param_native_and_legacy_diff.ivy`
  (native `$a` + legacy `lt(b)`) -> `x [lt:a]`.
* **Duplicates**: a native `$a` + a legacy `lt_def(a)` is a *duplicate* (native is added
  first) and reports the **legacy** wording `duplicate lifetime 'a'` (no `$`).
  `syntax/x30_dup_legacy_vs_native.ivy`, `syntax/x39_param_lt_and_legacy_same.ivy`.
  A duplicate *native* name is caught earlier, by the **parser**:
  `duplicate lifetime '$a'` (with `$`).  `syntax/x12_dup_native.ivy` -> `1:16`.
  The equivalent check in `hir_builder.cpp:223` is therefore dead code.

---

## 5. The `--hir` dump format for lifetimes

`dumpHir()`, `src/app/main.cpp:577`:

```
function <name>[ [extern "C"]]
  return <rettype>(<paramtype> <paramname>[ [lt:X]], ...)
  lifetimes: <lt1> <lt2> ...        <- only if the function declares >= 1 lifetime
  return lifetime: <X>             <- only if the function has a return lifetime
  block
    ...
```

Rules, all measured:

* `  lifetimes:` - printed **only** when `fn->lifetimes` is non-empty; the names are
  space-separated, in declaration order (native first, then legacy).
* `  return lifetime: X` - printed **only** when `fn->returnLifetime` is non-empty; a
  single name, no list.
* ` [lt:X]` - printed **only** for a parameter that carries a lifetime; the suffix comes
  immediately after the parameter name, inside the parameter list.  Format `[lt:` + name
  + `]` - **no space after the colon**.  (The MIR dump uses ` [lt: X]` - with a space.)
* A **reference parameter without a lifetime** prints no suffix at all.
* A **return type with a lifetime** produces the `  return lifetime:` line only; it does
  **not** appear in the `return <type>(...)` line, because `dumpHirType()`
  (`src/app/main.cpp:360`) prints only `const` / `unsigned` / base / `*`. / `&`.
* The `lifetimes:` and `return lifetime:` lines come **after** the
  `return <type>(<params>)` line and **before** the body.
* A function with a lifetime but no parameters still prints `  lifetimes: a`
  (`syntax/x31_lt_ret_only.ivy`).

### 5.1 Verbatim dumps - lifetime annotations

**`syntax/x01_lt_prefix_leading_ret.ivy`** - leading return form, one lifetime, two params:

```
lifetime<$a>
const int32& $a first(const int32& $a x, const int32& $a y) {
    return x;
}
```

`ivy --ast` (exit 0) - **no lifetime information at all**:

```
function 
  return const int32_t first(const int32_t x, const int32_t y)
  block
    return
      ident x
```

`ivy --hir` (exit 0):

```
function first
  return const int32_t&(const int32_t& x [lt:a], const int32_t& y [lt:a])
  lifetimes: a
  return lifetime: a
  block
    return
      ident x
```

`ivy --mir` (exit 0) - note the **space** in `[lt: a]` and the `block 0` header:

```
function first
  return const int32_t&(const int32_t& x [lt:a], const int32_t& y [lt:a])
  lifetimes: a
  return lifetime: a
  block 0
    ret
      ident x [lt: a]
```

**`syntax/x02_lt_two_trailing_fn.ivy`** - two lifetimes, `fn` trailing form:

```
lifetime<$a, $b>
fn select_a(const int32& $a x, const int32& $b y) -> const int32& $a {
    return x;
}
```

`ivy --hir` (exit 0):

```
function select_a
  return const int32_t&(const int32_t& x [lt:a], const int32_t& y [lt:b])
  lifetimes: a b
  return lifetime: a
  block
    return
      ident x
```

**`syntax/x04_param_lt_only.ivy`** - parameter lifetime, by-value return (no return lifetime):

```
lifetime<$a>
fn f(const int32& $a x) -> int32 {
    return 1;
}
```

`ivy --hir` (exit 0):

```
function f
  return int32_t(const int32_t& x [lt:a])
  lifetimes: a
  block
    return
      int 1
```

**`syntax/x11_legacy_all.ivy`** - the legacy attributes produce exactly the same HIR:

```
[[ivy::lt_def(a)]]
[[ivy::lt_ret(a)]]
fn f(const int32& x [[ivy::lt(a)]]) -> const int32& {
    return x;
}
```

`ivy --ast` (exit 0) - the legacy attributes **are** visible:

```
function [[ivy::lt_def(a)]][[ivy::lt_ret(a)]]
  return const int32_t f(const int32_t x [[ivy::lt(a)]])
  block
    return
      ident x
```

`ivy --hir` (exit 0):

```
function f
  return const int32_t&(const int32_t& x [lt:a])
  lifetimes: a
  return lifetime: a
  block
    return
      ident x
```

**`syntax/x23_lt_constexpr.ivy`** - `lifetime<$a> constexpr fn f() -> int32 { return 1; }`:

```
function f
  return int32_t()
  lifetimes: a
  block
    return
      int 1
```

**`syntax/x27_native_and_legacy.ivy`** - the legacy return lifetime wins:

```
lifetime<$a>
[[ivy::lt_ret(b)]]
[[ivy::lt_def(b)]]
fn f(const int32& $a x) -> const int32& $a {
    return x;
}
```

`ivy --hir` (exit 1 - the dump is printed, then a **MIR** diagnostic):

```
function f
  return const int32_t&(const int32_t& x [lt:a])
  lifetimes: a b
  return lifetime: b
  block
    return
      ident x
```
(stderr)
```
...syntax\x27_native_and_legacy.ivy:5:5: error: returned reference has lifetime '$a' but the function declares lifetime '$b'
```

=> **do not put this shape in the H4.5 corpus** (MIR diagnostic).

**`syntax/x31_lt_ret_only.ivy`** - a return lifetime with no parameters and no
parameters to bind to; the oracle accepts it silently:

```
lifetime<$a>
fn f() -> const int32& $a {
    return 0;
}
```

`ivy --hir` (exit 0):

```
function f
  return const int32_t&()
  lifetimes: a
  return lifetime: a
  block
    return
      int 0
```

**`syntax/x37_legacy_lt_def_two_args.ivy`** - the legacy `lt_def` accepts several names:

```
[[ivy::lt_def(a, b)]]
fn f(const int32& x [[ivy::lt(a)]]) -> const int32& {
    return x;
}
```

`ivy --hir` (exit 0):

```
function f
  return const int32_t&(const int32_t& x [lt:a])
  lifetimes: a b
  block
    return
      ident x
```

**`syntax/x40_param_native_and_legacy_diff.ivy`** - the native parameter lifetime wins
over the legacy one:

```
lifetime<$a, $b>
fn f(const int32& $a x [[ivy::lt(b)]]) -> int32 {
    return 1;
}
```

`ivy --hir` (exit 0):

```
function f
  return int32_t(const int32_t& x [lt:a])
  lifetimes: a b
  block
    return
      int 1
```

**`syntax/x41_lt_only_param_no_ret.ivy`** - the same lifetime on two parameters:

```
lifetime<$a>
fn f(const int32& $a x, const int32& $a y) -> int32 {
    return 1;
}
```

`ivy --hir` (exit 0):

```
function f
  return int32_t(const int32_t& x [lt:a], const int32_t& y [lt:a])
  lifetimes: a
  block
    return
      int 1
```

**`syntax/x03_lt_empty.ivy`** - `lifetime<>` is accepted and produces **nothing**:

```
function noop
  return int32_t()
  block
    return
      int 1
```

---

## 6. The `__ret` lowering (new in commit `757ad75`)

### 6.1 Trigger conditions

Source: `src/hir/hir_builder.cpp`, `HirBuilder::buildReturn()` (starts at line 1681);
the new block is at lines ~1786-1832.

A `__ret` temp is created **iff all** of these hold:

1. `!dtorStmts.empty()` - at least one destructor call will run at this `return`.
   A destructor runs only for a local whose type is a struct/class with a **user-declared
   destructor** (`recordDtorVar()`, line 968: pointer types are skipped; the type must be
   found in `structs_` and have a `dtor`).  Scalars, structs without a `~T`, and
   pointers never trigger it.
2. `ret.value` is present (the `return` has a value).  A bare `return;` never triggers it.
3. `byValue` - the function's return type is **not** a reference, has `pointerDepth == 0`,
   and is **not** `void`.
4. `!plainLocal` - the return expression is **not** a bare `IdentRef` whose type is
   non-reference and whose `type.base` equals the return type's `base`.
   * `return local;` with the **same** base type -> no temp.
   * `return local;` with a **different** base type (e.g. an `int32` local returned from
     an `int64` function) -> **a temp IS created** (see 8.2).
   * a **reference** expression (`const int32& x`) is never "plain" -> a temp is created
     even when the bases match (see `ret/r17`).
5. `!scopes_.empty()` - always true inside a function body.

### 6.2 Name format

`__ret`.  If the name is already taken by a local **in scope** (the HIR builder's local
scope stack `scopes_`), the candidates are `__ret0`, `__ret1`, `__ret2`, ... in order
(`while (nameTaken(base)) base = "__ret" + std::to_string(n++);`).  The collision check
only looks at `scopes_` (locals), **not** at previously synthesized names and **not** at
sibling scopes that were already popped.  Measured: `ret/r03_local_named_ret.ivy` (a
local literally named `__ret`) -> the temp is `__ret0`; `ret/r05_sibling_scopes.ivy`
(two `return`s in two sibling scopes) -> **both** temps are named `__ret`.

### 6.3 Dump shape and ordering

The `return` statement is wrapped in a `Compound`; the compound's statements are, in
order:

```
1. decl <EXPR-TYPE> __ret =      <- the synthesized temp (the original return expression)
      <original return expression>
2. expr                          <- destructor calls, reverse declaration order
     call 'T::~T' : void
       ident <local>
3. return
     ident __ret
```

* The temp's **declared type is the type of the return *expression***
  (`dd.type = ret.value->type`), **not** the function's return type.
* The `IdentRef` used as the return value has `isReference` forced to `false`
  (lines 1821-1822) - invisible in the dump (idents print only their name).
* The temp is registered as a dtor variable (`recordDtorVar`) **and** in
  `returnedLocals_`, so the temp's *own* destructor is not emitted.
* The temp `decl` is inserted at the **front** of the dtor statement list
  (`dtorStmts.insert(dtorStmts.begin(), ...)`) - always first.
* If there are no destructor calls, the `return` is left completely untouched (no
  compound, no temp) - zero behaviour change.
* The temp **is** declared in the enclosing scope, so a second `__ret` in the same scope
  would be `__ret0`.  In practice that never happens: the first `return` clears that
  scope's destructor list (see 8.6), so the second `return` in the same scope creates
  no temp at all.

### 6.4 Before / after commit `757ad75`

```
before:  block { <dtor calls> ; return <expr> }
after:   block { decl __ret = <expr> ; <dtor calls> ; return ident __ret }
```

### 6.5 Verbatim dumps

All of these use this prelude:

```
#pragma ivy cnumber

struct Res {
    int32 v;
    Res(int32 v_) : v(v_) {
    }
    ~Res() {
    }
};
```

**`ret/r01_nontrivial.ivy`** - the canonical case (`return a + 1;` with a dtor local):

```
fn pick(int32 a) -> int32 {
    Res r(1);
    return a + 1;
}
```

`ivy --hir` (exit 0):

```
function Res::Res
  return void(Res& this, int32_t v_)
  block
    expr
      assign '=' : int32_t
        member . v
          this
        ident v_
function Res::~Res
  return void(Res& this)
  block
function pick
  return int32_t(int32_t a)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      decl int32_t __ret =
        binary '+' : int32_t
          ident a
          int 1
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident __ret
```

**`ret/r02_plain_local.ivy`** - `return q;` where `q` is an `int32` local -> **no temp**
(only the dtor compound):

```
function pick
  return int32_t(int32_t a)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    decl int32_t q =
      ident a
    block
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident q
```

**`ret/r03_local_named_ret.ivy`** - a local named `__ret` -> the temp is `__ret0`:

```
function pick
  return int32_t(int32_t a)
  block
    decl int32_t __ret =
      int 5
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      decl int32_t __ret0 =
        binary '+' : int32_t
          ident a
          int 1
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident __ret0
```

**`ret/r04_widen.ivy`** - `fn widen(int32 a) -> int64 { Res r(1); return a; }` -> the temp
type is the **expression's** type, not the return type (see 8.2):

```
function widen
  return int64_t(int32_t a)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      decl int32_t __ret =
        ident a
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident __ret
```

**`ret/r05_sibling_scopes.ivy`** - two `return`s in two sibling scopes -> two temps,
**both named `__ret`**:

```
function pick
  return int32_t(int32_t a, int32_t b)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    if
      binary '>' : bool
        ident a
        ident b
      block
        block
          decl int32_t __ret =
            binary '+' : int32_t
              ident a
              ident b
          expr
            call 'Res::~Res' : void
              ident r
          return
            ident __ret
    block
      decl int32_t __ret =
        binary '-' : int32_t
          ident a
          ident b
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident __ret
```

**`ret/r06_nested_scope.ivy`** - a `return` inside a nested block: the outer scope's
destructor is emitted **twice** (see 8.5):

```
function pick
  return int32_t(int32_t a)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      decl int32_t inner =
        ident a
      block
        decl int32_t __ret =
          binary '+' : int32_t
            ident inner
            int 1
        expr
          call 'Res::~Res' : void
            ident r
        return
          ident __ret
    expr
      call 'Res::~Res' : void
        ident r
```

**`ret/r07_same_scope_two_returns.ivy`** - two `return`s in the **same** scope (a
`switch`): the second one loses both the destructor and the temp (see 8.6):

```
function pick
  return int32_t(int32_t a)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    switch
      ident a
      case
        int 1
        block
          decl int32_t __ret =
            binary '+' : int32_t
              ident a
              int 1
          expr
            call 'Res::~Res' : void
              ident r
          return
            ident __ret
      default
        return
          binary '-' : int32_t
            ident a
            int 1
```

**`ret/r08_ref_return.ivy`** - reference return type -> **no temp** (`byValue` is false),
but the dtor compound is still there:

```
function pick
  return const int32_t&(const int32_t& x)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident x
```

**`ret/r11_struct_by_value.ivy`** - `fn make() -> Res { Res r(1); return r; }` -> **no
compound at all** (NRVO: `r` is in `returnedLocals_`, so no destructor runs):

```
function make
  return Res()
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    return
      ident r
```

**`ret/r13_local_only_dtor.ivy`** - `return a;` (a parameter) -> no temp:

```
function pick
  return int32_t(int32_t a)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident a
```

**`ret/r14_two_dtors.ivy`** - two dtor locals -> the temp first, then the destructors in
reverse declaration order (`s` then `r`):

```
function pick
  return int32_t(int32_t a)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      decl Res s
      expr
        call 'Res::Res' : void
          ident s
          int 2
    block
      decl int32_t __ret =
        binary '+' : int32_t
          ident a
          int 1
      expr
        call 'Res::~Res' : void
          ident s
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident __ret
```

**`ret/r15_return_literal.ivy`** - `return 42;` -> the temp type is `int` (the literal's
HIR type), **not** `int32_t`:

```
function pick
  return int32_t()
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      decl int __ret =
        int 42
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident __ret
```

**`ret/r16_return_call.ivy`** - `return g(a);` -> the temp holds the call:

```
function pick
  return int32_t(int32_t a)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      decl int32_t __ret =
        call 'g' : int32_t
          ident a
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident __ret
```

**`ret/r17_ref_expr_byvalue_return.ivy`** - `fn pick(const int32& x) -> int32 { Res r(1);
return x; }` -> the temp keeps the **reference** type:

```
function pick
  return int32_t(const int32_t& x)
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      decl const int32_t& __ret =
        ident x
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident __ret
```

**`ret/r19_struct_expr_return.ivy`** - `fn pick() -> Res { Res r(1); Res s(2); return s; }`
-> `s` is NRVO'd (no dtor), `r`'s dtor runs, and there is **no temp** (`s`'s base == the
return type's base):

```
function pick
  return Res()
  block
    block
      decl Res r
      expr
        call 'Res::Res' : void
          ident r
          int 1
    block
      decl Res s
      expr
        call 'Res::Res' : void
          ident s
          int 2
    block
      expr
        call 'Res::~Res' : void
          ident r
      return
        ident s
```

**`ret/r18_for_scope_dtor.ivy`** - a `return` inside a `for` body: the temp compound is
nested inside the loop body, the destructor runs once.

**`ret/r10_no_dtor.ivy`** / **`ret/r20_no_dtor_nontrivial.ivy`** - a struct **without** a
destructor -> no compound, no temp, the `return` stays plain:

```
function pick
  return int32_t(int32_t a)
  block
    decl NoDtor n
    return
      binary '+' : int32_t
        ident a
        int 1
```

### 6.6 MIR view of the temp (`ret/r01_nontrivial.ivy`, `ivy --mir`)

```
function pick
  return int32_t(int32_t a)
  block 0
    alloca Res r
    eval
      call : void [lt: unknown]
        ident r
        int 1
    alloca int32_t __ret
      binary '+' : int32_t
        ident a
        int 1
    eval
      call : void [lt: unknown]
        ident r
    ret
      ident __ret
```

Note `call : void [lt: unknown]` - a **void** call still gets a lifetime suffix.  See 8.1.

---

### 6.7 Exact diagnostic ordering inside one function (measured)

The HIR builder's `buildFunction` (`src/hir/hir_builder.cpp`) emits diagnostics in this
fixed order; the port must reproduce it exactly:

```
1. duplicate lifetime 'a'                (legacy lt_def, inside lowerLifetimeAttributes, line 234)
2. return lifetime '$X' is not declared in lifetime<...>      (line 259)
3. return type 'nullptr_t' requires an unsafe block          (line 857)
4. raw pointer return type requires an unsafe block          (line 860)
5. parameter errors, in SOURCE ORDER:                        (param loop, line 863)
     lifetime '$X' is not declared in lifetime<...>          (line 272)
     lifetime annotation requires a reference or pointer parameter   (line 277)
     raw pointer parameter type requires an unsafe block      (line 874)
6. function 'f' returns a pointer but has no return lifetime ...  (line 883)
7. body errors (e.g. cannot return value of type 'int' from function returning 'int32_t*')
```

Measured, `order/o1.ivy`:

```
lifetime<$a>
fn bad(int32 $a p, const int32& $b q) -> const int32& $c {
    return q;
}
```

`ivy --hir` (exit 1, no dump):

```
...order\o1.ivy:2:1: error: return lifetime '$c' is not declared in lifetime<...>
...order\o1.ivy:2:8: error: lifetime annotation requires a reference or pointer parameter
...order\o1.ivy:2:20: error: lifetime '$b' is not declared in lifetime<...>
```

Measured, `order/o3.ivy` (two parameter errors, in source order):

```
lifetime<$a>
fn bad3(const int32& $b x, const int32& $c y) -> const int32& $d {
    return x;
}
```

```
...order\o3.ivy:2:1: error: return lifetime '$d' is not declared in lifetime<...>
...order\o3.ivy:2:9: error: lifetime '$b' is not declared in lifetime<...>
...order\o3.ivy:2:28: error: lifetime '$c' is not declared in lifetime<...>
```

Measured, `order/o2.ivy` (the pointer-return rule comes **after** the parameter loop):

```
fn bad2(int32 $a p) -> int32* {
    return 0;
}
```

```
...order\o2.ivy:1:1: error: raw pointer return type requires an unsafe block
...order\o2.ivy:1:9: error: lifetime '$a' is not declared in lifetime<...>
...order\o2.ivy:1:1: error: function 'bad2' returns a pointer but has no return lifetime - use `lifetime<$a>` and annotate the return type with $a
...order\o2.ivy:2:5: error: cannot return value of type 'int' from function returning 'int32_t*'
```

(the 4th line is a body error; note the em dash in the 3rd line - see 3.4.)

---

## 7. What the HIR builder actually tracks (question 6)

`src/hir/hir.h`:

```cpp
struct Lifetime {                              // line 235
    std::string_view name;
    SourceLoc loc;
};
struct Param {
    Type type;
    std::string_view name;
    std::string_view lifetime;                 // line 243  - native $a / [[ivy::lt(a)]]
    const ivy::Expr* defaultValue;
    SourceLoc loc;
};
struct Function {
    ...
    std::vector<Lifetime> lifetimes;            // line 256  - lifetime<$a> / [[ivy::lt_def(a)]]
    std::string_view returnLifetime;            // line 257  - -> T $a / [[ivy::lt_ret(a)]]
    ...
};
```

* `hir::Type` is an alias of `ivy::Type` (`hir.h:14`) and has **no** lifetime field.
* `dumpHirType()` (`src/app/main.cpp:360`) prints only `const` / `unsigned` / base /
  `*` / `&` - **no lifetime**.
* The HIR builder computes **no** lifetime for expressions.  An `ident` that refers to a
  lifetime-carrying parameter carries **no** lifetime information in the HIR - the
  lifetime is only visible in the parameter list.
* All lifetime *inference* (`Named` / `Static` / `Local` / `Unknown`) is
  `mir::Expr::lifetime`, i.e. **MIR**.

**So the HIR dump shows exactly three lifetime items** - `lifetimes:`, `return lifetime:`
and `[lt:X]` - plus the `__ret` temp (which is not a lifetime feature, it is the RAII
return-evaluation change that shipped in the same rebuild).

---

## 8. Known oracle bugs / quirks (reproduce the code, not the intent)

### 8.1 MIR: every call result's lifetime is forced to `Unknown`

`src/mir/mir_builder.cpp:595-603` (verbatim):

```cpp
        if (e.type.pointerDepth > 0) {
            if (!call.returnLifetime.empty()) {
                out->lifetime.kind = mir::Lifetime::Kind::Named;
                out->lifetime.name = call.returnLifetime;
            } else {
                out->lifetime.kind = mir::Lifetime::Kind::Unknown;  // opaque, e.g. malloc
            }
        }
        out->lifetime.kind = mir::Lifetime::Kind::Unknown;  // virtual: unknown
```

Line 603 overwrites the `Named` lifetime computed at line 597, so **the result of any
call always has lifetime `unknown`**, even when the callee declares a return lifetime.
Observable in the MIR dump: `call : void [lt: unknown]`, `ret ident __ret [lt: unknown]`.
Consequence: the "returned reference has lifetime '$X' but the function declares lifetime
'$Y'" check can only fire for a **parameter** ident, never for a call result.

### 8.2 HIR: the `__ret` temp type is the *expression* type, not the return type

`ret/r04_widen.ivy`: `fn widen(int32 a) -> int64 { Res r(1); return a; }` ->

```
    block
      decl int32_t __ret =
        ident a
```

The function returns `int64_t` but the returned value is `int32_t`.  Pre-existing for
`return a;`, but the `__ret` lowering makes it **visible in the dump**.

The same applies to `ret/r17_ref_expr_byvalue_return.ivy`, where the temp type is
`const int32_t&` while the function returns `int32_t`.

### 8.3 HIR: `return <local>` with a different base type creates a temp

`plainLocal` compares only `type.base` (not the full type), so the `int32_t -> int64_t`
widening of 8.2 creates a `__ret`.  The NRVO path (`returnedLocals_`,
`hir_builder.cpp:1757-1765`) uses the same base comparison, so the local is *not* marked
as transferred.

### 8.4 Parser: `lifetime<$a> template<...> fn` is broken

`src/parsing/parser.cpp:789` does `expect(TokenKind::Lt, ...)` and then calls
`parseTemplateParams()`, which **also** does `expect(TokenKind::Lt, "expected '<' to
start template parameter list")` (parser.cpp:1649, whose comment wrongly claims "the
leading `<` has NOT been consumed yet").  The `<` is therefore consumed twice.
Observed (`syntax/x10_lt_on_template.ivy`):

```
...syntax\x10_lt_on_template.ivy:2:10: error: expected '<' to start template parameter list
...syntax\x10_lt_on_template.ivy:2:20: error: expected template parameter name
...syntax\x10_lt_on_template.ivy:5:1: error: expected '>' to close template parameter list
```

=> **Do not put `lifetime<...>` on a template function in the corpus.**

### 8.5 HIR: the outer-scope destructor is emitted twice

When a `return` inside a nested block triggers the dtor compound, `emitDtorCalls()` walks
*all* scopes down to `dtorBaseline_` and emits the outer scope's destructor inside the
compound, but only `dtorStacks_.back()` (the innermost scope) is cleared afterwards
(hir_builder.cpp:1836-1839).  The outer scope's destructor is therefore emitted **again**
at function-scope exit.  See the dump of `ret/r06_nested_scope.ivy` in 6.5: the
`call 'Res::~Res'` for `r` appears twice.

(The interpreter executes it only once - the `return` short-circuits the rest of the
function - but the HIR contains the duplicate call.  Native codegen may double-free.)

### 8.6 The first `return` in a scope clears that scope's destructor list

`buildReturn()` clears **only the innermost** scope's dtor list
(`dtorStacks_.back().clear()`).  Consequence: a *second* `return` in the **same** scope
finds an empty dtor list and therefore emits **neither the destructor calls nor the
`__ret` temp**.  See `ret/r07_same_scope_two_returns.ivy` (the `default` branch has no
dtor and no temp) and `ret/r21_temp_then_early_return.ivy` (the `if` branch gets the
temp, the fall-through `return` gets a second temp because it is in a *sibling* scope).
Reproduce the code, not the intent.

### 8.7 Borrow-checker gaps

* Writing through a variable while it is borrowed is **not** diagnosed
  (`stage/s14_assign_through_borrow.ivy` -> exit 0).
* Using a value after its owner was destroyed is **not** diagnosed
  (`stage/s8_use_after_destroy.ivy` -> exit 0).
* Binding a reference to a temporary is **not** diagnosed
  (`stage/s9_ref_to_temp.ivy` -> exit 0).
* `checkStore` (the dangling store) is practically unreachable outside `unsafe`,
  because every way of producing a `Local`-lifetime value (`&local`, raw pointer locals)
  requires `unsafe`, which disables the check.
* `unsafe` disables **all** of the borrow/lifetime checks
  (`stage/s17_borrow_in_unsafe.ivy` -> exit 0).

### 8.8 The AST dump is lossy

`dumpAst` (`src/app/main.cpp:331-356`) prints `fn.attrs` and `param.attrs` but **not**
`declaredLifetimes`, `returnLifetime` or `param.lifetime`.  Native `$a` syntax is
therefore invisible in the `--ast` dump.  The existing AST corpus
(`selfhost/corpus/parser/decl/11_lifetime.ivy`) only covers
`lifetime<$a> fn f() -> const int32& $a;` and therefore does **not** exercise this.

### 8.9 The legacy return-lifetime precedence contradicts its own comment

See 4.2: the legacy `[[ivy::lt_ret(x)]]` wins over the native `$a`, although
`hir_builder.cpp:244` says the opposite.  Reproduce the **code**, not the comment.

### 8.10 The em dash is mojibake in the oracle's output

See 3.4.  Write the real em dash into the `.ivy` source; the toolchain reproduces the
oracle's bytes exactly.

### 8.11 Crash-prone inputs

**No input was found that crashes the oracle.**  Every malformed lifetime input below
produced a clean diagnostic and exit 1 (or 0):

| Input | Fixture | Result |
|-------|---------|--------|
| `lifetime<$>` | `bug/b01_lt_dollar_only.ivy` | lexer: `unexpected character` |
| `lifetime<a>` | `syntax/x14_lt_no_dollar.ivy` | parser: `expected a lifetime variable ($name) in lifetime<...>` |
| `lifetime<$a` (unterminated) | `syntax/x15_lt_unterminated.ivy` | parser: `expected '>' to close lifetime<...>` |
| `lifetime<$a<$b>>` | `bug/b08_lt_nested_angle.ivy` | parser: `expected '>' to close lifetime<...>` |
| `fn f() -> $a` | `bug/b02_ret_lifetime_alone.ivy` | parser: `expected a type (void, bool, int8, ...)` |
| `const int32& $ x` | `bug/b09_param_lt_only_dollar.ivy` | lexer: `unexpected character` |
| `lifetime<$a> struct S {...}` | `syntax/x09_lt_on_struct.ivy` | parser: `expected a function declaration after lifetime<...>` |
| `lifetime<$a> enum E { A }` | `syntax/x28_lt_on_enum.ivy` | parser: `expected a function declaration after lifetime<...>` |
| `lifetime<$a>` (EOF) | `bug/b05_lt_then_nothing.ivy` | parser: `expected a function declaration after lifetime<...>` (at `0:0`!) |
| `'a` / `'static` | `bug/b03`, `bug/b04` | lexer: `unterminated character literal` |

**UNKNOWN:** whether a lifetime input can crash the oracle.  No fuzzing was performed;
the parser / HIR-builder lifetime paths are defensive (every branch emits a diagnostic).

Note: the **lexer** diagnostics use a different format from the parser's: the path is
**quoted and backslash-escaped**, e.g.

```
"C:\\Users\\ASUS\\AppData\\Local\\Temp\\opencode\\h45recon\\v2\\bug\\b01_lt_dollar_only.ivy":1:10: error: unexpected character
```

while the parser / HIR / MIR diagnostics use the plain path:

```
C:\Users\ASUS\AppData\Local\Temp\opencode\h45recon\v2\bug\b01_lt_dollar_only.ivy:1:10: error: unexpected character
```

---

## 9. The selfhost port today (gap analysis)

Measured with the freshly built port (`hirdrive.exe` / `astdrive.exe` built from
`selfhost/{parsing,hir}`) over all 95 fixtures.  `GAP_hir.txt` / `GAP_ast.txt`.

**The port has NO lifetime support at all.**  `grep -n lifetime selfhost/parsing/ast.ivy
selfhost/hir/hir.ivy selfhost/hir/hir_builder.ivy selfhost/hir/hirdrive.ivy` returns
**nothing**.  Only `selfhost/parsing/parser.ivy` mentions lifetimes, and only to *skip*
them.

`--hir` differential over the 98 fixtures (95 probes + 3 `enc/` ones): **26 identical /
72 different**.
(`--ast` differential: **52 identical / 43 different**.)  Every identical one is a
fixture that does **not** use a lifetime annotation (or that only trips a *lexer* error
whose path format differs between the oracle and the port).  **Every** fixture that uses
a lifetime annotation is currently different.

### 9.1 Parser gaps (`selfhost/parsing/parser.ivy`)

| # | Gap | Evidence |
|---|-----|----------|
| P1 | `parse_lifetime_decl()` (line 1739) **discards the names** - it never records them.  The C++ version returns a `std::vector<std::string_view>`. | `syntax/x01` .. `x07`, `x23`, `x24`, `bug/b07`: the port prints no `lifetimes:` line |
| P2 | `parse_lifetime_decl()` has **no duplicate check** (`duplicate lifetime '$a'`). | `syntax/x12_dup_native.ivy`: the oracle errors, the port prints an AST/HIR dump |
| P3 | `parse_lifetime_decl()` is **missing the `else break;`** after a non-comma lifetime (C++ parser.cpp:622-626).  Consequence: `lifetime<$a` (unterminated) produces `expected a lifetime variable ($name) in lifetime<...>` in the port but `expected '>' to close lifetime<...>` in the oracle. | `syntax/x15_lt_unterminated.ivy`, `bug/b08_lt_nested_angle.ivy` |
| P4 | `parse_lifetime_decl()` does not implement the "what follows" dispatch of `src/parsing/parser.cpp:755-841`: no `constexpr`/`consteval` branch, no `template` branch, no `fn` branch, and no `expected a function declaration after lifetime<...>` check. | `syntax/x23` (constexpr), `x32` (attrs then fn), `x09`/`x28`/`bug/b05` (the "expected a function declaration" message) |
| P5 | `parse_params()` (line 1783) **discards** the native `$a` parameter lifetime. | `syntax/x01` .. `x07` etc.: the port prints no `[lt:a]` |
| P6 | `parse_function()` (line 3024) and `parse_fn_trailing()` (line 3073) **discard** the native `$a` return lifetime. | `syntax/x05`, `x06`: the port prints no `return lifetime:` |
| P7 | **BUG**: in `parse_fn_trailing()` the variable `a_end` is captured **after** `parse_params()` (line 3077), so the function-level attribute allow-list (`validate_list(all, "lt_def", "lt_ret")`, line 3081) wrongly includes the **parameters'** attributes.  Result: `fn f(const int32& x [[ivy::lt(a)]]) -> const int32& { . }` is rejected with `unknown Ivy attribute 'ivy::lt' in this position` by the port, while the oracle accepts it.  The **leading** form (`parse_function`, line 3028) is unaffected because it captures `a_end` before `parse_params()`. | `syntax/x11`, `x19`, `x20`, `x21` (all use the `fn` trailing form) - the port errors where the oracle does not |

**P7 is a pre-existing bug that must be fixed for H4.5** if the corpus contains a
legacy parameter lifetime in the `fn` trailing form.  It can be avoided by using the
C-style leading form in the corpus, but fixing it is cheap: move
`long a_end = this.ap_n_;` (and the `all`/`validate_list` block) to **before**
`this.parse_params()` in `parse_fn_trailing`, mirroring `parse_function`.

### 9.2 AST / HIR data-model gaps

| # | Gap | Where |
|---|-----|-------|
| A1 | `AParam` has **no** `lifetime` field. | `selfhost/parsing/ast.ivy:97` |
| A2 | `AFunction` has **no** declared-lifetime list and **no** `return_lifetime` field. | `selfhost/parsing/ast.ivy:111` |
| A3 | `HParam` has **no** `lifetime` field. | `selfhost/hir/hir.ivy:56` |
| A4 | `HFunction` has **no** lifetime list and **no** `return_lifetime` field. | `selfhost/hir/hir.ivy:64` |
| A5 | `hir_builder.ivy` has **no** `lowerLifetimeAttributes()` / `lowerParamAttribute()` equivalent. | `selfhost/hir/hir_builder.ivy` |
| A6 | `hirdrive.ivy`'s `emit()` (line 371) prints the function header but has no `lifetimes:` / `return lifetime:` lines and no ` [lt:X]` suffix. | `selfhost/hir/hirdrive.ivy:371-402` |

### 9.3 Missing machinery for the `__ret` lowering (all of it)

`selfhost/hir/hir_builder.ivy` contains **no** dtor tracking, **no** dtor emission, **no**
NRVO / `returnedLocals_`, **no** `synthesizedNames_`, and **no** struct construction.
Measured: the port rejects `Res r(1);` with

```
error: undeclared identifier 'Res'
error: call to undeclared function 'Res'
error: cannot initialize variable 'r' of type 'Res' with a value of type 'int'
```

and it does not lower the struct constructor body (`assign '=' : int32_t member . v this
ident v_`) either.  The `__ret` lowering is therefore **not** implementable in H4.5 -
it needs a new milestone (RAII / destructor lowering + struct construction).

---

## 10. Recommendation: H4.5 scope, corpus, and deferrals

### 10.1 Verdict

**H4.5 IS feasible as a `--hir` corpus** - but only for the *lifetime-annotation* part:

* the lifetime **syntax** (parser level),
* the lifetime **lowering** into the HIR (`lifetimes:`, `return lifetime:`, `[lt:X]`),
* the **HIR-stage lifetime diagnostics** (10 distinct messages).

It is **NOT** feasible for:

* the borrow checker / lifetime verifier / lifetime inference (they are MIR),
* the `__ret` lowering (it needs the whole RAII/destructor machinery plus struct
  construction, none of which the port has).

The `__ret` change is *not* a lifetime feature at all - it is the return-evaluation-order
fix that happened to land in the same rebuild.  It should be tracked as a separate
milestone, e.g. "H4.7 RAII / destructor lowering" (or pushed into H5, since the RAII
emission really belongs with the MIR/codegen work).

### 10.2 What H4.5 has to change in the port

1. **`selfhost/parsing/ast.ivy`**
   * `AParam`: add `const char* lifetime;` (native `$a`, stored **without** the `$`).
   * `AFunction`: add a declared-lifetime list (`const char**` + count) and
     `const char* return_lifetime;`.
2. **`selfhost/parsing/parser.ivy`**
   * `parse_lifetime_decl()` must **collect** the names, implement the **duplicate check**
     (`duplicate lifetime '$a'` at the token *after* the duplicate - see the oracle's
     column), and add the missing `else break;`.
   * Implement the post-`lifetime<...>` dispatch: `constexpr`/`consteval`, `template`,
     `fn`, `[[attrs]]`, and the `expected a function declaration after lifetime<...>`
     / `expected a function after lifetime<...> + constexpr` errors.
   * `parse_params()` must store the native `$a` into `AParam.lifetime`.
   * `parse_function()` / `parse_fn_trailing()` must store the return `$a` into
     `AFunction.return_lifetime`.
   * **Fix P7** (the `a_end` capture order in `parse_fn_trailing`).
3. **`selfhost/hir/hir.ivy`**
   * `HParam`: add `const char* lifetime;`
   * `HFunction`: add the lifetime-name list + `const char* return_lifetime;`
4. **`selfhost/hir/hir_builder.ivy`**
   * port `lowerLifetimeAttributes()` and `lowerParamAttribute()` **verbatim**, including
     the native-first / legacy-second ordering, the legacy-wins return-lifetime
     precedence (4.2) and the exact diagnostics + source locations.
   * add the pointer-return rule (`function 'f' returns a pointer but has no return
     lifetime ...`) - it needs the em dash (see 3.4).
5. **`selfhost/hir/hirdrive.ivy`**
   * in `emit()`: print `  lifetimes: a b` and `  return lifetime: X` after the
     `return <type>(<params>)` line, and ` [lt:X]` after each parameter name.
6. **New corpus** `selfhost/corpus/hir/borrow/` (see 10.3).

### 10.3 Recommended H4.5 corpus (`selfhost/corpus/hir/borrow/`)

**16 core files** (one rule per file) - all verified MIR-clean, i.e. `ivy --mir` exits 0
or reports the *same* HIR/parser error as `--hir`:

| # | file | rule it pins down | expected `--hir` |
|---|------|-------------------|-----------------|
| 01 | `01_lt_decl_param.ivy` | `lifetime<$a>` + one param with `$a` | `lifetimes: a` + `x [lt:a]` |
| 02 | `02_lt_two_lifetimes.ivy` | `lifetime<$a, $b>` + two params | `lifetimes: a b`, two `[lt:.]` |
| 03 | `03_lt_return_lifetime.ivy` | return lifetime `-> const int32& $a` | `return lifetime: a` |
| 04 | `04_lt_leading_return_form.ivy` | the C-style leading return form | same HIR as 03 |
| 05 | `05_lt_trailing_fn_form.ivy` | the `fn f(...) -> T` form | same HIR as 03 |
| 06 | `06_lt_empty_list.ivy` | `lifetime<>` | **no** `lifetimes:` line |
| 07 | `07_lt_constexpr.ivy` | `lifetime<$a> constexpr fn f()` | `lifetimes: a` |
| 08 | `08_lt_no_params.ivy` | `lifetime<$a>` on a function with no params | `lifetimes: a` only |
| 09 | `09_param_lt_byvalue_return.ivy` | param lifetime, by-value return | `lifetimes: a`, **no** `return lifetime:` |
| 10 | `10_legacy_lt_def.ivy` | `[[ivy::lt_def(a)]]` | same HIR as the native form |
| 11 | `11_legacy_lt_ret.ivy` | `[[ivy::lt_ret(a)]]` | `return lifetime: a` |
| 12 | `12_legacy_lt_param.ivy` | `[[ivy::lt(a)]]` on a parameter | `x [lt:a]` |
| 13 | `13_lt_two_params_one_lt.ivy` | the same lifetime on two params | two `[lt:a]`, one `lifetimes: a` |
| 14 | `e_01_undeclared_param_lt.ivy` | HIR error 1 | `lifetime '$b' is not declared in lifetime<...>` |
| 15 | `e_02_undeclared_ret_lt.ivy` | HIR error 2 | `return lifetime '$b' is not declared in lifetime<...>` |
| 16 | `e_03_lt_on_byvalue_param.ivy` | HIR error 3 | `lifetime annotation requires a reference or pointer parameter` |

**7 optional extras** (each one rule; they need extra parser work):

| # | file | rule | expected |
|---|------|------|----------|
| 17 | `e_04_legacy_undeclared.ivy` | legacy `lt(b)` with `b` undeclared | `[[ivy::lt(b)]]: lifetime 'b' is not declared via [[ivy::lt_def(...)]]` |
| 18 | `e_05_legacy_lt_arity.ivy` | `[[ivy::lt(a, b)]]` | `[[ivy::lt]] expects exactly one lifetime argument` |
| 19 | `e_06_legacy_lt_ret_arity.ivy` | `[[ivy::lt_ret(a, b)]]` | `[[ivy::lt_ret]] expects exactly one lifetime argument` |
| 20 | `e_07_legacy_lt_on_byvalue_param.ivy` | `[[ivy::lt(a)]]` on a by-value param | `[[ivy::lt]] requires a reference or pointer parameter` |
| 21 | `e_08_dup_legacy_ltdef.ivy` | two `lt_def(a)` | `duplicate lifetime 'a'` |
| 22 | `e_09_dup_native.ivy` | `lifetime<$a, $a>` | `duplicate lifetime '$a'` (parser error) |
| 23 | `e_10_dup_native_vs_legacy.ivy` | native `$a` + legacy `lt_def(a)` | `duplicate lifetime 'a'` |

Run with:

```
powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/hir/borrow -Kinds all -Oracle --hir `
  -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder `
  -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive
```

**Corpus rules (hard constraints):**

* **No MIR-stage diagnostic** in any corpus file (that would make the file
  un-matchable).  In particular avoid: returning a reference to a local, a lifetime
  mismatch on `return`, a borrow conflict, and a native+legacy combination where the
  legacy return lifetime differs from the native one (that one *does* produce a MIR
  error - see `syntax/x27`).
* **No raw-pointer parameters/returns** unless the file is wrapped in `unsafe` (the
  `unsafe` rules would add a second, unrelated diagnostic).
* **No struct with a destructor** (that is the `__ret`/RAII territory).
* Avoid `lifetime<...>` on a template function (oracle bug 8.4).

### 10.4 Explicit deferral list

| Item | Why it must be deferred | Target |
|------|-------------------------|--------|
| B1 `checkBorrow` (aliasing XOR mutability) | MIR stage - `src/mir/mir_builder.cpp:315` | H5 |
| B2 `checkReturn` (dangling / lifetime match) | MIR stage - `src/mir/mir_builder.cpp:228` | H5 |
| B3 `checkStore` (dangling store) | MIR stage - `src/mir/mir_builder.cpp:298` | H5 |
| B4 `mir::Expr::lifetime` inference (`Named`/`Static`/`Local`/`Unknown`) | MIR stage | H5.2 (`corpus/mir/lifetime`) |
| B5 the MIR dump (` [lt: a]` per expression, `block N`, `alloca`/`eval`/`ret`) | needs a MIR driver | H5.1 / H5.2 |
| B6 the `__ret` temp lowering | needs the whole RAII/dtor machinery + struct construction, neither of which the port has | new H4.x "RAII / destructor lowering", or H5 |
| B7 destructor emission / NRVO / `returnedLocals_` | same as B6 | new H4.x |
| B8 struct-constructor member-init lowering (`Res::Res` body) | not in the port | new H4.x |
| B9 use-after-destroy detection | the oracle does not implement it at all | - |
| B10 borrow-error corpus files | they produce MIR diagnostics -> not `--hir`-comparable | H5 corpus |

---

## 11. Answers to the eight questions, in one place

**Q1 - Which stage owns borrow/lifetime errors?**
HIR owns *annotation validation* only (section 2.2, 10 messages).  The borrow checker,
the return-lifetime verifier, the dangling-store checker and all lifetime *inference*
live in **MIR** (`src/mir/mir_builder.cpp`).  Because `ivy --hir` runs the whole pipeline,
MIR diagnostics still surface in `--hir` mode on stderr, after the HIR dump, exit 1; HIR
diagnostics surface *instead of* the dump, exit 1.

**Q2 - Lifetime syntax.**
`lifetime<$a, $b>` prefix; `$a` between a parameter's type and its name; `$a` after the
return type; the legacy `[[ivy::lt_def(a)]]` / `[[ivy::lt_ret(a)]]` / `[[ivy::lt(a)]]`.
`'a` / `'static` / `lifetime<'a>` are **not** Ivy syntax.  Full table in section 4.1.

**Q3 - HIR dump of lifetimes.**
`  lifetimes: a b` (only when non-empty, declaration order, native names first),
`  return lifetime: X` (only when non-empty), ` [lt:X]` per parameter (only for a
parameter that has one; **no space** after `lt:`).  A reference parameter without a
lifetime prints no suffix; a lifetime on the return type produces the
`  return lifetime:` line only.

**Q4 - The `__ret` lowering.**
Full spec in section 6 (triggers, name format, dump shape, ordering) with 14 verbatim
dumps (6.5).

**Q5 - HIR-stage lifetime diagnostics.**
Ten distinct messages, exact wording + source location in section 2.2 / section 4.1.
A failing file prints **diagnostics only, no dump**, exit 1.  The exact intra-function
ordering is measured in section 6.7.

**Q6 - What the HIR builder tracks.**
Only `Param::lifetime`, `Function::lifetimes`, `Function::returnLifetime` - three items,
all visible in the dump.  `hir::Type` has no lifetime field; `dumpHirType` prints none.
The HIR builder computes **no** lifetime for expressions.

**Q7 - Oracle quirks.**
Section 8 (11 items, including the unconditional `Unknown` overwrite, the
`__ret`/return-type mismatch, the double-`<` template bug, the duplicated destructor, the
first-return-clears-the-scope bug, the em-dash mojibake, and the AST dump's lossiness).
**No crashing input was found.**

**Q8 - Recommendation.**
Section 10.  H4.5 = a `--hir` corpus of **16 core files** (23 with the optional extras)
covering the lifetime syntax, the lifetime lowering and the HIR-stage diagnostics.  The
borrow checker, the lifetime inference and the `__ret`/RAII machinery go to H5 (MIR) and
to a separate RAII milestone respectively.

---

## 12. Fixture index

All under `C:\Users\ASUS\AppData\Local\Temp\opencode\h45recon\v2\`.

| Path | Contents |
|------|----------|
| `stage/s01..s18*.ivy` | stage ownership: dangling return, lifetime mismatch, unknown lifetime, the three borrow conflicts, use-after-destroy, ref-to-temp, borrow-in-unsafe, borrow-scope-release, ptr-return |
| `syntax/x01..x44*.ivy` | the whole lifetime syntax surface, accepted and rejected, native and legacy, plus the precedence cases |
| `ret/r01..r22*.ivy` | the `__ret` lowering: non-trivial, plain local, name collision, widening, sibling scopes, nested scope, same-scope double return, ref return, void return, no dtor, struct by value, ptr return, literal, call, reference expression, for-scope, two dtors, struct expression, early return |
| `bug/b01..b09*.ivy` | malformed lifetime input (no crashes) |
| `order/o1..o3*.ivy` | multi-diagnostic ordering inside one function (section 6.7) |
| `TRANSCRIPT.txt` | every fixture x 5 modes, stdout + stderr + exit code (the master record) |
| `SYNTAX.txt`, `RET.txt`, `STAGE.txt`, `X3.txt`, `X4.txt`, `R1.txt`, `R2.txt`, `S1.txt` | the extracted `--ast` / `--hir` / `--mir` dumps |
| `GAP_hir.txt`, `GAP_ast.txt` | the oracle-vs-selfhost differential (26/98 identical in `--hir`, 52/95 in `--ast` before any port change) |
| `port/` | the freshly built selfhost modules + `astdrive.exe` + `hirdrive.exe` |
| `enc/emdash.ivy`, `enc/gammanai.ivy`, `enc/hyphen.ivy` | the em-dash encoding experiment (section 3.4) |
| `gen2.ps1`, `gen3.ps1`, `run.ps1`, `extract.ps1`, `gap.ps1`, `buildport.ps1`, `msgs2.ps1` | the scripts that produced all of the above |

---

# Phase 2 findings - lifetime syntax + AST retention

**Milestone:** H4.5 (Borrow / lifetime check), phase 2/5 (lifetime syntax + AST retention).
**Repo:** `D:\project\Ivy\ivyc`  (drive **D:**).
**HEAD at time of writing:** `17c68cf` (unchanged - nothing was committed).
**Files changed by this phase:** `selfhost/parsing/ast.ivy`,
`selfhost/parsing/parser.ivy`, `selfhost/hir/hir_builder.ivy` (initialisation only)
and this file.  `selfhost/parsing/astdrive.ivy` was **not** changed (the `--ast`
dump is byte-identical without any change to the dumper).

Phase 2 owns the *syntax* and the *AST retention* only.  The HIR lowering
(`lifetimes:` / `return lifetime:` / ` [lt:X]`), the ten HIR-stage lifetime
diagnostics and the borrow checker are **phase 3+** and are still missing.

---

## 0. Path check (done first)

```
PS> Get-Location
    Path
    ----
D:\project\Ivy\ivyc

PS> git rev-parse --show-toplevel
D:/project/Ivy/ivyc
```

Both point at the repository on drive **D:**.  All shell commands were run with
`workdir = D:\project\Ivy\ivyc`; no `cd` was used.  `C:` was used only for the
scratch fixtures under `C:\Users\ASUS\AppData\Local\Temp\opencode\h45f2\`.

---

## 1. The AST layout phase 3 can rely on

`selfhost/parsing/ast.ivy` gained four new records and three new fields per
function/parameter.  **The layout is final; phase 3 must not need a further
parser change.**

### 1.1 The new records

```c
// One lifetime name.  `name` is the lifetime name without the leading `$`
// (native `lifetime<$a>`) or without the attribute brackets (legacy
// `[[ivy::lt_def(a)]]`).
export struct ALifetime {
    const char* name;          // 8 bytes
};

// A list of lifetime names: a contiguous run of the parser's lifetime pool.
// `items` is NULL when the list is ABSENT, which is distinct from an EMPTY
// list (`items` non-NULL, `n == 0`).
export struct ALifetimeList {
    ALifetime* items;
    long n;                    // 16 bytes
};

// A legacy lifetime annotation: `[[ivy::lt(x)]]` on a parameter,
// `[[ivy::lt_ret(x)]]` or `[[ivy::lt_def(a, b)]]` on a function.
// `kind` is 1 for `lt`, 2 for `lt_ret` and 3 for `lt_def`.
// `args` are the annotation's arguments in source order; `line`/`col` locate
// the attribute (the oracle's `Attribute::loc`).
export struct ALegacyLifetime {
    ALifetimeList args;
    long kind;
    long line;
    long col;                  // 40 bytes
};

// A list of legacy lifetime annotations (a contiguous run of the parser's
// legacy-lifetime pool).  `items` is NULL when the declaration carries no
// such annotation at all.
export struct ALegacyLifetimeList {
    ALegacyLifetime* items;
    long n;                    // 16 bytes
}
```

### 1.2 The new fields

| Record | Field | Meaning |
|--------|-------|---------|
| `AParam` (128 bytes) | `ALifetimeList lt_names` | the **native** `$a` of the parameter.  `items == NULL` && `n == 0` => no native lifetime; `n == 1` => the name is `lt_names.items[0].name` (without the `$`). |
| `AParam` | `ALegacyLifetimeList legacy_lts` | the **`[[ivy::lt(x)]]`** annotations of the parameter, in attribute order; every record has `kind == 1`. |
| `AFunction` (200 bytes) | `ALifetimeList lt_names` | the **native** `lifetime<$a, $b>` declaration, in declaration order.  `items == NULL` => no `lifetime<...>` prefix at all; `items != NULL` && `n == 0` => `lifetime<>`. |
| `AFunction` | `ALifetimeList ret_lts` | the **native** `$a` of the return type (0 or 1 entries). |
| `AFunction` | `ALegacyLifetimeList legacy_lts` | the **`[[ivy::lt_ret(x)]]`** (`kind == 2`) and **`[[ivy::lt_def(a, b)]]`** (`kind == 3`) annotations, in attribute order. |

**"absent" vs "empty".**  Every lifetime list distinguishes the two: a NULL
`items` means *the annotation is not there*, a non-NULL `items` with `n == 0`
means *the annotation is there but carries no name* (e.g. `lifetime<>`).  The
port deliberately avoids a NULL `const char*` for a single name because Ivy
rejects a NULL pointer literal outside an `unsafe` block and because a
`const char*` cannot distinguish "" from "not set".

### 1.3 What phase 3 has to do with the data

The AST stores **both** the native and the legacy form, so phase 3 applies the
oracle's precedence itself (section 4.2 of the phase-1 notes):

* **declared list**: the native names first, then the legacy `lt_def` names
  (`lowerLifetimeAttributes`, `src/hir/hir_builder.cpp:220-238`);
* **return lifetime**: the **legacy `lt_ret` wins** over the native `$a`
  (`hir_builder.cpp:239-254` - the code, not the comment);
* **parameter lifetime**: the **native `$a` wins** over the legacy `lt`
  (`lowerParamAttribute`, `hir_builder.cpp:264-303`).

The legacy argument list is already split for phase 3: `AAttr.args` is a
*joined* string in the port (the oracle has a `std::vector`), so the parser
splits it on `", "` and stores the individual names in
`ALegacyLifetime.args`.  Phase 3 therefore does **not** have to re-parse the
attribute text - it only has to look at `args.n` for the arity errors.

The arity / duplicate / "not declared" checks stay in the HIR stage (phase 3):
`[[ivy::lt]] expects exactly one lifetime argument`,
`[[ivy::lt_ret]] expects exactly one lifetime argument`,
`[[ivy::lt]] requires a reference or pointer parameter`,
`[[ivy::lt(x)]]: lifetime 'x' is not declared via [[ivy::lt_def(...)]]`,
`duplicate lifetime 'x'`, `lifetime '$x' is not declared in lifetime<...>`,
`return lifetime '$x' is not declared in lifetime<...>`.

---

## 2. Pool-stride bookkeeping

`AParam` and `AFunction` grew, so every hard-coded element size had to be
re-derived (x86-64: pointer 8, `long` 8, `bool` 1, alignment 8).

### 2.1 The re-derivation

```
AType       = base(8) tpl_args(8) tpl_n(8) is_unsigned(1) is_const(1) is_ref(1)
              [pad 4] ptr_depth(8) array_size(8)                      = 48   (unchanged)
ALifetime   = name(8)                                               =  8   (new)
ALifetimeList     = items(8) n(8)                                   = 16   (new)
ALegacyLifetime   = args(16) kind(8) line(8) col(8)                 = 40   (new)
ALegacyLifetimeList = items(8) n(8)                                 = 16   (new)

AParam      = type(48) name(8) attrs(8) nattrs(8) line(8) col(8)
              default_value(8) lt_names(16) legacy_lts(16)           = 128  (was 96)
AFunction   = attrs(8) nattrs(8) ret(48) name(8) params(8) nparams(8)
              is_extern_c(8) body(8) line(8) col(8) is_constexpr(8)
              is_virtual(8) tpl_params(8) tpl_n(8) lt_names(16)
              ret_lts(16) legacy_lts(16)                             = 200  (was 152)

AStruct     = name(8) is_class(8) fields(8) nfields(8) methods(8) nmethods(8)
              bstart(8) nbases(8) line(8) col(8) tpl_params(8) tpl_n(8) = 96 (unchanged:
              it only holds an `AFunction*`, not an AFunction)
AField      = type(48) name(8) line(8) col(8)                       = 72  (unchanged)
```

### 2.2 Every stride that changed

| file:line | allocation | old | new |
|------------|-----------|-----|-----|
| `selfhost/parsing/parser.ivy:284` | `this.fpool_ = (AFunction*) __ivy_alloc(this.fn_cap_ * N)` | 152 | **200** |
| `selfhost/parsing/parser.ivy:286` | `this.ppool_ = (AParam*) __ivy_alloc(this.pp_cap_ * N)` | 96 | **128** |
| `selfhost/parsing/parser.ivy:308` | `this.mpool_ = (AFunction*) __ivy_alloc(this.m_cap_ * N)` | 152 | **200** |
| `selfhost/parsing/parser.ivy:314` | `this.ltpool_ = (ALifetime*) __ivy_alloc(this.lt_cap_ * N)` | - | **8** (new pool) |
| `selfhost/parsing/parser.ivy:315` | `this.llpool_ = (ALegacyLifetime*) __ivy_alloc(this.ll_cap_ * N)` | - | **40** (new pool) |

The size comment at the top of `parser.ivy` was updated accordingly
(`AAttr 40, AParam 128, AFunction 200, AExpr 168, AStruct 96,
ATemplateParam 96, AConcept 48, AType 48, ALifetime 8, ALegacyLifetime 40`).

### 2.3 `selfhost/hir/hir_builder.ivy` - NO stride change

The task asked for the `AFunction`/`AParam` pools in `hir_builder.ivy` to be
checked: **there is no such pool**.  `hir_builder.ivy` allocates

* `AStruct*` pools (`__ivy_alloc(this.spc_cap_ * 96)`, line 390) and
* `AField*` pools (`__ivy_alloc(nf * 72)`, line 6338),

and it only ever *copies* `AFunction`/`AParam` records by value
(`AFunction mf = sd.methods[j];`).  `AStruct` holds an `AFunction*` (a
pointer), so its size does not depend on `sizeof(AFunction)`; `AField` does not
contain an `AParam` either.  **No stride constant in `hir_builder.ivy` had to
change**, and the file was touched only to initialise the two new `AParam`
fields in `get_param()` (the only place that creates an `AParam` that is not
immediately overwritten).

---

## 3. The two pre-existing parser bugs (both fixed)

### 3.1 P7 - `parse_fn_trailing` leaked the parameter attributes into the
function-level attribute allow-list

**Before.**  `parse_fn_trailing` captured `a_end = this.ap_n_` *after*
`parse_params()` had appended the parameter attributes to the shared attribute
pool, so the function's attribute range `[a_start, a_end)` also contained the
**parameter** attributes.  The function-level allow-list is
`{"lt_def", "lt_ret"}`, so a parameter attribute was rejected:

```
$ ivy --ast x11_legacy_all.ivy          # the oracle
function [[ivy::lt_def(a)]][[ivy::lt_ret(a)]]
  return const int32_t f(const int32_t x [[ivy::lt(a)]])
  block
    return
      ident x
$ astdrive x11_legacy_all.ivy            # the port, before this phase
x11_legacy_all.ivy:3:21: error: unknown Ivy attribute 'ivy::lt' in this position
```

**After.**  The function's attribute list is now built explicitly: the
caller's attributes are copied first, then the attributes that follow the
return type, so the list is exactly *caller + mid* - the same list the oracle
validates (`src/parsing/parser.cpp:2118-2122`).  The parameter attributes stay
in their own list and are validated with the parameter allow-list
(`{"lt"}`).  The port now prints the oracle's dump byte for byte and accepts
`fn f(const int32& x [[ivy::lt(a)]]) -> const int32& { ... }`.

The same fix also repaired the *AST dump* of the function attributes: before,
`f.attrs`/`f.nattrs` also spanned the parameter attributes.

### 3.2 P3 - `parse_lifetime_decl` was missing the `else break;`

**Before.**

```
$ ivy --ast x15_lt_unterminated.ivy      # the oracle
x15_lt_unterminated.ivy:2:1: error: expected '>' to close lifetime<...>
x15_lt_unterminated.ivy:2:4: error: expected a function declaration after lifetime<...>
x15_lt_unterminated.ivy:4:1: error: expected a type (void, bool, int8, ...)
$ astdrive x15_lt_unterminated.ivy        # the port, before this phase
x15_lt_unterminated.ivy:2:1: error: expected a lifetime variable ($name) in lifetime<...>
x15_lt_unterminated.ivy:4:1: error: expected a type (void, bool, ...)
```

**After.**  `parse_lifetime_decl` now breaks out of the loop when the next
token is not a comma (the oracle's `src/parsing/parser.cpp:622-626`), so the
diagnostic is `expected '>' to close lifetime<...>`.

Fixing only the `break` was **not** enough to match the oracle: the port also
lacked the oracle's *dispatch after `lifetime<...>`*
(`src/parsing/parser.cpp:752-841`), so it never produced
`expected a function declaration after lifetime<...>`.  The dispatch was
therefore ported as well (it is a parser diagnostic, not an HIR one):

| after `lifetime<...>` | oracle | port (phase 2) |
|-----------------------|--------|----------------|
| `constexpr`/`consteval` then `fn` | trailing form | same |
| `constexpr`/`consteval` then a type | `expected a function after lifetime<...> + constexpr` when the next token is not a type | same |
| `fn` | trailing form | same |
| `[[attrs]]` then `fn` | trailing form, the attributes are merged | same (the attributes before and after `fn` are merged into one list) |
| `[[attrs]]` then a type | leading form | same |
| anything else | `expected a function declaration after lifetime<...>` | same |

With that, `x05`, `x08`, `x09`, `x14`, `x15`, `x28`, `b05` and `b08` match the
oracle exactly.

---

## 4. The other two parser fixes that were needed for the same goal

### 4.1 `attribute must follow the parameter name`

The oracle rejects an attribute on an *unnamed* parameter
(`src/parsing/parser.cpp:2237-2241`); the port accepted it silently.  The rule
is now implemented (`parser.ivy`, `parse_params`):

```
$ ivy --ast attrname.ivy       # oracle and port, identical
attrname.ivy:1:19: error: attribute must follow the parameter name
```

### 4.2 `Function::loc` now matches the oracle

The oracle's `Function::loc` is the position of the **first token of the
declaration** (the leading attribute, the `fn` keyword, the `template` /
`export` keyword of the enclosing declaration, or the return type).  The port
used the *function name* token for the trailing form, so every
function-level HIR diagnostic of a `fn f(...)` declaration was off by a few
columns.  `parse_function` / `parse_fn_trailing` now receive the declaration
position (`dline`/`dcol`) from the dispatch.  Verified:

```
$ ivy --hir loc1.ivy     # fn f() -> int32 { int32 a = 1; }
loc1.ivy:1:1: error: function 'f' may reach the end without returning a value
```

(the port reported `1:4` before this phase).  Phase 3 needs this: the
`return lifetime '$x' is not declared in lifetime<...>` diagnostic is reported
at `af.loc` (e.g. `x18`/`x42` expect column 1, the `fn` keyword).

---

## 5. The legacy lifetime attributes: acceptance / rejection matrix

The port's attribute allow-lists are now exactly the oracle's:

| attribute | position | oracle | port (phase 2) |
|-----------|----------|--------|----------------|
| `[[ivy::lt_def(a)]]` | function | accepted | accepted |
| `[[ivy::lt_def(a, b)]]` | function | accepted (declares `a` and `b`) | accepted |
| `[[ivy::lt_ret(a)]]` | function | accepted | accepted |
| `[[ivy::lt(a)]]` | function | **rejected**: `unknown Ivy attribute 'ivy::lt' in this position` | rejected, same text and position |
| `[[ivy::lt_def(a)]]` | parameter | **rejected**: `unknown Ivy attribute 'ivy::lt_def' in this position` | rejected, same text and position |
| `[[ivy::lt(a)]]` | parameter, after the name | accepted | accepted |
| `[[ivy::lt(a)]]` | parameter, before the name | **rejected**: `attribute must follow the parameter name` | rejected (new in this phase) |
| `[[ivy::lt]]` / `[[ivy::lt()]]` | parameter | accepted by the parser; the HIR stage reports `[[ivy::lt]] expects exactly one lifetime argument` | accepted by the parser (HIR = phase 3) |
| `[[ivy::lt(a, b)]]` | parameter | accepted by the parser; the HIR stage reports the arity error | accepted by the parser (HIR = phase 3) |
| `[[ivy::lt_ret]]` / `[[ivy::lt_ret(a, b)]]` | function | accepted by the parser; the HIR stage reports the arity error | accepted by the parser (HIR = phase 3) |
| `[[ivy::lt(...)]]` | statement | **rejected**: `unknown Ivy attribute 'ivy::lt' in this position` | rejected (the statement allow-list is empty) |

The native syntax matrix:

| syntax | oracle | port (phase 2) |
|--------|--------|----------------|
| `lifetime<$a>` / `lifetime<$a, $b>` | accepted, the names are declared | accepted, the names are kept in `AFunction.lt_names` |
| `lifetime<>` | accepted, no lifetime | accepted, `lt_names.items != NULL` && `n == 0` |
| `lifetime<$a, $a>` | parser error `duplicate lifetime '$a'` at the token **after** the duplicate | same (`x12` expects 1:16) |
| `lifetime<a>` | `expected a lifetime variable ($name) in lifetime<...>` | same |
| `lifetime<$a` (unterminated) | `expected '>' to close lifetime<...>` | same (the `else break;` fix) |
| `lifetime<$a> struct S` / `enum E` / EOF | `expected a function declaration after lifetime<...>` | same (the dispatch fix) |
| `lifetime<$a> constexpr fn f()` | accepted | accepted |
| `const int32& $a x` (parameter) | accepted | accepted, kept in `AParam.lt_names` |
| `int32 $a x` (by-value parameter) | accepted by the parser; the HIR stage reports `lifetime annotation requires a reference or pointer parameter` | accepted by the parser (HIR = phase 3) |
| `-> const int32& $a` / `const int32& $a f()` (return) | accepted | accepted, kept in `AFunction.ret_lts` |

---

## 6. The `--ast` dump is unchanged

`selfhost/parsing/astdrive.ivy` was **not** modified: the new fields are not
printed, exactly like the oracle (which is lossy about lifetimes, section 8.8
of the phase-1 notes).

**Method.**  Every fixture of the recon corpus
(`C:\Users\ASUS\AppData\Local\Temp\opencode\h45recon\v2\`, 101 `.ivy` files)
was run through `ivy --ast <file>` and through the freshly built `astdrive.exe`
and the *combined stdout + stderr* of the two was compared (the same method as
the recon's `gap.ps1`; the driver's `=== path ===` marker line is stripped and
the C++ `std::filesystem` path rendering - which escapes the backslashes of an
absolute path - is normalised).

| | before phase 2 | after phase 2 |
|---|---|---|
| `--ast`, 101 fixtures | 52 identical / 49 different | **63 identical / 38 different** |

The 38 remaining differences are **not** lifetime-syntax differences:

| # | kind | fixtures | owner |
|---|------|----------|-------|
| 6 | the lexer diagnostic prints the path with unescaped backslashes (`lexer.ivy`, not in this phase's scope; the message, the line and the column are identical - with a *relative* path the outputs are byte-identical) | `b01`, `b03`, `b04`, `b09`, `x07`, `x08` | pre-existing (lexer) |
| 1 | the oracle's own bug: `lifetime<$a> template<...> fn` consumes the `<` twice (`src/parsing/parser.cpp:789` + `parseTemplateParams`) and reports three errors; the port reports `expected a function declaration after lifetime<...>` instead.  **Deliberately not reproduced** (the phase-1 notes say: do not put `lifetime<...>` on a template function in the corpus) | `x10` | deliberate |
| 27 | the HIR/MIR-stage lifetime diagnostics (the `--ast` mode of the oracle runs the *whole* pipeline, so the HIR diagnostics are printed after the AST dump) | `x16`-`x22`, `x27`, `x29`, `x30`, `x33`-`x36`, `x39`, `x42`, `x44`, `o1`-`o3`, `s2`, `s4`-`s7`, `s11`, `s13`, `s15`, `s16`, `r12` | **phase 3+** |
| 4 | the MIR-stage borrow diagnostics | `s1`, `s4`, `s5`, `s6` | phase 5 (MIR) |

For every lifetime fixture the **AST dump itself** is now byte-identical to
the oracle, including the legacy attributes:

```
$ ivy --ast x11_legacy_all.ivy        # oracle == port
function [[ivy::lt_def(a)]][[ivy::lt_ret(a)]]
  return const int32_t f(const int32_t x [[ivy::lt(a)]])
  block
    return
      ident x
```

`selfhost/corpus/parser/decl/11_lifetime.ivy` is part of the `decl` suite and
passes.

The `--hir` path is of course still different for the lifetime fixtures
(33 identical / 68 different): the port does not lower the lifetimes into the
HIR yet.  That is phase 3.

---

## 7. Verification (all run from `D:\project\Ivy\ivyc`)

Parser suites (`-Kinds all -Oracle --ast -Modules token,lexer,preprocessor,ast,parser
-Driver astdrive`):

| suite | files | result |
|-------|-------|--------|
| `selfhost/corpus/parser/decl` | 15 | PASS (identical) |
| `selfhost/corpus/parser/expr` | 13 | PASS |
| `selfhost/corpus/parser/stmt` | 15 | PASS |
| `selfhost/corpus/parser/aggregate` | 14 | PASS |
| `selfhost/corpus/parser/template` | 15 | PASS |
| `selfhost/corpus/parser/module` | 16 | PASS |
| `selfhost/corpus/parser/err` | 19 | PASS |

HIR suites (`-Kinds all -Oracle --hir -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder
-ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive`):

| suite | files | result |
|-------|-------|--------|
| `selfhost/corpus/hir/name` | 27 | PASS (identical) |
| `selfhost/corpus/hir/type` | 120 | PASS |
| `selfhost/corpus/hir/constexpr` | 70 | PASS |
| `selfhost/corpus/hir/template` | 99 | PASS |

Lexer suites (`-Oracle --lex -Modules token,lexer -Driver lexdrive`): `ident` 10,
`num` 8, `str` 7, `op` 5, `trivia` 7, `err` 9 - all PASS.

Preprocessor suites (`-Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive`):
`define_obj` 10, `define_fn` 12, `cond` 16, `expand` 16, `predef` 20, `line` 22,
`no_include` 9 - all PASS.

---

## 8. What phase 3 has to do

1. **Port `lowerLifetimeAttributes` / `lowerParamAttribute`**
   (`src/hir/hir_builder.cpp:217-305`) into `hir_builder.ivy` and add the
   lifetime fields to `HFunction` / `HParam` (`selfhost/hir/hir.ivy` - not
   touched by this phase).
2. Apply the precedence with the data of section 1.3:
   * declared list = native names first, then the legacy `lt_def` names;
   * the **legacy `lt_ret` wins** over the native return lifetime;
   * the **native `$a` wins** over the legacy `lt` on a parameter.
3. Emit the ten HIR-stage diagnostics with the exact wording of section 2.2
   of the phase-1 notes and the right source locations:
   * `af.loc` for the function-level ones - the port's `AFunction.line/col`
     now matches the oracle's `Function::loc` (section 4.2);
   * `ap.loc` for the parameter-level ones - the port's `AParam.line/col` is
     the parameter's type token, like the oracle's;
   * the attribute's own position for the legacy ones - available in
     `ALegacyLifetime.line/col`.
4. Print `  lifetimes: a b`, `  return lifetime: X` and the ` [lt:X]` parameter
   suffix in `hirdrive.ivy` (the dump format is in section 5 of the phase-1
   notes: **no space** after `lt:`).
5. Remember the em dash of `function 'f' returns a pointer but has no return
   lifetime - use the real U+2014 character in the `.ivy` source (section 3.4
   of the phase-1 notes).

## 9. Divergences from the phase-1 notes

| # | divergence | note |
|---|------------|------|
| 1 | The phase-1 notes list P1/P2/P5/P6 ("the port discards the lifetime names") as *gaps*.  They are now **implemented** (the AST retains the names), but the HIR still does not use them - that is phase 3. |
| 2 | P4 ("the post-`lifetime<...>` dispatch is missing") is now **implemented** except for the `template` branch, which is the oracle's own double-`<` bug (section 6). |
| 3 | P7 is **fixed** (section 3.1). |
| 4 | The phase-1 notes did not mention that the port's `Function::loc` differed from the oracle's for the `fn` trailing form; it is now aligned (section 4.2). |
| 5 | The phase-1 notes listed `[[ivy::lt(a)]]` on an unnamed parameter as "UNKNOWN - not probed".  The oracle rejects it with `attribute must follow the parameter name`; the port now does the same (section 4.1). |
| 6 | `AAttr.args` is a *joined* string in the port, not a `std::vector` like in the oracle.  The parser splits it for the lifetime attributes; a *non-lifetime* attribute with a `", "` inside a string argument would still be mis-split - irrelevant for H4.5. |
| 7 | The port's `parse_attrs` accepts any token as an attribute argument, the oracle only Identifier/Integer/Float/String/Keyword (`expected attribute argument`).  Pre-existing, irrelevant for the lifetime attributes. |

---

# Phase 3 findings - lifetime lowering into the HIR

**Milestone:** H4.5 (Borrow / lifetime check), phase 3/5 (lifetime lowering into the
HIR + the HIR-stage lifetime diagnostics).
**Repo:** `D:\project\Ivy\ivyc`  (drive **D:**).
**HEAD at time of writing:** `17c68cf` (unchanged - nothing was committed).
**Files changed by this phase:** `selfhost/hir/hir.ivy`, `selfhost/hir/hir_builder.ivy`,
`selfhost/hir/hirdrive.ivy` and this file.  `selfhost/parsing/**` was **not**
touched (phase 2 owns it).

Phase 3 owns the *lowering* of the lifetime annotations into the HIR and the ten
HIR-stage lifetime diagnostics.  The MIR-stage borrow checker, the lifetime
inference and the `__ret`/RAII machinery are **not** part of this phase (they
belong to H5 / a separate RAII milestone).

---

## 0. Path check (done first)

```
PS> Get-Location
    Path
    ----
D:\project\Ivy\ivyc

PS> git rev-parse --show-toplevel
D:/project/Ivy/ivyc
```

Both point at the repository on drive **D:**.  All shell commands were run with
`workdir = D:\project\Ivy\ivyc`; no `cd` was used.  `C:` was used only for the
scratch fixtures under `C:\Users\ASUS\AppData\Local\Temp\opencode\h45f3\`.

---

## 1. The lowering algorithm

The port mirrors `lowerLifetimeAttributes` (`src/hir/hir_builder.cpp:217`) and
`lowerParamAttribute` (`:264`) statement by statement.

### 1.1 The declared lifetime list of a function

`HirBuilder::lower_lifetime_attributes(AFunction af)`
(`selfhost/hir/hir_builder.ivy:2603`):

1. the **native** `lifetime<$a, $b>` names, in declaration order; a name that is
   already in the list is a duplicate (`duplicate lifetime '$a'`, at the
   function's declaration location - dead code, the parser rejects the duplicate
   first);
2. the **legacy** `[[ivy::lt_def(a, b)]]` names, in attribute order; a name that
   is already in the list is a duplicate (`duplicate lifetime 'a'`, at the
   *attribute's* location);
3. the **legacy** `[[ivy::lt_ret(x)]]` attributes: an attribute with a number of
   arguments other than one is an error (`[[ivy::lt_ret]] expects exactly one
   lifetime argument`, at the attribute's location) and is skipped; otherwise it
   sets the return lifetime **only if the return lifetime is still unset**, so
   the **first** `lt_ret` wins;
4. the **native** return lifetime (`-> T $a`) is used only when no legacy
   `lt_ret` set one - i.e. the **legacy `lt_ret` beats the native `$a`** (the
   code of `hir_builder.cpp:239-254`, not the comment above it);
5. the return lifetime must name a declared lifetime, otherwise
   `return lifetime '$b' is not declared in lifetime<...>` at the function's
   declaration location.

The result is stored in the HIR function record as a run of names in the
`HUnit.ltnames` pool (`lt_start`, `lt_n`).

### 1.2 The lifetime of a parameter

`HirBuilder::lower_param_attribute(long lt_start, long lt_n, AParam ap)`
(`selfhost/hir/hir_builder.ivy:2673`):

1. the **native** `$a` wins over the legacy form (the oracle short-circuits the
   legacy path);
   * not declared -> `lifetime '$b' is not declared in lifetime<...>` at the
     **parameter's type token**, and the parameter gets no lifetime;
   * declared but the parameter is neither a reference nor a pointer ->
     `lifetime annotation requires a reference or pointer parameter` at the
     **parameter's type token**, and the parameter gets no lifetime;
2. otherwise the **legacy** `[[ivy::lt(x)]]` attributes, in attribute order:
   * an attribute with a number of arguments other than one ->
     `[[ivy::lt]] expects exactly one lifetime argument` at the **attribute's
     own location**, and the next attribute is tried;
   * the name is not declared -> `[[ivy::lt(b)]]: lifetime 'b' is not declared
     via [[ivy::lt_def(...)]]` at the **attribute's own location**, and the next
     attribute is tried;
   * the parameter is neither a reference nor a pointer -> `[[ivy::lt]]
     requires a reference or pointer parameter` at the **attribute's own
     location**, and the next attribute is tried;
   * otherwise the attribute provides the lifetime and the loop stops.

The check uses the **AST** type of the parameter (`ptr_depth`, `is_ref`), exactly
like the oracle (`lowerParamAttribute` receives the AST `Param`, not the
resolved HIR type).

### 1.3 Precedence summary

| rule | result | measured fixture |
|------|--------|------------------|
| declared list | native names first, then the legacy `lt_def` names | `p01`, `p32` |
| return lifetime | the **legacy `lt_ret`** wins over the native `$a`; the **first** `lt_ret` wins | `p02`, `p36` |
| parameter lifetime | the **native `$a`** wins over the legacy `lt`; the **first** legacy `lt` wins | `p03`, `p37` |
| duplicate | a native `$a` + a legacy `lt_def(a)` is a duplicate -> `duplicate lifetime 'a'` (legacy wording, at the attribute) | `p09`, `x30` |

### 1.4 Methods and template specializations

* A **method** (a function inside a struct) has **no** declared lifetime list
  (`buildStruct` in the oracle never calls `lowerLifetimeAttributes`), so a
  parameter lifetime of a method is always undeclared:
  `lifetime '$a' is not declared in lifetime<...>` (fixture `p33`, a
  constructor).  The port implements this by calling `lower_param_attribute`
  with an empty list.
* A **template specialization** is built without any lifetime
  (`instantiateTemplate` in the oracle does not lower the lifetimes either), so
  a specialization never carries a lifetime and never prints a `lifetimes:`
  line.
* A **function template** is not lowered at all (the oracle returns early in
  `buildSignature`).

### 1.5 "absent" vs "present but unnamed"

The distinction survives the whole pipeline:

* the AST keeps it (`ALifetimeList.items == NULL` = absent,
  `items != NULL && n == 0` = present but unnamed - `lifetime<>`);
* the HIR keeps it as well: `HFunction.lt_start == -1` means *no lifetime
  declaration at all*, `lt_start >= 0 && lt_n == 0` means *a declaration that
  carries no name*, and `lt_n > 0` is the normal case.

The **dump** follows the oracle exactly: the `  lifetimes:` line is printed only
when the list is **non-empty** (`f.lt_n > 0`), so `lifetime<>` prints nothing -
the same as the oracle's `if (!fn->lifetimes.empty())`.

---

## 2. The HIR model

`selfhost/hir/hir.ivy`:

```c
export struct HParam {
    AType type;
    const char* name;
    long line;
    long col;
    long default_value;   // AST expression index of the default value (-1 none)
    const char* lt;       // the parameter's lifetime name (NULL when none)
};

export struct HFunction {
    const char* name;
    AType ret;
    long is_extern_c;
    long body;
    long pstart;
    long pn;
    long is_constexpr;
    long lt_start;              // first lifetime name index (-1 when absent)
    long lt_n;                  // number of declared lifetime names
    const char* return_lifetime;// the return lifetime name (NULL when none)
};
```

and `HUnit` gained the pool the lifetime names live in:

```c
    long* ltnames;   // (long) const char* - the names of every function
    long lt_n;
```

* `lifetime` is a **keyword** in Ivy, so the parameter field cannot be called
  `lifetime`; it is called `lt`.
* The names themselves are owned by the parser (they are interned there), so
  the pool only holds their addresses - the same idiom the port already uses
  for the symbol pool (`snm_`), the diagnostic messages (`dmsg_`) and the
  template names (`tpl_name_`).

### 2.1 The re-derived sizes (x86-64: pointer 8, `long` 8, align 8)

```
HParam    = type(48) name(8) line(8) col(8) default_value(8) lt(8)  =  88  (was 80)
HFunction = name(8) ret(48) is_extern_c(8) body(8) pstart(8) pn(8)
            is_constexpr(8) lt_start(8) lt_n(8) return_lifetime(8)   = 120  (was 96)
HUnit     = ... + ltnames(8) lt_n(8)                                = +16
```

`HUnit` is a value (it is copied, not pooled), so it has no stride.

### 2.2 Every stride that changed

| file:line | allocation | old | new |
|------------|-----------|-----|-----|
| `selfhost/hir/hir_builder.ivy:265` | `this.u.fns = (HFunction*) __ivy_alloc(this.u.fn_cap * N)` | 96 | **120** |
| `selfhost/hir/hir_builder.ivy:274` | `this.u.params = (HParam*) __ivy_alloc(this.u.p_cap * N)` | 80 | **88** |

No other stride changed:

* `selfhost/parsing/parser.ivy` - untouched by this phase (phase 2 already
  re-derived the `AParam`/`AFunction` strides);
* `selfhost/hir/hir_builder.ivy` has no `AFunction`/`AParam` pool: it copies
  the records by value (`AFunction mf = sd.methods[j];`), and the `AStruct`
  (96) and `AField` (72) pools hold no `AFunction`/`AParam` by value;
* the new pools are `ltnm_` (`long*`, 8 bytes per entry) and `pltn_` (`long*`,
  8 bytes per entry).

---

## 3. The dump format

`selfhost/hir/hirdrive.ivy`, `HDumper::emit` (line 372).  The line order is the
oracle's `dumpHir` (`src/app/main.cpp:577`):

```
function <name>[ [extern "C"]]
  return <type>(<paramtype> <paramname>[ [lt:X]], ...)
  lifetimes: <lt1> <lt2> ...        <- only when the list is non-empty
  return lifetime: <X>             <- only when the function has one
  block
    ...
```

* `  lifetimes:` - the names are space separated, in declaration order (native
  first, then the legacy `lt_def` names).
* `  return lifetime: X` - a single name, printed only when the function has
  one.
* ` [lt:X]` - the suffix comes immediately after the parameter name and there is
  **no space after `lt:`** (the MIR dump uses ` [lt: X]`, with a space - a
  different dumper).
* A reference parameter without a lifetime prints no suffix at all.
* The `lifetimes:` and `return lifetime:` lines come **after** the
  `return <type>(<params>)` line and **before** the body.

---

## 4. The HIR-stage lifetime diagnostics

The ten messages, their exact text and the location rule:

| # | message | location |
|---|---------|----------|
| 1 | `lifetime '$b' is not declared in lifetime<...>` | the **parameter's type token** |
| 2 | `return lifetime '$b' is not declared in lifetime<...>` | the **function declaration start token** |
| 3 | `lifetime annotation requires a reference or pointer parameter` | the **parameter's type token** |
| 4 | `[[ivy::lt(b)]]: lifetime 'b' is not declared via [[ivy::lt_def(...)]]` | the **attribute's own location** |
| 5 | `[[ivy::lt]] expects exactly one lifetime argument` | the **attribute's own location** |
| 6 | `[[ivy::lt_ret]] expects exactly one lifetime argument` | the **attribute's own location** |
| 7 | `[[ivy::lt]] requires a reference or pointer parameter` | the **attribute's own location** |
| 8 | `duplicate lifetime 'a'` (the legacy `lt_def`) | the **attribute's own location** |
| 9 | `duplicate lifetime '$a'` (the native declaration) | the **function declaration start token** |
| 10 | ``function 'f' returns a pointer but has no return lifetime — use `lifetime<$a>` and annotate the return type with $a`` | the **function declaration start token** |

* The **function declaration start token** is the `fn` keyword of the trailing
  form, the return type of the C-style leading form, the first `[[...]]` of a
  `lifetime<...> [[...]] fn` declaration, ...  Phase 2 aligned the port's
  `AFunction.line/col` with the oracle's `Function::loc` (section 4.2 of the
  phase-2 notes), so phase 3 could use it directly.
* The **parameter's type token** is the first token of the parameter's type.
* The **attribute's own location** is the position of the `[[` of the
  attribute.
* Message 9 is dead code (the parser rejects a duplicate native name first,
  with `duplicate lifetime '$a'`); it is implemented anyway so that the port
  mirrors the oracle's code.
* Message 10 uses an **em dash** (see section 6).

### 4.1 The diagnostic order inside one function

The port reproduces the oracle's order exactly
(`buildSignature` in `src/hir/hir_builder.cpp:821-900`):

```
1. duplicate lifetime 'a'                (the legacy lt_def, in lowerLifetimeAttributes)
2. [[ivy::lt_ret]] expects exactly one lifetime argument
3. return lifetime '$X' is not declared in lifetime<...>
4. return type 'nullptr_t' requires an unsafe block
5. raw pointer return type requires an unsafe block
6. parameter errors, in SOURCE ORDER, per parameter:
     lifetime '$X' is not declared in lifetime<...>
     lifetime annotation requires a reference or pointer parameter
     [[ivy::lt]] expects exactly one lifetime argument
     [[ivy::lt(x)]]: lifetime 'x' is not declared via [[ivy::lt_def(...)]]
     [[ivy::lt]] requires a reference or pointer parameter
     parameter type 'nullptr_t' requires an unsafe block
     raw pointer parameter type requires an unsafe block
7. function 'f' returns a pointer but has no return lifetime ...
8. body errors
```

Items 4, 5 and the last two of 6 are the oracle's **`unsafe`** rules, not
lifetime rules, but they are interleaved with the lifetime diagnostics, so
they had to be implemented as well to reproduce the order.  Two of them
(`raw pointer parameter type requires an unsafe block`) already existed in the
port; the three others are new.

The port resolves the **types** of all the parameters before it lowers the
lifetimes (a template-id type instantiates its specialization, whose methods
are added to the function pool, and the parameters of a function must stay
contiguous).  To keep the oracle's order, the lifetime lowering and the
`unsafe` checks of a parameter are done **in the resolution loop**, right after
the type of that parameter was resolved; the parameters are pushed afterwards.
The result is the same order as the oracle's, even when a parameter's type
resolution and a lifetime error occur in the same function (fixture `p41`).

---

## 5. The `__ret` lowering and the MIR stage are still missing

Not part of this phase.  The recon fixtures `ret/r01` ... `ret/r22` still
differ (the `__ret` temp needs the whole RAII/destructor machinery plus struct
construction), and the MIR-stage fixtures `stage/s1`, `s4`, `s5`, `s6`, `s7`,
`s11`, `s15` still differ (the port has no MIR stage).  `syntax/x27` and my
`p02` differ **only** in the MIR diagnostic; their HIR dumps are identical.

---

## 6. The em dash: the byte sequence is `E2 80 94`, not `CE 93 C3 87 C3 B6`

**This contradicts section 3.4 of the phase-1 notes.**  The notes measured the
mojibake `CE 93 C3 87 C3 B6` and concluded that the oracle's *diagnostics*
print the mojibake.  The measurement was taken through a **PowerShell text
capture**, which decodes the raw bytes with the console code page (CP437) and
re-encodes them as UTF-8: the raw bytes `E2 80 94` are decoded as CP437 into
`U+0253 U+00C7 U+00F6` ("`ΓÇö`") and re-encoded as `CE 93 C3 87 C3 B6`.

The **raw** bytes of the oracle's stderr are the plain UTF-8 em dash:

```
PS> ivy --hir syntax\x22_ptr_ret_no_lt.ivy 2> e.txt
PS> (byte dump of e.txt)
... 20 6E 6F 20 72 65 74 75 72 6E 20 6C 69 66 65 74 69 6D 65 20 E2 80 94 20 75 73 65 ...
```

The same is true for the recon's `enc/` experiment: `enc/emdash.exe` prints
`41 CE 93 C3 87 C3 B6 42`, but that is the **`io::println` path** of a *user
program* (the runtime re-encodes the string with the console code page), not
the diagnostic path.  The oracle's diagnostic printer writes the raw bytes.

The port therefore writes the **real em dash** (U+2014, UTF-8, no BOM) into the
`.ivy` source, exactly as the phase-1 notes recommended, and the result is
byte-identical to the oracle:

```
PS> oracle stderr  vs  port stdout (the driver's marker line stripped)
     ... 20 E2 80 94 20 75 73 65 20 60 6C 69 66 65 74 69 6D 65 3C 24 61 3E 60 ...
BYTE-IDENTICAL: True
```

(524 bytes compared byte by byte for `syntax/x22_ptr_ret_no_lt.ivy`.)

The em dash is at `selfhost/hir/hir_builder.ivy:2592`
(`err_ptr_ret_no_lt`):

```c
        this.db_put("' returns a pointer but has no return lifetime — ");
```

The file is UTF-8 without a BOM; the `.ivy` -> `.cpp` -> `.obj` path keeps the
bytes (verified: the generated `.i` and the `.obj` both contain `E2 80 94`).

---

## 7. The fixtures that were validated

### 7.1 The recon corpus (101 `.ivy` files)

`C:\Users\ASUS\AppData\Local\Temp\opencode\h45recon\v2\` - the oracle's
`--hir` output and the port's output were compared (stdout + stderr, CRLF
normalised, the driver's `=== path ===` marker stripped):

| | before phase 3 | after phase 3 |
|---|---|---|
| `--hir`, 101 fixtures | **33 identical / 68 different** | **67 identical / 34 different** |

**34 of the 68** probes that used to differ now match, and they are exactly the
HIR-stage lifetime ones:

* `bug/b07_lt_deep_nesting.ivy`
* `order/o1.ivy`, `order/o2.ivy`, `order/o3.ivy`
* `stage/s2_ptr_ret_no_lt.ivy`
* `syntax/x01, x02, x04, x05, x06, x11, x16, x17, x18, x19, x20, x21, x22,
  x23, x24, x30, x31, x32, x33, x34, x35, x36, x37, x38, x39, x40, x41, x42,
  x44` (29 files)

The 34 that still differ are **not** HIR-stage lifetime problems:

| kind | files | owner |
|------|-------|-------|
| the `__ret` temp | `ret/r01` ... `ret/r22` (19) | a separate RAII milestone |
| MIR-stage borrow diagnostics | `stage/s1, s4, s5, s6, s7, s11, s15` (7) | H5 (MIR) |
| the lexer prints the path quoted and escaped | `bug/b01, b03, b04, b09`, `syntax/x07, x08` (6) | pre-existing (lexer) |
| the oracle's own double-`<` bug (`lifetime<...> template<...>`) | `syntax/x10` (1) | deliberately not reproduced |
| the MIR diagnostic of the legacy return lifetime | `syntax/x27` (1) | H5 (MIR) - the **HIR dump is identical** |

### 7.2 The new fixtures of this phase

`C:\Users\ASUS\AppData\Local\Temp\opencode\h45f3\fix\` - 43 files, all
byte-identical to the oracle except `p02` (the MIR diagnostic only):

| file | rule it pins down | result |
|------|-------------------|--------|
| `p01_native_and_legacy_list.ivy` | the declared list: native first, then the legacy `lt_def` | `lifetimes: a b` |
| `p02_legacy_ret_wins.ivy` | the legacy `lt_ret` beats the native `$a` | `return lifetime: b` (the MIR error is out of scope) |
| `p03_native_param_wins.ivy` | the native `$a` beats the legacy `lt` | `x [lt:a]` |
| `p04_empty_lt_list.ivy` | `lifetime<>` | no `lifetimes:` line |
| `p05_empty_lt_list_with_param.ivy` | `lifetime<>` + a param `$a` | `lifetime '$a' is not declared in lifetime<...>` |
| `p06_lt_on_byvalue_param.ivy` | a lifetime on a by-value parameter | `lifetime annotation requires a reference or pointer parameter` |
| `p07_lt_no_params.ivy` | a lifetime on a function without parameters | `lifetimes: a` + `return lifetime: a` |
| `p08_multi_diag.ivy` | three diagnostics in one function | the oracle's order |
| `p09_dup_native_legacy.ivy` | a native `$a` + a legacy `lt_def(a)` | `duplicate lifetime 'a'` |
| `p10_method_param_lt.ivy` | a native param lifetime in a method | a parser error (identical) |
| `p11_decl_only.ivy` | a declaration-only function with lifetimes | the dump + `(declaration only)` |
| `p12_ptr_ret_with_lt.ivy` | a pointer return with a return lifetime | only the `unsafe` errors |
| `p13_legacy_all_forms.ivy` | `lt_def(a, b)` + `lt_ret(a)` + two `lt` params | `lifetimes: a b` |
| `p14_legacy_lt_arity2.ivy` | `[[ivy::lt(a, b)]]` | `[[ivy::lt]] expects exactly one lifetime argument` |
| `p15_legacy_lt_arity0.ivy` | `[[ivy::lt]]` | `[[ivy::lt]] expects exactly one lifetime argument` |
| `p16_legacy_lt_ret_arity2.ivy` | `[[ivy::lt_ret(a, b)]]` | `[[ivy::lt_ret]] expects exactly one lifetime argument` |
| `p17_legacy_lt_on_byvalue.ivy` | `[[ivy::lt(a)]]` on a by-value parameter | `[[ivy::lt]] requires a reference or pointer parameter` |
| `p18_legacy_undeclared.ivy` | `[[ivy::lt(b)]]` with `b` undeclared | `[[ivy::lt(b)]]: lifetime 'b' is not declared via [[ivy::lt_def(...)]]` |
| `p19_ptr_ret_no_lt.ivy` | a pointer return without a return lifetime | the em-dash diagnostic |
| `p20_order_ptr_and_lt.ivy` | the pointer-return rule vs the parameter errors | the oracle's order |
| `p21_two_params_one_lt.ivy` | the same lifetime on two parameters | two `[lt:a]` |
| `p22_lt_ret_on_byvalue_return.ivy` | a return lifetime on a by-value return | `return lifetime: a` |
| `p23_constexpr_lt.ivy` | `lifetime<$a> constexpr fn` | `lifetimes: a` |
| `p24_lt_leading_form.ivy` | the C-style leading return form | the same HIR as the trailing form |
| `p25_legacy_dup_ltdef.ivy` | two `lt_def(a)` | `duplicate lifetime 'a'` |
| `p26_undeclared_ret_lt.ivy` | an undeclared return lifetime | `return lifetime '$b' is not declared in lifetime<...>` |
| `p27_undeclared_param_lt.ivy` | an undeclared parameter lifetime | `lifetime '$b' is not declared in lifetime<...>` |
| `p28_lt_before_attrs.ivy` | `lifetime<...>` before `[[ivy::lt_ret(a)]]` | the same HIR as the other forms |
| `p29_lt_def_and_native_same_name.ivy` | different names in the native and the legacy list | `lifetimes: b a` |
| `p30_extern_c_decl.ivy` | `lifetime<...>` inside `extern "C"` | a parser error (identical) |
| `p31_method_legacy_param_lt.ivy` | a legacy `lt_def` before a struct | a parser error (identical) |
| `p32_empty_lt_and_legacy.ivy` | `lifetime<>` + `lt_def(a)` | `lifetimes: a` |
| `p33_ctor_param_lt.ivy` | a constructor with a param lifetime | `lifetime '$a' is not declared in lifetime<...>` (a method has no lifetime list) |
| `p34_two_lifetimes_two_params.ivy` | two lifetimes, two parameters | `lifetimes: a b`, two `[lt:]` |
| `p35_lt_ret_only_native.ivy` | a return lifetime only | `return lifetime: a` |
| `p36_two_legacy_lt_ret.ivy` | two `lt_ret` attributes | the **first** one wins |
| `p37_param_two_legacy_lt.ivy` | two `lt` attributes on a parameter | the **first** one wins |
| `p38_decl_only_ptr_ret.ivy` | a declaration-only pointer return | no error |
| `p39_extern_c_param_lt.ivy` | a native param lifetime in `extern "C"` | a parser error (identical) |
| `p40_extern_c_legacy_lt.ivy` | a legacy `lt_def` before `extern "C"` | a parser error (identical) |
| `p41_interleave_tpl_and_lt.ivy` | a template-id parameter type + a lifetime error | the oracle's interleaved order |
| `p42_interleave_tpl_only.ivy` | a template-id parameter type alone | identical |
| `p43_tpl_param_lt.ivy` | a lifetime on a template-id parameter | `lifetime annotation requires a reference or pointer parameter` |

**42 of the 43** new fixtures are byte-identical to the oracle; `p02` differs
only in the MIR-stage diagnostic (its HIR dump is identical).

---

## 8. Verification (all run from `D:\project\Ivy\ivyc`)

HIR suites (`-Kinds all -Oracle --hir -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder
-ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive`):

| suite | files | result |
|-------|-------|--------|
| `selfhost/corpus/hir/name` | 27 | **PASS** (identical) |
| `selfhost/corpus/hir/type` | 120 | **PASS** |
| `selfhost/corpus/hir/constexpr` | 70 | **PASS** |
| `selfhost/corpus/hir/template` | 99 | **PASS** |

Parser suites (`-Kinds all -Oracle --ast -Modules token,lexer,preprocessor,ast,parser
-Driver astdrive`):

| suite | files | result |
|-------|-------|--------|
| `selfhost/corpus/parser/decl` | 15 | **PASS** |
| `selfhost/corpus/parser/expr` | 13 | **PASS** |
| `selfhost/corpus/parser/stmt` | 15 | **PASS** |
| `selfhost/corpus/parser/aggregate` | 14 | **PASS** |
| `selfhost/corpus/parser/template` | 15 | **PASS** |
| `selfhost/corpus/parser/module` | 16 | **PASS** |
| `selfhost/corpus/parser/err` | 19 | **PASS** |

Lexer suites (`-Oracle --lex -Modules token,lexer -Driver lexdrive`):

| suite | files | result |
|-------|-------|--------|
| `ident` | 10 | **PASS** |
| `num` | 8 | **PASS** |
| `str` | 7 | **PASS** |
| `op` | 5 | **PASS** |
| `trivia` | 7 | **PASS** |
| `err` | 9 | **PASS** |

Preprocessor suites (`-Oracle --tokens -Modules token,lexer,preprocessor -Driver ppdrive`):

| suite | files | result |
|-------|-------|--------|
| `define_obj` | 10 | **PASS** |
| `define_fn` | 12 | **PASS** |
| `cond` | 16 | **PASS** |
| `expand` | 16 | **PASS** |
| `predef` | 20 | **PASS** |
| `line` | 22 | **PASS** |
| `no_include` | 9 | **PASS** |

(The lexer and the preprocessor suites share one temporary directory, so they
must not be run concurrently - two parallel runs collide on
`%TEMP%\ivydiff_selfhost\one.txt` and produce bogus failures.  Run them
sequentially.)

---

## 9. Divergences from the phase-1 and phase-2 notes

| # | divergence | note |
|---|------------|------|
| 1 | **The em dash.**  Section 3.4 of the phase-1 notes says the oracle's diagnostics print `CE 93 C3 87 C3 B6` (the mojibake).  The **raw** bytes of the oracle's stderr are `E2 80 94` (the plain UTF-8 em dash); the mojibake only appears when the bytes are captured through a PowerShell *text* pipeline (the console code page CP437 is used to decode them).  The port writes the real em dash into the `.ivy` source and is byte-identical to the oracle (section 6).  The recommendation of the notes ("put the real em dash into the source") is **correct** and was followed. |
| 2 | The phase-1 notes do not mention that the oracle's **parameter** diagnostics are reported at the parameter's **type** token while the function-level ones use the declaration start token.  Phase 2 aligned `Function::loc`; phase 3 confirmed the parameter rule (`AParam.line/col` is the type token) and the attribute rule (`ALegacyLifetime.line/col` is the `[[`). |
| 3 | The phase-1 notes list the diagnostic order of section 6.7 but do not say that the oracle's **`unsafe`** diagnostics are interleaved with the lifetime ones.  Phase 3 had to implement three more `unsafe` diagnostics (`return type 'nullptr_t' ...`, `raw pointer return type ...`, `parameter type 'nullptr_t' ...`) to reproduce the order. |
| 4 | The phase-1 notes do not say that the **`raw pointer parameter type requires an unsafe block`** diagnostic of the port was emitted in the wrong place (it was emitted for all the parameters before any lifetime diagnostic).  It is now emitted per parameter, right after that parameter's lifetime diagnostic, like the oracle. |
| 5 | The phase-1 notes do not mention that a **method** has no lifetime list: a parameter lifetime of a method is always "not declared".  Phase 3 implements it (fixture `p33`). |
| 6 | The phase-1 notes do not mention that the **first** `lt_ret` / the **first** `lt` attribute wins when there are several.  Phase 3 implements it (fixtures `p36`, `p37`). |

---

## 10. What phase 4 has to do

1. Turn the fixtures of section 7 (and the 34 matching recon fixtures of
   section 7.1) into the official corpus `selfhost/corpus/hir/borrow/`.
   **Constraint:** no file may trigger a MIR-stage diagnostic (the port has no
   MIR stage), so `syntax/x27` and `p02` must **not** go into the corpus.
2. Keep the bookkeeping (the milestone log, the plan, the corpus index).
3. The independent verification (phase 5) re-runs the whole differential
   against a freshly built oracle.

---

# Phase 4 findings - the official H4.5 corpus

**Milestone:** H4.5 (Borrow / lifetime check), phase 4/5 (the official corpus +
bookkeeping).
**Repo:** `D:\project\Ivy\ivyc`  (drive **D:**).
**HEAD at time of writing:** `17c68cf` (unchanged - nothing was committed).
**Files changed by this phase:** `selfhost/corpus/hir/borrow/**` (new, 44
files), `selfhost/difftest.ps1` (header comment only, **no behaviour
change**), `PLAN.md` and this file.  **No production code was touched**:
`selfhost/hir/{hir,hir_builder,hirdrive}.ivy` and `selfhost/parsing/**` are
exactly as phase 3 left them.

Phase 4 changes **no lifetime semantics**.  It only turns the validated
fixtures of phases 1-3 into the official corpus and does the bookkeeping.

---

## 0. Path check (done first)

```
PS> Get-Location
    Path
    ----
D:\project\Ivy\ivyc

PS> git rev-parse --show-toplevel
D:/project/Ivy/ivyc
```

Both point at the repository on drive **D:** (`Test-Path C:\project` is
`False`).  All shell commands were run with `workdir = D:\project\Ivy\ivyc`;
no `cd` was used.  `C:` was used only for the scratch fixtures under
`C:\Users\ASUS\AppData\Local\Temp\opencode\h45f4\`.

---

## 1. The corpus: 44 files, 6 topic groups + the error group

`selfhost/corpus/hir/borrow/` - every file is self-contained, deterministic
and free of any temp path.  The prefix groups by topic; the `e_` prefix
marks the error cases.

### 1.1 `01_` - the native parameter lifetime `$a`

| file | rule | expected `--hir` |
|------|------|------------------|
| `01_param_lt_ref.ivy` | a native `$a` on a **reference** parameter, the name is declared | `lifetimes: a` + `x [lt:a]` |
| `01_param_lt_ptr.ivy` | a native `$a` on a **pointer** parameter (a *declaration only* function, so the `unsafe` rule does not fire) | `lifetimes: a` + `p [lt:a]` |
| `01_param_lt_shared.ivy` | one lifetime shared by **two** parameters | two ` [lt:a]`, one `lifetimes: a` |
| `01_param_lt_leading_form.ivy` | the **C-style leading** declaration form takes a parameter lifetime | `x [lt:a]` + `return lifetime: a` |

The **by-value** parameter and the **undeclared** name are error cases and
live in the `e_` group (`e_03`, `e_01`).

### 1.2 `02_` - the native function lifetime list `lifetime<...>`

| file | rule | expected `--hir` |
|------|------|------------------|
| `02_lt_list_single.ivy` | `lifetime<$a>` declares one name | `lifetimes: a` |
| `02_lt_list_multiple.ivy` | `lifetime<$a, $b>` declares two names, in order | `lifetimes: a b` |
| `02_lt_list_empty.ivy` | `lifetime<>` declares nothing | **no** `lifetimes:` line |
| `02_lt_list_used_by_params.ivy` | the declared names are used by the parameters | `lifetimes: a b`, `x [lt:a]`, `y [lt:b]` |

The **duplicated** name is a parser diagnostic and lives in the `e_` group
(`e_09`).

### 1.3 `03_` - the native return lifetime `-> T $a`

| file | rule | expected `--hir` |
|------|------|------------------|
| `03_ret_lt_ref.ivy` | `-> const int32& $a` (the trailing form) | `return lifetime: a` |
| `03_ret_lt_leading_form.ivy` | the leading form `const int32& $a f(...)` | the same HIR |
| `03_ret_lt_by_value.ivy` | a return lifetime on a **by-value** return type is accepted (the oracle does not check it) | `return lifetime: a` |
| `03_ret_lt_ptr_return.ivy` | a **pointer** return type with a return lifetime (declaration only) | `return lifetime: a` |

The **undeclared** return lifetime and the "**no `lifetime<...>` at all**"
case are error cases and live in the `e_` group (`e_02`,
`e_02_..._no_list`); the "pointer return **without** a return lifetime" is
`e_10`.

### 1.4 `04_` - the legacy attributes

| file | rule | expected `--hir` |
|------|------|------------------|
| `04_legacy_lt_def.ivy` | `[[ivy::lt_def(a)]]` declares a name | `lifetimes: a` |
| `04_legacy_lt_def_multi.ivy` | `[[ivy::lt_def(a, b)]]` declares several names | `lifetimes: a b` |
| `04_legacy_lt_ret.ivy` | `[[ivy::lt_ret(a)]]` sets the return lifetime | `return lifetime: a` |
| `04_legacy_lt_param.ivy` | `[[ivy::lt(a)]]` on a parameter (after the name) | `x [lt:a]` |
| `04_legacy_all_forms.ivy` | the three legacy attributes together | the same HIR as the native form |

The **arity error**, the **undeclared** name and the **by-value parameter**
error are error cases and live in the `e_` group (`e_05`, `e_06`, `e_04`,
`e_07`).

### 1.5 `05_` - the precedence rules

| file | rule | expected `--hir` |
|------|------|------------------|
| `05_prec_declared_list_native_first.ivy` | the declared list = the **native** names first, then the legacy `lt_def` names | `lifetimes: a b` |
| `05_prec_param_native_wins.ivy` | on a parameter the **native** `$a` wins over the legacy `lt` | `x [lt:a]` |
| `05_prec_return_legacy_wins.ivy` | on the return the **legacy** `lt_ret` wins over the native `$a` | `return lifetime: b` |
| `05_prec_empty_list_and_legacy.ivy` | an empty native list + a legacy `lt_def` | `lifetimes: a` |
| `05_prec_dup_native_and_legacy.ivy` | the same name in the native and the legacy list is a **duplicate** | `duplicate lifetime 'a'` |

`05_prec_return_legacy_wins.ivy` is the **re-shaped** version of the recon's
`syntax/x27` / phase 3's `p02`: those two return a **reference** whose
lifetime does not match the declared one, so the **MIR** stage reports
`returned reference has lifetime '$a' but the function declares lifetime
'$b'` and the file can never be matched.  The corpus version returns a
**by-value** (`-> int32 $b`), and `checkReturn` returns early for a
by-value return (`src/mir/mir_builder.cpp:230`), so the file is MIR-clean
and still pins the "the legacy `lt_ret` wins" rule.

### 1.6 `06_` - the dump shapes

| file | rule | expected `--hir` |
|------|------|------------------|
| `06_dump_fn_lifetimes_and_params.ivy` | all three dump items at once | `lifetimes: a b` + `return lifetime: a` + two ` [lt:]` |
| `06_dump_method_param_lt.ivy` | a **method** has no declared lifetime list, so a parameter lifetime is never declared | `lifetime '$a' is not declared in lifetime<...>` |
| `06_dump_template_fn.ivy` | a **function template** is not lowered at all (the oracle returns early in `buildSignature`) | only `pick` is dumped, `apply` produces nothing |
| `06_dump_prototype_only.ivy` | a **prototype only** function | `(declaration only)` |
| `06_dump_extern_c.ivy` | an `extern "C"` block with the legacy attributes | `function g [extern "C"]` + `lifetimes: a` + `x [lt:a]` + `(declaration only)` |
| `06_dump_constexpr.ivy` | `lifetime<$a> constexpr fn f()` | `lifetimes: a` |

A **native** lifetime inside an `extern "C"` block is a *parser* error
(`syntax/x43`, `p30`, `p39`, `p40`), so the extern "C" shape uses the
legacy attributes, which the oracle accepts inside the block.

### 1.7 `e_` - the HIR-stage error cases (the 10 diagnostics)

Every `e_` file was run through `ivy --hir` individually and checked to
produce the **intended** diagnostic.  The oracle's output is quoted verbatim
(the path is the corpus-relative one, exactly as the harness runs it).

| file | # | diagnostic (verbatim) |
|------|---|-----------------------|
| `e_01_undeclared_param_lt.ivy` | 1 | `2:6: error: lifetime '$b' is not declared in lifetime<...>` |
| `e_01_lt_list_empty_with_param.ivy` | 1 | `2:6: error: lifetime '$a' is not declared in lifetime<...>` (an **empty** list declares nothing) |
| `e_02_undeclared_ret_lt.ivy` | 2 | `2:1: error: return lifetime '$b' is not declared in lifetime<...>` |
| `e_02_undeclared_ret_lt_no_list.ivy` | 2 | `1:1: error: return lifetime '$b' is not declared in lifetime<...>` (there is **no** `lifetime<...>` at all) |
| `e_03_lt_on_byvalue_param.ivy` | 3 | `2:6: error: lifetime annotation requires a reference or pointer parameter` |
| `e_04_legacy_undeclared.ivy` | 4 | `2:21: error: [[ivy::lt(b)]]: lifetime 'b' is not declared via [[ivy::lt_def(...)]]` |
| `e_05_legacy_lt_arity2.ivy` | 5 | `2:21: error: [[ivy::lt]] expects exactly one lifetime argument` (2 arguments) |
| `e_05_legacy_lt_arity0.ivy` | 5 | `2:21: error: [[ivy::lt]] expects exactly one lifetime argument` (0 arguments) |
| `e_06_legacy_lt_ret_arity2.ivy` | 6 | `2:1: error: [[ivy::lt_ret]] expects exactly one lifetime argument` (2 arguments) |
| `e_06_legacy_lt_ret_arity0.ivy` | 6 | `2:1: error: [[ivy::lt_ret]] expects exactly one lifetime argument` (0 arguments) |
| `e_07_legacy_lt_on_byvalue_param.ivy` | 7 | `2:14: error: [[ivy::lt]] requires a reference or pointer parameter` |
| `e_08_duplicate_legacy_ltdef.ivy` | 8 | `2:1: error: duplicate lifetime 'a'` |
| `e_09_duplicate_native.ivy` | 9 | `1:16: error: duplicate lifetime '$a'` (the **parser** rejects it first; the HIR builder's copy of the rule is dead code) |
| `e_10_ptr_ret_no_lt.ivy` | 10 | `1:1: error: function 'f' returns a pointer but has no return lifetime - use `lifetime<$a>` and annotate the return type with $a` |
| `e_11_diag_order.ivy` | - | the **order** of the diagnostics inside one function: `2:1 return lifetime '$c' ...`, `2:8 lifetime annotation requires ...`, `2:20 lifetime '$b' ...` |
| `e_12_tpl_param_lt_order.ivy` | - | the **interleaving** of the type resolution of a template-id parameter with its lifetime diagnostic: `3:6 lifetime annotation requires ...`, `3:23 lifetime '$b' ...` |

`e_10` inevitably carries the oracle's `unsafe` companion diagnostic
(`raw pointer return type requires an unsafe block`, emitted at
`src/hir/hir_builder.cpp:860` for **every** pointer-returning *definition*;
a declaration is exempt).  The body of the file is empty so that no further
diagnostic is produced - the file then carries exactly two diagnostics, the
intended one and the unavoidable `unsafe` one.

`e_11` and `e_12` are the two files that carry more than one diagnostic:
they pin the **order** of the diagnostics, which cannot be expressed with a
single diagnostic.

---

## 2. The excluded candidates

Every candidate of the recon corpus (101 files) and of the phase-3 corpus
(43 files) was either merged into the corpus above or excluded.  The
exclusions, with the reason and the oracle evidence:

### 2.1 A MIR-stage diagnostic - never matchable (9 files)

`ivy --hir` runs the whole pipeline, so these files print the **HIR dump**
and *then* the MIR diagnostic (exit 1).  The selfhost driver stops after the
HIR stage, so it can never reproduce the extra line.  Measured on the
recon fixtures:

| candidate | the oracle's extra line (after the HIR dump) |
|-----------|-----------------------------------------------|
| `syntax/x27_native_and_legacy.ivy` | `5:5: error: returned reference has lifetime '$a' but the function declares lifetime '$b'` |
| `p02_legacy_ret_wins.ivy` | `5:5: error: returned reference has lifetime '$a' but the function declares lifetime '$b'` |
| `stage/s1_ret_local_ref.ivy` | `3:5: error: returning a reference to a local variable (dangling reference)` |
| `stage/s4_mut_vs_immut.ivy` | `4:5: error: cannot create a mutable reference to 'a' - it is already borrowed immutably` |
| `stage/s5_two_mut.ivy` | `4:5: error: cannot create a second mutable reference to 'a' - only one mutable borrow at a time` |
| `stage/s6_immut_vs_mut.ivy` | `4:5: error: cannot create an immutable reference to 'a' - it is already borrowed mutably` |
| `stage/s7_lt_mismatch.ivy` | `3:5: error: returned reference has lifetime '$b' but the function declares lifetime '$a'` |
| `stage/s11_unknown_lt_ret.ivy` | `3:5: returning a reference to a local variable (dangling reference)` + `7:5: returned reference has unknown lifetime; ...` |
| `stage/s15_return_call_result.ivy` | `3:5: returning a reference to a local variable (dangling reference)` + `7:5: returned reference has unknown lifetime; ...` |

The port's output for `x27`/`p02`/`s4`/`s6` was checked: the **HIR dump is
identical**, only the MIR line is missing.  The two lifetime-relevant
fixtures were therefore **re-shaped** into MIR-clean equivalents
(`05_prec_return_legacy_wins.ivy`); the borrow ones belong to H5.

### 2.2 The `__ret` / RAII lowering - not a lifetime feature (22 files)

`ret/r01` ... `ret/r22` are MIR-clean (exit 0, no diagnostic) but the
oracle's HIR dump contains the `__ret` temp, and the port **cannot even
build the fixture**:

```
$ hirdrive ret/r01_nontrivial.ivy
=== .../ret/r01_nontrivial.ivy ===
.../ret/r01_nontrivial.ivy:12:11: error: undeclared identifier 'Res'
.../ret/r01_nontrivial.ivy:12:11: error: call to undeclared function 'Res'
.../ret/r01_nontrivial.ivy:12:5: error: cannot initialize variable 'r' of type 'Res' with a value of type 'int'
```

The `__ret` temp needs the whole RAII/destructor machinery (dtor tracking,
dtor emission, NRVO / `returnedLocals_`) **and** struct construction /
constructor injection, none of which the port has.  It is a return-evaluation
*ordering* change, not a lifetime feature: it needs a future RAII/DTOR
milestone (or H5).  (`r09` and `r12` are additionally broken Ivy source -
they only produce parser / `unsafe` diagnostics.)

### 2.3 A parser- or lexer-level rejection of the lifetime syntax (16 files)

These are **not** HIR-stage lifetime rules; they are the parser's and the
lexer's rejections of a malformed lifetime.  They are already covered by
the `parser/err` suite (and the `lexer/err` suite for the lexer ones).

| candidate | the oracle's diagnostic |
|-----------|--------------------------|
| `syntax/x07_static_kw.ivy`, `syntax/x08_lt_apos_a.ivy` | lexer: `unterminated character literal` (the oracle prints the path **quoted and backslash-escaped** - a pre-existing lexer difference) |
| `bug/b01_lt_dollar_only.ivy`, `bug/b03_apos_a.ivy`, `bug/b04_apos_static.ivy`, `bug/b09_param_lt_only_dollar.ivy` | the same lexer diagnostics (same path-format difference) |
| `syntax/x09_lt_on_struct.ivy`, `syntax/x28_lt_on_enum.ivy`, `bug/b05_lt_then_nothing.ivy` | parser: `expected a function declaration after lifetime<...>` |
| `syntax/x13_lt_no_lt.ivy` | parser: `expected '<' after 'lifetime'` |
| `syntax/x14_lt_no_dollar.ivy` | parser: `expected a lifetime variable ($name) in lifetime<...>` |
| `syntax/x15_lt_unterminated.ivy`, `bug/b08_lt_nested_angle.ivy` | parser: `expected '>' to close lifetime<...>` |
| `syntax/x25_legacy_lt_on_fn.ivy` | parser: `unknown Ivy attribute 'ivy::lt' in this position` |
| `syntax/x26_legacy_ltdef_on_param.ivy` | parser: `unknown Ivy attribute 'ivy::lt_def' in this position` |
| `syntax/x43_extern_c_with_lt.ivy`, `p30_extern_c_decl.ivy`, `p39_extern_c_param_lt.ivy`, `p40_extern_c_legacy_lt.ivy` | parser: the `extern "C"` block rejects a lifetime prefix (`expected a type ...`) |

### 2.4 The oracle's own bug - deliberately not reproduced (1 file)

`syntax/x10_lt_on_template.ivy`: `lifetime<$a>` followed by
`template<typename T>` makes the oracle consume the `<` **twice**
(`src/parsing/parser.cpp:789` + `parseTemplateParams`), so it reports
`2:10 expected '<' to start template parameter list` +
`2:20 expected template parameter name` + `5:1 expected '>' to close
template parameter list`.  The port reports
`expected a function declaration after lifetime<...>` instead.  The
phase-1 notes already decided **not** to reproduce the bug, so the shape is
not in the corpus; the "template function" shape of the corpus
(`06_dump_template_fn.ivy`) uses a template **without** a `lifetime<...>`
prefix.

### 2.5 Not a lifetime rule (the `unsafe` rules, the method syntax, the template syntax) (10 files)

| candidate | why it is out |
|-----------|--------------|
| `stage/s13_lt_on_ptr_param.ivy` | only `raw pointer parameter type requires an unsafe block` - an `unsafe` rule, not a lifetime rule |
| `stage/s16_check_store_dangling.ivy` | only `address-of operator '&' taking raw pointer requires an unsafe block` - the `checkStore` rule is practically unreachable (it returns early inside `unsafe`) |
| `p12_ptr_ret_with_lt.ivy` | only the two `unsafe` diagnostics; the pointer *shapes* are in the corpus as declaration-only prototypes (`01_param_lt_ptr`, `03_ret_lt_ptr_return`) |
| `p38_decl_only_ptr_ret.ivy` | no lifetime annotation at all - it pins the declaration-only exemption, not a lifetime rule |
| `p10_method_param_lt.ivy`, `p31_method_legacy_param_lt.ivy` | the parse error (`expected '{' for method body or ';' for a declaration`) is caused by the **trailing return form of a method** (`-> int32`), not by the lifetime: `int32 get(const int32& x) -> int32 { }` fails to parse without any lifetime.  The method shape of the corpus (`06_dump_method_param_lt.ivy`) uses the legal C-style method form. |
| `p42_interleave_tpl_only.ivy` | a template-id parameter type without any lifetime - the parser rejects the unknown type; it is an H4.4 case |
| `p20_order_ptr_and_lt.ivy`, `order/o2.ivy` | 4 diagnostics each (2 `unsafe` + 1 lifetime + 1 body error); the ordering is pinned by `e_11` |

### 2.6 Near-duplicates of a file that is already in the corpus

The remaining 86 candidates of the two source corpora are the *same rules*
as the 44 corpus files (the recon's `syntax/x01`..`x44` and the phase-3
`p01`..`p43` are largely pairwise identical).  The mapping:

| recon / phase-3 fixture | corpus file |
|--------------------------|-------------|
| `x01`, `p24` | `01_param_lt_leading_form.ivy` |
| `x29` (the pointer param + the `unsafe` rule) | `01_param_lt_ptr.ivy` |
| `x41`, `p21` | `01_param_lt_shared.ivy` |
| `x02`, `p34` | `02_lt_list_multiple.ivy`, `02_lt_list_used_by_params.ivy` |
| `x03`, `p04` | `02_lt_list_empty.ivy` |
| `p05` | `e_01_lt_list_empty_with_param.ivy` |
| `x05`, `x24` | `03_ret_lt_ref.ivy` |
| `x06` | `03_ret_lt_leading_form.ivy` |
| `x38`, `p22` | `03_ret_lt_by_value.ivy` |
| `p35` | `03_ret_lt_ptr_return.ivy` |
| `x11`, `p13` | `04_legacy_all_forms.ivy` |
| `x37` | `04_legacy_lt_def_multi.ivy` |
| `p36` (two `lt_ret`), `p37` (two `lt`) | `05_prec_return_legacy_wins.ivy`, `04_legacy_lt_param.ivy` |
| `p01`, `p29` | `05_prec_declared_list_native_first.ivy` |
| `p32` | `05_prec_empty_list_and_legacy.ivy` |
| `x30`, `p09`, `x39` | `05_prec_dup_native_and_legacy.ivy` |
| `p23`, `x23` | `06_dump_constexpr.ivy` |
| `p33` (the constructor variant of the method rule) | `06_dump_method_param_lt.ivy` |
| `p11` | `06_dump_prototype_only.ivy` |
| `x12` | `e_09_duplicate_native.ivy` |
| `x16`, `p06`, `p43` | `e_03_lt_on_byvalue_param.ivy` |
| `x17`, `p27` | `e_01_undeclared_param_lt.ivy` |
| `x18`, `x42`, `p26` | `e_02_undeclared_ret_lt.ivy` |
| `x19`, `p18` | `e_04_legacy_undeclared.ivy` |
| `x21`, `p14` | `e_05_legacy_lt_arity2.ivy` |
| `x35`, `p15` | `e_05_legacy_lt_arity0.ivy` |
| `x33`, `p16` | `e_06_legacy_lt_ret_arity2.ivy` |
| `x34` | `e_06_legacy_lt_ret_arity0.ivy` |
| `x36`, `p17` | `e_07_legacy_lt_on_byvalue_param.ivy` |
| `x20`, `p25` | `e_08_duplicate_legacy_ltdef.ivy` |
| `x22`, `p19`, `stage/s2_ptr_ret_no_lt.ivy` | `e_10_ptr_ret_no_lt.ivy` |
| `x04`, `x40`, `x44` | `01_param_lt_ref.ivy`, `05_prec_param_native_wins.ivy` |
| `x31`, `p07` | `03_ret_lt_by_value.ivy` |
| `x32`, `p28` | `05_prec_return_legacy_wins.ivy` |
| `order/o1`, `p08` | `e_11_diag_order.ivy` |
| `order/o3` | `e_11_diag_order.ivy` |
| `p41` | `e_12_tpl_param_lt_order.ivy` |
| `stage/s3`, `s3b`, `s3c`, `s10` | broken Ivy source (the `fn` trailing form is missing the `->`): parser errors, not lifetime rules |
| `stage/s8`, `s9`, `s12`, `s14`, `s17`, `s18` | exit 0, **no diagnostic at all** (the oracle does not implement use-after-destroy, ref-to-temp or assign-through-borrow) |

---

## 3. Verification

### 3.1 The new suite

```
PS> powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/hir/borrow -Kinds all -Oracle --hir `
     -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder `
     -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive
[1/4] building selfhost modules ...
[2/4] collecting corpus ...
      files: 44
[3/4] running C++ oracle + Ivy port ...
[4/4] comparing (kinds: all) ...
PASS: identical (filtered) output on 44 files
```

### 3.2 The 24 other suites (nothing else changed)

| suite | files | result |
|-------|-------|--------|
| `selfhost/corpus/lexer/ident` | 10 | **PASS** |
| `selfhost/corpus/lexer/num` | 8 | **PASS** |
| `selfhost/corpus/lexer/str` | 7 | **PASS** |
| `selfhost/corpus/lexer/op` | 5 | **PASS** |
| `selfhost/corpus/lexer/trivia` | 7 | **PASS** |
| `selfhost/corpus/lexer/err` | 9 | **PASS** |
| `selfhost/corpus/pp/define_obj` | 10 | **PASS** |
| `selfhost/corpus/pp/define_fn` | 12 | **PASS** |
| `selfhost/corpus/pp/cond` | 16 | **PASS** |
| `selfhost/corpus/pp/expand` | 16 | **PASS** |
| `selfhost/corpus/pp/predef` | 20 | **PASS** |
| `selfhost/corpus/pp/line` | 22 | **PASS** |
| `selfhost/corpus/pp/no_include` | 9 | **PASS** |
| `selfhost/corpus/parser/decl` | 15 | **PASS** |
| `selfhost/corpus/parser/expr` | 13 | **PASS** |
| `selfhost/corpus/parser/stmt` | 15 | **PASS** |
| `selfhost/corpus/parser/aggregate` | 14 | **PASS** |
| `selfhost/corpus/parser/template` | 15 | **PASS** |
| `selfhost/corpus/parser/module` | 16 | **PASS** |
| `selfhost/corpus/parser/err` | 19 | **PASS** |
| `selfhost/corpus/hir/name` | 27 | **PASS** |
| `selfhost/corpus/hir/type` | 120 | **PASS** |
| `selfhost/corpus/hir/constexpr` | 70 | **PASS** |
| `selfhost/corpus/hir/template` | 99 | **PASS** |
| **total** | **574** | **0 failures** |

(The lexer and the preprocessor suites share one temporary directory, so
they were run **sequentially**.)

With the new suite: **25 suites, 618 files, all byte-identical.**

### 3.3 A note on the em dash and the harness

`difftest.ps1` writes both the expected and the actual file with an
**ASCII** encoding, so the em dash of diagnostic 10 is folded to `?` on
both sides.  The comparison is still meaningful (a different number of
non-ASCII characters would produce a different number of `?`), and the
**raw** bytes were checked separately for `e_10_ptr_ret_no_lt.ivy`:

```
oracle stderr : ... 20 6C 69 66 65 74 69 6D 65 20 E2 80 94 20 75 73 65 ...
port stdout   : ... 20 6C 69 66 65 74 69 6D 65 20 E2 80 94 20 75 73 65 ...
```

(the raw bytes must be captured with `cmd /c`; a PowerShell text pipeline
decodes them with the console code page CP437 and shows the `ΓÇö`
mojibake - the pitfall described in section 6 of the phase-3 notes).

---

## 4. The bookkeeping done in this phase

* `selfhost/difftest.ps1` - the **header comment block** gained the H4.5
  invocation (the HIR feature list).  **No behaviour change**: the script's
  parameters, its logic and its output are untouched.
* `PLAN.md` -
  * the H4.5 row of the mapping table is now
    `| H4.5 | Borrow / lifetime check | ... | ✅ byte-identical 44 files |`
    (the real number of corpus files);
  * a new **H4.5 note** in the "Hiện trạng" section describes the whole
    feature set (the native and the legacy syntax, the precedence rules,
    the 10 HIR-stage diagnostics, the three dump lines), the corpus, the
    invocation, the MIR-cleanliness constraint and the deferred scope;
  * the "Còn lại (để milestone sau)" lists of the H4.2, H4.3 and H4.4
    notes now name the deferred items explicitly: the `__ret` return
    evaluation + destructor/RAII + NRVO + struct construction (a future
    RAII/DTOR milestone) and the MIR borrow checker / lifetime inference
    (H5.1/H5.2).  The lifetime **annotations** are no longer listed as
    "not ported" anywhere.
  * H4.6 (Move / use-after-move) is left unchecked.
* `selfhost/hir/LIFETIME_NOTES.md` - this section.

---

## 5. What phase 5 has to do

1. Re-run the whole differential (25 suites, 618 files) against a
   **freshly built** oracle and confirm that the H4.5 corpus is still
   byte-identical.
2. Check the corpus for the two properties this phase established:
   **one rule per file** and **MIR-cleanliness** (no file may produce a
   MIR-stage diagnostic after the HIR dump).
3. Confirm that the deferred scope is still recorded: the MIR borrow
   checker (`checkBorrow` / `checkReturn` / `checkStore`), the lifetime
   inference (`mir::Expr::lifetime`) and the `__ret` / RAII lowering.

