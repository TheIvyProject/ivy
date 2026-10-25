# H5.2 - MIR-stage borrow / move / lifetime checks: recon notes

Recorded 2026-10-22.  The C++ oracle is `build/Release/ivy.exe`, 27,031,552
bytes, mtime **2026-10-21 16:41:44**; it was **not** rebuilt.  The tree was
clean at `6ef0f05` when this recon started.  Everything measured below was
produced in the scratch directory
`C:\Users\ASUS\AppData\Local\Temp\opencode\h52r1\` (a *copy* of
`selfhost/{parsing,hir,mir}` was compiled there with the oracle, so no build
product of the tree was touched) and with that copy only.

Scope: the four deferred checks of `PLAN.md:693-700` - the borrow checker
(`checkBorrow`), the return-lifetime checker (`checkReturn`), the store
checker (`checkStore`), the MIR-level move tracker (`markMovedOut` /
`isMovedOut` / `reinitialize`) - plus the `nextcase` location (**G7**,
`PLAN.md:700-703`).
Deliverable of the next phase is the port change plus the
`selfhost/corpus/mir/lifetime/` suite named in `PLAN.md:781`.

---

## 1. The model and the state to add

### 1.1 Where the checks live

All of them are inside `MirBuilder`, i.e. they run after the HIR builder has
succeeded, while the CFG is lowered (`src/mir/mir_builder.cpp:704-1041` for
the statements, `:434-700` for the expressions).  A diagnostic never aborts
the build: `MirBuilder::error` (`:143-146`) only appends a
`Diagnostic{line, col, message}` to `diagnostics_` and sets `failed_`, so one
function can produce several diagnostics and every following function is
still built (`:1078-1401`).  The driver prints them and skips the dump
(see section 4).

### 1.2 The state of the oracle and its port counterpart

| oracle field | declared / semantics | the port today |
|---|---|---|
| `params_` (`map<string, Lifetime>`) | lifetime of every named parameter, filled in `buildFunction` (`:1058-1066`), used by `lookup` (`:175-183`) | `pm_name_` / `pm_lt_` / `pm_ltn_` (`mir_builder.ivy:74-79`), filled by `build_function` (`:2406-2442`), read by `lookup_param` (`:845-863`) |
| `scopes_` (`vector<map<string, Lifetime>>`) | one entry per compound / for / function scope, `declare` inserts without overwriting (`:185-187`) | `sc_name_` / `sc_lt_` / `sc_ltn_` + the `sc_start_` stack (`:62-72`), `declare` (`:754-807`) with the same "existing entry is not replaced" rule |
| `borrowScopes_` (`vector<BorrowScope>`) | `BorrowScope` = `states : map<string, BorrowState>` (`sharedCount`, `hasMutable`) + `activeRefs : vector<ActiveRef>` (`refVar`, `sourceVar`, `isMutable`) | **absent** |
| `moveStates_` (`vector<map<string, MoveState>>`) | `MoveState` = `Initialized` / `MovedOut`, one map per scope | **absent** |
| `inAssignLhs_` (`bool`) | set around the LHS of an assignment (`:633-635`) to bypass the use-after-move check | **absent** |
| `unsafeDepth_` (`int`) | incremented by `H::Unsafe` (`:976-981`), guards every new check | `unsafe_depth_` (`:60`), `st_unsafe` (`:1760-1768`) |

Two properties of `borrowScopes_` must be reproduced exactly:

1. A `BorrowState` is created in the **innermost scope that has the name**;
   when no scope has it (a parameter, or a variable of an enclosing
   function-level scope), it is created in the **outermost** scope
   (`:332-338`).
2. The borrow is *released* only through the `activeRefs` list, when the
   scope that registered it is popped (`releaseBorrowsInScope`, `:410-430`).
   `releaseBorrower` (`:381-405`) exists but has **no call site anywhere in
   the tree** (grep: only the definition) - the port does not need it.

### 1.3 The scope discipline is already in lockstep

The oracle pushes and pops the three lifetime/borrow/move stacks at exactly
three places: the compound statement (`:708-720`), the `for` (`:826-870`) and
the function body (`:1068-1075`).  The port's `push_scope` / `pop_scope`
(`mir_builder.ivy:721-750`) are called at exactly the same three places:
`st_compound` (`:1608`, `:1619`), `st_for` (`:1885`, `:1925`) and
`build_function` (`:2444`, `:2457`).  `while`, `do-while`, `if` and `switch`
introduce no scope in either implementation.  Consequently the borrow and
move scopes can be pushed and popped **inside `push_scope` / `pop_scope`**
and no call site has to change.  The release order of the oracle is
`scopes_.pop_back(); releaseBorrowsInScope(); moveStates_.pop_back();`
(`:716-718`), i.e. the borrower release happens *after* the lifetime scope is
gone but it only uses `borrowScopes_`, so it is independent.

### 1.4 The lifetime values

`mir::Lifetime::Kind` is `None, Named, Static, Local, Unknown`; the port's
codes are the same order (`selfhost/mir/mir.ivy:22-30`), and
`merge_lt` (`mir_builder.ivy:866-884`) already reproduces `mergeLifetime`.
`Expr::lifetime` is already computed for every expression by the port
(`ex_ident :1202-1204`, `ex_unary :1216-1245`, `ex_binary :1270-1285`,
`ex_ternary :1313-1318`, `ex_call :1360-1382`, `ex_index :1406-1418`,
`ex_member :1431-1444`, `ex_assign :1460-1473`).  H5.2 consumes it; it does
not extend it.

### 1.5 Where the diagnostics go

The port already has the whole channel: `diag(line, col, msg)`
(`mir_builder.ivy:494-533`) copies the message into the string pool, grows
`dline_` / `dcol_` / `dmsg_`, and sets `failed_`.  The scratch buffer helpers
`db_put` (`:420`) / `db_ch` (`:435`) build the `nextcase` message today
(`:1981-1988`), and `keep_string` (`:464-490`) is the only allocator for
message text.  The accessors `diag_count` / `diag_line` / `diag_col` /
`diag_msg` (`:690-713`) are what `mirdrive.ivy` reads.

### 1.6 The style rule that constrains the implementation

`mir_builder.ivy:24-31`: a method must **not** keep an `MExpr`, an `MInst` or
an `HStmt` in a local across a call - the C++ code generator of the port
stores such a record in a stack slot that a call clobbers.  Every new helper
must therefore pass indices and read fields through the pool pointers
(`this.sp[i].line`, `this.mu.exprs[i].lt`, ...) and keep only scalars in
locals.  The G7 experiment of section 5 hit the related restriction that a
pointer indexing expression is not allowed outside `unsafe` (the compiler
rejected `this.diag(this.sp[idx].line, ...)`), so field reads are done into
scalars inside an `unsafe` block first.

---

## 2. The checks, as pseudocode in the port's idiom

Notation: `mu` is the MIR unit, `ep`/`sp` the HIR pools, `diag(l, c, s)` the
port's `diag`.  `local_lifetime(name)` / `param_lifetime(name)` mean the
existing `lookup` / `lookup_param` protocol (result in `lt_kind_` /
`lt_name_`).

### 2.1 `checkReturn` (`src/mir/mir_builder.cpp:228-291`, called from `:877`)

```
check_return(ret_type, ret_lt_name, expr_idx, line, col):
    if ret_type.ptr_depth == 0 and ret_type.is_ref == 0: return
    if unsafe_depth_ > 0: return
    k = expr.lt ; n = expr.lt_name
    if k == 0 and expr.kind == 6:              # bare ident, value-typed local
        if is_local(expr.sval) and not is_param(expr.sval):
            k = 3                              # Local (dangling)
        else if is_param(expr.sval):
            lookup_param(expr.sval); k = lt_kind_; n = lt_name_
    if k == 1:                                 # Named
        if ret_lt_name != "" and n != ret_lt_name:
            diag(line, col, "returned reference has lifetime '$" + n +
                 "' but the function declares lifetime '$" + ret_lt_name + "'")
        return
    if k == 2 or k == 0: return                # Static / None
    if k == 3:
        diag(line, col, "returning a reference to a local variable (dangling reference)")
        return
    # k == 4 Unknown
    if ret_lt_name == "":
        diag(line, col, "returned reference has unknown lifetime; annotate the "
             "function with lifetime<$a> and tie the return to a parameter")
    else:
        diag(line, col, "returned reference has unknown lifetime; tie it to a "
             "parameter with lifetime annotation")
```

Notes measured on the oracle:

* The `Named` case compares **string equality** (`lt.name != fn.returnLifetime`),
  and the message prints the `$` literally followed by the name.
* `Static` never errors (a string literal).  `None` never errors.
* The `Local` synthesis is only for a bare `IdentRef` **whose lifetime is
  None**; a reference-typed local already carries the lifetime of its
  initializer.
* `is_param` is not available in the port: `lookup_param` returns kind 0 both
  for "not a parameter" and for "a parameter without a lifetime", and the
  oracle's test is `params_.contains(name)` (`:250`).  The port needs a small
  `bool is_param(const char*)` that scans `pm_name_` (the same loop as
  `lookup_param`, without writing `lt_kind_`).  A name that is both a local
  and a parameter (shadowing) must take the parameter branch, exactly as the
  `if (isLocal && !isParam) ... else if (isParam) ...` order does.
* A call result is always `Unknown` (`mir_builder.cpp:603` assigns
  `Unknown` unconditionally, even after the "named" branch of `:595-602`), so
  `return g();` reaches the `Unknown` branch.  The port reproduces that
  (`ex_call :1363-1382` sets kind 4 unconditionally except for `move`), so the
  port must keep the same quirk.

### 2.2 `checkBorrow` (`:315-362`, called from `:738` and `:514`)

```
check_borrow(name, is_mut, line, col) -> bool:
    if unsafe_depth_ > 0: return true
    if borrow_scope_count == 0: return true
    idx = find_state(name)                     # innermost -> outermost
    if idx < 0: idx = add_state(name)          # the *outermost* scope entry
    if is_mut:
        if shared[idx] > 0:
            diag(line, col, "cannot create a mutable reference to '" + name +
                 "' — it is already borrowed immutably")
            return false
        if mut_[idx] != 0:
            diag(line, col, "cannot create a second mutable reference to '" +
                 name + "' — only one mutable borrow at a time")
            return false
        mut_[idx] = 1
    else:
        if mut_[idx] != 0:
            diag(line, col, "cannot create an immutable reference to '" + name +
                 "' — it is already borrowed mutably")
            return false
        shared[idx] = shared[idx] + 1
    return true
```

The em dash is the three bytes `e2 80 94` (U+2014), see section 3.  A
successful `checkBorrow` is followed by `registerBorrower` (`:367-376`), which
appends `(refVar, sourceVar, isMutable)` to the **innermost** scope's
`activeRefs`.

`registerBorrower` is called at exactly one site - the reference declaration
(`:735-743`) - and **not** for the `&ident` form of `:514` (that borrow is
"temporary" and is only counted, never released; see the measured
consequence in 3.4).

### 2.3 `checkStore` (`:298-307`, called from `:968`)

```
check_store(target_type, value_lt, line, col):
    if unsafe_depth_ > 0: return
    if target_type.ptr_depth == 0 and target_type.is_ref == 0: return
    if value_lt == 3:                          # Local
        diag(line, col, "storing a reference to a local variable into a pointer "
             "or reference slot (dangling)")
```

Reachability: **none found** (section 9.1).  It should still be ported for
fidelity, it is four lines, but no fixture can pin it.

### 2.4 The move tracker (`:190-220`, call sites `:476`, `:569-573`, `:643`, `:749`)

```
mark_moved_out(name):  found = false
    for scope from innermost: if name in scope: state = MovedOut; found = true
is_moved_out(name) -> bool:
    for scope from innermost: if name in scope: return state == MovedOut
    return false
reinitialize(name):
    for scope from innermost: if name in scope: state = Initialized
```

Call sites, all guarded by `unsafe_depth_ == 0` except the `Decl` one:

| site | what | the oracle line |
|---|---|---|
| `ex_ident` | `if not inAssignLhs_ and unsafe == 0 and is_moved_out(name)` -> `use of moved-out variable '<n>'` | `:476-478` |
| `ex_call` (`move(x)`, one argument) | after building the argument, if it is a bare ident: `is_moved_out` -> `'<n>' is already moved-out`, then `mark_moved_out` | `:561-575` |
| `ex_assign` | `inAssignLhs_ = true` around the LHS (`:633-635`), and for `op == "="` on a bare ident LHS -> `reinitialize` (`:639-644`) | `:626-646` |
| `st_decl` | `moveStates_.back().emplace(name, Initialized)` - `emplace` does **not** overwrite an existing entry | `:749` |

Reachability: **effectively none** - the HIR builder implements the same
tracker earlier and without an unsafe bypass (`src/hir/hir_builder.cpp:2651-2659`,
`:2934-2950`, `:3603-3605`, `:3684-3694`).  Measured: all 60 fixtures of
`selfhost/corpus/hir/move/` give the same verdict for `--hir` and `--mir`
(no MIR-only diagnostic; the `--hir` run of `p6`/`p7` prints 0 bytes of
stdout, i.e. the diagnostic is HIR-stage).  This is a duplicate check, see
section 9.1.

### 2.5 `releaseBorrowsInScope` (`:410-430`, on every scope pop)

```
for each entry in the innermost scope's activeRefs (in order):
    if entry.released: continue                 # oracle: entry.refVar == ""
    idx = find_state(entry.source)               # OUTERMOST -> innermost
    if idx >= 0:
        if entry.is_mut: mut_[idx] = 0
        else: if shared[idx] > 0: shared[idx] = shared[idx] - 1
    entry.released = 1                          # oracle: refVar.clear()
drop the innermost scope
```

Two asymmetries to reproduce:

* The state search here is **outermost to innermost** (`:417`, a plain
  `for (auto& sc : borrowScopes_)`), unlike `checkBorrow` / `releaseBorrower`
  which search innermost to outermost (`:324`, `:388`).  With a shadowed
  name the release therefore decrements the *outer* state.
* The "already released" test is `refVar.empty()` (`:415`), i.e. the release
  clears the reference name; a port that keeps the name must use a `released`
  flag instead (equivalent, because a scope is released only once).

Because the port pops the scope stack in `pop_scope`, this is a loop over the
entries `[scope_start, n)` followed by restoring `n` to `scope_start`.

### 2.6 `nextcase` (`:893-919`)

Already implemented in the port (`st_nextcase`, `mir_builder.ivy:1944-1989`);
the only defect is the location: `:1988` passes `0, 0` where the oracle
passes `s.loc` (`:915-916`).  G7 fixes it (section 5).  Nothing else in the
`nextcase` path has to change.

---

## 3. The diagnostics table

Eleven messages exist.  `L:C` is the reported position; `s.loc` is the
**location of the HIR statement** (for the checks of `st_decl`, `st_return`,
`st_exprstmt`, `st_nextcase`) or of the expression (for `ex_ident` and the
`move` call).

| # | message (exact bytes, `$` and quotes included) | check | position | reachable? |
|---|---|---|---|---|
| 1 | `cannot create a mutable reference to '<n>' — it is already borrowed immutably` | `checkBorrow`, mut after shared | `s.loc` of the Decl (`:738`) | yes (`87`) |
| 2 | `cannot create a second mutable reference to '<n>' — only one mutable borrow at a time` | `checkBorrow`, mut after mut | `s.loc` of the Decl | yes (`29`) |
| 3 | `cannot create an immutable reference to '<n>' — it is already borrowed mutably` | `checkBorrow`, shared after mut | `s.loc` of the Decl | yes (`30`) |
| 4 | `returning a reference to a local variable (dangling reference)` | `checkReturn`, kind Local | `s.loc` of the Return (`:877`) | yes (`32`) |
| 5 | `returned reference has lifetime '$<a>' but the function declares lifetime '$<b>'` | `checkReturn`, kind Named mismatch | `s.loc` of the Return | yes (`80`) |
| 6 | `returned reference has unknown lifetime; annotate the function with lifetime<$a> and tie the return to a parameter` | `checkReturn`, kind Unknown, no declared return lifetime | `s.loc` of the Return | yes (`q1`) |
| 7 | `returned reference has unknown lifetime; tie it to a parameter with lifetime annotation` | `checkReturn`, kind Unknown, declared return lifetime | `s.loc` of the Return | yes (`q2`) |
| 8 | `storing a reference to a local variable into a pointer or reference slot (dangling)` | `checkStore` | `s.loc` of the ExprStmt (`:968`) | **no** (9.1) |
| 9 | `use of moved-out variable '<n>'` | `isMovedOut` on a read | expression `e.loc` (`:477`) | HIR-stage only (9.1) |
| 10 | `'<n>' is already moved-out` | second `move(x)` | expression `e.loc` (`:570`) | HIR-stage only (9.1) |
| 11 | `nextcase target '<n>' is not a case label in this switch` | `NextCase` label lookup | `s.loc` (`:915`) | yes (`61`, `q02`, `q15`), needs G7 |

### 3.1 The em dash

Messages 1-3 use U+2014 (`"—"`), not a hyphen.  Measured in the oracle's
stderr for `29_borrow_mut_mut`: the bytes of the message tail are

```
... 20 74 6f 20 27 78 27 20 e2 80 94 20 6f 6e 6c 79 ...
```

i.e. `'x'`, space, `e2 80 94`, space, `only`.  A probe source containing the
same literal, compiled by the oracle and run, emits the same three bytes, so
the em dash can be written literally in the `.ivy` source (as
`hir_builder.ivy` already does in its own messages) - the harness compiles
the port with the same oracle.

### 3.2 Line endings and the path prefix

The oracle writes every diagnostic to **stderr** with `\r\n`; the harness
normalizes `CR LF` to `LF` (`difftest.ps1:230-231`, `:238-239`), so the port
may emit `\n`.  The path in front of `L:C` is the path **as given on the
command line**; the harness passes the same relative `selfhost/corpus/...`
string to both sides (`:288`, `:298`), so my manual runs with absolute paths
show a longer prefix on both sides but the same shape.

### 3.3 Order and multiplicity

Diagnostics are appended in build order; the build does not stop.  Measured:

* `p1_multi_return` (two functions, `g` then `f`, each returning a local):
  two lines, `3:5` then `8:5` - function order, source order inside a
  function.
* `q6_mut_then_shared_then_mut` (`int32& a = x; const int32& b = x;
  int32& c = x;`): `4:5` message 3, then `5:5` message 2 - the failed
  borrow of `b` does not clear `hasMutable`, and `c` is still rejected.
* `p6_move_twice` (`int32 a = move(x); int32 b = move(x);`): `4:20`
  message 9 (the inner identifier read) **before** `4:19` message 10 (the
  `move` call), i.e. the nesting order of `buildExpr`, not source order.
  (HIR-stage, kept here because the same ordering rule applies to the port.)

### 3.4 The `&ident` borrow is counted but never released

`checkBorrow` is called for every `&<ident>` (`:514`), but `registerBorrower`
is not, so the count is permanent for the function.  A program with two
`&x` of the same local would report message 2 twice - except that a raw
pointer (and therefore `&local`) is rejected by the HIR outside `unsafe`
(measured: `p3_addr_twice` / `p4_addr_shared_then_mut` / `p5_store_local`
each produce 2-3 HIR diagnostics of the form `raw pointer ... requires an
unsafe block`).  Inside `unsafe` the check is bypassed.  So this asymmetry is
unreachable too; the port should nevertheless reproduce it exactly.

---

## 4. Transport: dump, driver, harness

Measured facts:

| situation | stdout | stderr | exit |
|---|---|---|---|
| `--mir`, no diagnostic | the dump (42 bytes for `void f() {}`) | 0 bytes | 0 |
| `--mir`, MIR diagnostic | **0 bytes** - no dump at all | the diagnostic(s) | 1 |
| `--hir`, MIR diagnostic | the HIR dump, then nothing else | the diagnostic(s) | 1 |
| `--hir`, HIR diagnostic | 0 bytes | the diagnostic(s) | 1 |

The `--hir` row was measured on `29_borrow_mut_mut` (stdout 151 bytes = the
HIR dump, stderr 172 bytes = the borrow message).  It matters because the
HIR suites use `--hir` over corpora that can contain MIR diagnostics - they
must stay MIR-clean for exactly this reason (`difftest.ps1:116-142`).

The port's driver already implements all of this: `mirdrive.ivy:544` prints
the `=== path ===` marker, `:626-651` runs the MIR builder, prints every
diagnostic as `path:L:C: error: msg`, and only dumps when
`md == 0`; `main` (`:668-674`) returns 1 when any stage failed.  No change to
`mirdrive.ivy` is needed for H5.2.

The harness (`selfhost/difftest.ps1`) is already wired for MIR
(`:133-142`): the oracle is invoked per file with `> one 2>&1`
(`:288`, stdout and stderr merged), the expected text gets the
`=== path ===` marker prepended by PowerShell (`:289`), the port reads the
file list on stdin and also writes marker + payload to stdout (`:298`, the
port's own marker line is kept by `Test-KeepBytes` because it starts with
`=== `, `:258-260`), and the comparison is byte-exact after CRLF
normalization (`:306-317`).  Empty lines are dropped (`:256`).  So the H5.2
suite uses the identical command as H5.1 with a different `-Dir`:

```
powershell -File selfhost/difftest.ps1 `
  -Dir selfhost/corpus/mir/lifetime -Kinds all -Oracle --mir `
  -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder,mir,mir_builder `
  -ModuleDirs selfhost/parsing,selfhost/hir,selfhost/mir `
  -DriverDir selfhost/mir -Driver mirdrive
```

---

## 5. G7: the statement location

### 5.1 Why it is needed

`checkBorrow` (Decl), `checkReturn` (Return), `checkStore` (ExprStmt) and
`nextcase` all report `s.loc`, the location of the **statement's first
token**; the port's `HStmt` has no location (`selfhost/hir/hir.ivy:36-47`),
so `st_nextcase` reports `0:0` (`mir_builder.ivy:1988`) and the three H5.2
checks would have no correct position to report.  `MIR_NOTES.md` §11.5 sized
this already; the measurements below confirm it and correct two drifted
citations.

### 5.2 The measured sizes

Probe `h52r1\sz.ivy` (`io::println("{}", sizeof(...))`, compiled by the
oracle against the port modules):

| record | today | with G7 | allocation site |
|---|---|---|---|
| `HStmt` | **120** | **136** | `selfhost/hir/hir_builder.ivy:281` (`* 120`) |
| `AStmt` | 136 | 136 | `selfhost/parsing/parser.ivy:304` (`* 136`) |
| `HExpr` | 144 | 144 | `selfhost/hir/hir_builder.ivy:278` (`* 144`) |
| `HParam` | 88 | 88 | `selfhost/hir/hir_builder.ivy:284` (`* 88`) |
| `HCase` / `ACase` | 32 | 32 | `hir_builder.ivy:287`, `parser.ivy:308` |
| `MExpr` / `MInst` | 176 / 136 | - | `mir_builder.ivy:549`, `:554` |
| `AType` | 48 | 48 | - |

`HStmt` is 120 because it is `kind, sval, AType(48), flag, a, b, c, sstart,
sn, next`; two more `long`s give 136, i.e. exactly `sizeof(AStmt)`.  The
only occurrence of the stride in the tree is `hir_builder.ivy:281` (`grep
"\* 120"` also finds `:275`, the `HFunction` pool, which stays 120); the pool
grows by 16/120 = 13.3% (`st_cap = ntoks * 3 + 128`, `:280`).

### 5.3 The five edits

| # | edit | file:line |
|---|---|---|
| 1 | add `long line; long col;` to `HStmt` | `selfhost/hir/hir.ivy:46` (after `next`) |
| 2 | `s.line = line;  s.col = col;` in `new_stmt` | `selfhost/hir/hir_builder.ivy:2147` (before `return s;`) |
| 3 | the statement pool stride `* 120` -> `* 136` | `selfhost/hir/hir_builder.ivy:281` |
| 4 | `st_nextcase`: report `this.sp[idx].line` / `.col` (into scalars inside `unsafe`, then `diag`) | `selfhost/mir/mir_builder.ivy:1981-1988` |
| 5 | the H5.2 checks read `this.sp[idx].line` / `.col` (the **statement's** location) when they call `diag`; the locations the port currently copies from the initializer/condition expression into the instruction (`st_decl :1643-1644`, `st_return :1667-1668`, `st_exprstmt :1689-1690`, `st_switch :2057-2058`) need not change | check sites `st_decl :1638-1649`, `st_return :1667`, `st_exprstmt :1739-1745` |

Edit 5 does **not** change the dump: no instruction location is printed
(`dumpMirBlock`, `src/app/main.cpp:723-767`).  It matters only for the
diagnostics, whose position is the statement's.

The parameters are already correct at the source: `new_stmt(long kind, long
line, long col)` (`hir_builder.ivy:2135-2148`) receives `a.line, a.col` at
**all 19 call sites** (checked by grep; `MIR_NOTES.md` §11.5 says 21, see
9.4), and `AStmt.line`/`col` are set from the statement's first token by the
parser (`ast.ivy:288-289`, e.g. `parser.ivy:2838-2839` for `nextcase`).

### 5.4 The verification (scratch prototype)

The four edits 1-4 were applied to the **scratch copy only**
(`h52r1\src\hir\hir.ivy`, `h52r1\src\hir\hir_builder.ivy`,
`h52r1\src\mir\mir_builder.ivy`), the modules were recompiled, and the
patched driver produced:

| fixture | oracle | patched port |
|---|---|---|
| `61_nextcase_label` | `5:13` | `5:13` |
| `q02_labelled_nextcase_in_loop_in_switch` | `6:17` | `6:17` |
| `q15_nextcase_to_default_label` | `6:13` | `6:13` |

`sizeof(HStmt)` in the patched build is 136.  The HIR dump is unchanged:
`hirdrive` built from the patched copy is byte-identical to `ivy --hir` on
`selfhost/corpus/mir/cfg/01_empty_function.ivy`,
`05_switch_with_default.ivy`, `07_nested_switches.ivy` and
`selfhost/corpus/hir/borrow/03_ret_lt_ref.ivy` (36, 232, 506, 130 bytes).
One compiler restriction was measured on the way: passing
`this.sp[idx].line` directly as an argument is rejected with
`pointer indexing requires an unsafe block`, so the values are read into
scalars first.

---

## 6. The 8 fixtures that need H5.2 (pre-fix evidence)

All eight come from `MIR_NOTES.md` §11.10 / §12.4(a).  Copied to
`h52r1\fx\`; the oracle was run as `ivy -I ... --mir <abs path>` and the port
as `mirdrive.exe` with the absolute path on stdin (hence the absolute prefix
in the captured bytes).

| fixture | oracle stderr (exact, after the path prefix) | oracle | port today |
|---|---|---|---|
| `29_borrow_mut_mut` | `:5:5: error: cannot create a second mutable reference to 'x' — only one mutable borrow at a time` | err 172 B, out 0, exit 1 | dump, out 235 B |
| `30_borrow_mut_shared` | `:5:5: error: cannot create an immutable reference to 'x' — it is already borrowed mutably` | err 168 B, out 0, exit 1 | dump, out 244 B |
| `87_borrow_mut_after_shared` | `:5:5: error: cannot create a mutable reference to 'x' — it is already borrowed immutably` | err 173 B, out 0, exit 1 | dump, out 250 B |
| `32_dangling_return` | `:4:5: error: returning a reference to a local variable (dangling reference)` | err 150 B, out 0, exit 1 | dump, out 194 B |
| `80_lt_mismatch` | `:4:5: error: returned reference has lifetime '$a' but the function declares lifetime '$b'` | err 160 B, out 0, exit 1 | dump, out 226 B |
| `61_nextcase_label` | `:5:13: error: nextcase target 'outer' is not a case label in this switch` | err 146 B, out 0, exit 1 | diagnostic but `0:0`, out 227 B |
| `q02_labelled_nextcase_in_loop_in_switch` | `:6:17: error: nextcase target 'one' is not a case label in this switch` | err 166 B, out 0, exit 1 | `0:0`, out 269 B |
| `q15_nextcase_to_default_label` | `:6:13: error: nextcase target 'two' is not a case label in this switch` | err 156 B, out 0, exit 1 | `0:0`, out 249 B |

The shapes (source of each): `29` = `int32 x; int32& a = x; int32& b = x;`;
`30` = `int32& a = x; const int32& b = x;`; `87` = `const int32& a = x;
int32& b = x;`; `32` = `fn f() -> const int32& { int32 x = 1; return x; }`;
`80` = `lifetime<$a, $b> fn f(const int32& $a x) -> const int32& $b { return
x; }`; the three `nextcase` fixtures are a labelled `nextcase` out of an
unlabelled switch, out of a loop inside a switch, and to a label that only
the `default` clause could have had.

`--hir` on each of the eight prints the HIR dump **and** the diagnostic
(151/157/157/112/140/178/464/176 bytes of stdout), which proves all eight are
MIR-stage, not HIR-stage.

The control `01_empty.ivy` (`void f() {}`) matches today: oracle 42 bytes,
port 123 bytes including the 80-byte `=== path ===` marker line.

---

## 7. The fixture inventory for `selfhost/corpus/mir/lifetime/`

The 8 legacy fixtures pin only messages 1-5 and 11.  The probes below were
run on the oracle (`--mir`, exit and byte counts recorded) and are proposed
as the additional corpus, because messages 6 and 7 have no legacy fixture and
because the *negative* cases (scope release, unsafe bypass, named-lifetime
match, two shared borrows) guard against over-checking:

| proposed file | source shape | oracle | pins |
|---|---|---|---|
| `29_borrow_mut_mut.ivy` | two mutable refs to one local | err 172, exit 1 | 2 |
| `30_borrow_mut_shared.ivy` | mut then `const` ref | err 168, exit 1 | 3 |
| `87_borrow_mut_after_shared.ivy` | `const` ref then mut | err 173, exit 1 | 1 |
| `30b_mut_shared_mut.ivy` (`q6`) | mut, shared, mut: **two** lines | err 357, exit 1 | 3 then 2, multiplicity |
| `30c_param_mut_mut.ivy` (`p2`) | two mut refs to a *parameter* | err 171, exit 1 | 2 + the parameter fallback of 2.2 |
| `32_dangling_return.ivy` | `return <local>` | err 150, exit 1 | 4 |
| `80_lt_mismatch.ivy` | `$a` parameter returned as `$b` | err 160, exit 1 | 5 |
| `80b_lt_match_ok.ivy` (`q7`) | `$a` returned as `$a` | out 145, exit 0 | no false positive on Named |
| `80c_unknown_ret_plain.ivy` (`q1`) | `return g();`, no declared lifetime | err 204, exit 1 | 6 |
| `80d_unknown_ret_annotated.ivy` (`q2`) | `return g();`, declared lifetime | err 181, exit 1 | 7 |
| `32b_dangling_two.ivy` (`p1`) | two functions, both dangling | err 294, exit 1 | order + multiplicity (3:5 then 8:5) |
| `61_nextcase_label.ivy` | labelled nextcase, unlabelled switch | err 146, exit 1 | 11 + G7 |
| `61b_nextcase_in_loop.ivy` (`q02`) | loop in switch | err 166, exit 1 | 11 + G7 |
| `61c_nextcase_to_default.ivy` (`q15`) | label that `default` cannot have | err 156, exit 1 | 11 + G7 |
| `90_borrow_scope_release.ivy` (`q3`) | two mut borrows in sibling blocks | out 153, exit 0 | the release on scope pop |
| `90b_two_shared_ok.ivy` (`q8`) | two `const` refs | out 165, exit 0 | `sharedCount` increments |
| `90c_unsafe_borrow.ivy` (`p10`) | two mut refs inside `unsafe` | out 171, exit 0 | the unsafe bypass |
| `90d_unsafe_return.ivy` (`q5`) | dangling return inside `unsafe` | out 120, exit 0 | the unsafe bypass of `checkReturn` |

That is **18 files** (8 legacy + 10 new), all with measured oracle evidence.
No fixture can pin messages 8-10 (section 9.1); they stay uncovered
on purpose, and that must be said in the corpus inventory comment.

Naming: `PLAN.md:781` fixes the directory as `selfhost/corpus/mir/lifetime`;
the `NN_` prefix follows the existing style of `corpus/mir/cfg/` (75 files).
`90*` is used for the dump (no-diagnostic) guards, `80*` for the return
lifetime family, `61*` for the `nextcase` family, `29/30/87/32` keep their
historical names because `MIR_NOTES.md` §11.10/§12.4(a) cite them.

---

## 8. The sub-phase plan and the expected counts

| step | the work | the evidence that closes it |
|---|---|---|
| S0 (G7) | the 5 edits of 5.3 | patched scratch port reproduces `5:13` / `6:17` / `6:13`; `sizeof(HStmt) == 136`; HIR dump unchanged on 4 files |
| S1 (state) | `inAssignLhs_`, the borrow-state pool (name, shared, mut), the active-borrower pool (ref, source, is_mut, released), the move-state pool (name, state), all pushed/popped in `push_scope`/`pop_scope`; extend `release()` | compiles; the 75 CFG fixtures stay byte-identical (no check has fired yet) |
| S2 (borrow) | `check_borrow` + `register_borrower` + the `release` on scope pop; call sites `st_decl` (ref initializer), `ex_unary` (`&ident`) | the 5 borrow fixtures of §7 (3 legacy + 2 new) |
| S3 (return) | `check_return`; call site `st_return` | the 6 return fixtures of §7 (2 legacy + 4 new) |
| S4 (store) | `check_store`; call site `st_exprstmt` | no fixture (9.1): verified by reasoning + the absence of a reachable shape; keep it, document it |
| S5 (move) | `is_moved_out` / `mark_moved_out` / `reinitialize`; call sites `ex_ident`, `ex_call`, `ex_assign`, `st_decl` | no fixture (9.1): 60/60 HIR move fixtures already fail at HIR; keep the code for parity, do not claim corpus coverage |
| S6 (corpus) | the 18 files of §7 in `selfhost/corpus/mir/lifetime/` | the new suite PASSes, the 27 existing suites stay PASS |

Expected counts: the 27 existing suites are **687 + 75 = 762 files**
(`PLAN.md:690-691`, `MIR_NOTES.md` §13.11); the new suite is 18 files, so the
end state of S6 is **28 suites / 780 files byte-identical**.  Nothing in
`src/**` and nothing in `selfhost/difftest.ps1` changes; the harness
invocation is the H5.1 one with `-Dir selfhost/corpus/mir/lifetime`.

The natural commit boundary is S0-S3 + S6 (a matchable milestone), with S4
and S5 folded in because they are four and twelve lines and cannot be
verified by corpus - if the reviewer prefers the smallest possible change,
S4/S5 can be dropped without losing any measured coverage, at the cost of a
divergence from the oracle that no fixture can see.

---

## 9. Gaps, oracle bugs, contradictions found during the recon

### 9.1 The dead checks (message 8; messages 9-10)

Message 8 (`checkStore`) and messages 9/10 (MIR move) cannot be triggered by
a program that reaches the MIR stage.  Evidence:

* Local lifetimes are produced only by `&<local>` (`mir_builder.cpp:503`) and by the
  synthesis inside `checkReturn` (`:251-252`, which does not modify the
  expression).  `&<local>` is a raw pointer outside `unsafe`, and the HIR
  rejects that (`raw pointer variable declaration requires an unsafe block`,
  `address-of operator '&' taking raw pointer requires an unsafe block`,
  measured on `p3`, `p4`, `p5`), while inside `unsafe` both `checkStore` and
  `checkReturn` return immediately (`:299`, `:231`).
* The MIR move tracker is a strict subset of the HIR one (same scopes, same
  `move()` shape, and the HIR one has **no** unsafe bypass).  Measured: the
  `--hir` and `--mir` verdicts agree on all 60 files of
  `selfhost/corpus/hir/move/`, and `p6`/`p7` produce their diagnostics with
  **0 bytes of stdout**, i.e. at the HIR stage.

Consequence for the port: implement both for fidelity (the code is small and
the messages exist in the oracle's binary), but do not invent fixtures and do
not claim coverage.

### 9.2 Oracle bug: a call result is always `Unknown`

`buildExpr` for a non-`move` call sets the lifetime in the `if
(e.type.pointerDepth > 0)` branch (`:595-602`) and then **unconditionally**
overwrites it with `Unknown` at `:603`.  So a call whose target declares a
return lifetime is still `Unknown`, and messages 6/7 fire for reference
returns through a call.  The port already mirrors the quirk
(`ex_call :1363`), and `q1`/`q2` depend on it; **do not "fix" it**.

### 9.3 Unreachable asymmetry: `&ident` counts a borrow that is never released

See 3.4.  Faithful behaviour is: the count from `&x` is permanent for the
function, but no valid program can observe it outside `unsafe`.

### 9.4 Documentation drift found and fixed

All three citations were fixed in H5.2 (`MIR_NOTES.md` 11.5):

* `MIR_NOTES.md` 11.5 cited `HStmt new_stmt(...)` at `:2129`; the pre-H5.2
  position was `:2135-2148`, after the two G7 lines the body is `:2135-2150`.
* 11.5 said "21 call sites"; `grep` finds **19** calls to `this.new_stmt(`.
* 11.5 said the port's G7 is "five edits" - correct, but it listed the
  `nextcase` change as `:1988`; after edits 1-3 it was `:1981-1988`, and in
  H5.2 it is `:2710-2723`.
* `selfhost/mir/MIR_NOTES.md` was 3209 lines before the H5.2 append
  (`[IO.File]::ReadAllBytes`: 3208 `LF`, no `CR`); after §14 it is 3287
  lines (3286 `LF`, no `CR`).  PowerShell's `Measure-Object -Line` reported
  2737 for the pre-append file, so that cmdlet must not be used for line
  counts in the later phases; `(Get-Content $f).Count` and the read tool
  agree with the byte count.
* The 8-fixture count is consistent everywhere: `PLAN.md:687-689`, `MIR_NOTES.md` §11.10 (eight `(d) H5.2` rows) and §12.4(a) all name the same eight files.  No drift there.

### 9.5 An oracle behaviour worth knowing for the corpus

`--hir` **continues** to the MIR stage after dumping the HIR, so a corpus
file used by the HIR suites must be MIR-clean.  The eight H5.2 fixtures
therefore cannot be reused in `corpus/hir/*`; the new directory must be
`corpus/mir/lifetime` (as `PLAN.md:781` says).

---

## 10. Harness notes

* The build is `-I selfhost/parsing -I selfhost/hir -I selfhost/mir`
  (`difftest.ps1:183-184`), module order
  `token,lexer,preprocessor,ast,parser,hir,hir_builder,mir,mir_builder`
  (`:190-202`), driver `mirdrive` (`:203-205`).  The `.obj`/`.ivm` products
  are written next to the sources and are covered by `.gitignore`
  (`*.obj`, `*.ivm`, `*.exe`), so a harness run never dirties `git status`.
* `difftest.ps1` calls `Set-Location $repo` (`:167`); a recon or verification
  run must not rely on that and must use `workdir` instead.
* The oracle call is per file with `> one 2>&1` (`:288`); the port call is
  one process over the whole list (`:298`).  Consequence: the port's state
  must be reset per file.  It is: `main` builds a fresh `MirBuilder` per
  file in `run()` (`mirdrive.ivy:630-652`), and the HIR/HIR builder objects
  are local to the loop body.
* The expected file gets the marker from PowerShell (`:289`), the port emits
  its own (`:544`), and the filter keeps every `=== ` line (`:258-260`).
  Lines are compared as raw bytes (`:227-231`), so the em dash is safe.
* Beware of tool text output: it is lossy for non-ASCII (U+2014 is shown as
  `-`, U+00A7 disappears).  Every byte count and every message in this file
  was verified with `[IO.File]::ReadAllBytes` / `ReadAllText` and hex dumps,
  never by eyeballing captured text; keep that discipline when verifying the
  port's em dash.
* A diagnostic fixture is a *normal* PASS: the expected text is one line
  `path:L:C: error: msg` per diagnostic, and the comparison is just as
  byte-exact as a dump.  `-Kinds all` keeps everything.
* During the recon the port was rebuilt from a scratch copy with
  `ivy <mod>.ivy -c -o <scratch>/<mod>.obj` and then
  `ivy -I ... mirdrive.ivy -o <scratch>/mirdrive.exe`.  **The `.ivm` is
  written next to the `-o` path**, not next to the source, so the `-o` of a
  module must stay in a directory that is on `-I` (otherwise the next module
  fails with `module '<x>' not found`).  This is the only build surprise
  encountered.

Bookkeeping of this recon: no file under `src/**` was modified, no existing
file was edited, nothing was rebuilt in the tree, no commit was made, and the
only repository file created is this one.  The scratch directory
`C:\Users\ASUS\AppData\Local\Temp\opencode\h52r1\` holds the port copy, the
probes and the captured bytes; nothing under
`C:\Users\ASUS\.local\share\opencode\` was read or written.
