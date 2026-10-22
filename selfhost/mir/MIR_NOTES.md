# H5.1 (MIR lowering: HIR -> MIR) - phase-1 working notes

## 0. The state of this file

This file collects the notes of milestone **H5.1** (the port of the oracle's
MIR construction - basic blocks, instructions, terminators, the CFG - to the
self-host port).

**This is phase 1 of 4** (research only).  Phases 2-4 implement, add the
corpus and verify.  Nothing in `src/**` was touched, nothing in
`selfhost/**` was touched except the creation of this file, no commit was
made and the oracle was not rebuilt.

### 0.1 The ground truth

| item | value |
|---|---|
| repo | `D:\project\Ivy\ivyc` (drive `D:`) |
| HEAD | `0cbcd31` (`selfhost: fix missing "not assignable" diagnostic after a failed member LHS`) |
| working tree | clean at the start of the phase (`git status --porcelain` empty); at the end the only entry is the new untracked `selfhost/mir/` |
| oracle | `build\Release\ivy.exe`, 27031552 bytes, mtime **2026-10-21 16:41:44** (unchanged) |
| scratch | `C:\Users\ASUS\AppData\Local\Temp\opencode\h51r1\` |

The scratch directory of this phase:

| path | what |
|---|---|
| `h51r1\fx\` | the 91 phase-1 fixtures (the sources) |
| `h51r1\out\` | the raw oracle captures of the `--mir` run: `<name>.out.bin` (stdout) and `<name>.err.bin` (stderr) |
| `h51r1\port\` | a **copy** of `selfhost/parsing` + `selfhost/hir`, compiled with the oracle (the port's HIR driver, used to measure the HIR-fidelity gaps).  Built outside the tree so that no build product is written into `selfhost/**` |
| `h51r1\portout\` | the port's `--hir` capture (`hirdrive.bin`) and the oracle's `--hir` capture per fixture (`oracle_<name>.bin`), used for the gap analysis |
| `h51r1\list.txt` | the fixture list (one absolute path per line) |
| `h51r1\gaplist.txt` | the 13 fixtures whose `--hir` output differs between the oracle and the port |

The captures were made with `cmd /c "... > file 2> file"` (raw byte
redirection), never with PowerShell text capture.

### 0.2 A correction to the task brief

The brief says that for a MIR-stage diagnostic "the oracle prints the dump
FIRST and the diagnostic AFTER, so the port can never match it byte-for-byte
(the same constraint as the HIR stage)".  **That is not what the oracle
does.**  In `src/app/main.cpp`:

```
1315:  ivy::MirBuilder mirBuilder(*hir);
1316:  std::unique_ptr<ivy::mir::TranslationUnit> mir = mirBuilder.build();
1317:  for (const ivy::Diagnostic& d : mirBuilder.diagnostics()) { ... std::cerr ... failed = true; }
1321:  if (failed) return 1;
1323:  if (showMir && mir) dumpMir(*mir, std::cout);
```

`MirBuilder::build()` returns `nullptr` when a diagnostic was reported
(`mir_builder.cpp:1221`), so `dumpMir` is **never** reached and the MIR
dump is **not** printed.  Measured on the fixture `29_borrow_mut_mut.ivy`:
stdout = **0 bytes**, stderr = 172 bytes, exit code = 1.

The "dump first, diagnostic after" behaviour is the **HIR** stage
(`main.cpp:1313` prints the HIR dump, then `main.cpp:1315` runs the MIR
builder, whose diagnostics land on stderr afterwards).  That constraint
concerns `ivy --hir`, not `ivy --mir`.

**Consequence for H5.1**: a MIR-stage diagnostic *is* matchable byte for
byte - the port has to emit the same diagnostic text and print **no** MIR
dump.  Only the *diagnostics themselves* (the borrow checker, the lifetime
checker, the `nextcase` check) are H5.2 work; the *shape* of the output
(diagnostic, no dump) is already H5.1.

---

## 1. The oracle's MIR data model (`src/mir/mir.h`, `src/mir/mir_builder.h`)

### 1.1 Tag legend

* **H5.1** - must be in the port for the `--mir` dump to be byte-identical.
* **H5.2** - the lifetime/borrow/move *analyses* and the diagnostics they
  produce; deferred to the next milestone.
* **H5.3+** - needed only by the code generator / interpreter, not by the
  `--mir` dump.

### 1.2 `mir::Lifetime` (`src/mir/mir.h:22-32`)

```cpp
struct Lifetime {
    enum class Kind {
        None,     // 0  non-pointer value (or nullptr): no lifetime to track
        Named,    // 1  tied to a [[ivy::lt_def]] lifetime; name is valid
        Static,   // 2  string literal: lives forever
        Local,    // 3  points into a local variable's storage
        Unknown,  // 4  opaque (extern "C" call result, unannotated pointer param)
    };
    Kind kind = Kind::None;
    std::string_view name;  // valid when kind == Kind::Named
};
```

**The numbering matters**: the port must mirror `None=0, Named=1, Static=2,
Local=3, Unknown=4` (or use its own constants, as long as the *dump*
mapping is reproduced - see 3.3).

| element | tag | note |
|---|---|---|
| `Kind` enum + its order | **H5.1** | the dump prints the *name* of the kind, but the port should keep the same integers for clarity |
| `kind` | **H5.1** | the `[lt: X]` suffix is derived from it |
| `name` | **H5.1** | printed as `[lt: <name>]` for `Named` |

The `Lifetime` field of an expression is the *result of the lifetime
analysis*: **H5.2 in the oracle's own classification, but H5.1 in practice**
- the dump prints it, so the port cannot produce a byte-identical dump
without it.  See 7.4 for the exact scope decision.

### 1.3 `mir::Expr` (`src/mir/mir.h:37-95`)

A `std::variant` with **20 alternatives, in this exact order**:

| # | alternative | fields | tag |
|---|---|---|---|
| 0 | `IntegerLit` | `long long value` | H5.1 |
| 1 | `FloatLit` | `double value` | H5.1 |
| 2 | `StringLit` | `std::string_view raw` (with quotes), `std::string decoded` (filled by the back-fill pass) | H5.1 (raw) / H5.3 (decoded) |
| 3 | `CharLit` | `std::string_view raw`, `long long decoded = 0` | H5.1 (raw) / H5.3 (decoded) |
| 4 | `BoolLit` | `bool value` | H5.1 |
| 5 | `NullptrLit` | - | H5.1 |
| 6 | `IdentRef` | `std::string_view name` | H5.1 |
| 7 | `This` | - | H5.1 |
| 8 | `Unary` | `std::string_view op`, `bool isPrefix`, `Expr* operand` | H5.1 |
| 9 | `Binary` | `std::string_view op`, `Expr* lhs`, `Expr* rhs` | H5.1 |
| 10 | `Ternary` | `Expr* cond`, `Expr* thenBranch`, `Expr* elseBranch` | H5.1 |
| 11 | `Call` | `std::string_view callee`, `const Function* target = nullptr`, `std::string_view returnLifetime`, `std::vector<Expr*> args`, `bool isVirtual = false`, `std::size_t vtableSlot = 0`, `std::string_view methodName` | H5.1 (`callee`, `args`) / H5.3 (`target`, `returnLifetime`) / H5.1 (`isVirtual`, `vtableSlot`, `methodName` - they are **printed**) |
| 12 | `Index` | `Expr* base`, `Expr* index` | H5.1 |
| 13 | `Member` | `Expr* base`, `std::string_view name`, `bool isArrow`, `bool isScope = false` | H5.1 (`base`, `name`, `isArrow`) / H5.3 (`isScope` - not printed) |
| 14 | `Assign` | `std::string_view op`, `Expr* lhs`, `Expr* rhs` | H5.1 |
| 15 | `New` | `Type type`, `std::vector<Expr*> args` | H5.1 (printed) - **unreachable in the port today** (no `new` in the port's parser) |
| 16 | `Delete` | `Expr* operand`, `bool isArray` | H5.1 (printed) - same |
| 17 | `InitList` | `std::vector<Expr*> elements` | H5.1 |
| 18 | `Lambda` | `std::string_view funcName`, `std::string_view closureType`, `std::vector<Expr*> captureInits` | dead - the oracle crashes before the MIR stage on a lambda (fixture `37_lambda`) |
| 19 | `Cast` | `CastKind kind` (`ivy::Expr::CastKind` = `Static=0, Reinterpret=1, Const=2, CStyle=3`), `Expr* operand` | H5.1 |

The enclosing record:

```cpp
Type type;         // the resolved type (copied from the HIR expr)
Lifetime lifetime; // the lifetime annotation
SourceLoc loc;
std::variant<...> node;
```

| element | tag | note |
|---|---|---|
| `type` | **H5.1** | printed after every `:` in the dump |
| `lifetime` | **H5.1** (data) / **H5.2** (analysis) | printed as the ` [lt: X]` suffix |
| `loc` | **H5.2** | only used by the diagnostics |

**Important**: the port's HIR `HExpr` has **no** `kind` field for `New`,
`Delete`, `Lambda`, `PackExpansion`, `FoldExpr`, `SizeofPack` and **no**
`isVirtual`/`vtableSlot`/`methodName` on a call - see section 6.

### 1.4 `mir::Param` (`src/mir/mir.h:97-102`)

```cpp
struct Param {
    Type type;
    std::string_view name;      // empty if unnamed
    std::string_view lifetime;  // lowered [[ivy::lt(a)]]
    SourceLoc loc;
};
```

| element | tag | printed as |
|---|---|---|
| `type` | H5.1 | `<type>` |
| `name` | H5.1 | ` <name>` (only if non-empty) |
| `lifetime` | H5.1 | ` [lt:<name>]` - **note: no space after the colon**, unlike the expression suffix |
| `loc` | H5.2 | - |

### 1.5 `mir::Inst` (`src/mir/mir.h:106-128`)

```cpp
struct Inst {
    enum class Kind { Alloca, Store, Eval, Ret, CondBranch, Jump, Switch };
    //  0        1     2    3   4            5      6
```

| # | alternative | fields | printed by `dumpMirBlock`? |
|---|---|---|---|
| 0 | `Alloca` | `std::string_view var`, `Type type`, `Expr* init` (may be null) | yes: `alloca <type> <var>` + the init expr |
| 1 | `Store` | `Expr* target`, `Expr* value` | yes: `store` + target + value |
| 2 | `Eval` | `Expr* value` | yes: `eval` + value |
| 3 | `Ret` | `Expr* value` (may be null) | yes: `ret` + value |
| 4 | `CondBranch` | `Expr* cond`, `Block* thenBlock`, `Block* elseBlock` | yes: `condbranch` + cond (**the targets are NOT printed**) |
| 5 | `Jump` | `Block* target` | yes: `jump` (**the target is NOT printed**) |
| 6 | `Switch` | `Expr* cond`, `std::vector<SwitchArm> arms`, `Block* defaultBlock` | **NO - the switch instruction prints NOTHING** |

`SwitchArm { long long value; Block* block; }` (`mir.h:117`).

The enclosing record:

```cpp
Kind kind;
bool inUnsafe = false;   // the instruction was built inside an [[ivy::unsafe]] block
SourceLoc loc;
std::variant<Alloca, Store, Eval, Ret, CondBranch, Jump, Switch> node;
```

| element | tag | note |
|---|---|---|
| `kind` | H5.1 | selects the printed line |
| `inUnsafe` | H5.1 | printed as the ` [unsafe]` suffix on the instruction line |
| `loc` | H5.2 | - |

**The "scope" / "slot" concept.**  The task brief mentions an
"alloca vs eval" slot distinction.  In the oracle there is no separate slot
structure: a *local variable* becomes an **`Inst::Alloca`** (the variable's
storage plus its optional initializer) and a *value that is computed but not
stored* becomes an **`Inst::Eval`**.  An assignment to an existing lvalue
becomes an **`Inst::Store`**.  The "scope" concept lives in the *builder*,
not in the model: `scopes_` is a stack of `name -> Lifetime` maps
(`mir_builder.h:45`), pushed/popped by `Compound` and `For`.

### 1.6 `mir::Block` (`src/mir/mir.h:130-132`)

```cpp
struct Block {
    std::vector<std::unique_ptr<Inst>> insts;
};
```

A block has **no** label, no predecessors and no successors list: the CFG
edges live in the instructions (`CondBranch::thenBlock/elseBlock`,
`Jump::target`, `Switch::arms[i].block`, `Switch::defaultBlock`).  A block
is identified by its **index in `Function::blocks`** - that index is what
the dump prints (`block 0`, `block 1`, ...).

| element | tag |
|---|---|
| `insts` | H5.1 |

### 1.7 `mir::Function` (`src/mir/mir.h:134-153`)

```cpp
struct Function {
    std::string_view name;            // the QUALIFIED name: "math::abs", "Counter::get"
    std::string_view namespacePrefix;  // "ns1::ns2::" or ""
    Type returnType;
    std::vector<Param> params;
    std::vector<Lifetime> lifetimes;   // lowered [[ivy::lt_def(a)]]
    std::string_view returnLifetime;   // lowered [[ivy::lt_ret(a)]]
    std::vector<std::unique_ptr<Block>> blocks;
    bool isExternC = false;
    bool hasBody = false;
    bool isVariadic = false;
    bool isConstexpr = false;
    bool isConsteval = false;
    bool isCtor = false;
    bool isDtor = false;
    bool isVirtual = false;
    bool isPureVirtual = false;
    std::string_view vtableOf;
    SourceLoc loc;
};
```

| element | tag | printed as |
|---|---|---|
| `name` | H5.1 | `function <name>` (qualified) |
| `namespacePrefix` | H5.3 | not printed (the name already carries it) |
| `returnType` | H5.1 | `  return <type>` |
| `params` | H5.1 | `(<type> <name> [lt:<l>], ...)` |
| `lifetimes` | H5.1 | `  lifetimes: a b` |
| `returnLifetime` | H5.1 | `  return lifetime: a` |
| `blocks` | H5.1 | the `block N` sections |
| `isExternC` | H5.1 | ` [extern "C"]` after the name |
| `hasBody` | H5.1 | a bodyless function has **no** `block` lines at all |
| `isVariadic`, `isConstexpr`, `isConsteval`, `isCtor`, `isDtor`, `isVirtual`, `isPureVirtual`, `vtableOf`, `loc` | H5.3 | not printed |

**The `this` parameter** is a *real* parameter of the MIR function: a
method is dumped as `function Counter::get` / `  return int32_t(Counter& this)`
(fixture `23_this`).  The `this` parameter is injected by the **HIR**
builder, not by the MIR builder, so the port's HIR already has it.

### 1.8 `mir::EnumInfo` / `mir::StructInfo` / `mir::TranslationUnit`

* `EnumInfo` (`mir.h:159-164`): `name`, `namespacePrefix`, `isScoped`,
  `underlyingBase` - **H5.3**, `dumpMir` does not print the enums.
* `StructInfo` (`mir.h:174-193`): `name`, `namespacePrefix`, `fields`
  (ordered), `bases`, `isPolymorphic`, `vtable` - **H5.3**, not printed.
* `TranslationUnit` (`mir.h:195-212`): `functions`, `enums`, `structs` and
  the `hasMain()` helper - `functions` is **H5.1** (the dump iterates it in
  order), the rest is **H5.3**.

The `StructInfo` construction (`mir_builder.cpp:1087-1197`) is a sizeable
chunk of code (base-class flattening, the polymorphic flag, the vtable with
the `~ClassName` -> `~dtor` normalisation).  **None of it is printed**, so
H5.1 can skip it entirely and H5.3 can port it later.

### 1.9 The builder's private state (`src/mir/mir_builder.h:32-136`)

| member | purpose | tag |
|---|---|---|
| `hir_`, `mir_`, `diagnostics_`, `failed_` | the plumbing | H5.1 |
| `current_`, `cur_`, `unsafeDepth_` | the current function / block / unsafe nesting | H5.1 |
| `params_` | `name -> Lifetime` for the parameters | H5.1 |
| `scopes_` | the stack of `name -> Lifetime` for the locals | H5.1 |
| `borrowScopes_`, `BorrowEntry`, `BorrowState`, `BorrowScope` | the borrow checker | **H5.2** |
| `moveStates_`, `MoveState`, `inAssignLhs_` | the move tracker | **H5.2** (and unreachable - see 5.3) |
| `loops_` (`LoopCtx`), `caseIndex_` | the break/continue/nextcase context | H5.1 |
| `checkBorrow`, `registerBorrower`, `releaseBorrower`, `releaseBorrowsInScope` | the borrow checker | **H5.2** |
| `markMovedOut`, `isMovedOut`, `reinitialize` | the move tracker | **H5.2** |
| `checkReturn`, `checkStore` | the lifetime checker | **H5.2** |
| `resolveCalls`, `decodeLiterals`, `walkExpr` | the post-pass (call-target resolution + literal decoding) | H5.3 (the post-pass results are not printed) |
| `cloneExpr` | the deep clone used by the compound-assignment expansion | **H5.1** (it *is* visible in the dump) |

---

## 2. The lowering (`src/mir/mir_builder.cpp`, 1401 lines)

### 2.1 The entry point

`MirBuilder::build()` (`mir_builder.cpp:1078-1242`):

1. create the `mir::TranslationUnit`;
2. copy the enum info (`:1081-1084`) and the struct info (`:1087-1197`) -
   **H5.3**, not printed;
3. for every `hir::Function` (`:1198-1220`): create a `mir::Function`, copy
   the name / namespace / return type / params / flags / loc, **push it into
   `mir_->functions` BEFORE building its body** (so that the functions are
   in HIR order in the dump) and then call `buildFunction()` if the HIR
   function has a body;
4. `if (failed_) return nullptr;` (`:1221`);
5. the post-pass (`:1222-1240`): walk every instruction's expressions and
   resolve the call targets / decode the string and char literals -
   **H5.3**, not printed.

`MirBuilder::buildFunction()` (`mir_builder.cpp:1045-1076`):

1. reset the per-function state (`params_`, `scopes_`, `loops_`,
   `borrowScopes_`, `moveStates_`);
2. copy the declared lifetimes into `fn.lifetimes` as `Named` (`:1054-1056`)
   and the return lifetime (`:1057`);
3. fill `params_` from the HIR params (`:1058-1066`): a param with a
   declared lifetime gets `Named`, a param with `pointerDepth > 0` and no
   declared lifetime gets `Unknown`, everything else gets `None`;
4. push the function-level scope / borrow scope / move scope (`:1068-1070`);
5. `cur_ = newBlock();` - **the entry block is block 0** (`:1071`);
6. build every statement of the HIR body (`:1072`).

### 2.2 The block / instruction primitives

| function | line | what |
|---|---|---|
| `newBlock()` | `:148-153` | appends a fresh `mir::Block` to `current_->blocks` and returns its raw pointer.  **The block's index in that vector is its identity** |
| `emit(kind, loc)` | `:155-163` | appends a fresh `mir::Inst` to `cur_->insts`, sets `inUnsafe = (unsafeDepth_ > 0)` |
| `jumpTo(target)` | `:165-173` | **if the last instruction of `cur_` is a `Ret`, emits NOTHING**; otherwise emits a `Jump` |
| `lookup(name)` | `:175-183` | searches `scopes_` from the innermost outwards, then `params_`; returns `None` if not found |
| `declare(name, lt)` | `:185-187` | `scopes_.back().emplace(name, lt)` |

### 2.3 The statement lowering (`buildStmt`, `mir_builder.cpp:704-1041`)

The order of the block creation is **the order of the dump** - it is
normative.

| HIR statement | line | blocks created (in order) | instructions emitted |
|---|---|---|---|
| `Compound` | `:708-720` | none | the statements of the compound, in order.  Pushes a `scopes_` / borrow / move scope and pops it at the end |
| `Null` | `:721-723` | none | nothing |
| `Decl` | `:724-751` | none | **`Alloca`** (`var`, `type`, `init` = the built initializer).  `declare(name, init->lifetime)`.  If the declared type is a reference and the initializer is an `IdentRef`, the borrow checker runs (`:735-743`) - **H5.2** |
| `If` | `:752-784` | **`cont`**, then `thenB`, then `elseB` (a missing branch reuses `cont`) | `CondBranch(cond, thenB, elseB)` in the current block; then the then-branch (ending with `jumpTo(cont)`), then the else-branch (ending with `jumpTo(cont)`).  `if constexpr` folded to a constant with no branch at all -> `jumpTo(cont)` only |
| `While` | `:785-806` | `condB`, `bodyB`, `exitB` | `jumpTo(condB)` in the current block; `CondBranch(cond, bodyB, exitB)` in `condB`; the body in `bodyB` (ending with `jumpTo(condB)`) |
| `DoWhile` | `:807-828` | `bodyB`, `condB`, `exitB` | `jumpTo(bodyB)`; the body in `bodyB` (ending with `jumpTo(condB)`); `CondBranch(cond, bodyB, exitB)` in `condB` |
| `For` | `:829-870` | the **init is built first, in the current block**, then `condB`, `bodyB`, `incrB`, `exitB` | `jumpTo(condB)`; `CondBranch` in `condB` (or `jumpTo(bodyB)` when there is no condition); the body in `bodyB` (ending with `jumpTo(incrB)`); an **`Eval`** with the increment expression in `incrB` (only when there is an increment), then `jumpTo(condB)` |
| `Return` | `:871-880` | none | **`Ret`** with the value (or without one) |
| `Break` | `:881-884` | none | `jumpTo(loops_.back().exit)` |
| `Continue` | `:885-891` | none | `jumpTo(loops_.back().incr ? incr : cond)` |
| `NextCase` | `:893-919` | none | unlabelled: `jumpTo(caseBlocks[caseIndex_+1])`, or `jumpTo(loop.exit)` for the last case; labelled: `jumpTo(caseLabels[target])`, or the error `nextcase target '<t>' is not a case label in this switch` |
| `ExprStmt` | `:920-975` | none | if the expression is an `Assign` -> **`Store`** (target, value) with the compound-assignment expansion (`:930-950`); otherwise **`Eval`** with the expression |
| `Unsafe` | `:976-981` | none | `++unsafeDepth_`, build the body, `--unsafeDepth_`.  Every instruction emitted inside gets `inUnsafe = true` |
| `Switch` | `:982-1040` | **`exitB`**, then **one block per case clause** (in the order of the clauses) | a **`Switch`** instruction in the current block (cond, the arms of the non-default cases, `defaultBlock` = the default clause's block, or `exitB` when there is no `default`); then each case body is built in its own block, each ending with `jumpTo(exitB)` |

Notes that matter for the dump:

* **`Switch` prints nothing** (`main.cpp:726-765` has no `case Kind::Switch`),
  but the *blocks* it creates are printed (they are empty or hold the case
  bodies).
* The `Switch` **default block** is the block of the `default` clause; if
  there is no `default` clause it is `exitB`.
* The case **values** are read from the HIR: an `IntegerLit` gives its
  value, a `BoolLit` gives 0/1, anything else gives 0 (`:1013-1019`).
  The values are not printed either.
* `caseIndex_` is the index of the case clause currently being built; it is
  what makes the unlabelled `nextcase` work.

### 2.4 The expression lowering (`buildExpr`, `mir_builder.cpp:434-700`)

`out->loc = e.loc; out->type = e.type;` for every expression, then one
branch per HIR alternative:

| HIR alternative | line | MIR node | the lifetime of the result |
|---|---|---|---|
| `IntegerLit` | `:441-444` | `IntegerLit` | `None` |
| `FloatLit` | `:445-448` | `FloatLit` | `None` |
| `StringLit` | `:449-453` | `StringLit{raw}` | **`Static`** |
| `CharLit` | `:454-457` | `CharLit{raw}` | `None` |
| `BoolLit` | `:458-461` | `BoolLit` | `None` |
| `NullptrLit` | `:462-465` | `NullptrLit` | `None` |
| `This` | `:466-470` | `This` | `None` |
| `IdentRef` | `:471-482` | `IdentRef` | `lookup(name)` |
| `Unary` | `:483-530` | `Unary` | see below |
| `Binary` | `:531-544` | `Binary` | for `+`/`-`: the pointer operand's lifetime, else `None` |
| `Ternary` | `:545-555` | `Ternary` | `mergeLifetime(then, else)` |
| `Call` | `:556-605` | `Call` | see below |
| `Index` | `:606-613` | `Index` | the base's lifetime |
| `Member` | `:614-625` | `Member` | the base's lifetime |
| `Assign` | `:626-646` | `Assign` | the right-hand side's lifetime |
| `New` | `:647-654` | `New` | `Unknown` |
| `Delete` | `:655-661` | `Delete` | the operand's lifetime |
| `InitList` | `:662-670` | `InitList` | `None` |
| `Lambda` | `:671-683` | `Lambda` | `None` |
| `Cast` | `:686-696` | `Cast` | the operand's lifetime |
| anything else | `:698-699` | **the default-constructed variant = `IntegerLit{0}`** | `Unknown` |

The `Unary` cases (`:490-529`):

* `"&"` (`:490-519`): if the operand is an `IdentRef`, the lifetime is
  `Local` when the name is a **local** (found in `scopes_`), otherwise the
  parameter's lifetime (`params_`), otherwise `None`.  If the operand is
  **not** an `IdentRef`, the lifetime is `Unknown`.  The borrow checker
  runs here (`:514`) - **H5.2**.
* `"*"` (`:520-523`): the operand's lifetime (the pointee borrows the
  pointer's lifetime).
* `"++"` / `"--"` (`:524-528`): the operand's lifetime if the operand is a
  pointer, else `None`.
* anything else (`-`, `+`, `!`, `~`): `None`.

The `Call` cases (`:556-605`):

* the `move()` builtin (`:561-584`): the argument is built first, an
  `IdentRef` argument is marked moved-out (**H5.2**, and unreachable
  because the HIR stage reports the use-after-move first), the node is a
  `Call` with `callee = "move"` and one argument, and **the result lifetime
  is the argument's lifetime** (so a `move(x)` of a plain local prints
  **no** `[lt: ...]` suffix - fixture `27_move`).
* every other call (`:585-604`): `callee`, `isVirtual`, `vtableSlot`,
  `methodName`, `returnLifetime` (from `v.target`) and the arguments are
  copied; then, **if the result type is a pointer**, the lifetime is set to
  `Named(returnLifetime)` or `Unknown` (`:595-602`) - **but line 603
  overwrites it unconditionally with `Unknown`**:

  ```cpp
  603:  out->lifetime.kind = mir::Lifetime::Kind::Unknown;  // virtual: unknown
  ```

  This is an oracle bug (the comment says "virtual"), but it is the
  **observable behaviour**: **every** non-`move` call prints
  ` [lt: unknown]`, whatever its type (fixture `11_call`:
  `call : int32_t [lt: unknown]`).  The port must reproduce it.

### 2.5 The compound-assignment expansion (`mir_builder.cpp:928-950`)

In the `ExprStmt` handler, when the expression is an `Assign` with an
operator that is not `"="` and that ends with `=` and has at least two
characters:

```
lhs op= rhs      ==>      store lhs, (clone(lhs) op rhs)
```

* the plain operator is `op` minus the trailing `=`;
* the new `Binary`'s **type is the right-hand side's type** (or the left's
  when there is no right-hand side) - this is visible in the dump: a
  compound assignment of an `int` literal prints `binary '+' : int`
  (fixture `67_compound_assign`), while a plain `x + y` of two `int32_t`
  locals prints `binary '+' : int32_t` (fixture `10_arith`);
* the new `Binary`'s lifetime is the left's if it is a pointer, else the
  right's;
* the left-hand side is **cloned** (`cloneExpr`, `:1323-1399`), so the store
  target and the binary operand are independent trees.

### 2.6 `mergeLifetime` (`mir_builder.cpp:29-37`)

```cpp
if (a.kind == Named && b.kind == Named && a.name == b.name) return a;
if (a.kind == Local  || b.kind == Local)  return Local;
if (a.kind == b.kind)                      return a;   // None/None, Static/Static, Unknown/Unknown
return Unknown;
```

### 2.7 The lifetime of a local (`mir_builder.cpp:724-750`)

`declare(name, init->lifetime)` - the local's lifetime is the lifetime of
its **initializer**:

| initializer | the local's lifetime | the dump |
|---|---|---|
| `int 1` | `None` | `ident x` (no suffix) |
| `string "hello"` | `Static` | `ident s [lt: static]` |
| `&x` (a local) | `Local` | `ident p [lt: local]` |
| `new int32` | `Unknown` | `ident p [lt: unknown]` |
| none (a struct local) | `None` | `ident p` |

### 2.8 The parameters' lifetimes (`mir_builder.cpp:1058-1066`)

| parameter | the lifetime in `params_` |
|---|---|
| with a declared lifetime (`$a` / `[[ivy::lt(a)]]`) | `Named(a)` |
| a raw pointer without a declared lifetime | `Unknown` |
| anything else (a value, a reference without an annotation) | `None` |

### 2.9 The features the builder handles that the port's HIR cannot express

See section 6 for the full list.  The summary:

* **`new` / `delete`** (`mir_builder.cpp:647-661`): the port's *parser*
  does not support them at all.
* **struct construction `S s(1)`** and the **destructor injection**: the
  port's HIR builder does not produce the injected ctor/dtor calls, so the
  MIR would miss the `eval / call` instructions.
* **braced init lists `Point p = {1,2}`**: the port's parser does not
  support them.
* **`namespace` blocks**: the port's parser does not support them.
* **virtual calls**: the port's HIR does not mark a call as virtual, so the
  `virtual_call '<m>' slot=N` label cannot be produced.
* **`->` member access**: unreachable in the oracle itself (see 5.4).

### 2.10 The features the builder handles that are *dead* in the oracle

| feature | why it is dead |
|---|---|
| `mir::Expr::Lambda` | the oracle **crashes** (0xC0000005) in the HIR stage on a lambda (fixture `37_lambda`) |
| `hir::Expr::PackExpansion` / `FoldExpr` / `SizeofPack` | folded away by the HIR builder |
| `hir::Expr::SizeOf` / `AlignOf` | folded to an `IntegerLit` by the HIR builder (`hir_builder.cpp:3743-3760`); the port does the same |
| `mir::Lifetime::Kind::Unknown` from a call's `returnLifetime` | overwritten by `:603` |
| `checkStore` | a `Local` lifetime can only be created by `&<local>`, which requires an `unsafe` block, and `checkStore` returns early in `unsafe` - **unreachable** |
| the "returned reference has unknown lifetime" message | an unannotated pointer parameter needs a body, and a body with a raw pointer parameter is an HIR error (`hir_builder.cpp:873-875`) - **unreachable** |
| the move diagnostics (`:476`, `:569-572`) | the HIR stage reports the use-after-move first and `main.cpp:1311` returns before the MIR builder runs |
| `Labeled switch` / `nextcase <label>` | the parser does not accept a labeled switch (`switch (a) label outer {`), so `CaseClause::caseLabel` and `Switch::switchLabel` are always empty.  The *unlabelled* `nextcase` works and the *labelled* one always produces the MIR error `nextcase target '...' is not a case label in this switch` (fixture `61_nextcase_label`) |
| `Inst::Switch` in the dump | `dumpMirBlock` has no `case Kind::Switch` - the instruction prints nothing |

---

## 3. The `--mir` dump format (`src/app/main.cpp`)

### 3.1 The pipeline order (`main.cpp:1303-1323`)

```
lexer -> preprocessor -> parser -> HIR builder -> [dumpHir] -> MIR builder -> [dumpMir]
```

* The HIR builder's diagnostics are printed to **stderr**
  (`main.cpp:1307-1310`) and, if any, `main.cpp:1311` returns 1 **before**
  the MIR builder runs.  So a HIR-stage error means: the diagnostic on
  stderr, **no MIR dump**, exit code 1.
* The MIR builder's diagnostics are printed to **stderr**
  (`main.cpp:1317-1320`) and, if any, `main.cpp:1321` returns 1 **before**
  `dumpMir`.  So a MIR-stage error means: the diagnostic on stderr, **no MIR
  dump**, exit code 1.
* `dumpMir` writes to **stdout** (`main.cpp:1323`).

The `std::cerr` / `std::cout` tie means that, in a merged `2>&1` capture,
the stdout content appears **before** the stderr content (writing to
`cerr` flushes `cout` first).  The port's driver writes everything to
stdout, so the merged byte streams agree - as they already do for the HIR
stage.

### 3.2 The exact format

```
dumpMir(tu, os)                                     main.cpp:769-794
  for each function (in tu.functions order):
    "function " <name> [ " [extern \"C\"]" ] "\n"
    "  return " <type> "(" <params> ")" "\n"
    [ "  lifetimes:" ( " " <name> )* "\n" ]          (only if non-empty)
    [ "  return lifetime: " <name> "\n" ]            (only if non-empty)
    for each block (index i):
      dumpMirBlock(block, os, i, 1)                  main.cpp:723-767
    "\n"
```

`dumpMirBlock`:

```
  pad = 2 spaces (depth 1)
  "  block " <index> "\n"
  for each instruction:
    mark = " [unsafe]" if inst.inUnsafe else ""
    Alloca      -> "    alloca " <type> " " <var> <mark> "\n"   [ then the init expr at depth+2 ]
    Store       -> "    store" <mark> "\n"                     [ then target, value at depth+2 ]
    Eval        -> "    eval" <mark> "\n"                      [ then the value at depth+2 ]
    Ret         -> "    ret" <mark> "\n"                       [ then the value at depth+2 ]
    CondBranch  -> "    condbranch" <mark> "\n"                [ then the cond at depth+2 ]
    Jump        -> "    jump" <mark> "\n"
    Switch      -> NOTHING
```

`dumpMirExpr` (depth = the expression's depth; the pad is `depth * 2`
spaces):

| node | line | output |
|---|---|---|
| `IntegerLit` | `:637` | `<pad>int <value>[ [lt: X]]\n` |
| `FloatLit` | `:638` | `<pad>float <value>[ [lt: X]]\n` |
| `StringLit` | `:639` | `<pad>string <raw>[ [lt: X]]\n` |
| `CharLit` | `:640` | `<pad>char <raw>[ [lt: X]]\n` |
| `BoolLit` | `:641` | `<pad>bool true|false[ [lt: X]]\n` |
| `NullptrLit` | `:642` | `<pad>nullptr[ [lt: X]]\n` |
| `IdentRef` | `:643` | `<pad>ident <name>[ [lt: X]]\n` |
| `This` | `:644` | `<pad>this[ [lt: X]]\n` |
| `Unary` | `:645-650` | `<pad>prefix '<op>' : <type>[ [lt: X]]\n` or `<pad>postfix '<op>' : ...` then the operand at depth+1 |
| `Binary` | `:651-655` | `<pad>binary '<op>' : <type>[ [lt: X]]\n` then lhs, rhs at depth+1 |
| `Ternary` | `:656-661` | `<pad>ternary ?: : <type>[ [lt: X]]\n` then cond, then, else at depth+1 |
| `Call` | `:662-670` | `<pad>call : <type>[ [lt: X]]\n` (or `<pad>virtual_call '<method>' slot=<n> : <type>[ [lt: X]]\n`) then the **non-null** args at depth+1 |
| `Index` | `:671-675` | `<pad>index : <type>[ [lt: X]]\n` then base, index at depth+1 |
| `Member` | `:676-681` | `<pad>member .<name> : <type>[ [lt: X]]\n` or `<pad>member -><name> : <type>` then the base at depth+1.  **Note: no space between the `.`/`->` and the name, and the type IS printed** (the *HIR* dump uses a different format: `member . v` with a space and no type) |
| `Assign` | `:682-686` | `<pad>assign '<op>' : <type>[ [lt: X]]\n` then lhs, rhs at depth+1 |
| `New` | `:687-692` | `<pad>new : <type>[ [lt: X]]\n` then the args at depth+1 |
| `Delete` | `:693-696` | `<pad>delete[ [lt: X]]\n` or `<pad>delete[]` then the operand at depth+1 |
| `InitList` | `:697-702` | `<pad>init-list : <type>[ [lt: X]] ( <n> )\n` then the elements at depth+1 |
| `Lambda` | `:703-710` | `<pad>lambda : <type>[ [lt: X]] func=<f> closure=<c> ( <n> captures)\n` then the captures at depth+1 |
| `Cast` | `:711-718` | `<pad>static_cast : <type>[ [lt: X]]\n` (or `reinterpret_cast` / `const_cast` / `C-cast`) then the operand at depth+1 |

The ` [lt: X]` suffix is added by `dumpMirExprHeader` (`:627-628`) and by
`dumpMirExpr` (`:634-635`) when the lifetime is not `None`.

The type printer is `dumpHirType` (`main.cpp:360-366`), the **same**
function the HIR dump uses:

```
[ "const " ] [ "unsigned " ] <base> ( "*" * pointerDepth ) [ "&" ]
```

### 3.3 The lifetime suffix (`mirLifetimeSuffix`, `main.cpp:608-618`)

| `Lifetime::Kind` | the suffix |
|---|---|
| `None` | *(empty - nothing is printed)* |
| `Named` | the name, e.g. `a` |
| `Static` | `static` |
| `Local` | `local` |
| `Unknown` | `unknown` |

**Two different spacings** - this is a real trap:

* the **expression** suffix is `" [lt: " << lt << "]"` - a space after the
  colon: `ident x [lt: a]`;
* the **parameter** suffix is `" [lt:" << lifetime << "]"` - **no** space:
  `const int32_t& x [lt:a]`.

### 3.4 The line terminator

The oracle writes to `std::cout` in text mode, so on Windows every `\n`
becomes **CR LF**.  The measured raw bytes of `01_empty.out.bin` are:

```
66 75 6e 63 74 69 6f 6e 20 66 0d 0a 20 20 72 65 74 75 72 6e 20 76 6f 69 64 28 29 0d 0a
20 20 62 6c 6f 63 6b 20 30 0d 0a 0d 0a
```

i.e. `"function f\r\n  return void()\r\n  block 0\r\n\r\n"`.

The port's `io::println` produces CRLF too (the port's runtime writes to
`stdout` in text mode), so the two byte streams agree **without** any
normalisation.  `selfhost/difftest.ps1` normalises CRLF -> LF anyway
(`Get-ByteLines`).

### 3.5 Real samples (raw captures)

**`01_empty.ivy`** - `h51r1/out/01_empty.out.bin` (42 bytes):

```
function f\r\n
  return void()\r\n
  block 0\r\n
\r\n
```

**`26_lifetime.ivy`** - `h51r1/out/26_lifetime.out.bin` (145 bytes):

```
function f\r\n
  return const int32_t&(const int32_t& x [lt:a])\r\n
  lifetimes: a\r\n
  return lifetime: a\r\n
  block 0\r\n
    ret\r\n
      ident x [lt: a]\r\n
\r\n
```

**`13_if.ivy`** - `h51r1/out/13_if.out.bin`:

```
function f\r\n
  return int32_t(int32_t a)\r\n
  block 0\r\n
    condbranch\r\n
      binary '>' : bool\r\n
        ident a\r\n
        int 0\r\n
  block 1\r\n
  block 2\r\n
    ret\r\n
      int 1\r\n
  block 3\r\n
    ret\r\n
      int 2\r\n
\r\n
```

(`block 1` is the `cont` block: both branches return, so it is unreachable
and empty.)

**`19_break_continue.ivy`** - `h51r1/out/19_break_continue.out.bin`
(the block numbering was derived by hand and matches the oracle exactly -
see 5.5):

```
function f\r\n
  return int32_t()\r\n
  block 0\r\n
    alloca int32_t s\r\n
      int 0\r\n
    alloca int32_t i\r\n
      int 0\r\n
    jump\r\n
  block 1\r\n
    condbranch\r\n
      binary '<' : bool\r\n
        ident i\r\n
        int 10\r\n
  block 2\r\n
    condbranch\r\n
      binary '==' : bool\r\n
        ident i\r\n
        int 3\r\n
  block 3\r\n
    eval\r\n
      assign '=' : int32_t\r\n
        ident i\r\n
        binary '+' : int32_t\r\n
          ident i\r\n
          int 1\r\n
    jump\r\n
  block 4\r\n
    ret\r\n
      ident s\r\n
  block 5\r\n
    condbranch\r\n
      binary '==' : bool\r\n
        ident i\r\n
        int 7\r\n
  block 6\r\n
    jump\r\n
    jump\r\n
  block 7\r\n
    store\r\n
      ident s\r\n
      binary '+' : int32_t\r\n
        ident s\r\n
        ident i\r\n
    jump\r\n
  block 8\r\n
    jump\r\n
    jump\r\n
\r\n
```

Note the **double `jump`** in blocks 6 and 8: the `break` emits a jump to
the loop's exit block, and the `if` handler then emits its own
`jumpTo(cont)`.  `jumpTo` only suppresses the jump when the last
instruction is a `Ret`.

**`17_switch_int.ivy`** - `h51r1/out/17_switch_int.out.bin`:

```
function f\r\n
  return int32_t(int32_t a)\r\n
  block 0\r\n
  block 1\r\n
  block 2\r\n
    ret\r\n
      int 10\r\n
  block 3\r\n
    ret\r\n
      int 20\r\n
  block 4\r\n
    ret\r\n
      int 0\r\n
\r\n
```

`block 0` is the entry block and holds the **`Switch` instruction, which
prints nothing**.  `block 1` is the switch's `exitB`.  `block 2..4` are the
three case clauses.

**`77_virtual_call.ivy`** - `h51r1/out/77_virtual_call.out.bin` (the
virtual-call label):

```
  block 0\r\n
    ret\r\n
      virtual_call 'speak' slot=0 : int32_t [lt: unknown]\r\n
        ident a\r\n
```

**`34_struct_ctor.ivy`** - `h51r1/out/34_struct_ctor.out.bin` (the
injected constructor call):

```
function f\r\n
  return void()\r\n
  block 0\r\n
    alloca S s\r\n
    eval\r\n
      call : void [lt: unknown]\r\n
        ident s\r\n
        int 1\r\n
\r\n
```

**`22_ptr.ivy`** - `h51r1/out/22_ptr.out.bin` (the `[unsafe]` marker and
the `local` lifetime):

```
function f\r\n
  return void()\r\n
  block 0\r\n
    alloca int32_t x [unsafe]\r\n
      int 1\r\n
    alloca int32_t* p [unsafe]\r\n
      prefix '&' : int32_t* [lt: local]\r\n
        ident x\r\n
    ret [unsafe]\r\n
      prefix '*' : int32_t [lt: local]\r\n
        ident p [lt: local]\r\n
\r\n
```

---

## 4. The experiments (the core of phase 1)

### 4.1 The method

* 91 fixtures in `h51r1/fx/`.
* For each fixture: `cmd /c "ivy --mir <file> > <name>.out.bin 2> <name>.err.bin"`,
  the exit code is recorded.
* The same fixtures were run through `ivy --hir` and through the **port's**
  HIR driver (`h51r1/port/hir/hirdrive.exe`, built from a *copy* of
  `selfhost/parsing` + `selfhost/hir` so that no build product is written
  into the tree) and the two `--hir` byte streams were compared.  That
  comparison is what produced the gap list of section 6.

### 4.2 The classification

| class | count | meaning |
|---|---|---|
| **A - H5.1-reachable** | **66** | the oracle prints a MIR dump and the port could match it byte for byte once the MIR builder + dumper are implemented |
| **B - blocked by a missing HIR feature** | **6** | the oracle prints a MIR dump but the port's HIR cannot express the construct: `34_struct_ctor`, `35_struct_dtor`, `36_new_delete`, `38_namespace`, `47_struct_initlist`, `77_virtual_call` |
| **C - MIR-stage diagnostic** | **6** | the oracle prints a **MIR** diagnostic and **no** dump: `29_borrow_mut_mut`, `30_borrow_mut_shared`, `87_borrow_mut_after_shared`, `32_dangling_return`, `80_lt_mismatch`, `61_nextcase_label`.  **Matchable**, but only when the H5.2 checks are implemented |
| **D - HIR/parse-stage diagnostic** | **12** | the oracle never reaches the MIR stage: `05_local_noinit`, `09_param_ptr`, `18_switch_enum`, `28_borrow_error`, `44_labeled_switch`, `49_global`, `63_variadic_body`, `64_string_literal`, `74_struct_ptr_member`, `79_sizeof_expr`, `85_move_use`, `86_assign_expr` |
| **E - the oracle crashes** | **1** | `37_lambda`: the oracle dies with 0xC0000005 (access violation) **in the HIR stage** (`--ast`, `--hir` and `--mir` all crash) |

Total: **91**.

### 4.3 The fixture table

| # | fixture | class | what it covers | oracle stdout | oracle stderr | rc |
|---|---|---|---|---|---|---|
| 1 | `01_empty.ivy` | A | an empty body | 42 | 0 | 0 |
| 2 | `02_return_void.ivy` | A | a bare `return` | 51 | 0 | 0 |
| 3 | `03_return_const.ivy` | A | a constant return | 68 | 0 | 0 |
| 4 | `04_local_init.ivy` | A | an `int32` local with an initializer | 104 | 0 | 0 |
| 5 | `05_local_noinit.ivy` | D | an uninitialized scalar local -> HIR error | 0 | 159 | 1 |
| 6 | `06_two_locals.ivy` | A | two locals | 186 | 0 | 0 |
| 7 | `07_param.ivy` | A | a by-value parameter | 78 | 0 | 0 |
| 8 | `08_param_ref.ivy` | A | a reference parameter | 85 | 0 | 0 |
| 9 | `09_param_ptr.ivy` | D | a raw pointer parameter + body -> HIR error | 0 | 133 | 1 |
| 10 | `10_arith.ivy` | A | arithmetic on locals | 288 | 0 | 0 |
| 11 | `11_call.ivy` | A | a call to another function | 157 | 0 | 0 |
| 12 | `12_call_args.ivy` | A | a call with two arguments | 256 | 0 | 0 |
| 13 | `13_if.ivy` | A | a comparison and an `if`/`else` | 204 | 0 | 0 |
| 14 | `14_while.ivy` | A | a `while` loop | 317 | 0 | 0 |
| 15 | `15_for.ivy` | A | a `for` loop | 496 | 0 | 0 |
| 16 | `16_dowhile.ivy` | A | a `do-while` loop | 317 | 0 | 0 |
| 17 | `17_switch_int.ivy` | A | a `switch` over an int | 166 | 0 | 0 |
| 18 | `18_switch_enum.ivy` | D | a `switch` over an enum -> HIR error | 0 | 125 | 1 |
| 19 | `19_break_continue.ivy` | A | `break` and `continue` | 728 | 0 | 0 |
| 20 | `20_nested_block.ivy` | A | a nested block | 180 | 0 | 0 |
| 21 | `21_struct.ivy` | A | a struct local + member read/write | 334 | 0 | 0 |
| 22 | `22_ptr.ivy` | A | a pointer dereference and an address-of | 266 | 0 | 0 |
| 23 | `23_this.ivy` | A | a `this` method | 123 | 0 | 0 |
| 24 | `24_template.ivy` | A | a template function + its instantiation | 193 | 0 | 0 |
| 25 | `25_constexpr.ivy` | A | a `constexpr` function | 68 | 0 | 0 |
| 26 | `26_lifetime.ivy` | A | a lifetime annotation (H4.5 syntax) | 145 | 0 | 0 |
| 27 | `27_move.ivy` | A | `move()` (H4.6 syntax) | 126 | 0 | 0 |
| 28 | `28_borrow_error.ivy` | D | `int32& a = &x` -> HIR type error | 0 | 330 | 1 |
| 29 | `29_borrow_mut_mut.ivy` | C | two mutable borrows -> MIR borrow error | 0 | 172 | 1 |
| 30 | `30_borrow_mut_shared.ivy` | C | a shared borrow under a mutable one -> MIR borrow error | 0 | 168 | 1 |
| 31 | `31_borrow_shared_shared.ivy` | A | two shared borrows (allowed) | 165 | 0 | 0 |
| 32 | `32_dangling_return.ivy` | C | returning a local by reference -> MIR dangling error | 0 | 150 | 1 |
| 33 | `33_extern_c.ivy` | A | `extern "C"` declarations (bodyless) | 130 | 0 | 0 |
| 34 | `34_struct_ctor.ivy` | **B** | a struct constructor + `S s(1)` | 263 | 0 | 0 |
| 35 | `35_struct_dtor.ivy` | **B** | a struct destructor (RAII) | 236 | 0 | 0 |
| 36 | `36_new_delete.ivy` | **B** | `new` / `delete` | 174 | 0 | 0 |
| 37 | `37_lambda.ivy` | **E** | a lambda -> **the oracle crashes** | 0 | 0 | -1073741819 |
| 38 | `38_namespace.ivy` | **B** | a `namespace` block | 86 | 0 | 0 |
| 39 | `39_ternary.ivy` | A | a ternary | 180 | 0 | 0 |
| 40 | `40_index.ivy` | A | an index expression | 210 | 0 | 0 |
| 41 | `41_cast.ivy` | A | a `static_cast` | 98 | 0 | 0 |
| 42 | `42_unsafe_block.ivy` | A | an `unsafe` block | 176 | 0 | 0 |
| 43 | `43_nextcase.ivy` | A | an unlabelled `nextcase` | 163 | 0 | 0 |
| 44 | `44_labeled_switch.ivy` | D | a labeled switch -> a parse error | 0 | 778 | 1 |
| 45 | `45_virtual.ivy` | A | virtual methods (definitions only) | 176 | 0 | 0 |
| 46 | `46_multi_decl.ivy` | A | a multi-declarator statement | 112 | 0 | 0 |
| 47 | `47_struct_initlist.ivy` | **B** | `Point p = {1, 2}` | 121 | 0 | 0 |
| 48 | `48_string.ivy` | A | a string and a char literal | 156 | 0 | 0 |
| 49 | `49_global.ivy` | D | a global variable -> a parse error | 0 | 796 | 1 |
| 50 | `50_static_cast.ivy` | A | `static_cast<int64>(ptr)` | 277 | 0 | 0 |
| 51 | `51_array.ivy` | A | an array local | 143 | 0 | 0 |
| 52 | `52_auto.ivy` | A | `auto` type inference | 111 | 0 | 0 |
| 53 | `53_method_call.ivy` | A | method calls on a local | 527 | 0 | 0 |
| 54 | `54_operator_overload.ivy` | A | an operator overload | 295 | 0 | 0 |
| 55 | `55_default_arg.ivy` | A | a default argument | 393 | 0 | 0 |
| 56 | `56_enum_const.ivy` | A | an enum constant | 100 | 0 | 0 |
| 57 | `57_inherit.ivy` | A | an inherited field | 200 | 0 | 0 |
| 58 | `58_constexpr_call.ivy` | A | a folded `constexpr` call | 198 | 0 | 0 |
| 59 | `59_scoped_enum.ivy` | A | a scoped enum | 75 | 0 | 0 |
| 60 | `60_return_struct.ivy` | A | returning a struct by value | 87 | 0 | 0 |
| 61 | `61_nextcase_label.ivy` | C | `nextcase <label>` -> MIR nextcase error | 0 | 146 | 1 |
| 62 | `62_extern_c_body.ivy` | A | an `extern "C"` definition | 151 | 0 | 0 |
| 63 | `63_variadic_body.ivy` | D | a variadic definition -> HIR error | 0 | 137 | 1 |
| 64 | `64_string_literal.ivy` | D | a raw pointer return + body -> HIR error | 0 | 135 | 1 |
| 65 | `65_char_and_float.ivy` | A | char / float / bool literals | 192 | 0 | 0 |
| 66 | `66_prefix_inc.ivy` | A | `++` / `--` (prefix and postfix) | 303 | 0 | 0 |
| 67 | `67_compound_assign.ivy` | A | `+= -= *= /= %=` | 487 | 0 | 0 |
| 68 | `68_logical.ivy` | A | `&&` / `||` | 326 | 0 | 0 |
| 69 | `69_shift_bitwise.ivy` | A | `<< >> & \| ^` | 335 | 0 | 0 |
| 70 | `70_nested_if.ivy` | A | a nested `if`/`else` | 332 | 0 | 0 |
| 71 | `71_for_empty.ivy` | A | `for (;;)` | 136 | 0 | 0 |
| 72 | `72_return_in_loop.ivy` | A | a `return` inside a loop | 385 | 0 | 0 |
| 73 | `73_scope_shadow.ivy` | A | a shadowed local | 112 | 0 | 0 |
| 74 | `74_struct_ptr_member.ivy` | D | `this->x` -> HIR error | 0 | 274 | 1 |
| 75 | `75_mangled_template.ivy` | A | a two-argument template instantiation | 233 | 0 | 0 |
| 76 | `76_sizeof.ivy` | A | `sizeof` / `alignof` (folded at the HIR stage) | 174 | 0 | 0 |
| 77 | `77_virtual_call.ivy` | **B** | a virtual call through a base reference | 315 | 0 | 0 |
| 78 | `78_base_call.ivy` | A | a static call to a base method | 289 | 0 | 0 |
| 79 | `79_sizeof_expr.ivy` | D | `sizeof(expr)` -> a parse error | 0 | 271 | 1 |
| 80 | `80_lt_mismatch.ivy` | C | a lifetime mismatch -> MIR lifetime error | 0 | 160 | 1 |
| 81 | `81_lt_unknown_return.ivy` | A | returning an unannotated reference (allowed) | 92 | 0 | 0 |
| 82 | `82_store_dangling.ivy` | A | a pointer local (the store check is unreachable) | 167 | 0 | 0 |
| 83 | `83_ret_named_ok.ivy` | A | a matching named lifetime | 145 | 0 | 0 |
| 84 | `84_param_unknown_lt.ivy` | A | a bodyless declaration with a pointer param | 44 | 0 | 0 |
| 85 | `85_move_use.ivy` | D | a use after `move()` -> HIR error | 0 | 111 | 1 |
| 86 | `86_assign_expr.ivy` | D | `y = (x = 1)` -> a parse error | 0 | 360 | 1 |
| 87 | `87_borrow_mut_after_shared.ivy` | C | a mutable borrow under a shared one -> MIR borrow error | 0 | 173 | 1 |
| 88 | `88_ternary_store.ivy` | A | a ternary in a store | 229 | 0 | 0 |
| 89 | `89_call_in_expr.ivy` | A | calls inside a binary expression | 225 | 0 | 0 |
| 90 | `90_void_call_stmt.ivy` | A | a void call statement | 127 | 0 | 0 |
| 91 | `91_nested_call_arg.ivy` | A | a call as a call argument | 223 | 0 | 0 |

The raw bytes are in `h51r1/out/<name>.out.bin` and
`h51r1/out/<name>.err.bin`.

### 4.4 The MIR-stage diagnostics (the exact texts)

The three borrow messages use an **em dash (U+2014, `e2 80 94`)**, not a
hyphen - the same trap as the H4.5 lifetime diagnostics:

| source | message |
|---|---|
| `mir_builder.cpp:342-343` | `cannot create a mutable reference to '<x>' — it is already borrowed immutably` |
| `mir_builder.cpp:347-348` | `cannot create a second mutable reference to '<x>' — only one mutable borrow at a time` |
| `mir_builder.cpp:355-356` | `cannot create an immutable reference to '<x>' — it is already borrowed mutably` |
| `mir_builder.cpp:267-269` | `returned reference has lifetime '$<a>' but the function declares lifetime '$<b>'` |
| `mir_builder.cpp:278` | `returning a reference to a local variable (dangling reference)` |
| `mir_builder.cpp:915-916` | `nextcase target '<t>' is not a case label in this switch` |

The three borrow messages use an **em dash (U+2014, `e2 80 94`)**, not a
hyphen - the same trap as the H4.5 lifetime diagnostics.  The measured
bytes of the middle of `29_borrow_mut_mut.err.bin` are:

```
62 6c 65 20 72 65 66 65 72 65 6e 63 65 20 74 6f 20 27 78 27 20
e2 80 94 20 6f 6e 6c 79 20 6f 6e 65 20 6d 75 74 61 62 6c 65 20 62 6f 72 72 6f 77 20 61 74
```

i.e. `"... to 'x' " + E2 80 94 + " only one mutable borrow at a time"`.

The diagnostic line format is `<file>:<line>:<col>: error: <message>`
(`main.cpp:1318`), with `<file>` = the path as given on the command line.
The port's driver already prints the HIR diagnostics in this format, so
the MIR driver must use the same one.

### 4.5 The block-numbering derivation (checked by hand against the oracle)

The dump's `block N` numbers are the positions in `Function::blocks`.  The
creation order is normative.  Two worked examples:

* **`if (c) { A } else { B }`** - the current block is `k`; the handler
  creates `cont = k+1`, `then = k+2`, `else = k+3` and emits the
  `CondBranch` into `k`.  The then-branch ends with `jumpTo(cont)`, the
  else-branch with `jumpTo(cont)`.  If both branches return, `cont` stays
  empty (the `jumpTo` is suppressed by the "the last instruction is a
  `Ret`" rule).
* **`for (init; cond; incr) body`** - `init` is built in the current block
  `k`; then `cond = k+1`, `body = k+2`, `incr = k+3`, `exit = k+4`.  The
  body is built in `body`, so the `if`s inside the body allocate their
  blocks **after** `exit` - which is why the `19_break_continue` dump has
  the `exit` block (4) *before* the body's `if` blocks (5-8).

---

## 5. What the experiments proved about the oracle's reachability

### 5.1 The MIR dump is reachable for a very wide class of programs

Everything the port's HIR already supports (values, locals, control flow,
calls, operators, casts, templates, lifetimes, moves, methods, operators
overloads, inheritance, enums, arrays, `unsafe`, `auto`, default
arguments) reaches the MIR stage cleanly: **66 of the 91 fixtures**.

### 5.2 The MIR-stage diagnostics that are reachable

Exactly **three** MIR checks can fire (six distinct messages):

1. the borrow checker (`checkBorrow`) - three messages;
2. the lifetime checker on a return (`checkReturn`) - two messages
   (the dangling one and the lifetime-mismatch one);
3. the `nextcase` label check.

### 5.3 The MIR-stage diagnostics that are unreachable

* **`checkStore`** - a `Local` lifetime can only be produced by
  `&<local>`, which the HIR stage requires to be inside an `unsafe` block
  (`hir_builder.cpp:2751`), and `checkStore` returns immediately inside
  `unsafe` (`mir_builder.cpp:299`).  A `Local` value therefore can never
  reach it.
* **the "unknown lifetime" return message** - it needs an unannotated
  pointer parameter, but a function with a body cannot have a raw pointer
  parameter (`hir_builder.cpp:873-875`), and a pointer *return* type has
  the same restriction (`:859-861`).
* **the move diagnostics** - the HIR stage reports the use-after-move
  first (`main.cpp:1311` returns before the MIR builder runs).

### 5.4 The oracle's own bugs found on the way

* **`&<struct>` crashes the oracle.**  `Point* pp = &p;` dies with
  0xC0000005 already in the `--ast` stage (so in the parser), i.e. before
  the HIR or the MIR.  This makes the `->` member access unreachable: a
  struct pointer can only come from `&<struct>` (a crash) or from a raw
  pointer parameter (an HIR error).  The port must **not** try to
  reproduce a crash.
* **a lambda crashes the oracle** in the HIR stage (fixture `37_lambda`).
* **`mir_builder.cpp:603`** unconditionally sets every call's lifetime to
  `Unknown` (the comment says "virtual: unknown" but the line is not
  conditional).  The dump therefore prints `[lt: unknown]` on **every**
  non-`move` call.

### 5.5 The `this` parameter, the ctor and the dtor are HIR-level

The `this` parameter (`Counter& this`), the injected constructor call
(`call 'S::S'` with the object as argument 0), the injected destructor
call (`call 'S::~S'`) and the NRVO / `__ret` synthesis are all produced by
the **HIR** builder (`hir_builder.cpp`), not by the MIR builder.  The MIR
builder just lowers them like any other call or local.  The port's HIR
builder produces the `this` parameter already; it does **not** produce the
ctor/dtor calls nor the `__ret` temporary (see section 6).

---

## 6. The HIR-fidelity gap list (the most important output)

The port's HIR model (`selfhost/hir/hir.ivy`) is a **dump-oriented**
reduction of `src/hir/hir.h`: 19 expression kinds, 14 statement kinds and
a handful of function fields.  It reproduces the `--hir` dump byte for
byte, but the dump does not exercise every field of the oracle's HIR.

The measurement: 91 fixtures, `ivy --hir` vs. the port's `hirdrive`:
**78 byte-identical, 13 different**.  The 13 break down as:

* **6** are *not* HIR gaps - they are MIR-stage diagnostics that the
  HIR-only port cannot produce (`29`, `30`, `32`, `61`, `80`, `87`);
* **1** is the oracle's crash (`37`);
* **6** are **real HIR gaps** (`34`, `35`, `36`, `38`, `47`, `77`).

### 6.1 The gaps, one by one

| # | gap | the oracle's HIR | the port's HIR | the impact on the MIR dump | severity |
|---|---|---|---|---|---|
| **G1** | **the virtual dispatch** | `hir::Expr::Call` has `bool isVirtual`, `std::size_t vtableSlot`, `std::string_view methodName`.  The HIR builder sets them when the receiver's static type is a base class that declares the method `virtual` | `HExpr` (kind 12) has only `sval` (the callee) and the args - **no** `isVirtual` / `vtableSlot` / `methodName`.  The port's HIR builder does not implement the virtual dispatch at all (it only tracks `is_virtual` for the *struct layout*) | the oracle's dump prints `virtual_call 'speak' slot=0 : <type>` for a virtual call and `call : <type>` otherwise.  The port would always print `call`.  **The `--hir` dump differs too** (the oracle prints `virtual_call 'speak' slot=0 : int32_t`, the port prints `call 'Animal::speak' : int32_t`) | **H5.1-blocking** for virtual dispatch |
| **G2** | **`new` / `delete`** | `hir::Expr::New {Type type; args}`, `hir::Expr::Delete {operand; isArray}` | `HExpr` has no `New` / `Delete` kind; the **parser** rejects `new` / `delete` outright | the dump prints `new : <type>` and `delete` / `delete[]` | **parser-level**, unreachable in the port |
| **G3** | **the struct construction `S s(1)`** | the HIR builder turns it into a `Decl` whose initializer is a `Call` to the ctor (`S::S`), with the object as argument 0 | the port's parser does not parse `S s(1)` as a direct initialization; the port reports `undeclared identifier 'S'` / `call to undeclared function 'S'` / `cannot initialize variable 's' of type 'S' with a value of type 'int'` | the oracle's MIR has an extra `eval / call : void [lt: unknown] / ident s / int 1` after the `alloca` | **parser + HIR-builder-level** |
| **G4** | **the destructor injection (and the `__ret` temporary)** | at the end of every scope, the HIR builder emits the `~T` calls for the locals that have a destructor (reverse declaration order).  When a function returns a by-value aggregate that has a destructor, the HIR builder first moves the value into a synthesized local named `__ret` (`hir_builder.cpp:1790-1827`) and returns that | the port's HIR builder does **not** inject the destructor calls and does **not** synthesize `__ret` | the oracle's MIR has an extra `eval / call : void [lt: unknown] / ident s` (and, for `__ret`, an extra `alloca <T> __ret`) | **HIR-builder-level** |
| **G5** | **the braced init list** | `hir::Expr::InitList` - `Point p = {1, 2}` becomes an `InitList` with 2 elements | the port's parser rejects `{...}` as an initializer (`unexpected token in expression`) | the oracle's MIR has `alloca Point p` + `init-list : Point (2)` | **parser-level** |
| **G6** | **the `namespace` blocks** | the parser supports `namespace X { ... }`; the function name becomes `X::f` | the port's parser has no `namespace` support (it only rejects `using namespace`) | the oracle's MIR prints `function math::abs` | **parser-level** |
| **G7** | **`hir::Stmt::loc`** | every `hir::Stmt` carries a `SourceLoc` | `HStmt` has **no** `line` / `col` | the MIR diagnostics are located at the *statement* (`checkReturn`, `checkStore`, the `Decl` borrow check, the `nextcase` check) - the port could not print `file:line:col` | **H5.2-blocking** (the diagnostics) |
| **G8** | **`hir::Expr::Member::isScope`** | `bool isScope` - true for `::` (scope resolution) | `HExpr.flag` holds only `isArrow` for a member | the MIR dump prints only `member .name` / `member ->name`, so `isScope` is **not** printed | **not needed for the dump** (H5.3) |
| **G9** | **`hir::Expr::Call::target`** | `const Function* target` (the resolved overload) | absent | `call.returnLifetime = v.target->returnLifetime` feeds the lifetime analysis, but `:603` overwrites the result with `Unknown`, so it is **not** observable in the dump | **not needed for the dump** (needed for H5.2 correctness) |
| **G10** | **the function flags** | `isTemplate`, `tplArgs`, `isConsteval`, `isCtor`, `isDtor`, `isVirtual`, `isPureVirtual`, `isOverride`, `vtableOf`, `namespacePrefix` | `HFunction` has `name`, `ret`, `is_extern_c`, `body`, `pstart`, `pn`, `is_constexpr`, `lt_start`, `lt_n`, `return_lifetime` | none of them is printed by `dumpMir`; `namespacePrefix` is redundant (the name is already qualified) | **not needed for the dump** (H5.3) |
| **G11** | **`hir::Expr::Lambda`** | `funcName`, `closureType`, `captureInits` | absent | unreachable: the oracle crashes in the HIR stage | dead |
| **G12** | **`hir::Expr::PackExpansion` / `FoldExpr` / `SizeofPack`** | present in the HIR model | absent | folded away by the HIR builder; the MIR builder does not handle them anyway | dead |
| **G13** | **`hir::Expr::SizeOf` / `AlignOf`** | present in the HIR model | the port folds them to an `IntegerLit` as well (kind 1) | the MIR builder does not handle them (it would fall through to `IntegerLit{0}`), but they never survive | dead |
| **G14** | **`hir::Expr::NulloptLit`** | the AST has it; the HIR builder folds it into an `InitList` / an `optional<T>` | the port handles it the same way | not printed differently | OK |
| **G15** | **`hir::Param::defaultValue`** | `const ivy::Expr*` (the AST default) | `HParam.default_value` (the AST expr index) | the HIR builder already expands the default at each call site, so the MIR never sees it | OK |

### 6.2 The summary

For the **MIR dump** to be byte-identical, the port's HIR must be able to
express: the function name / return type / params / lifetimes / return
lifetime / extern-C flag / body, and the expressions `int`, `float`,
`string`, `char`, `bool`, `nullptr`, `ident`, `this`, `unary`, `binary`,
`ternary`, `call` (with the callee name and the args), `index`, `member`,
`assign`, `init-list`, `cast`.

**Five constructs break that**, listed in G1-G6: the virtual dispatch
(G1), `new`/`delete` (G2), the struct construction + destructor injection
(G3, G4), the braced init lists (G5) and the namespaces (G6).

**Recommendation**: G1 is the only gap that affects a **reachable** and
common construct (a virtual call through a base reference), but it is
**not** cheap: the port's HIR builder does not implement the virtual
dispatch at all, so it needs (a) the detection in the HIR builder (the
receiver's static type is a base class that declares the method `virtual`
-> the vtable slot) **and** (b) three more fields in `HExpr`.  It is
therefore a candidate for a *sub-phase of its own* (5.1.7) rather than a
prerequisite.  G2, G3, G5, G6 are parser-level features that belong to a
later milestone (the parser is a *subset* of the oracle's parser by
design).  G4 is an HIR-builder feature (the RAII / destructor milestone).
G7 is an H5.2 prerequisite.

### 6.3 The measured evidence (the 13 differing fixtures)

| fixture | the oracle's `--hir` | the port's `--hir` |
|---|---|---|
| `34_struct_ctor` | `decl S s` + `expr / call 'S::S' : void / ident s / int 1` | `error: undeclared identifier 'S'` + `error: call to undeclared function 'S'` + `error: cannot initialize variable 's' of type 'S' with a value of type 'int'` |
| `35_struct_dtor` | `decl S s` + `expr / call 'S::~S' : void / ident s` | `decl S s` (no dtor call) |
| `36_new_delete` | `new : int32_t*` / `delete` | 4 parse errors |
| `37_lambda` | (the oracle crashes) | (the port does not crash - it reports the lambda as an undeclared identifier) |
| `38_namespace` | `function math::abs` | 3 parse errors |
| `47_struct_initlist` | `decl Point p = / init-list : Point (2)` | 6 parse errors |
| `77_virtual_call` | `virtual_call 'speak' slot=0 : int32_t` | `call 'Animal::speak' : int32_t` |

---

## 7. The port gap analysis

### 7.1 The files to create

| file | content | size estimate |
|---|---|---|
| `selfhost/mir/mir.ivy` | the MIR model: `MExpr`, `MInst`, `MBlock`, `MFunction`, `MParam`, `MLifetime`, `MTranslationUnit` (+ `MEnumInfo` / `MStructInfo` for H5.3) | 200-280 lines |
| `selfhost/mir/mir_builder.ivy` | the builder: `MirBuilder` with the CFG construction, the expression lowering, the lifetime propagation and (optionally) the H5.2 checks | 1100-1500 lines |
| `selfhost/mir/mirdrive.ivy` | the driver: read the file list from stdin, run the lexer / preprocessor / parser / HIR builder / MIR builder and print the `--mir` dump (or the diagnostics) | 250-350 lines |

### 7.2 The model in the port's idiom

The port's idiom is a **flat pool of records** with index-based links
(see `selfhost/hir/hir.ivy`):

* `HExpr` has `a`, `b`, `c` as the child *indices* and `astart`/`an` for
  the call arguments;
* `HStmt` has `a`, `b`, `c` and a `next` sibling link;
* `HUnit` owns the pools and the `release()` frees every one of them.

The MIR model should follow the same shape:

```ivy
//  1 int  2 float  3 string  4 char  5 bool  6 nullptr  7 ident  8 this
//  9 unary 10 binary 11 ternary 12 call 13 index 14 member 15 assign
// 16 new 17 delete 18 init-list 19 lambda 20 cast
export struct MExpr {
    long kind;
    long ival;          // the integer value (1) / the bool value (5) / the call's vtable slot (12)
    double fval;        // the float value (2)
    const char* sval;   // the raw text (3, 4) / the name (7) / the operator (9, 10, 15) / the member name (14) / the callee (12)
    long flag;          // the unary isPrefix (9) / the member isArrow (14) / the delete isArray (17) / the cast kind (20)
    AType type;
    long lt;            // the lifetime: 0 none 1 named 2 static 3 local 4 unknown
    const char* ltname; // the lifetime name (kind 1)
    long a, b, c;       // the child indices (-1 when absent)
    long astart, an;    // the call arguments / the init-list elements
    long line, col;
};

export struct MInst {
    long kind;          // 1 alloca 2 store 3 eval 4 ret 5 condbranch 6 jump 7 switch
    long in_unsafe;
    long a, b, c;       // the operand indices / the block indices
    long var;           // the alloca's variable index (into the name pool)
    AType type;         // the alloca's type
    long line, col;
};

export struct MBlock { long inst_start; long inst_n; };
export struct MFunction { ... };
```

The **blocks** must keep the *creation order* (the dump prints the index),
so the natural representation is a flat `MBlock` array plus an
instruction pool with a per-block run - exactly like the port's `HStmt`
sibling lists.

**Sizes** (x86-64, the port's convention - see `selfhost/hir/hir.ivy:23`):
`AType` is 48 bytes, a pointer is 8, a `long` is 8, a `bool` is 1.  The
pool element sizes must be multiples of 8 (or the struct must use
`bool` flags to avoid padding surprises) - the port's `HExpr` is 144
bytes, `HStmt` 120, `HParam` 88.

### 7.3 The port's idioms to follow

* `#pragma ivy cnumber` at the top of every file (the port uses the C
  type names `int32`, `long`, `bool`, ...).
* `export module <name>;` + `import <dep>;` (the module system of the
  port: `token`, `lexer`, `preprocessor`, `ast`, `parser`, `hir`,
  `hir_builder`).
* `extern "C" void* __ivy_alloc(unsigned long long n);` and
  `extern "C" void __ivy_free(void* p);` for the pools.
* **No globals**: everything is a field of a struct (`HirBuilder`,
  `HDumper`); the driver puts the raw pointers into a struct because
  "free functions cannot take raw-pointer parameters" (see
  `selfhost/hir/hirdrive.ivy:25`).
* `unsafe { ... }` around every raw-pointer dereference.
* NUL-terminated `const char*` strings; the names come from the parser's
  interned pool, so they can be stored as pointers without copying.
* Every pool has a `release()` that frees **every** allocation (see
  `selfhost/hir/hir_builder.ivy:454-520`).
* English comments; the port's comment style is a `//` block at the top of
  the file explaining the mirror, then short comments on the non-obvious
  lines.
* The diagnostics are collected in three parallel pools (`dline_`,
  `dcol_`, `dmsg_`) and rendered by the driver - the port's HIR builder
  already does this, so the MIR builder should use the same shape.

### 7.4 The scope decision

**H5.1 covers**:
* the MIR model (the CFG part: `MInst`, `MBlock`, `MFunction`, `MExpr`);
* the builder: the block creation and linking, every statement and every
  expression form, the compound-assignment expansion, the `move()`
  special case, the `cloneExpr` deep copy;
* the **lifetime propagation** (the `lifetime` field of every expression
  and the `params_` / `scopes_` maps) - **this is needed for the
  byte-identical dump**, because the `[lt: X]` suffix is printed;
* the `--mir` dump format, byte for byte.

**H5.2 defers**:
* the **borrow checker** (`checkBorrow`, `registerBorrower`,
  `releaseBorrower`, `releaseBorrowsInScope`) and its three diagnostics;
* the **lifetime checker** (`checkReturn`, `checkStore`) and its
  diagnostics;
* the **move tracker** (`moveStates_`, `markMovedOut`, `isMovedOut`,
  `reinitialize`) - unreachable in the oracle, but the port should mirror
  it anyway for fidelity;
* the **`nextcase` label check** (the error) - a one-liner, but it belongs
  with the other diagnostics.

**Justification**: the `[lt: X]` suffix is a *pure function* of the HIR
(no diagnostics, no side effects) - it is data, not analysis.  Splitting
it off would make the H5.1 dump permanently non-byte-identical and would
mean re-touching every expression node in H5.2.  The checks, on the other
hand, are diagnostics: they change the *exit code* and the *stderr*, not
the dump.  Deferring them means the H5.1 corpus is "MIR-clean" in the
sense that every fixture must reach the MIR stage without a diagnostic.

**Consequence for the corpus**: the H5.1 corpus must consist of fixtures
that produce a dump.  The class-C fixtures (the MIR diagnostics) belong to
the H5.2 corpus.

### 7.5 The recommended decomposition (the sub-phases)

| sub-phase | content | reviewable output | depends on |
|---|---|---|---|
| **5.1.1** | the model `selfhost/mir/mir.ivy` + the dump printer skeleton (the function/block/instruction lines, no expressions) | a driver that prints the function headers and the (empty) blocks | - |
| **5.1.2** | the builder's **statement** lowering: `Compound`, `Null`, `Decl`, `Return`, `Break`, `Continue`, `ExprStmt` (Store / Eval), `Unsafe` | the dump of the "straight-line" fixtures matches | 5.1.1 |
| **5.1.3** | the builder's **control flow**: `If`, `While`, `DoWhile`, `For`, `Switch`, `NextCase` | the dump of the control-flow fixtures matches | 5.1.2 |
| **5.1.4** | the builder's **expressions**: the literals, `IdentRef`, `Unary`, `Binary`, `Ternary`, `Call`, `Index`, `Member`, `Assign`, `InitList`, `Cast` + the compound-assignment expansion + `cloneExpr` | the dump of the expression fixtures matches | 5.1.2 |
| **5.1.5** | the **lifetime propagation** (`params_`, `scopes_`, `mergeLifetime`, the `&`/`*`/`+`/`-`/ternary rules, the `move()` rule, the unconditional `Unknown` of a call) + the `[lt: X]` suffix in the printer | the dump of the lifetime fixtures matches | 5.1.4 |
| **5.1.6** | the **driver** `mirdrive.ivy` + the `difftest.ps1` wiring + the corpus | the new suite passes | 5.1.5 |
| **5.1.7** | the HIR gap **G1**: the virtual dispatch (the HIR builder's detection + `HExpr`'s `isVirtual` / `vtableSlot` / `methodName`) | the virtual-call fixture matches | 5.1.4 + a `hir.ivy` / `hir_builder.ivy` change |
| **5.1.8** | the edge cases: the `float` formatting, the empty function, the bodyless declaration, the `[unsafe]` marker, the CRLF line endings | the whole corpus passes | 5.1.6 |

**The recommended order** is 5.1.1 -> 5.1.2 -> 5.1.4 -> 5.1.5 -> 5.1.3 ->
5.1.6 -> 5.1.7 -> 5.1.8.  The expressions before the control flow,
because the control flow only needs `buildExpr` to return *something*;
the lifetime propagation after the expressions, because it is a property
of the expression tree.

### 7.6 The harness wiring (`selfhost/difftest.ps1`)

The harness takes:

* `-Dir <dir>` - the corpus directory (the new `selfhost/corpus/mir/...`);
* `-Kinds all` - the byte filter; the MIR dump has no token kinds, so
  `all` is required (the `=== path ===` markers and every line are kept);
* `-Oracle --mir` - the oracle's flag;
* `-Modules token,lexer,preprocessor,ast,parser,hir,hir_builder,mir,mir_builder`
  - the modules, compiled **in this order** (each one's `.ivm` must exist
  before its dependents);
* `-ModuleDirs selfhost/parsing,selfhost/hir,selfhost/mir` - where the
  module sources live (the first directory that contains `<m>.ivy` wins);
* `-DriverDir selfhost/mir` - where the driver source lives;
* `-Driver mirdrive` - the driver's base name (`selfhost/mir/mirdrive.ivy`
  -> `selfhost/mir/mirdrive.exe`).

The expected invocation:

```
powershell -File selfhost/difftest.ps1 `
  -Dir selfhost/corpus/mir/basic -Kinds all -Oracle --mir `
  -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder,mir,mir_builder `
  -ModuleDirs selfhost/parsing,selfhost/hir,selfhost/mir `
  -DriverDir selfhost/mir -Driver mirdrive
```

**The driver must print the `=== <path> ===` marker** exactly like
`selfhost/hir/hirdrive.ivy:451` (the harness prepends the same marker to
the oracle's side, `difftest.ps1:277`), and it must print the
**diagnostics instead of** (never in addition to) the dump when the HIR or
the MIR stage reports an error - the oracle prints the diagnostic on
stderr and **no** dump.

**A harness caveat**: `difftest.ps1` captures the oracle with
`> "$one" 2>&1` (the stdout and the stderr are merged into one file) and
the port with `(type $list | $port) > $actRaw 2>&1`.  Because
`std::cerr` is tied to `std::cout`, the oracle's stdout content is
flushed **before** the stderr content in the merged file - which is the
same order as the port's (everything on stdout).  The order therefore
matches.  (This is exactly what the H4.5 / H4.6 corpora already rely on.)

### 7.7 The risks

| risk | assessment | mitigation |
|---|---|---|
| **the HIR fidelity** (section 6) | **the biggest risk.**  G1 (the virtual dispatch) breaks the dump for virtual calls; G2/G3/G5/G6 are parser-level gaps that make 6 of the 91 fixtures unreachable | close G1 in H5.1 sub-phase 5.1.7 (the HIR builder's detection + three fields in `HExpr`); declare G2/G3/G5/G6 out of scope and keep them out of the corpus; G4 (the dtor) belongs to the RAII milestone |
| **the `__ret` / destructor / RAII dependency** | the oracle's HIR builder injects the ctor/dtor calls **and** the `__ret` temporary (`hir_builder.cpp:1790-1827` - a synthesized local holding the returned by-value aggregate, so that the destructor does not run on it).  The `__ret` local appears in the MIR dump as an ordinary `alloca <T> __ret`.  The MIR builder just lowers them like any other local/call.  The port's HIR builder injects neither | G4 (and the `__ret` synthesis) must be closed before the *full* corpus can match; for H5.1 the corpus must exclude the fixtures with a destructor.  Doing the injection in the MIR builder instead would **not** match the oracle's block order (the injected calls land in the *scope's* block, not in a MIR-level block) |
| **the naming / mangling of the MIR dump** | the dump prints the *qualified* function name (`math::abs`, `Counter::get`, `id<int32_t>`).  The port's HIR already carries the qualified name (the `--hir` dump matches), so the MIR builder must **not** re-mangle it | reuse the HIR name verbatim |
| **the MIR builder's interaction with the ported HIR features** | the templates (H4.4), the lifetimes (H4.5) and the moves (H4.6) are already in the port's HIR; the MIR builder reads them as ordinary data | the `move()` call must be recognised by the **bare callee name** `move` with exactly one argument (the H4.6 rule) - the port's HIR keeps the call as a call, so the MIR builder can test `callee == "move"` |
| **the block ordering** | the dump is order-sensitive: the block creation order of every statement form must be exact | the notes of section 2.3 are the normative reference; each statement form should be implemented and checked against one fixture |
| **the `float` formatting** | the MIR dump prints `os << double` (6 significant digits, `%g`-like).  The port's HIR already prints the same text for the same literal (`float 1.5`), so the MIR builder must **reuse** the HIR's text rather than re-format a `double` | keep the HIR's `sval` for a float in the MIR expression; do **not** re-format |
| **the `Unknown` call lifetime** | easy to "fix" by accident: the port must reproduce the oracle's unconditional `Unknown` (`:603`) | a dedicated fixture (`11_call`) covers it |
| **the CRLF line endings** | the oracle writes CRLF on Windows; the port's runtime does too | verified; `difftest.ps1` normalises anyway |
| **the `Switch` instruction printing nothing** | easy to implement "correctly" (printing the switch) and break the dump | a dedicated fixture (`17_switch_int`) covers it |
| **the unreachable features** | the port could implement `new`/`delete`/lambdas and get them wrong | they are unreachable in the oracle too; skip them and note them |

### 7.8 The size of the job

| item | count |
|---|---|
| the MIR model types | 6 (`MLifetime`, `MExpr`, `MParam`, `MInst`, `MBlock`, `MFunction`) + 1 (`MTranslationUnit`) + 2 for H5.3 (`MEnumInfo`, `MStructInfo`) |
| the builder functions | ~35 (`build`, `buildFunction`, `buildStmt`, `buildExpr`, `newBlock`, `emit`, `jumpTo`, `lookup`, `declare`, `mergeLifetime`, `cloneExpr`, `walkExpr` (skippable), the 14 statement handlers, the 20 expression handlers, the pool helpers) |
| the dump printer functions | ~12 (the function header, the block, the 20 expression kinds, the type printer, the lifetime suffix) |
| the estimated line count | the model ~250, the builder ~1300, the driver ~300, the corpus ~60 files |
| the oracle's reference | `src/mir/mir.h` 215 lines, `src/mir/mir_builder.cpp` 1401 lines, `src/app/main.cpp`'s `dumpMir*` ~190 lines |

For comparison, the port's HIR builder is 7193 lines of Ivy for a 5612-line
C++ HIR builder (the port's is bigger because it is a single struct with
flat pools and explicit index handling).  A 1300-1600-line
`mir_builder.ivy` is a reasonable estimate.

---

## 8. Open questions and the experiment that would settle each

| # | question | the experiment |
|---|---|---|
| Q1 | Should the `[lt: X]` suffix be in H5.1 (as this file recommends) or in H5.2? | decide before phase 2.  If H5.2, the H5.1 corpus can only compare the dump **without** the suffix, i.e. the harness needs a byte filter that strips the suffix - which weakens the guarantee.  The recommendation is to put the *propagation* in H5.1 (it is pure data) and only the *checks* in H5.2 |
| Q2 | Can the port's HIR builder mark a call as virtual (G1) without breaking the H4.x corpora? | the port's HIR builder does **not** implement the virtual dispatch at all - it resolves the call statically (`call 'Animal::speak'`).  The fix has two parts: (a) the HIR builder must detect that the receiver's static type is a base class that declares the method `virtual` and must compute the vtable slot; (b) `HExpr` (kind 12) must keep `isVirtual` / `vtableSlot` / `methodName`.  The experiment: implement (a) + (b), re-run the 26 existing suites (the `--hir` dump of a virtual call changes from `call 'X'` to `virtual_call 'm' slot=N`, so the HIR corpora must be checked) and re-run the fixture `77_virtual_call` |
| Q3 | Where should the ctor/dtor injection (G3, G4) be ported? | the oracle does it in the HIR builder.  Doing it in the MIR builder would produce a different block order (the injected calls would land in a different block) and would not match the oracle.  The experiment: implement the injection in the port's HIR builder and compare the `--hir` output with the oracle's for `34_struct_ctor` / `35_struct_dtor` |
| Q4 | Is the port's parser expected to grow `new`/`delete`, namespaces and braced init lists (G2, G5, G6)? | they are parser features; the parser is a documented subset of the oracle's.  The experiment: check `PLAN.md` for the parser's scope.  If they are out of scope, the H5.1 corpus must simply not contain them |
| Q5 | How should the port's `HStmt` carry the source location (G7)? | add `line` / `col` to `HStmt` (it is a pure addition, the HIR dump does not print them) and re-run the 26 suites to confirm nothing changes |
| Q6 | Does the port's HIR builder need `Call.target` (G9) for the H5.2 lifetime analysis? | yes - the analysis needs the callee's declared return lifetime.  The experiment: add a `target` index to `HExpr` (the function index) in the port's HIR builder and check that the `--hir` dump does not change |
| Q7 | Should the H5.1 corpus include the class-C fixtures (the MIR diagnostics)? | no: without the H5.2 checks the port would print a dump where the oracle prints a diagnostic.  The experiment: after H5.2, re-run the class-C fixtures and check that the port prints the diagnostic and **no** dump |
| Q8 | Is the oracle's `mir::Inst::Switch` really unprinted in every path? | yes - `dumpMirBlock` (`main.cpp:726-765`) has no `case Kind::Switch`.  The experiment: a `switch` with a `default` and with arms - the dump shows nothing for the instruction in both cases (fixtures `17`, `43`) |
| Q9 | How does the port's runtime print a `double` in the MIR dump? | the port's HIR dump already prints the float literal as a *text* (`e.sval`); the MIR dump must print the same text.  The experiment: a fixture with `1.5`, `1.0`, `1e10` and `0.1` and compare with the oracle's `os << double` |
| Q10 | Does the port need `mir::EnumInfo` / `mir::StructInfo` at all in H5.1? | no - `dumpMir` does not print them.  The experiment: none; just skip them and add them in H5.3 (the codegen milestone) |

---

## 9. The bookkeeping record (phase 1)

* **Created**: `selfhost/mir/` (the directory) and
  `selfhost/mir/MIR_NOTES.md` (this file).
* **Unchanged**: `src/**` (the oracle was **not** rebuilt -
  `build\Release\ivy.exe` keeps its mtime `2026-10-21 16:41:44` and its
  size 27031552), `selfhost/parsing/**`, `selfhost/hir/**`,
  `selfhost/difftest.ps1`, `selfhost/corpus/**`, `PLAN.md`, `.gitignore`,
  `examples/`, `opencode.json`.
* **No corpus file was added to the repo** - the 91 fixtures live in the
  scratch directory `h51r1\fx/` only.
* **No commit** was made.  `git status --porcelain` shows only the new
  `selfhost/mir/MIR_NOTES.md`.
* **The port build was done outside the tree**: `selfhost/parsing` and
  `selfhost/hir` were **copied** to `h51r1\port\` and compiled there, so
  no `.obj` / `.ivm` / `.exe` was written into `selfhost/**`.
* **The opencode session store was never touched**: no file under
  `C:\Users\ASUS\.local\share\opencode\` was read, grepped, listed or
  written, and no `opencode.db*` was accessed anywhere on the machine.
  The only path used under the user's profile was the pre-approved
  scratch directory `C:\Users\ASUS\AppData\Local\Temp\opencode\h51r1\`.

---

## 10. Phase 2 (implementation): the MIR model, the builder and the driver

**This is phase 2 of 4** (implementation).  Phase 1 was the research
(MIR_NOTES sections 1-9); phase 3 finishes the lowering and adds the H5.2
checks; phase 4 adds the corpus and verifies.

### 10.1 The state of the tree at the end of the phase

| item | value |
|---|---|
| repo | `D:\project\Ivy\ivyc` (drive `D:`) |
| HEAD | `0cbcd31` (unchanged - **no commit was made**) |
| working tree | `git status --porcelain` shows only the untracked `selfhost/mir/` |
| oracle | `build\Release\ivy.exe`, 27031552 bytes, mtime **2026-10-21 16:41:44** (NOT rebuilt) |
| `src/**` | **unchanged** |
| `selfhost/parsing/**`, `selfhost/hir/**` | **unchanged** |
| `selfhost/difftest.ps1`, `PLAN.md`, `selfhost/corpus/**` | **unchanged** |
| scratch | `C:\Users\ASUS\AppData\Local\Temp\opencode\h51f2\` |

The three new files (the only sources that were added):

| file | lines | bytes | content |
|---|---|---|---|
| `selfhost/mir/mir.ivy` | 161 | 6.7 K | the MIR model (the mirror of `src/mir/mir.h`) |
| `selfhost/mir/mir_builder.ivy` | 2355 | 72.6 K | the builder (the mirror of `src/mir/mir_builder.cpp`) |
| `selfhost/mir/mirdrive.ivy` | 674 | 21.1 K | the driver (the `--mir` path of `src/app/main.cpp`) |

### 10.2 The model (`selfhost/mir/mir.ivy`)

The port keeps the oracle's **alternative order** and its **alternative
numbers**: the kind of an expression is the index of the oracle's
`std::variant` alternative, and the kind of an instruction is the index of
`mir::Inst::Kind`.  The dump does not print the numbers, so the numbering
is a pure cross-reference between the two implementations - but it makes
the mapping of section 10.3 mechanical.

| oracle | port | notes |
|---|---|---|
| `mir::Lifetime::Kind` (None, Named, Static, Local, Unknown) | `MExpr.lt` (0, 1, 2, 3, 4) | the same order; the name of a `Named` lifetime is `MExpr.ltname` |
| `mir::Expr::node` (20 alternatives) | `MExpr.kind` (0..19) | the same order, 0-based (the oracle's variant index) |
| `mir::Expr::type` | `MExpr.type` (`AType`) | |
| `mir::Expr::lifetime` | `MExpr.lt` + `MExpr.ltname` | the H5.2 analysis is not modelled, the annotation is |
| `mir::Expr::loc` | `MExpr.line` + `MExpr.col` | the port's HIR expressions carry a location |
| `mir::Expr::IntegerLit::value` | `MExpr.ival` | |
| `mir::Expr::FloatLit::value` | `MExpr.fval` (+ `MExpr.sval` = the oracle's text) | see 10.7 |
| `mir::Expr::StringLit::raw` / `::decoded` | `MExpr.sval` | the decoding is H5.3 |
| `mir::Expr::CharLit::raw` / `::decoded` | `MExpr.sval` | the decoding is H5.3 |
| `mir::Expr::BoolLit::value` | `MExpr.ival` | |
| `mir::Expr::IdentRef::name` | `MExpr.sval` | |
| `mir::Expr::Unary::op` / `::isPrefix` / `::operand` | `MExpr.sval` / `MExpr.flag` / `MExpr.a` | |
| `mir::Expr::Binary::op` / `::lhs` / `::rhs` | `MExpr.sval` / `MExpr.a` / `MExpr.b` | |
| `mir::Expr::Ternary::cond` / `::thenBranch` / `::elseBranch` | `MExpr.a` / `MExpr.b` / `MExpr.c` | |
| `mir::Expr::Call::callee` | `MExpr.sval` | |
| `mir::Expr::Call::target` | - (H5.3) | the call targets are resolved in the post-pass |
| `mir::Expr::Call::returnLifetime` | `MExpr.retlt` | H5.3 |
| `mir::Expr::Call::args` | the `MUnit.args` run, `MExpr.astart` / `::an` | |
| `mir::Expr::Call::isVirtual` | `MExpr.flag` | **H5.2 / G1**: the port's HIR does not mark a virtual call |
| `mir::Expr::Call::vtableSlot` | `MExpr.ival` | **H5.2 / G1** |
| `mir::Expr::Call::methodName` | `MExpr.sval2` | **H5.2 / G1** |
| `mir::Expr::Index::base` / `::index` | `MExpr.a` / `MExpr.b` | |
| `mir::Expr::Member::base` / `::name` / `::isArrow` / `::isScope` | `MExpr.a` / `MExpr.sval` / `MExpr.flag` | `isScope` is H5.3 (G8) |
| `mir::Expr::Assign::op` / `::lhs` / `::rhs` | `MExpr.sval` / `MExpr.a` / `MExpr.b` | |
| `mir::Expr::New::type` / `::args` | `MExpr.type` / the args run | unreachable (G2) |
| `mir::Expr::Delete::operand` / `::isArray` | `MExpr.a` / `MExpr.flag` | unreachable (G2) |
| `mir::Expr::InitList::elements` | the args run | |
| `mir::Expr::Lambda::funcName` / `::closureType` / `::captureInits` | `MExpr.sval` / `MExpr.sval2` / the args run | dead (the oracle crashes) |
| `mir::Expr::Cast::kind` / `::operand` | `MExpr.flag` / `MExpr.a` | the cast kind keeps the oracle's numbers (Static=0, Reinterpret=1, Const=2, CStyle=3) |
| `mir::Inst::Kind` (Alloca, Store, Eval, Ret, CondBranch, Jump, Switch) | `MInst.kind` (0..6) | the same order |
| `mir::Inst::inUnsafe` | `MInst.in_unsafe` | |
| `mir::Inst::Alloca::var` / `::type` / `::init` | `MInst.var` / `MInst.type` / `MInst.a` | |
| `mir::Inst::Store::target` / `::value` | `MInst.a` / `MInst.b` | |
| `mir::Inst::Eval::value` | `MInst.a` | |
| `mir::Inst::Ret::value` | `MInst.a` | |
| `mir::Inst::CondBranch::cond` / `::thenBlock` / `::elseBlock` | `MInst.a` / `MInst.b` / `MInst.c` | the block indices |
| `mir::Inst::Jump::target` | `MInst.b` | |
| `mir::Inst::Switch::cond` / `::arms` / `::defaultBlock` | `MInst.a` / the arm run / `MInst.b` | the arm run is `MUnit.arm_val` + `MUnit.arm_blk` |
| `mir::Block::insts` | the `MInst` run of the block (`MBlock.first` / `::last` + `MInst.next`) | a block has no label: its index is its identity |
| `mir::Param::type` / `::name` / `::lifetime` / `::loc` | `MParam.type` / `::name` / `::lt` / `::line` + `::col` | |
| `mir::Function::name` | `MFunction.name` | |
| `mir::Function::namespacePrefix` | - (H5.3) | the name already carries it |
| `mir::Function::returnType` | `MFunction.ret` | |
| `mir::Function::params` | the `MParam` run (`MFunction.pstart` / `::pn`) | |
| `mir::Function::lifetimes` | the `MLifetime` run (`MFunction.lt_start` / `::lt_n`) | **only for the functions that have a body** (see 10.6) |
| `mir::Function::returnLifetime` | `MFunction.return_lifetime` | |
| `mir::Function::blocks` | the `MBlock` run (`MFunction.block_start` / `::block_n`) | |
| `mir::Function::isExternC` | `MFunction.is_extern_c` | |
| `mir::Function::hasBody` | `MFunction.has_body` | |
| the other flags (`isVariadic` ... `vtableOf`) | - (H5.3) | not printed |
| `mir::TranslationUnit::functions` | `MUnit.fns` | |
| `mir::TranslationUnit::enums` / `::structs` | - (H5.3) | not printed |

**The sizes** (x86-64, the port's convention): `sizeof(MExpr)` = 176,
`sizeof(MInst)` = 136, `sizeof(MBlock)` = 16, `sizeof(MParam)` = 80,
`sizeof(MLifetime)` = 16, `sizeof(MFunction)` = 128.  Every `__ivy_alloc`
uses the exact element size; the sizes were **measured** with a test
program that writes a record at a candidate offset and reads it back
through the real indexing (`h51f2\sizes.ivy`).

> **Correction (phase 5, D2).**  `sizeof(MParam)` was documented as **88**
> here (and in `selfhost/mir/mir.ivy`); the real size is **80** (the
> 48-byte `AType` + four 8-byte fields = 32).  The "88" was an arithmetic
> slip in the sentence that follows it - the sentence already said "four
> 8-byte fields plus the 48-byte AType", which is 32 + 48 = 80.  The
> allocations used `* 88` (an over-allocation of 8 byte per element: it
> was harmless at runtime but it broke the invariant "pool size ==
> sizeof(struct)").  The measurement (phase 5): a C++ probe with the port's
> own field layout (`long` = 64 bit, as in the port's LLVM IR) prints
> `sizeof(MParam)` = **80**, `offsetof(MParam, name)` = 48, `::lt` = 56,
> `::line` = 64, `::col` = 72; the port's own LLVM IR agrees
> (`%struct.MParam = type { %struct.AType, ptr, ptr, i64, i64 }` with
> `%struct.AType = type { ptr, ptr, i64, i1, i1, i1, i64, i64 }`).
> The two allocations now use `* 80`
> (`selfhost/mir/mir_builder.ivy:308` - `grow_mparams` - and `:564` -
> `setup`).

### 10.3 The builder (`selfhost/mir/mir_builder.ivy`)

`MirBuilder` mirrors `ivy::MirBuilder`:

| oracle | port |
|---|---|
| `build()` | `build()` - the functions are pushed into the pool **before** their body is built, so the order of the dump is the order of the HIR |
| `buildFunction()` | `build_function()` - the per-function state is reset, the parameters get their lifetime, the function scope is pushed, the entry block is created and the statements are built |
| `newBlock()` | `new_block()` - the index of the block in the function's block list is its identity |
| `emit(kind, loc)` | `emit(kind)` - the instruction is appended to the current block (`MBlock.first` / `::last` + `MInst.next`) and `in_unsafe` is set from the unsafe depth |
| `jumpTo(target)` | `jump_to(target)` - **no** jump after a `Ret` |
| `lookup(name)` | `lookup(name)` - the innermost local that shadows the name, then the parameters |
| `declare(name, lt)` | `declare(name, kind, lname)` - like the oracle's `emplace`, an entry that already exists in the same scope is **not** replaced |
| `buildStmt()` | `build_stmt()` + one method per statement form (`st_compound`, `st_decl`, `st_if`, `st_while`, `st_dowhile`, `st_for`, `st_return`, `st_break`, `st_continue`, `st_nextcase`, `st_exprstmt`, `st_unsafe`, `st_switch`) |
| `buildExpr()` | `build_expr()` + one method per expression form (`ex_int` ... `ex_initlist`) |
| `mergeLifetime()` | `merge_lt()` |
| `cloneExpr()` | `clone_expr()` (the compound-assignment expansion) |
| `checkBorrow` / `checkReturn` / `checkStore` / the move tracker | **H5.2** - not implemented; the diagnostics are collected in the same pools as the HIR ones (`dline_` / `dcol_` / `dmsg_`) so that the driver already prints them instead of the dump |
| `resolveCalls` / `decodeLiterals` / `walkExpr` | **H5.3** - not needed for the dump |

The lowering that is implemented covers **every** statement form and every
expression form of the oracle (including the control flow, the compound
assignment expansion and the `move()` special case), so phase 3 only has
to add the H5.2 checks.

### 10.4 The driver (`selfhost/mir/mirdrive.ivy`)

The driver is a copy of `selfhost/hir/hirdrive.ivy` with the MIR stage
added:

* it reads the file paths from stdin (one per line), prints the
  `=== <path> ===` marker and runs the pipeline
  `lexer -> preprocessor -> parser -> HIR builder -> MIR builder -> dump`;
* a diagnostic of **any** stage replaces the dump (the oracle prints the
  diagnostic on stderr and **no** dump at all - see 0.2);
* the dump is printed by `MirDumper`, the port's copy of `dumpMir` /
  `dumpMirBlock` / `dumpMirExpr` of `src/app/main.cpp`, including the
  byte-sensitive details: the ` [lt: X]` suffix of the expressions (with a
  space after the colon) and the ` [lt:X]` suffix of the parameters
  (**without** a space), the ` [unsafe]` marker, the `switch` instruction
  that prints **nothing**, the `jump` / `condbranch` that print no target,
  and the ` [lt: unknown]` of **every** non-`move` call (the oracle bug of
  `mir_builder.cpp:603`);
* the exit code is 1 when a stage reported a diagnostic (the oracle's
  behaviour), 2 when a file could not be opened (the oracle's behaviour
  too) and 0 otherwise.

### 10.5 The byte-exact results

The comparison is done with **raw byte redirection** (`cmd /c "..." >
file 2>&1`), never with a PowerShell text capture.  For every fixture
below the oracle's stdout and the port's stdout are **identical byte for
byte** (including the CRLF line endings) and the exit codes are equal:

| fixture | shape | oracle bytes | port bytes | rc | result |
|---|---|---|---|---|---|
| `01_empty` | an empty function | 42 | 42 | 0 | IDENTICAL |
| `02_return_void` | a bare `return` | 51 | 51 | 0 | IDENTICAL |
| `03_return_const` | a constant return | 68 | 68 | 0 | IDENTICAL |
| `04_local_init` | an `int32` local with an initializer | 104 | 104 | 0 | IDENTICAL |
| `06_two_locals` | two locals | 186 | 186 | 0 | IDENTICAL |
| `07_param` | a by-value parameter | 78 | 78 | 0 | IDENTICAL |
| `08_param_ref` | a reference parameter | 85 | 85 | 0 | IDENTICAL |
| `10_arith` | arithmetic over locals | 288 | 288 | 0 | IDENTICAL |
| `13_if` | an `if` / `else` | 204 | 204 | 0 | IDENTICAL |
| `14_while` | a `while` loop | 317 | 317 | 0 | IDENTICAL |
| `15_for` | a `for` loop | 496 | 496 | 0 | IDENTICAL |
| `17_switch_int` | a `switch` | 166 | 166 | 0 | IDENTICAL |
| `19_break_continue` | `break` / `continue` | 728 | 728 | 0 | IDENTICAL |
| `26_lifetime` | a lifetime annotation | 145 | 145 | 0 | IDENTICAL |
| `31_borrow_shared_shared` | two shared borrows | 165 | 165 | 0 | IDENTICAL |
| `43_nextcase` | an unlabelled `nextcase` | 163 | 163 | 0 | IDENTICAL |
| `48_string` | a string and a char literal | 156 | 156 | 0 | IDENTICAL |
| `65_char_and_float` | char / float / bool literals | 192 | 192 | 0 | IDENTICAL |
| `67_compound_assign` | `+= -= *= /= %=` | 487 | 487 | 0 | IDENTICAL |
| `84_param_unknown_lt` | a bodyless declaration | 44 | 44 | 0 | IDENTICAL |
| `91_nested_call_arg` | a call as a call argument | 223 | 223 | 0 | IDENTICAL |

**21 / 21 identical, 21 / 21 equal exit codes.**

Over the **whole** set of the 91 phase-1 fixtures (the oracle's captured
stdout of `h51r1\out\*.out.bin` compared with the port's output, the
oracle's stderr used for the fixtures whose stdout is empty):

| class | count | result |
|---|---|---|
| **A - reachable** | **78** | **78 byte-identical** |
| **C - the MIR-stage diagnostics** | 6 | the port prints a dump where the oracle prints a diagnostic (the H5.2 checks) |
| **B - blocked by a missing HIR feature** | 7 | the port's HIR cannot express the construct (G1-G6) |

The 78 identical fixtures are **exactly** the 78 fixtures whose `--hir`
output is already byte-identical (the 13 exceptions are the same 13
fixtures as in phase 1).  In other words: **the MIR dump adds no new
fidelity gap** - every fixture the port's HIR can express is now
byte-identical at the MIR level too.

### 10.6 The shapes that are not byte-exact yet (phase 3)

| fixture | class | why | what phase 3 has to do |
|---|---|---|---|
| `29_borrow_mut_mut` | C | the borrow checker (H5.2) | implement `checkBorrow` + the three messages |
| `30_borrow_mut_shared` | C | the borrow checker (H5.2) | idem |
| `87_borrow_mut_after_shared` | C | the borrow checker (H5.2) | idem |
| `32_dangling_return` | C | the lifetime checker on a return (H5.2) | implement `checkReturn` + the two messages |
| `80_lt_mismatch` | C | the lifetime checker on a return (H5.2) | idem |
| `61_nextcase_label` | C | the `nextcase` check **is** implemented, but the port's HIR statements have no source location (G7), so the diagnostic is reported at `0:0` instead of `5:13` | add `line` / `col` to the port's `HStmt` (G7) or accept the location difference |
| `34_struct_ctor` | B | G3 (the struct construction is not parsed by the port) | out of scope |
| `35_struct_dtor` | B | G4 (the destructor is not injected by the port's HIR builder) | out of scope |
| `36_new_delete` | B | G2 (`new` / `delete` are not parsed by the port) | out of scope |
| `37_lambda` | B | the oracle crashes in the HIR stage | out of scope |
| `38_namespace` | B | G6 (the namespaces are not parsed by the port) | out of scope |
| `47_struct_initlist` | B | G5 (the braced init lists are not parsed by the port) | out of scope |
| `77_virtual_call` | B | G1 (the port's HIR does not mark a virtual call) | the virtual dispatch (sub-phase 5.1.7) |

### 10.7 The pitfalls that were found on the way

| # | pitfall | the workaround |
|---|---|---|
| 1 | **The C++ code generator of the port miscompiles a record that is kept in a local variable across a call.**  A `MInst` (136 bytes) that was read with `get_minst()`, modified and written back with `set_minst()` came back **corrupted** (the `kind` was `-1` instead of `4`, the block targets were garbage) when the enclosing function had many large locals.  The corruption is silent: the dump simply loses the `condbranch` instruction.  The same code in a small function works, and adding an unrelated statement to the function "fixes" it - the trigger is the frame layout. | **Every statement form and every expression form has its own method** (a small frame) and **no record is kept in a local variable across a call**: the records are read and written through the pool pointers (`this.mu.exprs[i].field = ...`).  The rule is documented at the top of `mir_builder.ivy`.  This is a bug of the oracle's code generator (`src/codegen/codegen.cpp`), **not** of the port - it must not be "fixed" in `src/**`. |
| 2 | **The runtime of the port formats a double differently from the oracle.**  `lib/ivyrt.c` uses `_gcvt(v, 15, buf)` on Windows, so `2.0` is printed as `2.`, while the oracle's `operator<<(ostream, double)` prints `2`. | The MIR dump prints the **text** of the float literal (`MExpr.sval`, the text that the port's HIR builder already formatted like the oracle) instead of the double.  The same rule as in 7.7. |
| 3 | **The element size of the expression pool.**  The first version of the model used 168 bytes for `MExpr`; the real size is 176 (sixteen 8-byte fields plus the 48-byte `AType`).  The pool then overflowed into the neighbouring allocation and corrupted the instructions. | The sizes are documented in `mir.ivy` and were **measured** (see 10.2).  Every `__ivy_alloc` uses the exact size. |
| 4 | The C-style cast `(const char*)` is not accepted by the parser (only the built-in number types are). | `reinterpret_cast<const char*>(...)` (inside an `unsafe` block), like the rest of the port. |
| 5 | A raw pointer variable and a pointer dereference require an `unsafe` block, and a C-style cast requires one too. | The pools and the raw pointers are declared and dereferenced inside `unsafe` blocks. |
| 6 | The port's HIR statements have **no** source location (the gap **G7** of section 6.1): the `HStmt` of `selfhost/hir/hir.ivy` has no `line` / `col`.  The MIR instructions therefore take the location of their expression (and `0` when they have none). | The diagnostics of the MIR stage are therefore located at the *expression*; the `nextcase` diagnostic is located at `0:0`.  Fixing it needs a `line` / `col` in the port's `HStmt` (a change in `selfhost/hir/hir.ivy`, i.e. **G7**). |
| 7 | The `Function::lifetimes` of the MIR are filled by the oracle's `buildFunction()`, so a function **without a body** has **no** lifetimes in the MIR (the `lifetimes:` line is not printed), even though its HIR has them. | The port copies the lifetimes in `build_function()` (and not in `build()`), like the oracle.  Without this, `84_param_unknown_lt` differs. |
| 8 | The arguments of a **nested** call are pushed into the argument pool before the arguments of the enclosing call, so the run of the arguments of a call does not start where the enclosing call's run starts. | The start of the run is the position of the **first** argument that the call pushes (not the position of the pool before the arguments are built). |
| 9 | The oracle cannot open a file that does not exist: it prints `ivy: error: cannot open file '"<path>"'` and exits with **2** (not 1). | The driver prints the same message and returns 2; a diagnostic of a stage returns 1 (the oracle's behaviour).  The driver processes a list of files, so the code of the **first** failure is the exit code. |

### 10.8 The harness wiring that phase 4 needs

The wiring was **validated end to end** (in a copy of the repo in the
scratch, so that no corpus file was added to the repo): with a corpus of
ten fixtures the harness reports

```
[1/4] building selfhost modules ...
[2/4] collecting corpus ...
      files: 10
[3/4] running C++ oracle + Ivy port ...
[4/4] comparing (kinds: all) ...
PASS: identical (filtered) output on 10 files
```

with **exactly** this invocation:

```
powershell -File selfhost/difftest.ps1 `
  -Dir selfhost/corpus/mir/<group> -Kinds all -Oracle --mir `
  -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder,mir,mir_builder `
  -ModuleDirs selfhost/parsing,selfhost/hir,selfhost/mir `
  -DriverDir selfhost/mir -Driver mirdrive
```

* the modules must be compiled **in that order** (each one's `.ivm` must
  exist before its dependents);
* `-Kinds all` is required (the MIR dump has no token kinds);
* the corpus must stay **MIR-clean** for H5.1: a fixture that makes the
  oracle print a MIR-stage diagnostic cannot match before the H5.2 checks
  are implemented (the six fixtures of section 10.6);
* the corpus must also stay free of the HIR gaps G1-G6 (the seven
  fixtures of section 10.6).

### 10.9 The regression run of the 26 existing suites

The 26 suites of `selfhost/difftest.ps1` were run **sequentially**, each
with a private temp directory (`%TEMP%` = `h51f2\tmp_<suite>`), with the
invocations of the header comment of `difftest.ps1`:

| suite | corpus | files | result |
|---|---|---|---|
| H1.1 | lexer/ident | 10 | PASS |
| H1.2 | lexer/num | 8 | PASS |
| H1.3 | lexer/str | 7 | PASS |
| H1.4 | lexer/op | 5 | PASS |
| H1.5 | lexer/trivia | 7 | PASS |
| H1.6 | lexer/err | 9 | PASS |
| H2.1 | pp/define_obj | 10 | PASS |
| H2.2 | pp/define_fn | 12 | PASS |
| H2.3 | pp/cond | 16 | PASS |
| H2.4 | pp/expand | 16 | PASS |
| H2.5 | pp/predef | 20 | PASS |
| H2.6 | pp/line | 22 | PASS |
| H2.7 | pp/no_include | 9 | PASS |
| H3.1 | parser/decl | 15 | PASS |
| H3.2 | parser/expr | 13 | PASS |
| H3.3 | parser/stmt | 15 | PASS |
| H3.4 | parser/aggregate | 14 | PASS |
| H3.5 | parser/template | 16 | PASS |
| H3.6 | parser/module | 16 | PASS |
| H3.7 | parser/err | 19 | PASS |
| H4.1 | hir/name | 27 | PASS |
| H4.2 | hir/type | 124 | PASS |
| H4.3 | hir/constexpr | 70 | PASS |
| H4.4 | hir/template | 99 | PASS |
| H4.5 | hir/borrow | 45 | PASS |
| H4.6 | hir/move | 60 | PASS |

**26 / 26 suites PASS, 684 / 684 files.**

### 10.10 The bookkeeping record (phase 2)

* **Created**: `selfhost/mir/mir.ivy`, `selfhost/mir/mir_builder.ivy`,
  `selfhost/mir/mirdrive.ivy` (and this section of the notes).
* **Unchanged**: `src/**` (the oracle was **not** rebuilt - it keeps its
  mtime `2026-10-21 16:41:44` and its size 27031552),
  `selfhost/parsing/**`, `selfhost/hir/**`, `selfhost/difftest.ps1`,
  `selfhost/corpus/**`, `PLAN.md`.
* **No corpus file was added to the repo**: the ten fixtures of the
  harness validation live in `h51f2\corpus\mir\basic\` (and in a copy of
  the repo in `h51f2\shadow\`) only.
* **No commit** was made.  `git status --porcelain` shows only the new
  `selfhost/mir/`.
* **The build products** (`selfhost/mir/*.obj`, `selfhost/mir/*.ivm`) are
  the ones the harness itself writes (they are covered by `.gitignore`,
  like the ones of `selfhost/parsing` and `selfhost/hir`).
* **The opencode session store was never touched**: no file under
  `C:\Users\ASUS\.local\share\opencode\` was read, grepped, listed or
  written, and no `opencode.db*` was accessed anywhere on the machine.
  The only path used under the user's profile was the pre-approved
  scratch directory `C:\Users\ASUS\AppData\Local\Temp\opencode\h51f2\`.


---

## 11. Phase 3 (implementation): widening the probe set, the fixes and the open questions

**This is phase 3 of 4** (implementation).  Phase 1 was the research
(sections 1-9), phase 2 wrote the model, the builder and the driver
(section 10), phase 3 widens the probe set far beyond the 91 fixtures of
the recon, fixes every divergence that is in the H5.1 scope and settles the
three open questions of the brief (the `nextcase` location, the code
generator bug of the oracle and the double formatting); phase 4 adds the
corpus and verifies.

### 11.1 The state of the tree at the end of the phase

| item | value |
|---|---|
| repo | `D:\project\Ivy\ivyc` (drive `D:`) |
| HEAD | `0cbcd31` (unchanged - **no commit was made**) |
| working tree | `git status --porcelain` shows only the untracked `selfhost/mir/` |
| oracle | `build\Release\ivy.exe`, 27031552 bytes, mtime **2026-10-21 16:41:44** (NOT rebuilt) |
| `src/**` | **unchanged** |
| `selfhost/parsing/**` | **CHANGED** (3 places, see 11.4) |
| `selfhost/hir/**` | **CHANGED** (3 places, see 11.4) |
| `selfhost/mir/**` | changed (the builder, see 11.4) |
| `selfhost/difftest.ps1`, `PLAN.md`, `selfhost/corpus/**` | **unchanged** |
| scratch | `C:\Users\ASUS\AppData\Local\Temp\opencode\h51f3\` |

The phase-3 probe set: **222 new fixtures** in five groups (`fx`, `fx2`,
`fx3`, `fx4`, `fx5`) plus the **91 recon fixtures** of phase 1
(`h51r1\fx`) re-verified.  The probe harness (`h51f3\probe.ps1`) compares
the **raw bytes** of `ivy --mir <file>` with the raw bytes of the port's
`mirdrive.exe` (the output of the two streams is captured with `cmd /c`
redirection, never with a PowerShell text capture).

### 11.2 The probe inventory and the per-fixture verdicts

**222 new fixtures, 219 byte-identical, 3 different.**  The three
differences are one (d) H5.2 case (the location of the `nextcase`
diagnostic, the gap **G7**) and one (c) oracle crash; the second (d) case
is the same G7 problem in a second shape.

| group | what it covers | fixtures | identical | different |
|---|---|---|---|---|
| `fx` | the systematic sweep: 20 calls, 20 declarations/parameters, 29 expressions, 30 statements, 10 edge cases | 109 | **109** | 0 |
| `fx2` | the risky shapes: the labelled case, the lifetime data, the compound assignments, the templates, the inheritance, the operators | 40 | **40** | 0 |
| `fx3` | the discriminating probes: the `nextcase` in every loop/switch context, the nested switches | 18 | **16** | 2 (d) |
| `fx4` | the float rendering, the lifetime propagation, the stress shapes | 40 | **40** | 0 |
| `fx5` | the last batch: the compound assignments on a member/pointer, the `move()` of a struct, the `extern "C"` body | 15 | **14** | 1 (c) |

The three differences:

| fixture | class | what differs | why |
|---|---|---|---|
| `q02_labelled_nextcase_in_loop_in_switch` | (d) | the oracle reports `…ivy:6:17: error: nextcase target 'one' is not a case label in this switch`, the port reports the same message at `0:0` | **G7**: the port's `HStmt` carries no source location (11.5) |
| `q15_nextcase_to_default_label` | (d) | the same, at `6:13` vs `0:0` | **G7** |
| `t01_compound_lhs_call` | (c) | the oracle **crashes** (access violation, exit code `-1073741819` = `0xC0000005`) and prints nothing; the port prints the HIR diagnostic `struct 'S' has no matching operator '&'` | an oracle bug: the address of a **struct-typed local** crashes the oracle (see 11.7) |

The 91 recon fixtures re-verified: **78 byte-identical (0 regressions)**, the
same **13** that phase 2 listed (`29`, `30`, `32`, `34`, `35`, `36`, `37`,
`38`, `47`, `61`, `77`, `80`, `87`).

#### The full per-fixture table

The verdicts are `IDENT` (byte-identical), `(d)` (an H5.2 check), `(b)` (a
pre-existing port gap outside the H5.1 scope) or `(c)` (an oracle bug).
The oracle's byte count and the port's byte count are in
`h51f3\report_fx*.txt` and `h51f3\report_fx3.txt`.

**`fx` - the systematic sweep (109 fixtures, all IDENT)**

| area | fixtures | verdict |
|---|---|---|
| calls | `c01_call_zero` `c02_call_one` `c03_call_many` `c04_call_nested` `c05_call_result_used` `c06_call_in_condition` `c07_call_in_return` `c08_call_struct_arg` `c09_call_enum_arg` `c10_call_method_this` `c11_call_method_local` `c12_call_builtin_printf` `c13_call_builtin_io` `c14_call_template` `c15_call_recursive` `c16_call_void_stmt` `c17_call_ptr_arg` `c18_call_ref_arg` `c19_call_default_arg` `c20_call_operator_overload` | IDENT (the three that the oracle rejects are rejected identically: `c12`, `c17` are the C-style `char` / the raw pointer rules) |
| declarations and parameters | `d01_param_byvalue` `d02_param_byref` `d03_param_ptr` `d04_param_const` `d05_param_array` `d06_param_struct` `d07_lifetime_native` `d08_lifetime_legacy` `d09_lifetime_ptr_param` `d10_this_method` `d11_extern_c` `d12_constexpr_fn` `d13_if_constexpr` `d14_decl_only` `d15_many_params` `d16_many_locals` `d17_template_instantiated` `d18_struct_method` `d19_static_method` `d20_const_local` | IDENT |
| expressions | `e01_arith_int32` `e02_arith_mixed_widths` `e03_arith_unsigned` `e04_bool_logic` `e05_unary_ops` `e06_unary_incdec` `e07_compare_chain` `e08_ternary_nested` `e09_cast_static` `e10_cast_reinterpret` `e11_cast_const` `e12_sizeof_types` `e13_sizeof_expr` `e14_alignof` `e15_string_literal` `e16_char_literal` `e17_float_literal` `e18_nullptr` `e19_enum_const` `e20_enum_local` `e21_scoped_enum` `e22_bool_local` `e23_float_local` `e24_char_local` `e25_bitwise_shift` `e26_modulo` `e27_deep_nesting` `e28_all_binops` `e29_ptr_arith` | IDENT |
| statements | `s01_nested_blocks` `s02_if_else` `s03_else_if_chain` `s04_if_no_else` `s05_while` `s06_dowhile` `s07_for_full` `s08_for_empty` `s09_for_no_cond` `s10_switch_int` `s11_switch_enum` `s12_switch_no_default` `s13_switch_default_only` `s14_switch_nextcase` `s15_break_continue` `s16_continue_in_loop_in_switch` `s17_return_positions` `s18_exprstmt` `s19_decl_noinit` `s20_decl_multi` `s21_assign_simple` `s22_assign_compound` `s23_assign_chained` `s24_assign_field` `s25_assign_through_ptr` `s26_assign_array_elem` `s27_switch_many_cases` `s28_many_statements` `s29_unsafe_block` `s30_nested_loops` | IDENT |
| edge cases | `x01_large_frame` `x02_deep_nesting` `x03_many_cases` `x04_move_use` `x05_move_in_call_arg` `x06_reassign_lt` `x07_shadowing` `x08_multi_function` `x09_return_in_loop` `x10_empty_function` | IDENT |

**`fx2` - the risky shapes (40 fixtures, all IDENT)**
`p01_labelled_case` `p02_labelled_case_mismatch` `p03_lt_named_local`
`p04_lt_static_local` `p05_lt_local_addr` `p06_lt_addr_param` `p07_lt_deref`
`p08_lt_store_update` `p09_compound_member` `p10_compound_index`
`p11_compound_ptr` `p12_auto_decl` `p13_alias` `p14_inherit`
`p15_switch_in_loop` `p16_break_outside_loop` `p17_nested_unsafe`
`p18_empty_loop_body` `p19_for_shadow` `p20_method_on_ptr`
`p21_float_ops` `p22_bool_ops` `p23_wide_ints` `p24_sizeof_struct`
`p25_struct_across_call` `p26_multi_decl_stmt` `p27_ternary_lt`
`p28_return_ptr_decl_only` `p29_const_ptr_param_decl`
`p30_call_in_ternary` `p31_nested_ternary_call` `p32_void_in_unsafe`
`p33_struct_in_switch` `p34_qualified_enum_case` `p35_many_fns`
`p36_while_with_return` `p37_dowhile_with_return`
`p38_if_without_cond_branch` `p39_for_without_body` `p40_seq_expr`.

**`fx3` - the discriminating probes (18 fixtures)**

| fixture | verdict | comment |
|---|---|---|
| `q01_nextcase_in_loop_in_switch` | IDENT | the unlabelled `nextcase` in a loop inside a switch: the jump target is the loop's exit (not printed by the dump) |
| `q02_labelled_nextcase_in_loop_in_switch` | **(d)** | the oracle rejects the label (the label belongs to the switch, the innermost context is the loop): `6:17` vs `0:0` (G7) |
| `q03_nextcase_after_nested_switch` | IDENT | **was a defect** (the nested switch broke the outer switch) |
| `q04_labelled_nextcase_after_nested_switch` | IDENT | **was a defect** |
| `q05_switch_in_loop` | IDENT | |
| `q06_nextcase_in_switch_in_loop` | IDENT | |
| `q07_nextcase_last_case` | IDENT | the unlabelled `nextcase` in the last case: the jump is to the exit |
| `q08_nextcase_before_inner` | IDENT | **was a defect** |
| `q09_switch_in_switch_case` | IDENT | **was a defect** (the labelled case) |
| `q10_nextcase_in_while` | IDENT | |
| `q11_break_in_switch_in_loop` | IDENT | |
| `q12_continue_in_switch_in_loop` | IDENT | |
| `q13_switch_empty_body` | IDENT | |
| `q14_labelled_switch_with_case_label` | IDENT | **was a defect** (the labelled case) |
| `q15_nextcase_to_default_label` | **(d)** | the oracle rejects the unknown label: `6:13` vs `0:0` (G7) |
| `q16_nested_nextcase_chain` | IDENT | **was a defect** (the labelled case) |
| `q17_switch_in_for_with_nextcase` | IDENT | **was a defect** (the labelled case) |
| `q18_deep_switch_nesting` | IDENT | **was a defect** (the nested switch) |

**`fx4` (40 fixtures, all IDENT)**
`r01_float_rendering` `r02_float_negative` `r03_float_hex`
`r04_float_suffix` `r05_lt_named_param` `r06_lt_static_string`
`r07_lt_store_updates` `r08_lt_call_unknown` `r09_lt_move`
`r10_lt_ternary` `r11_lt_member` `r12_lt_index` `r13_addr_of_member`
`r14_template_multi` `r15_template_recursive` `r16_inherit_method`
`r17_operator_overloads` `r18_default_args` `r19_variadic_decl`
`r20_consteval` `r21_unsafe_marker` `r22_string_escapes`
`r23_raw_string` `r24_class_fields` `r25_enum_switch_values`
`r26_bool_switch` `r27_char_switch` `r28_many_locals_calls`
`r29_struct_local_across_call` `r30_deep_expr_nesting`
`r31_nested_calls_deep` `r32_many_stmts_with_calls`
`r33_switch_many_cases_calls` `r34_empty_switch_body` `r35_nested_if_deep`
`r36_multi_decl` `r37_conditional_assign` `r38_compound_all_ops`
`r39_float_local_ops` `r40_ptr_compound`.

**`fx5` (15 fixtures)**

| fixture | verdict | comment |
|---|---|---|
| `t01_compound_lhs_call` | **(c)** | the oracle crashes (the address of a struct local) |
| `t02_compound_ptr_field` `t03_move_complex_arg` `t04_nextcase_in_dowhile` `t05_extern_c_body` `t06_const_lt_param` `t07_switch_unsigned` `t08_constexpr_in_body` `t09_method_with_lt` `t10_nested_struct_member` `t11_return_void_call` `t12_empty_function_bodies` `t13_if_in_loop_in_switch` `t14_many_returns` `t15_assign_to_this_field` | IDENT | |

### 11.3 What the 222 probes found

Four real defects, all of them in the CFG construction of the `switch` /
`nextcase` machinery.  **Three of the four are in the HIR/parser layer**
(the case clauses of a switch were a contiguous run of the case pool, which
breaks as soon as a switch is nested in a case body, and the A6 case label
was dropped by the parser); **one is in the MIR layer** (the `nextcase`
used the case blocks of the *innermost switch* instead of the case blocks
of the *innermost loop context*).

| # | the defect | the smallest fixture that shows it | the class |
|---|---|---|---|
| D1 | the **A6 case label** (`case LABEL: value:`) was parsed but **dropped**: the port's `ACase` had no label field.  The oracle's `hir::Stmt::CaseClause::caseLabel` carries it and the MIR builder uses it to resolve `nextcase LABEL;`.  The port always reported "not a case label" | `p01_labelled_case` | (a) |
| D2 | the **case clauses of a switch were a contiguous run of the case pool**.  A switch that is nested in a case body pushes its own clauses into the same pool, so the run of the outer switch contained the clauses of the inner switch: the port built the wrong CFG *and* reported a spurious `switch has more than one 'default' case` | `q03_nextcase_after_nested_switch` | (a) |
| D3 | the **`nextcase` used the case blocks of the innermost switch** instead of the case blocks of the innermost *loop context*.  The oracle's `LoopCtx` carries the case blocks only for a switch context, so a `nextcase` inside a loop that is inside a switch jumps to the **exit of the loop** | `q01` (the jump target is not printed, so the difference is invisible in the dump, but the CFG differs) | (a) |
| D4 | the **base of the case-block run was not restored** after a nested switch, so the unlabelled `nextcase` of the enclosing switch used the wrong case count | `q03` | (a) |

All four are fixed (11.4).  After the fixes the whole probe set is
byte-identical except for the two G7 locations and the one oracle crash.

### 11.4 The fixes (port file:line <-> oracle file:line)

#### D1 - the A6 case label

| port | oracle |
|---|---|
| `selfhost/parsing/ast.ivy:311-316` - `ACase` gets `long next;` and `const char* label;` | `src/parsing/ast.h:320-324` - `ast::Stmt::CaseClause::caseLabel` |
| `selfhost/parsing/parser.ivy:3082` - the label is interned when the clause is `case LABEL: value:` | `src/parsing/parser.cpp:2505-2521` |
| `selfhost/parsing/parser.ivy:2123` / `:2139` - `push_case` / `set_case_next` | `src/parsing/parser.cpp:2517` (`clause.caseLabel = peek().lexeme`) |
| `selfhost/hir/hir.ivy:63-68` - `HCase` gets `long next;` and `const char* label;` | `src/hir/hir.h:217-221` - `hir::Stmt::CaseClause::caseLabel` |
| `selfhost/hir/hir_builder.ivy:3489` - `hc.label = ac.label;` | `src/hir/hir_builder.cpp:2103` - `cc.caseLabel = ac.caseLabel;` |
| `selfhost/mir/mir_builder.ivy:1993` (`st_switch`), `:2224` (`loop_label_blk`), `:2252` (`push_lbl`), `:1944` (`st_nextcase`) - the case-label map of the switch | `src/mir/mir_builder.cpp:993-997` (`caseLabels`), `:911-917` (the lookup) |

The oracle's `caseLabels` map is built from the clauses of the switch
*before* the bodies are built (`mir_builder.cpp:993-997`) and the lookup
happens in the innermost `LoopCtx` (`mir_builder.cpp:911`).  The port keeps
the labels in a pool (`lbl_name_` / `lbl_blk_`) and records, per loop
context, the range of the labels of its switch (`lp_lbl_base_` /
`lp_lbl_n_`).

#### D2 - the case clauses must be a linked list

The oracle's `ast::Stmt::Switch` holds a `std::vector<CaseClause>`, so
the clauses of a switch are **structurally** nested; the port's flat case
pool cannot express that with a contiguous run.  The fix replaces the
run with a **linked list**:

| port | oracle |
|---|---|
| `selfhost/parsing/ast.ivy:314` - `ACase.next` (the next clause of the same switch) | `src/parsing/ast.h:320-324` - `std::vector<CaseClause> cases` |
| `selfhost/parsing/parser.ivy:3112-3125` - `parse_switch` builds the list and counts the clauses | `src/parsing/parser.cpp:2498-2536` |
| `selfhost/parsing/astdrive.ivy:361` - the AST dump walks the list | `src/app/main.cpp` - `dumpStmt` (the switch) |
| `selfhost/hir/hir.ivy:66` - `HCase.next` | `src/hir/hir.h:219-220` |
| `selfhost/hir/hir_builder.ivy:3447-3503` - `build_switch` walks the AST list and links the HIR clauses | `src/hir/hir_builder.cpp:2101-2137` |
| `selfhost/hir/hirdrive.ivy:349` - the HIR dump walks the list | `src/app/main.cpp` - `dumpHirStmt` (the switch) |
| `selfhost/mir/mir_builder.ivy:1993-2152` - `st_switch` walks the HIR list | `src/mir/mir_builder.cpp:982-1040` |

The **pool element size** changed: `ACase` and `HCase` are now
`value(8) + sstart(8) + next(8) + label(8) = 32` bytes (measured with
`h51f3\sizes.ivy`, not guessed).  The two allocations that use the size
were updated: `selfhost/parsing/parser.ivy:308` (`* 24` -> `* 32`) and
`selfhost/hir/hir_builder.ivy:287` (`* 24` -> `* 32`).

**The dump impact of D1 and D2: none.**  Neither the `--ast` dump nor the
`--hir` dump prints the case label (`src/app/main.cpp` has no
`caseLabel`/`switchLabel` at all), so the 26 existing suites (which
compare the `--ast` and the `--hir` dumps byte for byte) are unaffected -
they were re-run and all of them still pass (11.9).

#### D3 and D4 - the `nextcase` and the loop context

The oracle's `LoopCtx` (`src/mir/mir_builder.h:102-110`) carries the case
blocks (`caseBlocks`) and the case labels (`caseLabels`) **only for a
switch context**; a loop context has neither.  The port had a single
`sw_base_`/`sw_n_` pair for the switch data, so it could not tell a loop
context from a switch context.

| port | oracle |
|---|---|
| `selfhost/mir/mir_builder.ivy:96-99` - the per-context switch data (`lp_sw_base_`, `lp_sw_n_`, `lp_lbl_base_`, `lp_lbl_n_`) | `src/mir/mir_builder.h:102-110` - `LoopCtx::caseBlocks` / `LoopCtx::caseLabels` |
| `selfhost/mir/mir_builder.ivy:2189` `loop_is_switch`, `:2198` `loop_case_n`, `:2206` `loop_case_blk`, `:2224` `loop_label_blk` | `src/mir/mir_builder.cpp:905` (`l.caseBlocks.size()`), `:911` (`l.caseLabels`) |
| `selfhost/mir/mir_builder.ivy:2288` - `push_loop_sw` (the switch data is pushed with the context) | `src/mir/mir_builder.cpp:1027-1030` (`loops_.push_back(std::move(lc))`) |
| `selfhost/mir/mir_builder.ivy:1944-1989` - `st_nextcase` | `src/mir/mir_builder.cpp:893-919` |
| `selfhost/mir/mir_builder.ivy:2099-2121` - the label map of the switch | `src/mir/mir_builder.cpp:993-997` |

The unlabelled `nextcase` now follows the oracle exactly:

```
next = case_index_ + 1;
if (next < ncases)  jump_to(case_block(next));
else                jump_to(loop_exit());
```

where `ncases` is the number of the case blocks of the innermost **switch
context** and `0` when the innermost context is a **loop** (the oracle's
`l.caseBlocks` is empty there, so the jump always goes to the exit of the
loop).  The labelled `nextcase` looks the label up in the case labels of
the innermost context only, so a label of an enclosing switch is **not**
visible from a nested loop (this is the oracle's behaviour, verified by
`q02`).

### 11.5 G7 - the sizing of the source location of a statement

**The question**: why does the port report the `nextcase` diagnostic at
`0:0`, and what would an H5.2 implementer have to add?

**Why `0:0`.**  The oracle's `hir::Stmt` has a `SourceLoc loc` field
(`src/hir/hir.h:228`) that the HIR builder copies from the AST statement
(`src/hir/hir_builder.cpp:1250`, `:1985`, `:1990`, ...).  The MIR builder
reports the `nextcase` diagnostic with **the location of the statement**:
`error(s.loc, "nextcase target '" + ... + "' is not a case label in this
switch")` (`src/mir/mir_builder.cpp:915`).  The port's `HStmt`
(`selfhost/hir/hir.ivy:36-47`) has **no** `line`/`col` field, so the port
has no location to report and passes `0, 0` to `diag()`
(`selfhost/mir/mir_builder.ivy:1988`).

**Where the location comes from in the oracle.**  The AST statement's
`loc` is the position of the **first token of the statement**
(`src/parsing/parser.cpp:2261`: `const SourceLoc loc = locOf(peek());` at
the top of `parseStatement`).  For `nextcase;` the first token is the
`nextcase` keyword, so the diagnostic is reported at the `nextcase`
keyword (fixture `61_nextcase_label`: the oracle says `5:13`, the `nextcase`
keyword is at line 5, column 13).

**The good news: the port's parser already has the exact location.**  The
port's `AStmt` carries `line`/`col` (`selfhost/parsing/ast.ivy:288-289`)
and the parser sets them from the first token of the statement
(`selfhost/parsing/parser.ivy:2838-2839` for `nextcase`: `s.line =
start.line; s.col = start.col;` where `start = this.peek(0)`).

**And the port's HIR builder already passes the location around**: the
helper `HStmt new_stmt(long kind, long line, long col)`
(`selfhost/hir/hir_builder.ivy:2129`) receives the AST statement's
`a.line`/`a.col` at **every** call site (21 call sites, all of the form
`this.new_stmt(<kind>, a.line, a.col)`) - the two parameters are simply
**discarded**.

**So G7 is a five-line change:**

| # | change | file | the impact |
|---|---|---|---|
| 1 | add `long line; long col;` to `HStmt` | `selfhost/hir/hir.ivy:36-47` | the record grows from **120** to **136** bytes (measured, see below) |
| 2 | `s.line = line; s.col = col;` in `new_stmt` | `selfhost/hir/hir_builder.ivy:2129-2142` | - |
| 3 | the statement pool: `* 120` -> `* 136` | `selfhost/hir/hir_builder.ivy:281` | the HIR statement pool grows by 13% (`st_cap = ntoks * 3 + 128`) |
| 4 | the `nextcase` diagnostic: `this.diag(this.sp[idx].line, this.sp[idx].col, ...)` | `selfhost/mir/mir_builder.ivy:1988` | the location of fixture `61_nextcase_label` becomes `5:13` |
| 5 | (for the H5.2 checks) the location of the instructions: the oracle uses `s.loc` for every `emit` (`src/mir/mir_builder.cpp:726`, `:793`, `:844`, `:861`, `:873`, `:925`), the port uses the location of the *expression* | `selfhost/mir/mir_builder.ivy` (`st_decl`, `st_return`, `st_exprstmt`, `st_if`, `st_while`, `st_dowhile`, `st_for`, `st_switch`) | the location is **not printed** by the `--mir` dump, so the dump does not change |

**The measured sizes** (x86-64, the port's convention; the measurement
program is `h51f3\sizes_hstmt.ivy`, it writes a record at a candidate
offset and reads it back through the real indexing):

| record | the fields | the size | the allocation |
|---|---|---|---|
| `HStmt` (today) | kind, sval, type(48), flag, a, b, c, sstart, sn, next | **120** | `selfhost/hir/hir_builder.ivy:281` (`* 120`) |
| `HStmt` (with G7) | + line, col | **136** | `* 136` |
| `HExpr` | 144 (unchanged) | 144 | `selfhost/hir/hir_builder.ivy:278` |
| `HParam` | 88 (unchanged) | 88 | `selfhost/hir/hir_builder.ivy:284` |
| `ACase` / `HCase` | 32 (after D1/D2) | 32 | `parser.ivy:308`, `hir_builder.ivy:287` |

**The dump impact: none.**  The `--hir` dump does not print the location
of a statement (`src/app/main.cpp`, `dumpHirStmt`), so the 26 existing
suites would still pass; the `--mir` dump does not print the location of
an instruction either (`dumpMirBlock`, `src/app/main.cpp:723-767`), so
the MIR dump would not change either.

**Why it was NOT done in this phase**: G7 is a prerequisite of the H5.2
checks (the borrow checker, the lifetime checker and the `nextcase`
diagnostic all need the location of the *statement*), and the brief puts
the H5.2 checks out of the H5.1 scope.  The task is therefore **sized
and ready** for H5.2: five edits, one struct-size change (120 -> 136),
no dump change.

### 11.6 The code generator bug of the oracle (the (c) item)

The phase-2 note (10.7, pitfall 1) recorded a miscompilation of the
oracle's code generator: "a record that is kept in a local variable across
a call ... came back corrupted ... when the enclosing function had many
large locals".  Phase 3 reproduced the symptom and isolated the trigger.

**The reproduction.**  The phase-2 builder (`h51f2\dbgb.ivy`) still
miscompiles with the current oracle: the `condbranch` instruction of a
function comes back with `kind = -1` and garbage block targets, so the
dump **silently loses the instruction**:

```
  block 4
DBG   inst 3 kind=-1 uns=0 a=5 b=33 c=7 next=-1 var=
```

(the `condbranch` line is missing: the dump's `switch` over the
instruction kind has no case for `-1`).  This is the exact symptom of the
phase-2 note.

**The trigger (bisected).**  The builder's "if" case of the phase-2 code
was:

```ivy
long inst = this.emit(4);
MInst cb = this.get_minst(inst);     // a 136-byte record in a local
cb.a = ci; cb.b = thenb; cb.c = elseb;
MExpr ce = this.get_mexpr(ci);        // a CALL that returns a 176-byte record
cb.line = ce.line;                    // a write to the record AFTER the call
cb.col = ce.col;
this.set_minst(inst, cb);
```

Deleting the three lines `MExpr ce = this.get_mexpr(ci); cb.line =
ce.line; cb.col = ce.col;` makes the corruption **disappear**
(`inst 3 kind=4 uns=0 a=4 b=5 c=7` - correct).  So the trigger is:

> **a record local that is written to after a call that returns a record
> by value.**

The self-contained equivalent (the same shape with **local** record types,
`h51f3\cg_p5.ivy`) does **not** corrupt; the reproduction needs the record
types of an **imported module** (`mir.ivm`'s `MInst`/`MExpr`).  The
emitted LLVM IR is correct in both cases (the same type definitions, the
same instructions - only the type *names* differ), so the miscompilation
happens in the **machine code**, not in the IR.  The oracle compiles the
IR **in process** with an **empty data layout**
(`module->setDataLayout("")`, `src/codegen/codegen.cpp:2763`), which is
the prime suspect: without a data layout LLVM does not know the ABI
classification of a large aggregate return, and the frame layout of the
caller and the callee can disagree.

**Is it a frame-size issue?  NO.**  The minimal reproducer
(`h51f3\cg_phase4.ivy`, 60 lines) miscompiles in a **small** function
with two record locals.  The phase-2 note's "the enclosing function had
many large locals" was an incidental observation: what matters is that
the record local is **live across** the call.

**A second, independent code generator bug** was found in the same
investigation (it is much easier to trigger and it is a real hazard for
the port's own sources): **the stack slots of the local variables are
keyed by NAME only - the code generator has no scope information.**
`src/codegen/codegen.cpp:2503-2526` emits one `alloca` per local with the
name `%<name>.<n>` and stores it in `vars_[<name>]`; two locals with the
same name in **different scopes** therefore share one slot, and the last
one wins.  The minimal reproducer (`h51f3\cg_shadow2.ivy`):

```ivy
MInst x;  x.kind = 4; x.a = 7;
{ long x = 5; x = x + 1; }
io::println("kind={}", x.kind);   // the oracle prints 6, not 4
```

**The impact on the port: none.**  The port's `mir_builder.ivy` never
keeps a record in a local across a call (the STYLE rule at the top of the
file: the records are read and written through the pool pointers,
`this.mu.exprs[i].field = ...`), and **no method of the port's self-host
sources declares the same local name twice** (checked with
`h51f3\dupnames.ps1` for `selfhost/mir/mir_builder.ivy`,
`selfhost/mir/mirdrive.ivy`, `selfhost/mir/mir.ivy`,
`selfhost/hir/hir_builder.ivy`, `selfhost/hir/hirdrive.ivy`,
`selfhost/parsing/parser.ivy` and `selfhost/parsing/astdrive.ivy`: **0
duplicates**).  This is why the 222 new probes and the 78 recon fixtures
are byte-identical.

**The consequence for the corpus (phase 4):** the corpus contains *Ivy
source files*, not the port's own sources, and the oracle only compiles
the corpus when the corpus is used with `--run` or a native build - the
`--mir` dump does not run the code generator at all.  The bug therefore
**cannot affect the MIR corpus**; it is only a hazard for the port's own
sources, and the two rules above (no record across a call, no duplicate
local names) must be kept.

### 11.7 The other oracle bug that the probes found

`h51f3\fx5\t01_compound_lhs_call.ivy` makes the **oracle crash**: the
address of a **struct-typed local** (`int32* p = &s;` with `s` a `struct`)
raises an access violation (`0xC0000005`) - the oracle prints nothing and
exits with `-1073741819`.  The port rejects the same program with a clean
HIR diagnostic (`struct 'S' has no matching operator '&'`).  The minimal
reproducer is `h51f3\crash\a1.ivy`.  This is an oracle bug (the port's
behaviour is the documented one), it is **outside** the MIR layer and it
must simply be avoided by the corpus.

### 11.8 The double formatting (the second open question)

**The rule.**  The oracle prints a float literal in the MIR dump with
`operator<<(ostream&, double)` (`src/app/main.cpp:638`), i.e. the C++
default formatting: **six significant digits in the `%g` layout** (fixed
notation when `-4 <= exp < 6`, scientific otherwise).  The port's runtime
formats a double differently (`lib/ivyrt.c:77` uses `_gcvt(v, 15, buf)`,
so `2.0` is printed as `2.`), but **the MIR dump never prints a double**:
the port prints the **text** of the float literal (`MExpr.sval`), which is
already formatted the way the oracle formats it:

* the **parser** renders the literal with the `%g`/six-digit rule itself
  (`selfhost/parsing/parser.ivy:734-741` documents the rule, the
  implementation is `format_float`);
* the **HIR builder** renders the **folded** literals with the same rule
  (`fmt_double`, `selfhost/hir/hir_builder.ivy:5142-5146`: "the oracle
  prints a float literal with `operator<<(ostream&, double)`, i.e.
  `printf("%g")` with 6 significant digits (C locale)").

**So the double formatting difference cannot affect the MIR dump, and no
change was needed in the MIR layer.**  The evidence: the fixture
`h51f3\fx4\r01_float_rendering.ivy` puts 13 different float literals in
one function and the oracle's and the port's dumps are byte-identical:

```
    alloca float64_t a      float 2            (2.0)
    alloca float64_t b      float 0.5          (0.5)
    alloca float64_t c      float 0.1          (0.1)
    alloca float64_t d      float 1e+10        (1e10)
    alloca float64_t e      float 100000       (100000.0)
    alloca float64_t g      float 1e+06        (1000000.0)
    alloca float64_t h      float 1e-05        (1.0e-5)
    alloca float64_t i      float 1.23457e+06  (1234567.0)
    alloca float64_t j      float 1e+06        (999999.5)
    alloca float64_t k      float 3.14159      (3.14159265358979)
    alloca float64_t l      float 0            (0.0)
    alloca float64_t m      float 1e+20        (1e20)
    alloca float64_t n      float 123457       (123456.789)
```

(The HIR layer has the same rule: the corpus
`selfhost/corpus/hir/constexpr/16_float_literal_rendering.ivy` covers it
and the H4.3 suite passes.)

### 11.9 The regression

**The 26 existing suites**, run **sequentially**, each with a **private
temp** directory (`%TEMP%` = `h51f3\tmp_<suite>`), with the invocations of
the header comment of `selfhost/difftest.ps1`:

| suite | corpus | files | result |
|---|---|---|---|
| H1.1 | lexer/ident | 10 | PASS |
| H1.2 | lexer/num | 8 | PASS |
| H1.3 | lexer/str | 7 | PASS |
| H1.4 | lexer/op | 5 | PASS |
| H1.5 | lexer/trivia | 7 | PASS |
| H1.6 | lexer/err | 9 | PASS |
| H2.1 | pp/define_obj | 10 | PASS |
| H2.2 | pp/define_fn | 12 | PASS |
| H2.3 | pp/cond | 16 | PASS |
| H2.4 | pp/expand | 16 | PASS |
| H2.5 | pp/predef | 20 | PASS |
| H2.6 | pp/line | 22 | PASS |
| H2.7 | pp/no_include | 9 | PASS |
| H3.1 | parser/decl | 15 | PASS |
| H3.2 | parser/expr | 13 | PASS |
| H3.3 | parser/stmt | 15 | PASS |
| H3.4 | parser/aggregate | 14 | PASS |
| H3.5 | parser/template | 16 | PASS |
| H3.6 | parser/module | 16 | PASS |
| H3.7 | parser/err | 19 | PASS |
| H4.1 | hir/name | 27 | PASS |
| H4.2 | hir/type | 124 | PASS |
| H4.3 | hir/constexpr | 70 | PASS |
| H4.4 | hir/template | 99 | PASS |
| H4.5 | hir/borrow | 45 | PASS |
| H4.6 | hir/move | 60 | PASS |

**26 / 26 suites PASS, 684 / 684 files.**  The suites that exercise the
changed layers are H3.3 (the parser: the switch) and H4.3 (the HIR: the
switch and the constexpr folding) - both PASS, so the `--ast` and the
`--hir` dumps are unchanged.

**The 91 recon fixtures**: **78 byte-identical (0 regressions)**, the same
13 as in phase 2 (the 6 H5.2 checks and the 7 HIR gaps G1-G6).

### 11.10 The known-unmatchable set (for phase 4)

| fixture | class | why | what it would need |
|---|---|---|---|
| `29_borrow_mut_mut` | (d) H5.2 | the borrow checker (`checkBorrow`) | the H5.2 borrow checker |
| `30_borrow_mut_shared` | (d) H5.2 | the borrow checker | the H5.2 borrow checker |
| `87_borrow_mut_after_shared` | (d) H5.2 | the borrow checker | the H5.2 borrow checker |
| `32_dangling_return` | (d) H5.2 | the lifetime checker on a return (`checkReturn`) | the H5.2 lifetime checker |
| `80_lt_mismatch` | (d) H5.2 | the lifetime checker on a return | the H5.2 lifetime checker |
| `61_nextcase_label` | (d) H5.2 | the `nextcase` diagnostic is reported at `0:0` instead of `5:13` | **G7** (11.5) |
| `34_struct_ctor` | (b) | G3: the ctor is not injected by the port's HIR builder | the ctor/dtor injection in the HIR builder |
| `35_struct_dtor` | (b) | G4: the dtor is not injected by the port's HIR builder | the ctor/dtor injection in the HIR builder |
| `36_new_delete` | (b) | G2: `new`/`delete` are not parsed | the parser |
| `37_lambda` | (b) | the oracle crashes in the HIR stage | - |
| `38_namespace` | (b) | G6: the namespaces are not parsed | the parser |
| `47_struct_initlist` | (b) | G5: the braced init lists are not parsed | the parser |
| `77_virtual_call` | (b) | G1: the port's HIR does not mark a virtual call | the virtual dispatch |
| `q02_labelled_nextcase_in_loop_in_switch` | (d) H5.2 | the same as `61_nextcase_label` | **G7** |
| `q15_nextcase_to_default_label` | (d) H5.2 | the same as `61_nextcase_label` | **G7** |
| `t01_compound_lhs_call` | (c) | the oracle crashes on `&<struct local>` | - (the corpus must avoid it) |

**Nothing else is known to be unmatchable.**  Every other shape that the
222 phase-3 probes and the 78 recon fixtures cover is byte-identical.

### 11.11 The corpus recommendations for phase 4

**Include** (all of them are byte-identical today):

* the **statements**: the nested blocks, the `if`/`else`/`else if` chains,
  the `while`, the `do-while`, the `for` (with and without the
  initializer/condition/increment), the `switch` (int, `default`, the
  fall-through, the `break`, the `continue` in a loop inside a switch),
  the `return` in every position, the expression statements, the
  declarations (with and without an initializer - the latter is a
  *diagnostic* fixture, the oracle rejects an uninitialized variable), the
  multiple declarations, the assignments (simple, compound, chained, to a
  field, through a pointer, to an array element);
* the **expressions**: all the arithmetic operators over the mixed integer
  widths, the boolean logic (with the short circuit), the unary
  operators (`-`, `!`, `~`, `++`, `--`), the comparisons, the nested
  ternaries, the casts (`static_cast`, `reinterpret_cast`, `const_cast`),
  `sizeof`/`alignof` (the **type** form only - `sizeof(expr)` is rejected
  by the oracle), the string/char/float literals, `nullptr`, the enum
  constants, the enum-typed locals, the `bool`/float/char locals;
* the **calls**: 0/1/many arguments, the nested calls, the call whose
  result is used, in a condition, in a return, with a struct/enum
  argument, the method on `this`, the method on a local struct, the
  builtin (`printf`, `io::println`), the instantiated template, the
  recursive call;
* the **declarations and the parameters**: by value, by reference, the
  pointer (in a *declaration only* - a definition with a raw pointer
  parameter is rejected by the oracle), the `const`, the struct-typed,
  the lifetime-annotated (the native `$a` and the legacy `[[ivy::lt]]`),
  the `move()`, the `this` in a method, the `extern "C"`, the `constexpr`
  and `if constexpr`, the declaration only;
* the **stress shapes**: a function with many locals, with many
  parameters, with 20+ statements, a `switch` with many cases, a deeply
  nested expression, a deeply nested control flow, a struct local kept
  across a call;
* the **lifetime data**: the `[lt: X]` suffixes of every kind (`static`
  for a string literal, `local` for the address of a local, `named` for a
  `$a`, `unknown` for a call, the merge of a ternary, the propagation
  through a dereference, a member, an index, a store).

**Exclude** (and why):

| shape | why |
|---|---|
| the 6 H5.2 fixtures (29, 30, 32, 80, 87, 61) | the port prints a dump where the oracle prints a diagnostic; they only match after the H5.2 checks (and G7 for `61`) |
| the 7 HIR-gap fixtures (34, 35, 36, 37, 38, 47, 77) | the port's HIR cannot express them (G1-G6) or the oracle crashes (37) |
| the address of a **struct** local | **the oracle crashes** (11.7) |
| a `switch` over an **enum** | the oracle rejects it (`switch condition must be an integral type`) |
| `sizeof(<expression>)` | the oracle rejects it (`expected type after 'sizeof('`) |
| a **definition** with a raw pointer parameter, a raw pointer return type or a `nullptr_t` parameter | the oracle rejects them (`requires an unsafe block`) |
| an **uninitialized** local | the oracle rejects it (`must be initialized`) |
| the C-style types (`char`, `double`, `short`, `int`, `long`) without `#pragma ivy cnumber` | the oracle rejects them |
| a `static` method | the oracle's parser does not accept the `static` keyword |
| a template **struct** with a member access (`Box<int32>.value`) | the oracle's HIR rejects it (`'Box' is not a struct type`) |
| an array-typed **parameter** (`int32 arr[4]`) | the oracle's parser rejects it |
| a `nextcase` to a label that is not a case label | it is a diagnostic whose location needs G7 (it matches except for the `0:0`) |

**The recommended split** (the harness invocation of 10.8):

```
powershell -File selfhost/difftest.ps1 `
  -Dir selfhost/corpus/mir/<group> -Kinds all -Oracle --mir `
  -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder,mir,mir_builder `
  -ModuleDirs selfhost/parsing,selfhost/hir,selfhost/mir `
  -DriverDir selfhost/mir -Driver mirdrive
```

with the groups `mir/stmt`, `mir/expr`, `mir/call`, `mir/decl`,
`mir/lt` (the lifetime data), `mir/stress` and `mir/diag` (the
diagnostics: the HIR-stage ones, which are identical today).

**The corpus must stay MIR-clean**: a fixture that makes the oracle print
a MIR-stage diagnostic (the 6 H5.2 fixtures) cannot match before the H5.2
checks are implemented, and it must stay free of the HIR gaps G1-G6 and of
the oracle crash of 11.7.

### 11.12 The bookkeeping record (phase 3)

* **Created**: `selfhost/mir/mir_builder.ivy` (the fixes of D1-D4),
  `selfhost/mir/mir_builder.ivm` / `.obj` (the build products of the
  harness, covered by `.gitignore`), and this section of the notes.
* **Changed**: `selfhost/parsing/ast.ivy` (the `ACase` record),
  `selfhost/parsing/parser.ivy` (the case list, the case label, the case
  count), `selfhost/parsing/astdrive.ivy` (the AST dump walks the case
  list), `selfhost/hir/hir.ivy` (the `HCase` record),
  `selfhost/hir/hir_builder.ivy` (the case list, the case label), and
  `selfhost/hir/hirdrive.ivy` (the HIR dump walks the case list).
  **The `--ast` and the `--hir` dumps are unchanged** (the 26 suites, 684
  files, all pass).
* **Unchanged**: `src/**` (the oracle was **not** rebuilt - it keeps its
  mtime `2026-10-21 16:41:44` and its size 27031552),
  `selfhost/difftest.ps1`, `selfhost/corpus/**`, `PLAN.md`.
* **No corpus file was added to the repo**: the 222 phase-3 fixtures live
  in `h51f3\fx`, `h51f3\fx2`, `h51f3\fx3`, `h51f3\fx4` and `h51f3\fx5`
  only.
* **No commit** was made.  `git status --porcelain` shows only the new
  `selfhost/mir/`.
* **The opencode session store was never touched**: no file under
  `C:\Users\ASUS\.local\share\opencode\` was read, grepped, listed or
  written, and no `opencode.db*` was accessed anywhere on the machine.
  The only path used under the user's profile was the pre-approved
  scratch directory `C:\Users\ASUS\AppData\Local\Temp\opencode\h51f3\`.

---

## 12. Phase 4 (the corpus and the bookkeeping)

**This is phase 4 of 4** (the corpus + the bookkeeping).  Phase 1 was the
research (sections 1-9), phase 2 wrote the model / the builder / the driver
(section 10), phase 3 widened the probe set and fixed the four CFG defects
(section 11), phase 4 adds the **official corpus**, verifies it byte for
byte, re-runs the 26 existing suites and writes the bookkeeping.  **No
implementation change was needed in this phase**: the lowering that phases 2
and 3 produced already matches the oracle byte for byte on every fixture of
the corpus.

### 12.1 The state of the tree at the end of the phase

| item | value |
|---|---|
| repo | `D:\project\Ivy\ivyc` (drive `D:`) |
| HEAD | `0cbcd31` (unchanged - **no commit was made**) |
| working tree | `git status --porcelain` shows the phase-3 changes (`selfhost/parsing/ast.ivy`, `astdrive.ivy`, `parser.ivy`, `selfhost/hir/hir.ivy`, `hir_builder.ivy`, `hirdrive.ivy`) plus the two untracked directories `selfhost/mir/` and `selfhost/corpus/mir/` |
| oracle | `build\Release\ivy.exe`, 27031552 bytes, mtime **2026-10-21 16:41:44** (NOT rebuilt) |
| `src/**` | **unchanged** |
| `selfhost/parsing/**`, `selfhost/hir/**` | **unchanged** by this phase (the phase-3 changes are still in the tree) |
| `selfhost/mir/**` | **unchanged** by this phase (the model / the builder / the driver are the phase-2/3 ones) |
| `selfhost/difftest.ps1` | **comment only** (the H5.1 invocation was added to the header comment block) |
| `PLAN.md` | the H5.1 row + the H5.1 note + the deferred lists |
| `selfhost/corpus/mir/cfg/` | **created** (72 files) |
| scratch | `C:\Users\ASUS\AppData\Local\Temp\opencode\h51f4\` |

### 12.2 The corpus: `selfhost/corpus/mir/cfg/`

The corpus is built from the **phase-3 probe set** (222 fixtures in
`h51f3\fx`, `fx2`, `fx3`, `fx4`, `fx5`) and the **phase-1 recon fixtures**
(91 fixtures in `h51r1\fx`).  The two sets overlap heavily (the phase-3
probes were derived from the recon fixtures), so the corpus is a **merge**:
one representative per rule, deduplicated, renamed with a **numeric prefix
that groups by topic**, and with a **first-line comment that states the rule
the file pins** (the style of `selfhost/corpus/hir/move/`).

| group | topic | files | prefix |
|---|---|---|---|
| 01 | functions and returns | 10 | `01_` |
| 02 | locals and declarations | 12 | `02_` |
| 03 | expressions | 12 | `03_` |
| 04 | calls | 13 | `04_` |
| 05 | statements | 13 | `05_` |
| 06 | the lifetime data | 9 | `06_` |
| 07 | the CFG shapes | 6 | `07_` |
| | **total** | **75** | |

The rules of the corpus:

* **one rule per file**, self-contained, deterministic, no temp path, no
  dependence on any other file;
* **MIR-clean**: the oracle prints a real MIR dump for every file (stdout >
  0 byte).  A file whose oracle output is a **MIR-stage diagnostic** is
  *matchable* (the shape - diagnostic, no dump - is already H5.1) but it
  belongs to **H5.2**, so it is **not** in this corpus (the list is in
  12.4);
* **no** file that hits a known out-of-scope condition (the list and the
  oracle evidence are in 12.4);
* **no** file that carries the oracle's code-generator miscompilation
  pattern (a record local written to **after** a call that returns a record
  **by value**): the only file that returns a record by value is
  `01_return_struct_by_value.ivy`, and it does not call anything - so the
  pattern does not occur in the corpus.  (The bug is in `src/**` and must
  not be "fixed" there - see 11.6.)

#### 12.2.1 The inventory (file -> the rule it pins -> the source fixture)

| file | the rule | the source |
|---|---|---|
| `01_bodyless_declaration.ivy` | a declaration without a body: the function has no block at all | phase-3 probe `d14_decl_only` |
| `01_constant_return.ivy` | a constant return value | phase-1 fixture `03_return_const` |
| `01_constexpr_function.ivy` | a constexpr function: it is folded, but it is still in the dump | phase-1 fixture `25_constexpr` |
| `01_empty_function.ivy` | an empty body: the entry block is created and stays empty | phase-1 fixture `01_empty` |
| `01_extern_c_declaration.ivy` | an extern C declaration: the extern C marker after the function name | phase-1 fixture `33_extern_c` |
| `01_multi_function_order.ivy` | several functions in one unit: the order of the dump is the HIR order | phase-3 probe `x08_multi_function` |
| `01_return_call.ivy` | the return of a call: the call keeps the unknown lifetime | phase-1 fixture `11_call` |
| `01_return_local.ivy` | the return of a local: the ident of the local in the ret | phase-1 fixture `04_local_init` |
| `01_return_struct_by_value.ivy` | returning a struct by value: no NRVO temporary without a destructor | phase-1 fixture `60_return_struct` |
| `01_void_return.ivy` | a bare return: a ret instruction with no value | phase-1 fixture `02_return_void` |
| `02_decl_multiple_names.ivy` | a declaration with several names: one alloca per name | phase-1 fixture `46_multi_decl` |
| `02_local_auto.ivy` | an auto local: the alloca carries the deduced type | phase-1 fixture `52_auto` |
| `02_local_bool.ivy` | a local of type bool | phase-3 probe `e22_bool_local` |
| `02_local_char.ivy` | a local of type char | phase-3 probe `e24_char_local` |
| `02_local_enum.ivy` | a local of an enum type, initialised with an enum constant | phase-1 fixture `56_enum_const` |
| `02_local_float.ivy` | a local of a floating-point type: the C-style double under the cnumber pragma | phase-3 probe `e17_float_literal` |
| `02_local_shadowed.ivy` | a shadowed local: the inner scope declares its own binding | phase-1 fixture `73_scope_shadow` |
| `02_locals_several.ivy` | several locals: one alloca per local, in the declaration order | phase-1 fixture `06_two_locals` |
| `02_param_by_reference.ivy` | a by-reference parameter: the reference marks the parameter type | phase-1 fixture `08_param_ref` |
| `02_param_by_value.ivy` | a by-value parameter | phase-1 fixture `07_param` |
| `02_param_const.ivy` | a const parameter | phase-3 probe `d04_param_const` |
| `02_param_pointer.ivy` | a raw pointer parameter: only a declaration is allowed, a definition needs an unsafe block | phase-3 probe `p29_const_ptr_param_decl` |
| `03_arith_mixed_widths.ivy` | arithmetic on the mixed integer widths: the int64 and the uint64 operands | phase-3 probe `p23_wide_ints` |
| `03_bool_logic_short_circuit.ivy` | the boolean operators and the short circuit | phase-3 probe `e04_bool_logic` |
| `03_cast_const.ivy` | a const_cast | phase-3 probe `e11_cast_const` |
| `03_cast_reinterpret.ivy` | a reinterpret_cast inside an unsafe block | phase-3 probe `e10_cast_reinterpret` |
| `03_cast_static.ivy` | a static_cast to another integer width | phase-1 fixture `41_cast` |
| `03_comparisons.ivy` | the comparison operators | phase-3 probe `e07_compare_chain` |
| `03_literal_nullptr.ivy` | the nullptr literal | phase-3 probe `e18_nullptr` |
| `03_literal_string_escapes.ivy` | a string literal with the escapes: the raw text is kept in the dump | phase-3 probe `r22_string_escapes` |
| `03_sizeof_alignof_types.ivy` | sizeof and alignof of the types: they are folded to integer literals at the HIR stage | phase-1 fixture `76_sizeof` |
| `03_ternary_nested.ivy` | a ternary with nested branches | phase-3 probe `e08_ternary_nested` |
| `03_unary_incdec.ivy` | the prefix and the postfix increment and decrement | phase-3 probe `e06_unary_incdec` |
| `03_unary_operators.ivy` | the unary operators: the minus, the bitwise not and the logical not | phase-3 probe `e05_unary_ops` |
| `04_call_as_argument.ivy` | a call as a call argument: the nested call is the **first** argument (the only position where the argument run stayed contiguous before the phase-5 fix) | phase-1 fixture `91_nested_call_arg` |
| `04_call_builtin.ivy` | a call to the io builtin | phase-3 probe `c13_call_builtin_io` |
| `04_call_in_condition.ivy` | a call in a condition | phase-3 probe `c06_call_in_condition` |
| `04_call_many_args.ivy` | a call with many arguments | phase-3 probe `c03_call_many` |
| `04_call_method_local_struct.ivy` | a method called on a local struct | phase-3 probe `c11_call_method_local` |
| `04_call_method_this.ivy` | a method called on a local: the this parameter is the first one | phase-3 probe `c10_call_method_this` |
| `04_call_nested_arg_last.ivy` | a call with a nested call at the **last** position of a three-argument call (the phase-5 regression) | phase-5 probe `f02_nested_arg_last` |
| `04_call_nested_arg_pos1.ivy` | a call with a nested call at argument position **1** (the phase-5 regression) | phase-5 probe `f01_nested_arg_pos1` |
| `04_call_nested_arg_two_levels.ivy` | a nested call nested **two levels deep** in a non-first position (the phase-5 regression) | phase-5 probe `f03_nested_arg_two_levels` |
| `04_call_no_args.ivy` | a call without arguments | phase-3 probe `c01_call_zero` |
| `04_call_one_arg.ivy` | a call with one argument | phase-3 probe `c02_call_one` |
| `04_call_recursive.ivy` | a recursive call | phase-3 probe `c15_call_recursive` |
| `04_call_template.ivy` | a call to an instantiated template function | phase-3 probe `c14_call_template` |
| `05_assign_array_element.ivy` | an assignment to an array element | phase-3 probe `s26_assign_array_elem` |
| `05_assign_compound.ivy` | the compound assignments: the store and the expanded binary | phase-3 probe `s22_assign_compound` |
| `05_assign_field.ivy` | an assignment to a struct field | phase-3 probe `s24_assign_field` |
| `05_assign_through_pointer.ivy` | an assignment through a pointer | phase-3 probe `s25_assign_through_ptr` |
| `05_break_continue.ivy` | break and continue inside a loop | phase-3 probe `s15_break_continue` |
| `05_do_while.ivy` | a do-while loop: the body block comes before the condition block | phase-3 probe `s06_dowhile` |
| `05_else_if_chain.ivy` | a chain of else-if branches | phase-3 probe `s03_else_if_chain` |
| `05_for_full.ivy` | a for loop with an initializer, a condition and an increment: the increment is an eval in its own block | phase-3 probe `s07_for_full` |
| `05_if_else.ivy` | an if with an else: the cont block comes first, then the two branches | phase-3 probe `s02_if_else` |
| `05_nested_blocks.ivy` | a nested block: the compound pushes and pops a scope | phase-1 fixture `20_nested_block` |
| `05_switch_fallthrough_nextcase.ivy` | a switch with a fall-through: the unlabelled nextcase jumps to the next case block | phase-1 fixture `43_nextcase` |
| `05_switch_with_default.ivy` | a switch with a default: the switch instruction itself prints nothing | phase-3 probe `s10_switch_int` |
| `05_while.ivy` | a while loop: the condition block, the body block and the exit block | phase-3 probe `s05_while` |
| `06_lt_call_unknown.ivy` | every call has the unknown lifetime | phase-3 probe `r08_lt_call_unknown` |
| `06_lt_deref_propagates.ivy` | the dereference of a pointer inherits its lifetime | phase-3 probe `p07_lt_deref` |
| `06_lt_legacy_attributes.ivy` | the legacy lifetime attributes: the lifetimes line and the parameter lifetime suffixes | phase-3 probe `d08_lifetime_legacy` |
| `06_lt_local_address.ivy` | the address of a local has the local lifetime | phase-3 probe `p05_lt_local_addr` |
| `06_lt_move_argument.ivy` | move borrows the lifetime of its argument | phase-3 probe `r09_lt_move` |
| `06_lt_native_syntax.ivy` | the native lifetime syntax: the lifetime parameter and the named lifetime of the parameter | phase-3 probe `d07_lifetime_native` |
| `06_lt_static_string.ivy` | a string literal has the static lifetime | phase-3 probe `r06_lt_static_string` |
| `06_lt_store_updates.ivy` | a store into a local updates the lifetime of the local | phase-3 probe `r07_lt_store_updates` |
| `06_lt_ternary_merge.ivy` | the lifetime of a ternary is the merge of the branches | phase-3 probe `r10_lt_ternary` |
| `07_deep_nesting.ivy` | a deeply nested control flow: the blocks are numbered in the creation order | phase-3 probe `x02_deep_nesting` |
| `07_long_function.ivy` | a long function with many statements and calls | phase-3 probe `r32_many_stmts_with_calls` |
| `07_many_locals_and_calls.ivy` | many locals and many calls: the block order of a long straight-line function | phase-3 probe `r28_many_locals_calls` |
| `07_many_parameters.ivy` | a function with many parameters | phase-3 probe `d15_many_params` |
| `07_many_switch_cases.ivy` | a switch with many cases: one block per case clause | phase-3 probe `x03_many_cases` |
| `07_nested_switches.ivy` | three levels of nested switches | phase-3 probe `q18_deep_switch_nesting` |

### 12.3 The per-file verification (raw bytes)

Every file was verified **individually** with a **raw byte capture**
(`cmd /c "... > f.bin 2> f.bin"`, never a PowerShell text capture): the
oracle's `ivy --mir <file>` stdout / stderr against the port's `mirdrive`
output for the same file.  The comparison is byte for byte (the CRLF line
endings included).

**75 / 75 byte-identical, 75 / 75 with a real MIR dump (stdout > 0 byte),
75 / 75 with the same exit code (0).**

(The count was **72 / 72** when phase 4 closed; the three phase-5
regression fixtures of 13.4 bring it to 75 - the 72 phase-4 files are
unchanged, their byte counts are the same.)

The byte counts of the oracle's stdout (the port's byte count is the same
for every file):

| group | the byte counts (stdout) |
|---|---|
| 01 | 42, 51, 68, 68, 77, 87, 104, 130, 157, 249 |
| 02 | 47, 57, 78, 85, 100, 111, 111, 112, 112, 186, 218, 235 |
| 03 | 89, 98, 174, 192, 234, 249, 322, 330, 365, 386, 539, 593 |
| 04 | 122, 158, 183, 223, 234, 267, 311, 315, 316, 339, 433, 499, 531 |
| 05 | 163, 166, 180, 204, 276, 278, 300, 300, 348, 356, 483, 588, 899 |
| 06 | 111, 138, 145, 167, 172, 176, 276, 285, 333 |
| 07 | 274, 331, 527, 856, 1372, 1972 |

(The three new entries of the group 04 are the phase-5 fixtures: 315 =
`04_call_nested_arg_pos1`, 433 = `04_call_nested_arg_two_levels`,
531 = `04_call_nested_arg_last`.)

### 12.4 The excluded candidates and the oracle evidence

The candidate pool was the 313 fixtures of `h51r1\fx` (91) + `h51f3\fx*`
(222).  **257** of them produce a real MIR dump, **54** produce a
diagnostic and **2** crash the oracle.  Of the 257, **6** are blocked by a
HIR-level gap (12.4b), so **251** are reachable for the port - of those the
72 that pin a distinct rule form the corpus (the rest are near-duplicates
or are covered by another file).  The measurement is reproducible with
`h51f4\probe_dump.ps1` (the raw captures are in `h51f4\dump_probe.txt`).

#### (a) The 8 candidates that need the H5.2 checks (a MIR-stage diagnostic)

The oracle prints the diagnostic on stderr and **no** dump (stdout 0 byte,
exit 1).  The shape is matchable, but the port would print a dump, so they
belong to the **H5.2** corpus.  The `nextcase` family additionally needs
**G7** (the port's `HStmt` has no `line` / `col`).

| fixture | the oracle's stderr (measured) | what it needs |
|---|---|---|
| `29_borrow_mut_mut` | `:5:5: error: cannot create a second mutable reference to 'x' — only one mutable borrow at a time` | the H5.2 borrow checker |
| `30_borrow_mut_shared` | `:5:5: error: cannot create an immutable reference to 'x' — it is already borrowed mutably` | the H5.2 borrow checker |
| `87_borrow_mut_after_shared` | `:5:5: error: cannot create a mutable reference to 'x' — it is already borrowed immutably` | the H5.2 borrow checker |
| `32_dangling_return` | `:4:5: error: returning a reference to a local variable (dangling reference)` | the H5.2 lifetime checker (`checkReturn`) |
| `80_lt_mismatch` | `:4:5: error: returned reference has lifetime '$a' but the function declares lifetime '$b'` | the H5.2 lifetime checker (`checkReturn`) |
| `61_nextcase_label` | `:5:13: error: nextcase target 'outer' is not a case label in this switch` | the H5.2 `nextcase` check **+ G7** (the port reports `0:0`) |
| `q02_labelled_nextcase_in_loop_in_switch` | `:6:17: error: nextcase target 'one' is not a case label in this switch` | the H5.2 `nextcase` check **+ G7** |
| `q15_nextcase_to_default_label` | `:6:13: error: nextcase target 'two' is not a case label in this switch` | the H5.2 `nextcase` check **+ G7** |

(The three borrow messages use an **em dash** (U+2014, the bytes
`e2 80 94`), not a hyphen - see 4.4.  The path in front of the position is
the path as given on the command line.)

#### (b) The 7 candidates blocked by a HIR-level gap (G1-G6) or by an oracle crash

The oracle prints a MIR dump (so the *shape* is fine) but the port's HIR
cannot express the construct.

| fixture | the oracle | the port | the gap |
|---|---|---|---|
| `34_struct_ctor` | 263 byte of dump | `error: undeclared identifier 'S'` | **G3** (the struct construction `S s(1)`) |
| `35_struct_dtor` | 236 byte of dump | the dump without the injected `~S` call | **G4** (the destructor injection) |
| `36_new_delete` | 174 byte of dump | 4 parse errors | **G2** (`new` / `delete`) |
| `37_lambda` | **crash** (exit `-1073741819` = `0xC0000005`, 0 byte out, 0 byte err) | 3 parse errors | the oracle crashes in the HIR stage |
| `38_namespace` | 86 byte of dump (`function math::abs`) | 3 parse errors | **G6** (the `namespace` blocks) |
| `47_struct_initlist` | 121 byte of dump | 6 parse errors | **G5** (the braced init lists) |
| `77_virtual_call` | 315 byte of dump (`virtual_call 'speak' slot=0`) | `call 'Animal::speak'` | **G1** (the virtual dispatch - the port's HIR does not implement it at all, so even the `--hir` dump differs) |

#### (c) The 2 candidates that crash the oracle

| fixture | the oracle | the port |
|---|---|---|
| `37_lambda` (the last of the 7 of (b)) | exit `-1073741819` (`0xC0000005`), 0 byte of output | a clean HIR diagnostic |
| `t01_compound_lhs_call` | exit `-1073741819` (`0xC0000005`), 0 byte of output | `error: struct 'S' has no matching operator '&'` |

The trigger is the **address of a struct-typed local** (`&s` with `s` a
struct) - see 11.7.  The minimal reproducer is `h51f3\crash\a1.ivy`.  The
port's behaviour is the documented one; the crash is an oracle bug and must
**not** be reproduced.  The corpus therefore contains **no** `&<struct
local>`.

#### (d) The 46 candidates that the oracle itself rejects (out of scope)

These produce a **HIR/parse-stage** diagnostic on **both** sides (so they
are byte-identical as diagnostics), but they do not exercise the MIR
lowering at all - the oracle never reaches the MIR stage.  They are
excluded from the corpus because the H5.1 corpus must be **MIR-clean**
(every file must produce a real MIR dump).

| the shape | the candidates | the oracle's diagnostic |
|---|---|---|
| an **uninitialized** local | `05_local_noinit`, `s19_decl_noinit`, `s20_decl_multi` | `variable 'x' must be initialized (uninitialized variables are not allowed)` |
| a **raw pointer** parameter / return in a definition | `09_param_ptr`, `63_variadic_body`, `64_string_literal`, `c17_call_ptr_arg`, `d03_param_ptr`, `d09_lifetime_ptr_param` | `raw pointer parameter type requires an unsafe block` / `raw pointer return type requires an unsafe block` |
| a **C-style type** without `#pragma ivy cnumber` | `c12_call_builtin_printf`, `d01_param_byvalue`, `d11_extern_c`, `e23_float_local`, `r19_variadic_decl` | `C-style type 'double' requires #pragma ivy cnumber; use float64` (and the same for `char`) |
| a **removed `_t` suffix** | `e02_arith_mixed_widths`, `e03_arith_unsigned`, `e09_cast_static`, `e12_sizeof_types` | `_t-suffixed type 'int8_t' is removed - use 'int8' instead` |
| `sizeof` / `alignof` of an **expression** / of a non-type | `79_sizeof_expr`, `e13_sizeof_expr`, `e14_alignof` | `expected type after 'sizeof(' (sizeof(expression) is not supported)` |
| a **switch over an enum** | `18_switch_enum`, `e20_enum_local`, `s11_switch_enum` | `switch condition must be an integral type` |
| an **array-typed parameter** | `d05_param_array` | `expected ',' between parameters` |
| a **template-struct member access** | `d17_template_instantiated` | `'Box' is not a struct type - cannot access member 'value'` |
| a **`static` method** | `d19_static_method` | `expected field or method declaration in struct body` |
| a **compound assignment on a pointer** | `p11_compound_ptr`, `r40_ptr_compound`, `t02_compound_ptr_field` | `compound assignment '+=' expects numeric operands` |
| the **comma operator** / an assignment in a condition | `p40_seq_expr`, `r37_conditional_assign` | `expected ')' to close parenthesized expression` |
| a **parenthesised** expression | `r30_deep_expr_nesting` | `expected ';' after return statement` |
| a **lifetime annotation on a local** | `p03_lt_named_local`, `r05_lt_named_param` | `expected variable name` |
| the **address of a member** | `r13_addr_of_member` | `'&' is only supported on variables in the Ivy subset` |
| an **array-to-pointer decay** | `e29_ptr_arith` | `cannot initialize variable 'p' of type 'int32_t*' with a value of type 'int32_t'` |
| a **hexadecimal float literal** | `r03_float_hex` | `expected ';' after declaration` |
| a **template comparison** | `r15_template_recursive` | `'<=' expects numeric operands` |
| an **undeclared lifetime** in a method | `t09_method_with_lt` | `lifetime '$a' is not declared in lifetime<...>` |
| a **reference initialised with `&x`** | `28_borrow_error` | `cannot initialize variable 'a' of type 'int32_t' with a value of type 'int32_t*'` (an HIR type error, **not** the MIR borrow checker) |
| a **labelled switch** (the oracle's parser rejects it) | `44_labeled_switch` | `expected '{' to start switch body` |
| a **global variable** | `49_global` | `expected '(' after function name 'g_counter'` |
| `y = (x = 1)` | `86_assign_expr` | `expected ')' to close parenthesized expression` |
| a **use after move** (the HIR stage reports it first) | `85_move_use` | `use of moved-out variable 'x'` |
| `this->x` on a non-pointer | `74_struct_ptr_member` | `'->' requires a pointer to struct (got 'Point')` |

### 12.5 The suite results

#### 12.5.1 The new H5.1 suite

```
$env:TEMP='C:\Users\ASUS\AppData\Local\Temp\opencode\h51f4\tmp'; $env:TMP=$env:TEMP
powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/mir/cfg -Kinds all -Oracle --mir `
  -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder,mir,mir_builder `
  -ModuleDirs selfhost/parsing,selfhost/hir,selfhost/mir -DriverDir selfhost/mir -Driver mirdrive
```

```
[1/4] building selfhost modules ...
[2/4] collecting corpus ...
      files: 72
[3/4] running C++ oracle + Ivy port ...
[4/4] comparing (kinds: all) ...
PASS: identical (filtered) output on 72 files
```

**PASS - 72 files, byte-identical.**

(The transcript above is the **phase-4** run: the corpus had 72 files then.
The corpus has **75** files today - the three phase-5 regression fixtures of
13.7 were added after it; the phase-5 run is in **13.11**.)

#### 12.5.2 The regression of the 26 existing suites

The 26 suites were run **sequentially**, each with a **private** temp
directory (`%TEMP%` = `h51f4\tmp_<group>`; the six lexer suites share one
temp directory, the seven pp suites share another one, as the brief notes).

| suite | corpus | files | result |
|---|---|---|---|
| H1.1 | lexer/ident | 10 | PASS |
| H1.2 | lexer/num | 8 | PASS |
| H1.3 | lexer/str | 7 | PASS |
| H1.4 | lexer/op | 5 | PASS |
| H1.5 | lexer/trivia | 7 | PASS |
| H1.6 | lexer/err | 9 | PASS |
| H2.1 | pp/define_obj | 10 | PASS |
| H2.2 | pp/define_fn | 12 | PASS |
| H2.3 | pp/cond | 16 | PASS |
| H2.4 | pp/expand | 16 | PASS |
| H2.5 | pp/predef | 20 | PASS |
| H2.6 | pp/line | 22 | PASS |
| H2.7 | pp/no_include | 9 | PASS |
| H3.1 | parser/decl | 15 | PASS |
| H3.2 | parser/expr | 13 | PASS |
| H3.3 | parser/stmt | 15 | PASS |
| H3.4 | parser/aggregate | 14 | PASS |
| H3.5 | parser/template | 16 | PASS |
| H3.6 | parser/module | 16 | PASS |
| H3.7 | parser/err | 19 | PASS |
| H4.1 | hir/name | 27 | PASS |
| H4.2 | hir/type | 124 | PASS |
| H4.3 | hir/constexpr | 70 | PASS |
| H4.4 | hir/template | 99 | PASS |
| H4.5 | hir/borrow | 45 | PASS |
| H4.6 | hir/move | 60 | PASS |

**26 / 26 suites PASS, 684 / 684 files** (plus the new suite: **27 suites /
756 files**).

### 12.6 The bookkeeping record (phase 4)

* **Created**: `selfhost/corpus/mir/cfg/` (72 files) and this section of the
  notes.
* **Changed**: `selfhost/difftest.ps1` (**comment only** - the H5.1
  invocation was added to the header comment block; **no** behaviour change,
  `$Oracle` was already a generic mode string), `PLAN.md` (the H5.1 row ->
  `✅ byte-identical 72 files`, the H5.1 note, and the four deferred
  ("Con lai") lists of H4.2 / H4.3 / H4.4 / H4.5 / H4.6 that pointed at
  H5.1/H5.2), and this section.
* **Unchanged**: `src/**` (the oracle was **not** rebuilt - it keeps its
  mtime `2026-10-21 16:41:44` and its size 27031552),
  `selfhost/mir/mir.ivy`, `selfhost/mir/mir_builder.ivy`,
  `selfhost/mir/mirdrive.ivy`, `selfhost/parsing/**`, `selfhost/hir/**`
  (the phase-3 changes are still in the tree), the other corpora, the
  `.gitignore`.
* **No port defect was found in this phase**: every one of the 72 corpus
  files was already byte-identical after the phase-3 fixes, so **no** file
  of `selfhost/mir/**` had to be touched.
* **No commit** was made.  `git status --porcelain` shows the six modified
  phase-3 files plus the two untracked directories `selfhost/mir/` and
  `selfhost/corpus/mir/`.
* **The build products** (`selfhost/{parsing,hir,mir}/*.obj`,
  `*.ivm`, `*.exe`) are the ones the harness itself writes (they are covered
  by `.gitignore`).
* **The opencode session store was never touched**: no file under
  `C:\Users\ASUS\.local\share\opencode\` was read, grepped, listed or
  written, and no `opencode.db*` was accessed anywhere on the machine.  The
  only path used under the user's profile was the pre-approved scratch
  directory `C:\Users\ASUS\AppData\Local\Temp\opencode\h51f4\`.

### 12.7 What is still deferred, and where it is recorded

| the deferred item | the milestone | the record |
|---|---|---|
| the borrow checker (`checkBorrow` + the three messages) | **H5.2** | 12.4(a), 11.10 |
| the lifetime checker on a return (`checkReturn`: the dangling one + the mismatch one) | **H5.2** | 12.4(a), 11.10 |
| `checkStore` (the dangling store) | **H5.2** | 7.4, 5.3 |
| the **MIR-level move tracker** (`markMovedOut` / `isMovedOut` / `reinitialize`) | **H5.2** | 10.3, 2.4 |
| the **`nextcase` label check** | **H5.2** | 12.4(a) |
| **G7** - the source location of a statement (the `HStmt` has no `line` / `col`; the oracle's `hir::Stmt::loc` supplies the location of the `nextcase` diagnostic) | the prerequisite of **H5.2** | **11.5** (the exact sizing: 5 edits, `HStmt` 120 -> 136 byte) |
| **G1** - the virtual dispatch (the port's HIR does not implement it at all, so even the `--hir` dump differs) | a sub-phase of H5.1 / H5.2 | 6.1, 6.2, 8 (Q2) |
| **G2** - `new` / `delete` | a later milestone (the parser) | 6.1, 6.2, 8 (Q4) |
| **G3** - the struct construction `S s(1)` | the RAII/DTOR milestone | 6.1, 6.2, 8 (Q3) |
| **G4** - the destructor injection + the `__ret` temporary + the NRVO | the RAII/DTOR milestone | 6.1, 6.2, 8 (Q3) |
| **G5** - the braced init lists | a later milestone (the parser) | 6.1, 6.2, 8 (Q4) |
| **G6** - the `namespace` blocks | a later milestone (the parser) | 6.1, 6.2, 8 (Q4) |
| the oracle's code-generator bugs (the record across a call, the name-keyed stack slots) | recorded, **never** "fixed" in `src/**` | **11.6** |
| the oracle's crash on `&<struct local>` | recorded, avoided by the corpus | **11.7**, 12.4(c) |
| the MIR post-pass (the call-target resolution, the literal decoding), `mir::EnumInfo` / `mir::StructInfo` | **H5.3** | 1.8, 2.1, 10.3 |
| the interpreter, the `--run` outputs, the codegen | **H5.3 / H5.4 / H6** | the table of `PLAN.md` §5 |

## 13. Phase 5 (the defect repair: the argument run, the pool stride, the leak and the regressions)

**This is phase 5** - the repair phase that follows the **verification of the
H5.1 milestone**.  The verification found **one blocker** (D1: the run of the
call arguments was not contiguous) and **six smaller issues** (D2-D7).  All of
them are fixed here, the missing regression coverage is added, and the whole
suite is re-run.

### 13.1 The state of the tree at the start of the phase

| item | value |
|---|---|
| repo | `D:\project\Ivy\ivyc` (drive `D:`) |
| HEAD | `0cbcd31` (unchanged - **no commit was made**) |
| working tree | the 8 modified files of the H5.1 work (`PLAN.md`, `selfhost/difftest.ps1`, `selfhost/hir/hir.ivy`, `selfhost/hir/hir_builder.ivy`, `selfhost/hir/hirdrive.ivy`, `selfhost/parsing/ast.ivy`, `selfhost/parsing/astdrive.ivy`, `selfhost/parsing/parser.ivy`) plus the two untracked directories `selfhost/mir/` and `selfhost/corpus/mir/` |
| oracle | `build\Release\ivy.exe`, 27031552 bytes, mtime **2026-10-21 16:41:44** (NOT rebuilt) |
| `src/**` | **unchanged** |
| scratch | `C:\Users\ASUS\AppData\Local\Temp\opencode\h51fix\` |

### 13.2 D1 (the blocker) - the run of the call arguments was not contiguous

**The symptom.**  When a call's argument list contains a **nested call at any
position >= 1**, the port's `--mir` dump printed the nested call's *first
argument* instead of the nested call itself, so the dump diverged from the
oracle's byte for byte.  The probe of the verification:

```
int32 f() { return add(add(1, 2), add(3, 4)); }
```

```
ORACLE (372 B)                          PORT (315 B)
  call : int32_t [lt: unknown]            call : int32_t [lt: unknown]
    call : int32_t [lt: unknown]            call : int32_t [lt: unknown]
      int 1                                int 1
      int 2                                int 2
    call : int32_t [lt: unknown]            int 3      <-- WRONG (the nested call's 1st arg)
      int 3
      int 4
```

**The root cause.**  The arguments of a call (and the elements of an init
list) live in the **shared pool** `MUnit.args`, and the call's record points
into it with the pair `MExpr.astart` (the first index) + `MExpr.an` (the
count).  The run was therefore recorded by **pushing every argument into the
pool as it was built**.  Building an argument that is itself a call pushes
**the nested call's own arguments** into the same pool - **in the middle of
the enclosing run**.  The run `[argstart, argstart + an)` is contiguous only
while the nested call sits at position **0**; from position 1 on,
`mu.args[argstart + j]` resolves to the nested call's *first* argument
instead of the nested call.  The old comment stated the assumption that only
holds for position 0.

**The fix - reserve the run BEFORE building the arguments.**  Two helpers
were added to `MirBuilder` (`selfhost/mir/mir_builder.ivy`):

```ivy
    // Reserve `n` slots in the args pool and return the index of the first
    // one.  The slots are placeholders (-1) until the caller fills them in
    // with `set_arg`.
    //
    // The run MUST be reserved before the elements are built: an element
    // may itself be a call, and the nested call pushes its own arguments
    // into the same pool.  Reserving the run first keeps it contiguous
    // (the nested run lands *after* it), so an argument at any position
    // resolves to the right expression.  Pushing the arguments as they
    // are built would only keep the run contiguous while the nested call
    // sits at position 0.
    long reserve_args(long n) {
        long start = this.mu.a_n;
        long i = 0;
        while (i < n) {
            this.push_arg(-1);
            i = i + 1;
        }
        return start;
    }

    // Fill the slot `i` of the args pool (a slot reserved by `reserve_args`).
    void set_arg(long i, long idx) {
        unsafe {
            this.mu.args[i] = idx;
        }
    }
```

`reserve_args` (`mir_builder.ivy:369`) pushes `n` **placeholder** slots
(`-1`) and returns the index of the first one; `set_arg`
(`mir_builder.ivy:380`) fills a reserved slot.  The nested call's own run is
reserved **after** the enclosing run (the pool grows at the end), so the
enclosing run stays contiguous and the nested run lands behind it.

**The change in `ex_call` (`mir_builder.ivy:1334`).**

```ivy
        // before (the defect): the arguments are pushed into the shared
        // pool as they are built -> the run breaks at the first nested call
        long argstart = -1;
        long j = 0;
        while (j < an) {
            long ai = -1;
            unsafe {
                ai = this.ha[start + j];
            }
            long mi = -1;
            if (ai >= 0) {
                mi = this.build_expr(ai);
            }
            long slot = this.push_arg(mi);     // <-- the run is broken here
            if (j == 0) {
                argstart = slot;
            }
            j = j + 1;
        }

        // after (the fix): the run is reserved first, then it is filled
        long argstart = this.reserve_args(an);
        long j = 0;
        while (j < an) {
            long ai = -1;
            unsafe {
                ai = this.ha[start + j];
            }
            long mi = -1;
            if (ai >= 0) {
                mi = this.build_expr(ai);
            }
            this.set_arg(argstart + j, mi);
            j = j + 1;
        }
```

The `move()` builtin, which reads its argument back from the pool
(`this.mu.args[argstart]`, `mir_builder.ivy:1372`), keeps working: the slot
holds the **built** expression (`set_arg` stores the built index, not the
HIR index).

### 13.3 D1 - the two latent copies of the same pattern

The same defect was present (but unreachable) in two other places; both were
fixed the same way, so that the defect cannot survive there:

| site | before | after |
|---|---|---|
| `ex_initlist` (`mir_builder.ivy:1505`) - the elements of a braced init list | the elements were pushed as they were built | `long elstart = this.reserve_args(an);` (`:1514`) + `this.set_arg(elstart + j, mi);` (`:1525`) |
| `clone_expr` (`mir_builder.ivy:959`) - the cloned arguments of a compound assignment | the cloned arguments were pushed as they were cloned | `long start = this.reserve_args(an);` (`:1022`) + `this.set_arg(start + j, ci);` (`:1033`) |

`ex_initlist` is currently **unreachable** (the parser rejects the braced
init lists - gap **G5**) and the compound-assignment LHS clone never carries
a call argument (the oracle itself crashes on `&<struct local>`), so neither
is covered by the corpus today.  They are fixed anyway: a latent defect that
survives in an unreachable branch is a defect that comes back as soon as the
branch becomes reachable.

### 13.4 D1 - the byte comparison (the 7 failing shapes and the previously working ones)

15 probe shapes were written (9 that expose the defect + 6 that must **not**
regress).  The comparison is **byte for byte** against the oracle's
`ivy --mir` (a raw byte capture through `cmd /c` redirection, never a
PowerShell text capture) and against the port's `mirdrive`.

| # | the shape | pre-fix | post-fix |
|---|---|---|---|
| 1 | `add(add(1, 2), 3)` - the nested call at position **0** | PASS | PASS |
| 2 | `add(1, add(2, 3))` - the nested call at position **1** | **FAIL** | PASS |
| 3 | `add3(1, 2, add(3, 4))` - the nested call at the **last** position | **FAIL** | PASS |
| 4 | `add(add(1, add(2, 3)), add(4, 5))` - **two levels** deep, non-first | **FAIL** | PASS |
| 5 | `if (add(1, add(2, 3)) > 0)` - the nested call in a **condition** | **FAIL** | PASS |
| 6 | `add(add(1, 2), add(3, 4))` - a call whose argument is a call | **FAIL** | PASS |
| 7 | `add3(add(1, 2), 3, add(4, add(5, 6)))` and `add(x, add(add(7, 8), 9))` - several nested calls at several positions | **FAIL** | PASS |
| 8 | `add3(1, add(2, 3), 4)` - the nested call at the **middle** position | **FAIL** | PASS |
| 9 | `x = add(1, add(2, 3))` - the nested call in an **assignment** | **FAIL** | PASS |
| 10 | `one()` - a call with **no** argument | PASS | PASS |
| 11 | `inc(1)` - a call with **one** argument | PASS | PASS |
| 12 | `add5(1, 2, 3, 4, 5)` - a call with **many** arguments | PASS | PASS |
| 13 | `return add(1, 2)` - a call in a **return** | PASS | PASS |
| 14 | `if (add(1, 2) > 0)` - a call in a **condition** | PASS | PASS |
| 15 | `g(g(1))` - a call whose argument is a **call** | PASS | PASS |

**The pre-fix port: 8 failures, 7 passes.  The post-fix port: 15 / 15
passes, 0 regressions.**  The 8 failing shapes are exactly the shapes with a
nested call at a position **>= 1**; the shape with the nested call at
position **0** (shape 1) and the 6 shapes without a nested call (shapes
10-15) pass on **both** sides - which is the proof that the defect was
invisible at position 0 and that the fix does not change the other shapes.

The pre-fix port used for this measurement was **rebuilt from a copy** of the
pre-fix `selfhost/mir/mir_builder.ivy` in the scratch directory
(`h51fix\prefix\`): the three hunks of 13.2 / 13.3 were reverted (the
push-as-you-build form) and the copy was compiled with `ivy.exe` into a
separate `mirdrive_prefix.exe`.  The repository's own builder was **not**
touched to produce it.

### 13.5 D2 - the `MParam` pool stride was 88, the struct is 80

`selfhost/mir/mir.ivy` documented `sizeof(MParam)` as **88** ("four 8-byte
fields plus the 48-byte AType") and the two allocations used `* 88`.  The
sentence was already self-contradictory: four 8-byte fields plus the
48-byte `AType` is 32 + 48 = **80**.  The over-allocation was harmless at
runtime (the pool was bigger than needed), but the documented invariant
"pool size == sizeof(struct)" was false.

**The measurement (not a guess).**  A C++ probe with the port's own field
layout (`long` = a 64-bit integer, as in the port's LLVM IR) prints:

```
sizeof(AType)=48 align=8
sizeof(MParam)=80 align=8
sizeof(MExpr)=176 align=8
sizeof(MInst)=136 align=8
sizeof(MBlock)=16 align=8
sizeof(MLifetime)=16 align=8
sizeof(MFunction)=128 align=8
offsetof(MParam,type)=0
offsetof(MParam,name)=48
offsetof(MParam,lt)=56
offsetof(MParam,line)=64
offsetof(MParam,col)=72
```

The port's own compiler agrees - the LLVM IR of a module that imports `mir`:

```
%struct.AType  = type { ptr, ptr, i64, i1, i1, i1, i64, i64 }
%struct.MParam = type { %struct.AType, ptr, ptr, i64, i64 }
```

**The option that was chosen: the real size (80), not a padding field.**
Adding an 8-byte field to `MParam` would have made "88" true, but it would
grow the record for every parameter of every function and change the oracle's
record layout for no benefit: the pool only needs the **exact** size of the
record it stores.  The minimum and the safest option is therefore to use the
real size everywhere.

| what | before | after |
|---|---|---|
| `selfhost/mir/mir.ivy:97` | `sizeof(MParam) is 88 bytes` | `sizeof(MParam) is 80 bytes` |
| `selfhost/mir/mir_builder.ivy:308` (`grow_mparams`) | `__ivy_alloc(ncap * 88)` | `__ivy_alloc(ncap * 80)` |
| `selfhost/mir/mir_builder.ivy:564` (`setup`) | `__ivy_alloc(npa * 88)` | `__ivy_alloc(npa * 80)` |
| `selfhost/mir/MIR_NOTES.md` (10.2, the sizes) | `sizeof(MParam)` = 88 | `sizeof(MParam)` = 80 + a note with the measurement |
| `PLAN.md` (the H5.1 note) | "88" | "80" |

`grow_mparams` also got the comment that the other `grow_*` helpers have
(`// Grow the parameter pool (the element size is 80 bytes).`).

### 13.6 D3 - the leak of `substr1`

The compound-assignment expansion needs the **plain** operator of a compound
operator (`+=` -> `+`).  The port had two helpers: `substr1` allocated
`n + 1` bytes and copied the text, and `keep_string` allocated a **second**
buffer and registered **that** copy in the `strs_` pool.  The buffer of
`substr1` was never freed: **one leaked allocation per compound-assignment
statement**.

**The fix: one allocation, registered in `strs_`.**  `substr1` was folded
into `keep_string`, which now allocates the copy itself, NUL-terminates it
and registers **that** pointer in `strs_` (`mir_builder.ivy:464`):

```ivy
    // Copy the first `n` characters of a string into a fresh allocation and
    // remember it, so that it stays valid until `release`.  The copy is
    // NUL-terminated.  The allocation is the one that is registered in
    // `strs_`: there is no second copy and no leak.
    const char* keep_string(const char* s, long n) {
        unsafe {
            char* p = (char*) __ivy_alloc(n + 1);
            long i = 0;
            while (i < n) {
                p[i] = s[i];
                i = i + 1;
            }
            p[n] = 0;
            ...                          // the pool of the kept strings
            this.strs_[this.strs_n_] = (long) p;
            this.strs_n_ = this.strs_n_ + 1;
            return p;
        }
        return "";
    }
```

**The lifetime stays correct:** the string that the dump prints is the one
that `strs_` owns; `release()` frees every entry of `strs_`
(`mir_builder.ivy:678`), so the strings stay valid from the copy until the
dump is finished and are then released - exactly once, and with no second
allocation.  The two callers (`keep_string` for a diagnostic message and the
compound-assignment operator) are unchanged.

### 13.7 D4 - the corpus coverage hole (three new fixtures)

The corpus covered the nested call at argument position **0** only
(`04_call_as_argument.ivy`) - the single position where D1 was invisible.
Three fixtures were added to `selfhost/corpus/mir/cfg/`:

| file | the rule it pins |
|---|---|
| `04_call_nested_arg_pos1.ivy` | a call with a nested call at argument position **1** (`return add(1, add(2, 3));`) |
| `04_call_nested_arg_last.ivy` | a call with a nested call at the **last** position of a three-argument call (`return add3(1, 2, add(3, 4));`) |
| `04_call_nested_arg_two_levels.ivy` | a nested call nested **two levels deep** in a non-first position (`return add(add(1, add(2, 3)), add(4, 5));`) |

**The "bites" evidence.**  The pre-fix port (13.4) was run against the whole
corpus: the **3 new fixtures FAIL** (they are the only failures) and the 72
phase-4 files still pass.  The post-fix port: **75 / 75 pass**.

```
pre-fix :  72 passed,  3 failed   (04_call_nested_arg_last, 04_call_nested_arg_pos1,
                                    04_call_nested_arg_two_levels)
post-fix:  75 passed,  0 failed
```

The corpus is now **75 files** (the group 04 grows from 10 to 13); the
inventory of 12.2 and the counts of 12.3 were updated.

### 13.8 D5 - a drifted citation

`MIR_NOTES.md` cited `selfhost/mir/mir_builder.ivy:1957` for the
`this.diag(0, 0, ...)` call of the `nextcase` diagnostic (in 11.5 and in the
G7 table).  The call is at **`mir_builder.ivy:1988`** in the current file;
the cited `:1957` was the `unsafe {` of the preceding `db_put` in the file
as it was when the note was written (the file has shifted since - the
phase-5 edits moved the line).  Both citations were corrected.

While the phase-5 edits were made, **9 further drifted citations** into the
files that this phase touched (`mir_builder.ivy`, `parser.ivy`,
`hir_builder.ivy`, `hirdrive.ivy`) were found and corrected as well (the
phase-3 / phase-4 notes were written against the line numbers of an earlier
state of the files):

| the note | before | after |
|---|---|---|
| 11.4 - the case-label map | `:1967`, `:2213`, `:2241`, `:1947` | `:1993`, `:2224`, `:2252`, `:1944` |
| 11.4 - `st_switch` walks the list | `:1967-2140` | `:1993-2152` |
| 11.4 - the per-context switch data | `:96-104` | `:96-99` |
| 11.4 - the loop-context helpers | `:2178`, `:2187`, `:2195`, `:2213` | `:2189`, `:2198`, `:2206`, `:2224` |
| 11.4 - `push_loop_sw` | `:2277` | `:2288` |
| 11.4 - `st_nextcase` | `:1918-1963` | `:1944-1989` |
| 11.4 - the label map of the switch | `:2062-2110` | `:2099-2121` |
| 11.4 - `push_case` / `set_case_next` | `parser.ivy:2119` / `:2134` | `parser.ivy:2123` / `:2139` |
| 11.4 - `hc.label = ac.label;` | `hir_builder.ivy:3483` | `hir_builder.ivy:3489` |
| 11.4 - `build_switch` walks the list | `hir_builder.ivy:3437-3492` | `hir_builder.ivy:3447-3503` |
| 11.6 - the `nextcase` location | `parser.ivy:2833-2834` | `parser.ivy:2838-2839` |
| 11.8 - the marker of the driver | `hirdrive.ivy:446` | `hirdrive.ivy:451` |
| 11.8 - `fmt_double` | `hir_builder.ivy:5135-5138` | `hir_builder.ivy:5142-5146` |

### 13.9 D6 - a stale count

`PLAN.md` said "6 fixture co chan doan MIR-stage thuoc ve **H5.2**" while
`MIR_NOTES.md` (11.10 and 12.4(a)) lists **8** (the three borrow diagnostics,
the two return diagnostics and the three `nextcase` ones).  The number in
`PLAN.md` was corrected to **8**.

### 13.10 D7 - the latent pool-overflow assumption

`selfhost/parsing/parser.ivy` (`push_case`) and
`selfhost/hir/hir_builder.ivy` (`push_hcase`) returned the index of a slot
that was **never pushed** when the pool was full, so the `next` link of the
list could point past the end of the pool.  The capacity bounds are
provably sufficient (the parser's case pool is `token count + 16` and every
clause needs at least three tokens; the HIR's case pool is the same), so the
branch is unreachable - but the fix is small and provably safe, so it was
made:

* `push_case` (`parser.ivy:2123`) and `push_hcase`
  (`hir_builder.ivy:2105`) return **-1** when the pool is full and do **not**
  record the clause;
* the two callers skip the link in that case:
  `if (ci >= 0) { ... }` (`parser.ivy:3117`, `hir_builder.ivy:3491`).

### 13.11 The suite results

All **27** suites were run **sequentially**, each group with its **own
private** temp directory (`%TEMP%` = `h51fix\suites\<group>`; the six lexer
suites share one temp directory, the seven pp suites share another one, the
parser, the hir and the mir groups have one each).  The shared
`%TEMP%\ivydiff_selfhost` of the harness was never used.

| suite | corpus | files | result |
|---|---|---|---|
| H1.1 | lexer/ident | 10 | PASS |
| H1.2 | lexer/num | 8 | PASS |
| H1.3 | lexer/str | 7 | PASS |
| H1.4 | lexer/op | 5 | PASS |
| H1.5 | lexer/trivia | 7 | PASS |
| H1.6 | lexer/err | 9 | PASS |
| H2.1 | pp/define_obj | 10 | PASS |
| H2.2 | pp/define_fn | 12 | PASS |
| H2.3 | pp/cond | 16 | PASS |
| H2.4 | pp/expand | 16 | PASS |
| H2.5 | pp/predef | 20 | PASS |
| H2.6 | pp/line | 22 | PASS |
| H2.7 | pp/no_include | 9 | PASS |
| H3.1 | parser/decl | 15 | PASS |
| H3.2 | parser/expr | 13 | PASS |
| H3.3 | parser/stmt | 18 | PASS |
| H3.4 | parser/aggregate | 14 | PASS |
| H3.5 | parser/template | 16 | PASS |
| H3.6 | parser/module | 16 | PASS |
| H3.7 | parser/err | 19 | PASS |
| H4.1 | hir/name | 27 | PASS |
| H4.2 | hir/type | 124 | PASS |
| H4.3 | hir/constexpr | 70 | PASS |
| H4.4 | hir/template | 99 | PASS |
| H4.5 | hir/borrow | 45 | PASS |
| H4.6 | hir/move | 60 | PASS |
| H5.1 | mir/cfg | 75 | PASS |

**27 / 27 suites PASS, 762 / 762 files** (687 of the 26 pre-existing suites
+ the 75 files of the H5.1 corpus, of which 72 are the phase-4 ones and 3
are the new phase-5 regression fixtures; the 687 include the 3 direct-init
argument-run regression fixtures added to `parser/stmt`).

The **75 files of the MIR corpus were also re-verified individually** with a
raw byte capture (`cmd /c "... > f.bin 2> f.bin"`, the oracle's stdout and
stderr separately, against the port's output for the same file): **75 / 75
byte-identical, 75 / 75 with a real MIR dump (stdout > 0 byte), 75 / 75 with
exit code 0** - and the 72 phase-4 files have exactly the same byte counts
as before (no regression).

### 13.12 The bookkeeping record (phase 5)

* **Changed**:
  * `selfhost/mir/mir_builder.ivy` - the D1 fix (the three sites of 13.2 /
    13.3 + the two helpers `reserve_args` / `set_arg`), the D2 fix (the two
    allocations `* 88` -> `* 80` + the comment of `grow_mparams`) and the D3
    fix (`substr1` folded into `keep_string`).
  * `selfhost/mir/mir.ivy` - the D2 size claim (88 -> 80).
  * `selfhost/parsing/parser.ivy` - the D7 fix (`push_case` returns -1, the
    caller skips the link).
  * `selfhost/hir/hir_builder.ivy` - the D7 fix (`push_hcase` returns -1, the
    caller skips the link).
  * `selfhost/corpus/mir/cfg/` - the three D4 fixtures.
  * `selfhost/mir/MIR_NOTES.md` - the D2 size claim + the note, the D5
    citations (+ the 9 other drifted citations), the D4 inventory and counts,
    and this section 13.
  * `PLAN.md` - the H5.1 row (72 -> 75 files), the H5.1 note (the corpus
    count, the D2 size claim, the D6 count) and the D1 defect record.
* **Unchanged**: `src/**` (the oracle was **not** rebuilt - it keeps its
  mtime `2026-10-21 16:41:44` and its size 27031552), `selfhost/difftest.ps1`
  (**no** change was needed - the H5.1 invocation was already there),
  `selfhost/mir/mirdrive.ivy`, the other corpora, the `.gitignore`.
* **No commit** was made.  `git status --porcelain` shows the 8 modified
  files plus the two untracked directories `selfhost/mir/` and
  `selfhost/corpus/mir/`.
* **The build products** (`selfhost/{parsing,hir,mir}/*.obj`, `*.ivm`) are
  the ones the harness itself writes (they are covered by `.gitignore`).
* **The opencode session store was never touched**: no file under
  `C:\Users\ASUS\.local\share\opencode\` was read, grepped, listed or
  written, and no `opencode.db*` was accessed anywhere on the machine.  The
  only path used under the user's profile was the pre-approved scratch
  directory `C:\Users\ASUS\AppData\Local\Temp\opencode\h51fix\`.