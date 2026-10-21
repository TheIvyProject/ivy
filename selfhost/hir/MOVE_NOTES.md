# H4.6 (Move / use-after-move) - working notes

## 0. The state of this file

This file collects the notes of milestone **H4.6** (the port of the oracle's
move / use-after-move tracking to the self-host port).

**Phase 1** (the reconnaissance: the oracle's move semantics, the port's
gaps, the fixture corpus) wrote sections 1-5 and 7-10 here.  The first
**phase 2** attempt was cancelled mid-flight and it *overwrote* this file
with its own report, so **sections 1-5 and 7-10 are lost**.  What is left
is:

* this section 0;
* **section 6** - the fixture inventory, **reconstructed** from the phase-1
  artefacts that are still on disk (the 140 fixtures in
  `C:\Users\ASUS\AppData\Local\Temp\opencode\h46r1\final\` and their
  classification in `...\h46r1\classification.txt`); the fixture *sources*
  are the ground truth, so the table is exact;
* **section 11** - the phase-2 implementation report, re-verified from
  scratch by the second phase-2 attempt (the code, the byte comparison and
  the 25 regression suites were all re-run; the numbers in section 11 are
  the measured ones, not the cancelled attempt's).

The **phase-4 verification** re-wrote **sections 9 and 10** (they had been
lost with the rest of the phase-1 sections) and corrected two off-by-one /
stale entries (§11.3's `declare()` call sites, §12.1's row 18).  The file
now contains the sections **0-12**; each of them carries a head note that
says whether it is a phase-1, phase-2, phase-3 or phase-4 text.

The reconstruction of section 6 was checked mechanically: the 140 names are
exactly the 140 files of `h46r1\final\` (sorted) and the 140 class letters
are exactly the ones in `h46r1\classification.txt`.

**The phase-2 scratch directory** is
`C:\Users\ASUS\AppData\Local\Temp\opencode\h46f2\`:

| path | what |
|---|---|
| `final\` | the 140 phase-1 fixtures (a copy of `h46r1\final\`) |
| `probe\` | the 22 edge-case probes of the first phase-2 attempt |
| `probe2\` | the 28 further probes of the second phase-2 attempt (generality) |
| `port\` | the port build (a copy of `selfhost/parsing` + `selfhost/hir`, compiled with the oracle) |
| `port_base\` | the same build from the **HEAD** (`75332c9`) sources - the "before" baseline |
| `out_v1\`, `out_v2\` | the raw byte captures + `TABLE.txt` / `DIFFS.txt` of the comparison |
| `verify.ps1` | the byte-exact comparison harness (stdout + stderr, CRLF-normalised) |
| `suites.ps1` | the runner of the 25 existing regression suites (sequentially) |
| `SECTION6.tsv` | the source of the section-6 table |

---

## 6. The fixture corpus (140 files)

The corpus was built in phase 1 out of the oracle's move semantics.  Every
file is run through the oracle (`ivy --hir`) and through the port
(`hirdrive`), and the two byte streams are compared.

### 6.1 The classes

| class | count | meaning |
|---|---|---|
| **HIR** | **83** | **in scope** for H4.6 - the move semantics are decided in the *HIR* builder, so the port must reproduce them byte for byte |
| **CTRL** | 39 | controls - the files that must *not* change (they were already byte-identical before the milestone) |
| **MIR** | 4 | the move semantics are decided in the *MIR* builder - out of scope for H4.6 (deferred to H5) |
| **OUT** | 14 | pre-existing port gaps unrelated to the move tracking (globals, multi-declarator statements, lambdas, struct constructors, namespaces) |

### 6.2 The fixtures

| # | fixture | class | rule it covers |
|---|---|---|---|
| 1 | `e01_user_defined_move.ivy` | HIR | a user-defined `move` function does **not** shadow the builtin: the builtin still marks the operand (the use-after-move is reported) |
| 2 | `e02_index_lhs_bypass.ivy` | HIR | the assignment-LHS bypass covers the identifiers of the index expression (`a[i] = 1`) |
| 3 | `e03_deref_lhs_bypass.ivy` | OUT | the assignment-LHS bypass covers a `*p` left-hand side (a raw pointer needs `unsafe`) |
| 4 | `e04_member_lhs_bypass.ivy` | HIR | the assignment-LHS bypass covers the base of a member left-hand side (`s.v = 1`) |
| 5 | `e05_move_in_for_init.ivy` | HIR | `move(<literal>)` in a for-init is **not** the builtin (the operand is not an identifier) - the callee is resolved normally |
| 6 | `e06_move_in_switch_case.ivy` | CTRL | a `move(x)` inside a switch case body marks the outer variable |
| 7 | `e07_lambda_body_use.ivy` | OUT | a lambda body is a separate scope (the port's parser has no lambda support at all) |
| 8 | `e08_user_move_non_ident.ivy` | CTRL | a user-defined `move` called with a literal goes to the user's function (the builtin needs an identifier operand) |
| 9 | `e09_move_struct_with_dtor.ivy` | HIR | `move()` of a struct that has a destructor + the use-after-move on the field read (needs the struct constructor - not in the port) |
| 10 | `e10_sibling_scopes_same_name.ivy` | CTRL | two sibling blocks declare the same name: the second binding starts `Initialized` |
| 11 | `e11_move_in_while_body.ivy` | CTRL | a `move(x)` in a while body + a re-assignment in the same body |
| 12 | `e12_move_then_use_in_condition.ivy` | HIR | the use-after-move in a `while` condition |
| 13 | `e13_move_of_param_in_method.ivy` | HIR | a method parameter is moved and then read |
| 14 | `e14_move_in_constexpr_call.ivy` | HIR | the argument of a `constexpr` call is checked |
| 15 | `e15_move_of_array_var.ivy` | CTRL | `move()` of an array variable (the operand is an identifier - the builtin fires) |
| 16 | `e16_move_lhs_reinit_then_move.ivy` | CTRL | a plain `=` re-initialises the variable, so the second `move()` is **not** "already moved-out" |
| 17 | `e17_move_in_nested_block_shadow.ivy` | CTRL | the inner block's `move` does not touch the outer binding |
| 18 | `e18_move_of_const_param.ivy` | HIR | a `const` parameter can be moved (the oracle has no const check in the move branch) |
| 19 | `e19_move_call_result_move.ivy` | HIR | `move(g(x))` - the operand is a call, not an identifier - is **not** the builtin |
| 20 | `e20_move_in_unsafe_then_use.ivy` | HIR | the `unsafe` block does **not** bypass the check (the spec would; the oracle does not) |
| 21 | `f01_move_enum_const.ivy` | HIR | `move(Red)` - an unscoped enum constant folds to an integer literal, so the operand is not an identifier - the builtin does **not** fire |
| 22 | `f02_move_function_name.ivy` | HIR | `move(g)` - a function name used as a value: "function 'g' cannot be used as a value" |
| 23 | `f03_move_param_shadow_body.ivy` | HIR | a parameter shadowed by a body local: the two bindings have separate move states |
| 24 | `f04_move_this_read.ivy` | CTRL | `this.v` - the receiver is not a variable use that is checked |
| 25 | `f05_move_in_nested_template.ivy` | HIR | a template parameter is moved and then read |
| 26 | `f06_move_in_if_branch.ivy` | HIR | the `move` in the if-branch is visible after the `if` |
| 27 | `f07_move_in_else_branch.ivy` | HIR | the `move` in the else-branch is visible after the `if` |
| 28 | `f08_move_of_var_in_unsafe_only.ivy` | HIR | the `unsafe` block does **not** bypass the check |
| 29 | `f09_move_assign_lhs_in_unsafe.ivy` | CTRL | a re-assignment inside `unsafe` re-initialises the variable |
| 30 | `f10_move_struct_ctor_then_use.ivy` | OUT | the struct constructor (`S s(1);`) is not supported by the port |
| 31 | `m01_move_local_read.ivy` | HIR | the basic use-after-move (`move(x); int32 y = x;`) |
| 32 | `m02_move_twice.ivy` | HIR | the second `move(x)`: the operand's use-after-move **and** "'x' is already moved-out", in that order |
| 33 | `m03_move_reassign_then_read.ivy` | CTRL | a plain `=` re-initialises the variable, so the read is clean |
| 34 | `m04_move_compound_assign.ivy` | HIR | a compound assignment bypasses the check but does **not** re-initialise |
| 35 | `m05_move_param.ivy` | HIR | a parameter is moved and then read |
| 36 | `m06_move_const.ivy` | HIR | a `const` local can be moved |
| 37 | `m07_move_for_var.ivy` | HIR | the for-loop variable is moved in the body and read there |
| 38 | `m08_move_nested_block.ivy` | HIR | the `move` in a nested block is visible in that block |
| 39 | `m09_move_shadow_inner.ivy` | HIR | the inner shadowing binding has its own state (the outer one is untouched) |
| 40 | `m10_move_before_decl.ivy` | HIR | `move(x)` before the declaration: `x` is not a variable yet - the operand is still an `IdentRef` |
| 41 | `m100_move_qualified_ivy_move.ivy` | HIR | `ivy::move(x)` is a qualified name, not the bare `move` - the builtin does **not** fire |
| 42 | `m101_move_after_nested_move_ok.ivy` | CTRL | the assignment re-initialises the variable after the nested `move` |
| 43 | `m102_move_unsafe_bypass_spec.ivy` | CTRL | the spec's `unsafe` bypass - the oracle does **not** implement it |
| 44 | `m11_move_zero_args.ivy` | HIR | `move()` with no argument is **not** the builtin |
| 45 | `m12_move_two_args.ivy` | HIR | `move(x, y)` is **not** the builtin |
| 46 | `m13_move_no_call.ivy` | HIR | `move` used as a value: "undeclared identifier 'move'" |
| 47 | `m14_move_qualified.ivy` | HIR | `ivy::move(x)` is **not** the builtin |
| 48 | `m15_move_literal.ivy` | HIR | `move(42)` is **not** the builtin |
| 49 | `m16_move_call_result.ivy` | HIR | `move(g())` is **not** the builtin |
| 50 | `m17_move_field.ivy` | HIR | `move(s.v)` is **not** the builtin (a member expression) |
| 51 | `m18_move_index.ivy` | HIR | `move(a[0])` is **not** the builtin |
| 52 | `m19_move_binary.ivy` | HIR | `move(x + 1)` is **not** the builtin |
| 53 | `m20_move_global.ivy` | OUT | a global variable - the port's parser has no global variables |
| 54 | `m21_move_after_block.ivy` | HIR | the `move` in a nested block is visible after the block |
| 55 | `m22_move_sibling_block.ivy` | HIR | the `move` in a block is visible in the next sibling block |
| 56 | `m23_move_read_expr.ivy` | HIR | the use-after-move inside a binary expression |
| 57 | `m24_move_assign_lhs.ivy` | CTRL | the assignment left-hand side is bypassed (no use-after-move) |
| 58 | `m25_move_addr_of.ivy` | HIR | the use-after-move inside the address-of expression (`&x`) |
| 59 | `m26_move_call_arg.ivy` | HIR | a moved-out variable used as a call argument |
| 60 | `m27_move_return.ivy` | HIR | `return x;` after the move |
| 61 | `m28_move_sizeof_type.ivy` | CTRL | `sizeof(int32)` is unaffected by the move state |
| 62 | `m29_move_in_while.ivy` | CTRL | a `move(x)` in a while body + a re-assignment |
| 63 | `m30_move_in_switch.ivy` | HIR | a `move(x)` in a switch case + a read after the switch |
| 64 | `m31_move_struct_value.ivy` | HIR | `move(s)` of a struct value + the field read |
| 65 | `m32_move_template.ivy` | HIR | a template function parameter is moved and then read |
| 66 | `m33_move_lifetime_param.ivy` | CTRL | a lifetime parameter is moved |
| 67 | `m34_move_constexpr.ivy` | CTRL | a `constexpr` function body that moves a local |
| 68 | `m35_move_struct_ctor.ivy` | HIR | the struct constructor (`S s(1);`) is not supported by the port |
| 69 | `m36_move_in_unsafe.ivy` | HIR | the `unsafe` block does **not** bypass the check |
| 70 | `m37_move_lambda_capture.ivy` | MIR | a lambda capture: the diagnostic is MIR-stage, and the port's parser has no lambdas |
| 71 | `m38_move_this.ivy` | HIR | `move(this)` is **not** the builtin (`this` is not an `IdentRef`) |
| 72 | `m39_move_in_for_body_after_loop.ivy` | HIR | the `move` in the for body is visible after the loop |
| 73 | `m40_move_ternary.ivy` | HIR | the use-after-move in a ternary condition |
| 74 | `m41_move_nested_move.ivy` | HIR | `move(move(x))` - the outer operand is a call - is **not** the builtin |
| 75 | `m42_move_assign_from_move.ivy` | HIR | `int32 y = move(x);` + a read of `x` |
| 76 | `m43_move_redeclare_same_scope.ivy` | OUT | a re-declaration in the same scope: "redefinition of variable 'x'" |
| 77 | `m44_move_ref_param.ivy` | HIR | a reference parameter is moved and then read |
| 78 | `m45_move_multi_decl.ivy` | OUT | a multi-declarator statement (`int32 x = 1, y = 2;`) - not supported by the port |
| 79 | `m46_move_switch_case_block.ivy` | CTRL | a `move(x)` inside a case's own block |
| 80 | `m47_move_ok_baseline.ivy` | CTRL | the clean baseline (`int32 y = move(x);`) - the dump shows `call 'move' : int32_t` |
| 81 | `m48_move_dump_only.ivy` | CTRL | the dump-only case (`move(x); x = 2;`) |
| 82 | `m49_move_in_unsafe_then_read.ivy` | HIR | the `unsafe` block does **not** bypass the check |
| 83 | `m50_move_method_this_field.ivy` | HIR | `move(this)` in a method |
| 84 | `m51_move_mir_only_ret_ref.ivy` | MIR | returning a reference to a local - a **MIR-stage** diagnostic |
| 85 | `m52_move_mir_only_borrow.ivy` | CTRL | a reference bound to a borrowed value - MIR only |
| 86 | `m53_move_in_do_while.ivy` | CTRL | a `move(x)` in a do-while body + a re-assignment |
| 87 | `m54_move_initializer.ivy` | HIR | `int32 x = move(1);` is **not** the builtin |
| 88 | `m55_move_conditional_arg.ivy` | HIR | `move(<conditional>)` is **not** the builtin |
| 89 | `m56_move_in_exprstmt.ivy` | CTRL | an expression statement |
| 90 | `m57_move_param_reassign.ivy` | CTRL | a parameter is re-assigned after the move - the re-assignment re-initialises it |
| 91 | `m58_move_scope_after_loop.ivy` | CTRL | the for-loop scope ends with the loop |
| 92 | `m59_move_struct_ptr.ivy` | CTRL | a struct value is moved + a field write |
| 93 | `m60_move_array_elem_write.ivy` | HIR | `a[0] = 1; move(a[0]);` - the operand is not an identifier - the builtin does **not** fire |
| 94 | `m61_move_lambda_capture_ok.ivy` | OUT | a lambda capture - the port's parser has no lambdas |
| 95 | `m62_move_template_move_ok.ivy` | CTRL | a template that does not move anything |
| 96 | `m63_move_lifetime_ok.ivy` | CTRL | a lifetime parameter that is not moved |
| 97 | `m64_move_multidecl_only.ivy` | OUT | a multi-declarator statement - not supported by the port |
| 98 | `m65_move_this_in_method.ivy` | HIR | `move(this)` in a method + a `return` |
| 99 | `m66_move_in_unsafe_only.ivy` | CTRL | the `unsafe` block alone |
| 100 | `m67_move_nested_block_only.ivy` | CTRL | the nested block alone |
| 101 | `m68_move_exprstmt_only.ivy` | CTRL | the expression statement alone |
| 102 | `m69_move_in_callarg_position.ivy` | HIR | `g(move(x))` - the `move` as a call argument |
| 103 | `m70_move_assign_chain.ivy` | HIR | `y = x;` with `x` moved out: the right-hand side **is** checked |
| 104 | `m71_move_ternary_assign.ivy` | HIR | the use-after-move in the right-hand side of an assignment |
| 105 | `m72_move_in_if_cond.ivy` | HIR | the use-after-move in an `if` condition |
| 106 | `m73_move_while_cond.ivy` | HIR | the use-after-move in a `while` condition |
| 107 | `m74_move_in_switch_cond.ivy` | HIR | the use-after-move in a `switch` condition |
| 108 | `m75_move_in_for_cond.ivy` | HIR | the use-after-move in a `for` condition |
| 109 | `m76_move_prefix_inc.ivy` | HIR | `x++` after the move |
| 110 | `m77_move_postfix_dec.ivy` | HIR | `x--` after the move |
| 111 | `m78_move_deref.ivy` | HIR | `*p` where `p` points to a moved-out `x` |
| 112 | `m79_move_struct_field_write.ivy` | HIR | `s.v = 1;` after `move(s)` - the member base is bypassed |
| 113 | `m80_move_array_write.ivy` | CTRL | `a[0] = 1;` alone |
| 114 | `m81_move_assign_from_moved_in_rhs.ivy` | HIR | the right-hand side of an assignment is always checked |
| 115 | `m82_move_move_of_param_reexpr.ivy` | CTRL | a parameter is moved, re-assigned and read |
| 116 | `m83_move_lambda_no_return.ivy` | MIR | a lambda without a `return` - MIR-stage + no lambdas in the port |
| 117 | `m84_move_lambda_explicit_ret.ivy` | MIR | a lambda with an explicit `return` - MIR-stage + no lambdas in the port |
| 118 | `m85_move_in_method.ivy` | OUT | a method that does not move anything |
| 119 | `m86_move_local_in_method.ivy` | HIR | a method-local is moved and then read |
| 120 | `m87_move_outer_scope_from_inner.ivy` | CTRL | the inner shadowing binding is moved; the outer one is not |
| 121 | `m88_move_before_decl_outer.ivy` | HIR | `move(x)` before the declaration in an outer scope |
| 122 | `m89_move_for_var_after_loop.ivy` | HIR | the for-loop variable is out of scope after the loop |
| 123 | `m90_move_in_constexpr_body.ivy` | CTRL | a `constexpr` body that moves a local |
| 124 | `m91_move_call_builtin.ivy` | OUT | a builtin call (`printf`) after a move |
| 125 | `m92_move_assign_to_const.ivy` | OUT | an assignment to a `const` variable |
| 126 | `m93_move_after_reassign_then_move.ivy` | CTRL | move -> `=` -> move again: no "already moved-out" |
| 127 | `m94_move_ref_param_assign.ivy` | CTRL | a reference parameter is re-assigned after the move |
| 128 | `m95_move_in_nested_fn_call.ivy` | HIR | a moved-out variable used as a call argument |
| 129 | `m96_move_struct_ctor_param.ivy` | OUT | the struct constructor (`S s(1);`) is not supported by the port |
| 130 | `m97_move_global_var_parse.ivy` | OUT | a global variable - the port's parser has no global variables |
| 131 | `m98_move_method_this.ivy` | HIR | `move(this)` in a method |
| 132 | `m99_move_this_field_read.ivy` | HIR | `move(this); return this.v;` |
| 133 | `q01_qualified_global_move.ivy` | OUT | `::move(x)` - the global qualifier; the port has no namespace support |
| 134 | `q02_local_named_move.ivy` | HIR | a local variable named `move`: the builtin still wins |
| 135 | `q04_nested_call_in_lhs.ivy` | CTRL | the assignment-LHS bypass **leaks into a nested call** (`a[f(x)] = 1;`) |
| 136 | `q05_move_param_then_read.ivy` | HIR | a parameter is moved and then read |
| 137 | `q06_template_named_move.ivy` | CTRL | a template named `move` called with a literal: the template is used |
| 138 | `q07_move_in_lhs_of_ternary.ivy` | HIR | `y = x > 0 ? 1 : 2;` with `x` moved out |
| 139 | `q08_move_reinit_by_compound.ivy` | HIR | a compound assignment does **not** re-initialise |
| 140 | `q09_move_again_after_reinit.ivy` | HIR | move -> `=` -> move again: no "already moved-out" |

---

## 11. Phase 2 (implementation) - the move-state tracking in the port

**Milestone:** H4.6, phase 2/4 (implementation).
**HEAD at the time of writing:** `75332c9` (unchanged - no commit was made).
**Production code changed by this phase:** `selfhost/hir/hir_builder.ivy` only.
**Scratch dir:** `C:\Users\ASUS\AppData\Local\Temp\opencode\h46f2\`.

### 11.1 The implemented change list

Every line number below is the line number in the file **after** the change
(the file is `selfhost/hir/hir_builder.ivy`; the oracle is
`src/hir/hir_builder.cpp` + `src/hir/hir_builder.h`).

| # | Port | What | Oracle |
|---|---|---|---|
| 1 | `:33-41` (the struct fields) | `long* smv_` - the move state of every symbol, parallel to `snm_` / `sty_`; `long in_assign_lhs_` - the assignment-LHS bypass flag | `hir_builder.h:52-53` (`MoveState` + `moveStates_`), `hir_builder.h:74-77` (`inAssignLhs_`) |
| 2 | `:296` (`setup`) | `this.smv_ = (long*) __ivy_alloc(this.s_cap_ * 8);` (a `long` is 8 bytes on x86-64) | - (the oracle uses a `std::vector`) |
| 3 | `:374` (`setup`) | `this.in_assign_lhs_ = 0;` | `hir_builder.h:74` (the initial value) |
| 4 | `:464` (`release`) | `__ivy_free(this.smv_);` | - |
| 5 | `:764-772` (`err_use_moved_out`) | `"use of moved-out variable '<name>'"` | `hir_builder.cpp:2657` |
| 6 | `:774-782` (`err_already_moved_out`) | `"'<name>' is already moved-out"` | `hir_builder.cpp:2945` |
| 7 | `:1971-1983` (`sym_moved` / `set_sym_moved`) | the per-symbol move-state accessors | - |
| 8 | `:1985-2002` (`mark_moved_out`) | the inner -> outer search + the "already moved-out" diagnostic + the marking | `hir_builder.cpp:2941-2950` |
| 9 | `:2004-2012` (`reinitialize`) | the inner -> outer search + the `Initialized` state | `hir_builder.cpp:3686-3694` |
| 10 | `:2039-2040` (`declare`) | `this.set_sym_moved(this.s_n_ - 1, 0);` - a new variable starts `Initialized` | `hir_builder.cpp:964-965` |
| 11 | `:3616-3625` (`build_ident`) | the use-after-move check, at the identifier's own location | `hir_builder.cpp:2644-2659` |
| 12 | `:4169-4194` (`build_call`) | the `move()` builtin branch, **before the callee resolution** | `hir_builder.cpp:2932-2963` |
| 13 | `:4348` (the old `move` branch) | **removed** - it is now dead code (the hoisted branch handles every case it did) | - |
| 14 | `:4904-4906` (`build_assign`) | `in_assign_lhs_ = 1` around the LHS build, `= 0` after it | `hir_builder.cpp:3603-3605` |
| 15 | `:4947-4952` (`build_assign`) | the re-initialisation for `=` with an `IdentRef` LHS | `hir_builder.cpp:3684-3695` |

No change was needed in `selfhost/hir/hir.ivy` (the HIR nodes carry no move
state) nor in `selfhost/hir/hirdrive.ivy` (the dump has no move annotation)
- see 11.8 for the driver's exit code.

### 11.2 What the cancelled attempt had written, and what the second attempt did

The cancelled attempt had already written most of the change.  The second
attempt **reviewed every line of it against the oracle**, re-ran the whole
verification from scratch (including a fresh "before" measurement of the
HEAD build) and made these changes:

* **Kept as is** (each item re-checked against the oracle, see the change
  list in 11.1): the `smv_` array parallel to `snm_` / `sty_`, the two
  diagnostic helpers, the use-after-move check in `build_ident`, the
  assignment-LHS bypass flag, the re-initialisation in `build_assign`, the
  allocation / initialisation / `release` of the new pool, and the body of
  the `move()` builtin branch.
* **Kept, but re-verified against the oracle:** the **hoisting** of the
  `move()` branch above the callee resolution.  The oracle checks the
  builtin at `hir_builder.cpp:2937`, i.e. *before* the operator-overload
  probe (`:2969`), before the template-id call (`:3044`) and before the
  ordinary overload resolution (`:3119`); the port's branch sits directly
  after the arguments have been built (`build_call` `:4169-4194`), i.e.
  before the `ca.kind == 8` block that contains the template-id handling
  and the overload resolution.  The old branch (which sat *after* the
  overload resolution) was deleted - it is now unreachable.  The precedence
  is checked by `e01` (a user-defined `move`), `p01` / `p02` (a template
  and a template-id call named `move`), `q02` / `n14` (a local variable
  named `move`) and `p04` (a *method* named `move` - the callee is a
  `Member`, so the branch must **not** fire).
* **Two details of the branch that the cancelled attempt had already
  changed relative to the HEAD code, and that the second attempt confirmed
  against the oracle:**
  1. the argument-count test uses the **built** argument count
     (`e.an == 1`), not the syntactic one (`a.an == 1`) - the oracle tests
     `call.args.size()`, i.e. the number of arguments *after* the
     pack-expansion splicing (`hir_builder.cpp:2937`);
  2. the operand test is `ae.kind == 8` (an `IdentRef`), which is also
     true for an **undeclared** identifier - exactly like the oracle
     (`hir_builder.cpp:2681-2688`), so `move(undeclared)` takes the builtin
     branch in both and only the "undeclared identifier" diagnostic is
     reported.
* **Changed by the second attempt:** the comment blocks now cite the
  oracle's line numbers, so the two implementations can be audited against
  each other; and this notes file was rewritten (the cancelled attempt had
  overwritten phase 1's sections - see section 0).
* **Deliberately not changed:** the oracle's MIR-stage move tracking
  (`src/mir/mir_builder.cpp:476,568`) is out of scope for H4.6 - the port
  must not emit anything the oracle does not.

### 11.3 The scope push/pop mapping (the equivalence proof)

The oracle pushes and pops `moveStates_` at **exactly** the five places
where it pushes and pops `scopes_`:

| oracle | `scopes_` | `moveStates_` | scope |
|---|---|---|---|
| `buildBody` | `:929` / `:937` | `:930` / `:938` | the **function** scope (the parameters live here) |
| `buildCompound` | `:1252` / `:1260` | `:1253` / `:1261` | **every** compound statement |
| the `for` statement | `:2049` / `:2059` | `:2050` / `:2060` | the **for-init** scope (the loop variable lives here) |
| `buildLambda` | `:4207` / `:4310` and `:4282` / `:4302` | `:4208` / `:4311` and `:4283` / `:4303` | the lambda's parameter scope and body scope |

`grep -n "scopes_\.\(push_back\|pop_back\|emplace\)"` shows **no other**
insertion into `scopes_`: the only one is `scope.emplace(name, type)` at
`:963`, immediately followed by `moveStates_.back().emplace(name,
MoveState::Initialized)` at `:965` (both inside `declare()`).  Therefore the
two maps of a scope always have the **same key set**, and:

> the oracle's "the innermost scope that declares the name" (the `scopes_`
> search) == the oracle's "the innermost scope whose move-state map has the
> name" (the `moveStates_` search).

The port has **three** scope push/pop pairs - they are the only
`enter_scope()` / `leave_scope()` calls in the file:

| Port | Oracle | Scope | Move states |
|---|---|---|---|
| `build_body` `:3082` / `:3095` | `buildBody` `:929` / `:937` | the **function** scope (the parameters live here) | the parameters get their `Initialized` state through `declare()` |
| `build_compound` `:3113` / `:3133` | `buildCompound` `:1252` / `:1260` | **every** compound statement | - |
| `build_stmt` (the `for` statement) `:3550` / `:3561` | the `for` statement `:2049` / `:2059` | the **for-init** scope (the loop variable lives here) | the loop variable gets its state through `declare()` |
| - | `buildLambda` `:4207-4311` | the lambda's parameter scope and body scope | **not applicable**: the port's parser has no lambda support at all |

The port therefore does **not** need a scope-indexed move-state stack: the
flat `smv_` array is indexed by the *symbol* index, and the port's
`find_sym` scans the flat symbol stack **backwards**.  The symbols of a
scope are contiguous and `leave_scope()` truncates the stack back to the
scope's start marker, so the backwards scan finds the symbol of the
innermost scope that declares the name.  Because the oracle's
`moveStates_` map and its `scopes_` map have the same key set per scope,
the port's "the innermost symbol with that name" **is** the oracle's "the
innermost scope that declares the name" - the same *binding*.

`build_body` saves and restores `s_n_` / `sc_n_` (`:3070-3071` /
`:3104-3105`) around a template instantiation.  The move states are indexed
by the symbol index, so they follow `s_n_` automatically: the caller's
symbols keep their state, and the template's own symbols get fresh states
from `declare()` (which overwrites `smv_[i]`).  No extra save/restore is
needed.

**The declaration kinds.**  The oracle inserts a move state in `declare()`
only, and `declare()` is called at `:934` (the parameters), `:1298` / `:1303`
/ `:1319` (an `auto` variable), `:1421` (an ordinary local), `:1818` (the
NRVO temporary of `return <local>`), `:1903` / `:1929` (the
structured-binding temporaries) and `:4212` / `:4215` (the lambda closure
pointer and the lambda parameters).  The port calls its own `declare()` at
exactly the matching places for the features it supports - `:3090` (the
parameters), `:3261` / `:3283` (an `auto` variable) and `:3312` (an
ordinary local) - and the three oracle sites the port has no counterpart
for (NRVO, structured bindings, lambdas) belong to features the port does
not implement at all.  So: **no more, no less**.

### 11.4 The per-binding state (shadowing)

`smv_` has **one entry per symbol**, not per name.  A declaration pushes a
symbol *and* a fresh `Initialized` state, so a shadowing declaration in an
inner scope has its own state and leaves the outer binding untouched.
The scope-shadowing fixtures are byte-identical:

* `m09_move_shadow_inner.ivy` - the inner `x` is moved, the outer one is not;
* `f03_move_param_shadow_body.ivy` - the body's `x` is moved, the
  parameter's `x` is not;
* `m87_move_outer_scope_from_inner.ivy` / `e17_...` / `n33_...` (controls)
  - unchanged.

(The oracle's function scope and body scope are two different scopes -
`buildBody` pushes one, then `buildCompound` pushes another; the port has
the same two-level structure, so a parameter may be shadowed by a body
local.)

### 11.5 The `move()` branch and its position relative to the overload resolution

The recognition conditions are the oracle's, one for one:

* the callee is a **bare identifier** named `move` (`ca.kind == 8` &&
  `seq(ca.sval, "move")`) - the oracle's `callee->node` must be an
  `A::IdentRef` (`hir_builder.cpp:2932-2933`);
* the call has **exactly one** argument - the oracle's
  `call.args.size() == 1` is the number of *built* arguments, so the port
  uses `e.an == 1` (the post-pack-expansion count) and not `a.an == 1`;
* the argument's HIR node is an **`IdentRef`** (`ae.kind == 8`).  The port's
  `build_ident` mirrors the oracle's node kinds exactly: an unscoped enum
  constant is folded to an integer literal (kind 1), `this` is kind 7, and
  an *undeclared* identifier is **still** an `IdentRef` (kind 8) with a
  dummy type - exactly like the oracle (`hir_builder.cpp:2681`).

Then, in the oracle's order: mark the operand moved-out (`:2941-2950`), set
the result type to the **operand's** type (`:2955-2957`), set the callee to
`move` (`:2961`) and **return** (`:2963`) - the `Call` node survives, so
the dump still shows `call 'move' : <operand type>`.

The precedence is therefore: **the builtin wins over every shadowing of the
name `move`** (a local variable, a user-defined function, a function
template, a template-id call) whenever the argument is an `IdentRef`; and
the ordinary resolution is used when the argument is not an `IdentRef`
(`move(42)` -> `undeclared identifier 'move'`, `move(1)` with a user-defined
`move` -> the user's function).

### 11.6 The bypass rules and the re-initialisation

* **The assignment-LHS bypass** (`in_assign_lhs_`) is a plain flag (not
  saved/restored), set around the **whole** LHS expression
  (`build_assign` `:4904-4906`), so it covers every identifier of the LHS at
  any nesting depth - including the arguments of a call inside the LHS
  (`q04_nested_call_in_lhs.ivy`, byte-identical) - and it is set for **all**
  assignment operators, not only `=`.  The oracle's flag is set the same
  way (`hir_builder.cpp:3603-3605`), including the fact that it is *not*
  restored.
* The RHS is built after the flag is cleared, so the RHS is always checked
  (`m81_move_assign_from_moved_in_rhs.ivy`, `n05_...`, byte-identical).
* **The re-initialisation** happens only for the plain `=` operator and only
  when the LHS is a plain `IdentRef` (`le.kind == 8`), and it happens
  *after* the type checks, exactly like the oracle.  A compound assignment
  bypasses the check but does **not** re-initialise
  (`q08_move_reinit_by_compound.ivy`, `m04_move_compound_assign.ivy`,
  `n28_compound_mul_assign.ivy`).
* The diagnostic locations differ and are reproduced exactly: the
  use-after-move diagnostic is at the **identifier**'s location, the
  "already moved-out" diagnostic at the **call's** location (the `(` token -
  `selfhost/parsing/parser.ivy:2413` + `:2434-2435` set the call's location
  from the `(` token, exactly like the oracle's `parser.cpp:2751` + `:2777`).
* The **order** of the two diagnostics in one file is the oracle's: the
  use-after-move is emitted while the *operand* is built (before the builtin
  branch runs), so "use of moved-out" always precedes "already moved-out"
  (`m02_move_twice.ivy`: `4:10` then `4:9`).

### 11.7 The `unsafe` bypass - DECISION: NOT IMPLEMENTED

The oracle's **HIR** builder has **no** `unsafeDepth_` guard on the move
check and **no** guard on the move marking (the guard exists only in the
MIR builder, `src/mir/mir_builder.cpp:476,568`).  The spec
(`spec/ownership.md` §2.6) is aspirational; the requirement is to reproduce
the **code**, not the spec.  The port therefore does **not** implement an
`unsafe` bypass: the port's `unsafe_depth_` field is not consulted by the
move check or by the move marking.  The fixtures that would break if the
bypass were (wrongly) added - `m36_move_in_unsafe.ivy`,
`m49_move_in_unsafe_then_read.ivy`, `e20_move_in_unsafe_then_use.ivy`,
`f08_move_of_var_in_unsafe_only.ivy`, `m102_move_unsafe_bypass_spec.ivy` -
are all byte-identical.  This decision is recorded here (this file) and was
verified by the probe `p22_move_in_unsafe_move.ivy` and by
`n17_move_in_unsafe_block.ivy`.

### 11.8 The verification results

**The 140 fixtures of section 6** (the raw bytes of stdout + stderr, CRLF
normalised - the same convention as `selfhost/difftest.ps1`; the port's
`=== path ===` marker line is removed before the comparison):

| class | count | before (the HEAD build) | after (this phase) | remaining mismatches |
|---|---|---|---|---|
| **HIR (in scope)** | 83 | 29 | **81** | `m35_move_struct_ctor.ivy`, `e09_move_struct_with_dtor.ivy` (see below) |
| **CTRL (controls)** | 39 | 39 | **39** | - |
| **MIR (out of scope)** | 4 | 0 | 0 | `m37`, `m51`, `m83`, `m84` (expected - see 11.9) |
| **OUT (pre-existing gaps)** | 14 | 9 | 9 | `e03`, `e07`, `f10`, `m61`, `m96` (unchanged from the baseline) |
| total | 140 | 77 | **129** | 11 |

The "before" column is the measurement of the same port built from the
**HEAD** sources (`75332c9`) - it is the baseline of this milestone, and it
was measured again by the second phase-2 attempt
(`h46f2\port_base\` + `h46f2\out_base\`).  All **52** move-tracking
mismatches are fixed by this phase; the 11 remaining ones are exactly the
11 that were already different before the change.

**The two remaining in-scope mismatches** are the *pre-existing*
struct-constructor gap of the port (not a move gap): `S s(1);` (the direct
initialisation of a struct with a user-declared constructor) is not
supported by the port's HIR builder, so the port reports `undeclared
identifier 'S'` where the oracle builds the constructor.  The move
tracking itself is correct in both fixtures - the port emits `use of
moved-out variable 's'` (m35, at 9:15) and `use of moved-out variable 'r'`
(e09, at 11:15) at exactly the oracle's locations, but the surrounding
diagnostics differ.  (The struct construction is a planned gap of a later
milestone - `PLAN.md`: "struct construction (can khoi tao mau `Res` co
destructor) - canh RAII/DTOR milestone (hoac H5)".)

**The four MIR-only fixtures**: the port must not emit anything the oracle
does not.  Verified:

* `m51_move_mir_only_ret_ref.ivy` - the port emits exactly the oracle's HIR
  dump (124 bytes, byte-identical) and nothing more; the oracle
  additionally prints the MIR-stage diagnostic `returning a reference to a
  local variable (dangling reference)` after the dump.  The port has no
  MIR builder, so this diagnostic is unreachable - **deferred to H5**.
* `m37`, `m83`, `m84` - the oracle's diagnostic is MIR-stage (`use of
  moved-out variable 'x'` at 4:14 - the lambda capture is re-built by the
  MIR builder, `mir_builder.cpp:676-678`, where the capture's initialiser
  reads the moved-out `x`), and the port's **parser** has no lambda support,
  so the port fails earlier with parse diagnostics.  The port emits **no**
  move diagnostic for these three fixtures (checked explicitly); the
  difference is the pre-existing lambda gap, unchanged by this phase.

**The 50 probes** (`h46f2\probe\` = 22 from the first attempt,
`h46f2\probe2\` = 28 written by the second attempt to cover the shapes the
corpus does not contain) are byte-identical, except `p07`, which is a
**MIR-only** case (see 11.9).  The probes answered the open questions of
phase 1:

| probe | question | result (oracle == port) |
|---|---|---|
| `p01_tpl_move_ident.ivy` | a **template** named `move` with an `IdentRef` argument | the **builtin wins**: `use of moved-out variable 'y'` - the template is *not* instantiated |
| `p02_tplid_move.ivy` | a template-id call `move<int32>(x)` | the **builtin wins** (`use of moved-out variable 'x'`) - the oracle's move branch does not look at `tplArgs` |
| `p03_struct_named_move.ivy` | a **struct** named `move` | moot: the oracle's parser rejects `move s;` ("expected variable name") |
| `p04_method_named_move.ivy` | a **method call** `s.move(1)` | the move branch does **not** fire (the callee is a `Member`): the call is `call 'S::move' : void` |
| `p05_self_assign_moved.ivy` | `x = x;` with a moved-out `x` | the RHS is checked: `use of moved-out variable 'x'` at the RHS |
| `p06_compound_then_move.ivy` | `x += 1; move(x);` | a compound assignment does not re-initialise: "use of moved-out" then "already moved-out" |
| `p07_move_in_for_init.ivy` | `move(i)` on a for-loop variable | **MIR-only**: the oracle's HIR stage is clean, the **MIR** stage reports `use of moved-out variable 'i'` at the loop increment (2:34) |
| `n01_multi_diag_order.ivy` | three diagnostics in one file | the order is: "use of moved-out" (the operand), "already moved-out" (the call), "use of moved-out" (the next argument) |
| `n14_move_of_var_named_move.ivy` | a **local variable** named `move` | the builtin wins: the dump shows `call 'move' : int32_t / ident move` |
| `n21_struct_named_move_ctor.ivy` | a **struct** named `move` with a constructor | moot: the parser rejects `move` as a variable name |
| `n28_compound_mul_assign.ivy` | `x *= 2;` after a move | the compound assignment bypasses the check but does not re-initialise (the later read reports) |
| `p08` .. `p22`, `n02` .. `n33` | params, blocks, switch/while/do-while, shadowing, `unsafe`, pack expansions, `ns::move` | all byte-identical |

**The 25 existing suites** (620 files) all stay PASS:

| suite | files | verdict | suite | files | verdict |
|---|---|---|---|---|---|
| lexer/ident | 10 | PASS | parser/aggregate | 14 | PASS |
| lexer/num | 8 | PASS | parser/template | 16 | PASS |
| lexer/str | 7 | PASS | parser/module | 16 | PASS |
| lexer/op | 5 | PASS | parser/err | 19 | PASS |
| lexer/trivia | 7 | PASS | hir/name | 27 | PASS |
| lexer/err | 9 | PASS | hir/type | 120 | PASS |
| pp/define_obj | 10 | PASS | hir/constexpr | 70 | PASS |
| pp/define_fn | 12 | PASS | hir/template | 99 | PASS |
| pp/cond | 16 | PASS | hir/borrow | 45 | PASS |
| pp/expand | 16 | PASS | | | |
| pp/predef | 20 | PASS | | | |
| pp/line | 22 | PASS | | | |
| pp/no_include | 9 | PASS | | | |
| parser/decl | 15 | PASS | | | |
| parser/expr | 13 | PASS | | | |
| parser/stmt | 15 | PASS | | | |

(25 suites, 620 files, 25 PASS / 0 FAIL; the suites were run **sequentially**,
because the lexer and pp suites share one temp dir.)

**The exit code.**  The oracle exits 1 when the HIR build fails
(`src/app/main.cpp:1311`); the port's driver (`selfhost/hir/hirdrive.ivy`)
always exits 0 - the same behaviour as in H4.1-H4.5, and the reason why
`selfhost/difftest.ps1` compares the output bytes only.  The driver was
**not** changed in this phase: the move tracking does not change it, and the
HIR suites of the five previous milestones share it.

### 11.9 The stage boundary (why the port cannot match a MIR diagnostic)

`ivy --hir` runs the whole pipeline (`src/app/main.cpp:1303-1321`): the HIR
diagnostics are printed, **no** HIR dump is printed when the HIR build fails,
and the HIR dump **is** printed before a MIR-stage diagnostic.  The port
(`selfhost/hir/hirdrive.ivy:503-535`) stops after the HIR stage, so:

* an HIR-stage diagnostic is reproduced exactly (the port prints the
  diagnostics and no dump - the same bytes as the oracle);
* a MIR-stage diagnostic can never be reproduced: the oracle prints
  `dump + diagnostic`, the port prints `dump` only.

`p07_move_in_for_init.ivy` is a new instance of that class (a `move()` of a
for-loop variable is legal in the HIR stage - the oracle builds the loop
increment *before* the body, `hir_builder.cpp:2052-2058` - but the MIR
stage re-checks the increment and reports the use-after-move).  The H4.6
corpus must therefore contain no MIR-stage diagnostic.

### 11.10 The cleanliness statement (phase 2)

* Modified: `selfhost/hir/hir_builder.ivy` (the only production file).
* This file: section 0 and section 6 were **reconstructed** (the cancelled
  attempt had overwritten the file, see section 0) and section 11 is the
  phase-2 report.
* `src/**`: **unchanged**.  The oracle (`build\Release\ivy.exe`) was **not**
  rebuilt.
* `selfhost/parsing/**`, `selfhost/difftest.ps1`, `PLAN.md`, the corpus:
  **unchanged**.  The `.obj`/`.ivm` files that `selfhost/difftest.ps1`
  writes into `selfhost/parsing` and `selfhost/hir` (and the two `.ivm`
  files the oracle writes next to `selfhost/corpus/parser/module/h36_iface*.ivy`)
  were removed again after the run, so the tree is byte-identical to
  `75332c9` apart from the two files above.  `git status --porcelain` shows
  exactly ` M selfhost/hir/hir_builder.ivy` and `?? selfhost/hir/MOVE_NOTES.md`.
* No commit was made.  The corpus was not touched (phase 3 owns it).
* The scratch port was built in `C:\Users\ASUS\AppData\Local\Temp\opencode\h46f2\port\`
  from *copies* of `selfhost/parsing/*.ivy` and `selfhost/hir/*.ivy`, so no
  build product was written into the tree by the fixture runs.

---

*Reading order.*  The sections below are the **restored** sections §1-§5, §7
and §8 (the ones the cancelled phase-1 agent destroyed - see §0) plus the new
**§12** (phase 3, this phase).  They are **appended** after §11 because this
file is append-only: §0 (the state of the file), §6 (the phase-1 fixture
inventory) and §11 (the phase-2 implementation report) are kept verbatim.
The intended reading order is §0 → §1 → §2 → §3 → §4 → §5 → §6 → §7 → §8 →
§11 → §12.

## 1. Scope & summary

**H4.6** ports the oracle's **move / use-after-move** tracking - the "A4"
feature of `src/hir/hir_builder.cpp` - to the self-host port
(`selfhost/hir/hir_builder.ivy`).

### 1.1 What H4.6 covers

The move semantics are decided in the **HIR** builder.  The oracle keeps a
*move state* per variable binding (`Initialized` / `MovedOut`), and:

1. the **`move()` builtin** is recognised as a *call* whose callee is the bare
   identifier `move` and whose single argument is an identifier; the operand's
   binding is then marked `MovedOut`;
2. a **read** of a `MovedOut` binding is an error
   (`use of moved-out variable 'x'`);
3. a **`move()` of an already `MovedOut` binding** is an error
   (`'x' is already moved-out`);
4. the **left-hand side of an assignment** bypasses rule 2 (a re-assignment
   re-initialises the binding);
5. a **plain `=` to an identifier** re-initialises the binding; a compound
   assignment does not.

That is the whole of the HIR-stage feature: **two diagnostics, one bypass, one
re-initialisation**, and the recognition rules of the builtin.

### 1.2 What H4.6 defers

| deferred item | where it lives | why |
|---|---|---|
| the **MIR-stage** move tracking | `src/mir/mir_builder.cpp:189-220` (the `markMovedOut` / `isMovedOut` / `reinitialize` helpers), `:471-482` (the use-after-move check), `:556-584` (the `move()` builtin branch), `:626-645` (the LHS bypass + the re-initialisation) | the port has **no MIR builder**; the MIR stage belongs to **H5.2** |
| the **`unsafe` bypass** | the spec `spec/ownership.md` §2.6 asks for it; the **oracle's HIR builder has none** - the guard exists **only** in the MIR builder (`src/mir/mir_builder.cpp:476`, `:568`) | the requirement is to reproduce the **code**, not the spec; see §7.4 |
| the **lambdas** | the oracle's lambda builder (`src/hir/hir_builder.cpp:4207-4311`) has its own scope push/pop and its own `declare()` calls | the port's **parser has no lambda syntax at all** |
| the **struct constructors** | `S s(1);` (the direct initialisation of a struct with a user-declared constructor) | a pre-existing port gap of the **RAII/DTOR** milestone, not a move gap |

The `unsafe` bypass is the only place where the oracle and the spec disagree
in the HIR stage; §7.4 records the decision and the evidence.

### 1.3 The one-paragraph summary

`move(x)` is a **builtin function**, not a keyword: the port must recognise
the *call* `move(x)` in exactly the same syntactic situations as the oracle
(the callee is the bare name `move`, there is exactly one argument, and that
argument is an identifier) and must mark the operand's binding as moved-out;
every later read of that binding must report the use-after-move, a second
`move()` of the same binding must report the already-moved-out, the
left-hand side of an assignment must be exempt from the read check, and a
plain `=` must put the binding back into `Initialized`.

---

## 2. The oracle source map

All the line numbers below were **re-derived** from the oracle sources in
this phase (`src/hir/hir_builder.cpp` and `src/hir/hir_builder.h`, read
only - the oracle was not modified and not rebuilt).

### 2.1 The state store

| # | where | what |
|---|---|---|
| 1 | `src/hir/hir_builder.h:50-51` | the comment: "A4: Move-state tracking - parallel to `scopes_`, pushed/popped together. Tracks whether a variable is Initialized or MovedOut (after `move()`)." |
| 2 | `src/hir/hir_builder.h:52` | `enum class MoveState { Initialized, MovedOut };` - the two states of a binding. |
| 3 | `src/hir/hir_builder.h:53` | `std::vector<std::unordered_map<std::string_view, MoveState>> moveStates_;` - **one map per scope**, pushed and popped in lock-step with `scopes_` (`:49`). |
| 4 | `src/hir/hir_builder.h:74-77` | the bypass flag: `bool inAssignLhs_ = false;` with the comment "A4: When true, use-after-move check is bypassed for IdentRef (used when building the lhs of an assignment - re-assignment re-initializes a moved-out variable)." |
| 5 | `src/hir/hir_builder.cpp:84-97` | `bool isBuiltinFn(std::string_view name)` - the registry of the runtime builtins; `:85-86` is the `move` entry ("A4: move() builtin."). `isBuiltinFn` is used at `:3151`, `:3200` and `:3399` (the callee resolution); it is **not** used by the move branch, which tests the name directly. |

### 2.2 The scope push/pop sites

`scopes_` and `moveStates_` are pushed and popped **together**, always on
adjacent lines:

| scope | `scopes_` | `moveStates_` | the site |
|---|---|---|---|
| the **function** scope (the parameters live here) | `:929` / `:937` | `:930` / `:938` | `buildBody` |
| **every** compound statement | `:1252` / `:1260` | `:1253` / `:1261` | `buildCompound` |
| the **for-init** scope (the loop variable lives here) | `:2049` / `:2059` | `:2050` / `:2060` | the `A::For` branch of `buildStmt` |
| the **lambda parameter** scope | `:4207` / `:4310` | `:4208` / `:4311` | `buildLambda` |
| the **lambda body** scope | `:4282` / `:4302` | `:4283` / `:4303` | `buildLambda` |

`grep -n "scopes_\.\(push_back\|pop_back\|emplace\)"` shows **no other**
insertion into `scopes_`: the only one is `scope.emplace(name, type)` at
`:963`, immediately followed by the move-state insertion at `:965`.  The two
maps of a scope therefore always have the **same key set** - the key fact
for the equivalence proof of §7.2.

### 2.3 The declaration insertion site

| # | where | what |
|---|---|---|
| 1 | `src/hir/hir_builder.cpp:954-966` | `HirBuilder::declare()` - the only place a move state is created. |
| 2 | `src/hir/hir_builder.cpp:963` | `scope.emplace(name, type);` - the variable enters the scope map. |
| 3 | `src/hir/hir_builder.cpp:965` | `moveStates_.back().emplace(name, MoveState::Initialized);` - **a new binding starts `Initialized`**. |

The `declare()` call sites (and therefore the places where a move state is
created) are:

| where | what |
|---|---|
| `:934` | the parameters of a function/method body |
| `:1298`, `:1303`, `:1319` | an `auto` variable (three shapes) |
| `:1421` | an ordinary local |
| `:1818` | the NRVO temporary of `return <local>` |
| `:1903`, `:1929` | the structured-binding temporaries |
| `:4212`, `:4215` | the lambda closure pointer and the lambda parameters |

### 2.4 The two diagnostics and the two state transitions

| # | where | what |
|---|---|---|
| 1 | `src/hir/hir_builder.cpp:2644-2659` | the **use-after-move check**, in the `A::IdentRef` branch of `buildExpr`: the scope search at `:2648-2650`, the guard `if (!inAssignLhs_)` at `:2653`, the state lookup at `:2654-2656` and the diagnostic `use of moved-out variable '<name>'` at `:2657`.  The location is the **identifier's own** location (`e.loc`). |
| 2 | `src/hir/hir_builder.cpp:2932-2963` | the **`move()` builtin branch**, in the `A::Call` branch of `buildExpr`: the callee test at `:2932-2933` (`A::IdentRef` + the bare name), the recognition condition at `:2937-2938` (the name is `move`, **exactly one** built argument, the argument is an `hir::Expr::IdentRef`), the marking at `:2941-2950` (the inner-to-outer scope search, the already-moved-out diagnostic at `:2945` and the state change at `:2947`), the result type at `:2955-2957` (the operand's type), the callee at `:2961` and the `return` at `:2963` - the `Call` node **survives**. |
| 3 | `src/hir/hir_builder.cpp:3603-3605` | the **assignment-LHS bypass**, in the `A::Assign` branch of `buildExpr`: `inAssignLhs_ = true;` → `as.lhs = buildExpr(*v.lhs);` → `inAssignLhs_ = false;`.  The flag is **not** restored (it is simply cleared), and it is set for **every** assignment operator, not only `=`. |
| 4 | `src/hir/hir_builder.cpp:3684-3695` | the **re-initialisation**, at the end of the same branch: only for `as.op == "="` **and** an `IdentRef` left-hand side; the inner-to-outer scope search at `:3688-3690` and the state change to `Initialized` at `:3691`. |

Note the **order** inside the `move()` branch: the operand is built **before**
the branch runs (`:2904-2925`), so a read of an already-moved-out operand
reports the *use-after-move* **first**, and the *already-moved-out* is
reported afterwards - the two diagnostics of a double move appear in that
order (§4).

### 2.5 The MIR-stage counterparts (out of scope)

| where | what |
|---|---|
| `src/mir/mir_builder.cpp:189-198` | `MirBuilder::markMovedOut()` - the MIR twin of the marking. |
| `src/mir/mir_builder.cpp:200-209` | `MirBuilder::isMovedOut()` - the MIR twin of the read check. |
| `src/mir/mir_builder.cpp:211-220` | `MirBuilder::reinitialize()` - the MIR twin of the re-initialisation. |
| `src/mir/mir_builder.cpp:471-482` | the MIR **use-after-move check** - note the extra `unsafeDepth_ == 0` guard at `:476`. |
| `src/mir/mir_builder.cpp:556-584` | the MIR **`move()` builtin branch** - note the extra `unsafeDepth_ == 0` guard at `:568`. |
| `src/mir/mir_builder.cpp:626-645` | the MIR **assignment-LHS bypass** (`:633-635`) and the **re-initialisation** (`:638-644`). |

The MIR stage keeps its **own** move-state stack; it does not read the HIR
stage's.  The port has no MIR builder, so none of this is ported in H4.6.

---

## 3. The rules

Every rule is numbered; the corpus file(s) that cover it are named.  The
file names are the ones of `selfhost/corpus/hir/move/`.

### R1 - the `move()` builtin fires

`move(x)` where `x` is a **plain identifier** that names a variable marks
that binding as `MovedOut`.  The call node survives, so the dump shows
`call 'move' : <the operand's type>`.

* corpus: `30_builtin_beats_user_function.ivy` (the user-defined `move` does
  **not** shadow the builtin), `32_builtin_beats_local_named_move.ivy` (a
  local variable named `move` does not shadow it either),
  `59_move_dump_baseline.ivy` (the clean dump shape).
* oracle: `hir_builder.cpp:2937-2950`.

### R2 - the use-after-move diagnostic

A **read** of a `MovedOut` binding is an error:
`use of moved-out variable '<name>'`, at the **identifier's** location.

* corpus: `01_move_local_read.ivy` (the plain read), `02_move_in_call_arg.ivy`,
  `03_move_in_return.ivy`, `04_move_in_ternary.ivy`, `05_move_in_while_cond.ivy`,
  `06_move_in_prefix_inc.ivy`, `07_move_in_addr_of.ivy`,
  `08_move_in_assign_rhs.ivy` (the right-hand side is checked).
* oracle: `hir_builder.cpp:2653-2659`.

### R3 - the already-moved-out diagnostic

A `move()` of an already `MovedOut` binding is an error:
`'<name>' is already moved-out`, at the **call's** location (the `(` token),
and it comes **after** the operand's use-after-move.

* corpus: `09_move_twice.ivy` (both diagnostics, in that order).
* oracle: `hir_builder.cpp:2944-2946`.

### R4 - the assignment-LHS bypass

The **left-hand side** of an assignment is not checked.  The bypass covers
**every** identifier of the left-hand side expression, at any nesting depth,
and it is set for **every** assignment operator.

* corpus: `14_assign_lhs_bypass.ivy` (a plain identifier),
  `15_assign_lhs_bypass_index.ivy` (`a[i] = 1`),
  `16_assign_lhs_bypass_member.ivy` (`s.v = 1`),
  `17_assign_lhs_bypass_nested_call.ivy` (`a[f(x)] = 1` - the bypass leaks
  into a nested call).
* oracle: `hir_builder.cpp:3603-3605`.

### R5 - the right-hand side is always checked

The right-hand side of an assignment is built **after** the flag is cleared,
so it is always checked - even when the left-hand side is the same variable.

* corpus: `08_move_in_assign_rhs.ivy` (the plain `y = x;` form),
  `18_assign_rhs_is_checked.ivy` (a `move` in the right-hand side - the
  use-after-move **and** the already-moved-out).
* oracle: `hir_builder.cpp:3605-3612`.

### R6 - the re-initialisation

A **plain `=`** whose left-hand side is a **plain identifier** puts the
binding back into `Initialized`.

* corpus: `10_reassign_reinitializes.ivy`, `12_reinit_then_move_again.ivy`
  (move → `=` → move again: no already-moved-out),
  `13_param_reassign_reinitializes.ivy` (a parameter).
* oracle: `hir_builder.cpp:3684-3695`.

### R7 - a compound assignment does **not** re-initialise

`x += 1` bypasses the check (the left-hand side) but leaves the binding
`MovedOut`.

* corpus: `11_compound_assign_no_reinit.ivy`.
* oracle: `hir_builder.cpp:3685` (the test is `as.op == "="`).

### R8 - the recognition of the builtin: the callee

The callee must be the **bare** identifier `move`.  A qualified name
(`ivy::move(x)`) is **not** the builtin, and neither is a **method** call
(`s.move(1)` - the callee is a `Member`).

* corpus: `21_move_qualified.ivy`.
* oracle: `hir_builder.cpp:2932-2933` (the callee must be an `A::IdentRef`).

### R9 - the recognition of the builtin: the arity

The call must have **exactly one** argument, counted **after** the
pack-expansion splicing (`call.args.size()`, not the syntactic count).

* corpus: `19_move_zero_args.ivy`, `20_move_two_args.ivy`.
* oracle: `hir_builder.cpp:2937`.

### R10 - the recognition of the builtin: the operand

The argument's HIR node must be an `hir::Expr::IdentRef`.  Everything else
is **not** the builtin:

| operand | corpus | the oracle's diagnostic |
|---|---|---|
| a literal | `22_move_literal.ivy` | `undeclared identifier 'move'` |
| a call result | `23_move_call_result.ivy` | `undeclared identifier 'move'` |
| a member expression | `24_move_field.ivy` | `undeclared identifier 'move'` |
| an index expression | `25_move_index.ivy` | `undeclared identifier 'move'` |
| a nested call | `26_move_nested_move.ivy` | `undeclared identifier 'move'` |
| an **unscoped enum constant** (folded to an integer literal) | `27_move_enum_const.ivy` | `undeclared identifier 'move'` |
| a **function name** (not a variable) | `28_move_function_name.ivy` | `function 'g' cannot be used as a value (no function pointers)` |
| `this` (an `H::This`, not an `IdentRef`) | `29_move_this.ivy` | `undeclared identifier 'move'` |

* oracle: `hir_builder.cpp:2938`; the operand kinds come from the
  `buildExpr` node mapping - an unscoped enum constant becomes an
  `IntegerLit` (`:2670-2675`), `this` becomes an `H::This` (`:2631`) and an
  **undeclared** identifier stays an `IdentRef` (`:2681`).

### R11 - the builtin wins over every shadowing of the name `move`

The branch is tested **before** the operator-overload probe (`:2969`), the
template-id call (`:3044`) and the ordinary overload resolution (`:3119`),
so the builtin wins over a user-defined function, a function template, a
template-id call and a local variable named `move`.

* corpus: `30_builtin_beats_user_function.ivy` (a user-defined function),
  `32_builtin_beats_local_named_move.ivy` (a local variable),
  `31_user_function_used_for_literal.ivy` (a user-defined function **is**
  used when the operand is not an identifier),
  `33_template_named_move.ivy` (a template is used for a literal operand).
* oracle: `hir_builder.cpp:2932-2963` (the branch) vs `:2965-3119` (the
  ordinary resolution).

### R12 - the scope of a move state

A move is visible in the **enclosing** scope (the oracle has no
block-scoped move states), but the state belongs to the **binding**, not to
the name.

* corpus: `34_move_in_nested_block.ivy` (visible in the nested block),
  `35_move_after_nested_block.ivy` (visible after the block),
  `36_shadow_inner.ivy` (the inner shadowing binding has its **own** state),
  `37_shadow_param_by_local.ivy` (a parameter shadowed by a body local),
  `38_move_before_decl.ivy` (the operand is not a variable yet),
  `39_for_var_out_of_scope.ivy` (the for variable is gone after the loop).
* oracle: `hir_builder.cpp:929-938`, `:1252-1261`, `:2049-2060`, `:965`.

### R13 - the kinds of binding

The move state lives on the **binding**, whatever its kind: a by-value
parameter, a `const` parameter, a reference parameter, a `const` local, a
struct value.

* corpus: `40_move_param.ivy`, `41_move_const_param.ivy`,
  `42_move_ref_param.ivy`, `43_move_const_local.ivy`,
  `44_move_struct_value.ivy`.
* oracle: `hir_builder.cpp:965` (every `declare()`).

### R14 - the `unsafe` block does **not** bypass the check

The **HIR** builder of the oracle has **no** `unsafe` guard on either the
read check or the marking; only the **MIR** builder has one
(`src/mir/mir_builder.cpp:476`, `:568`).  The spec
(`spec/ownership.md` §2.6) is aspirational.

* corpus: `45_unsafe_no_bypass.ivy`, `46_unsafe_no_bypass_then_read.ivy`,
  `47_unsafe_assign_reinitializes.ivy` (the re-assignment inside `unsafe`
  re-initialises).
* oracle: `hir_builder.cpp:2653` (no `unsafeDepth_` in the test) and
  `:2941-2950` (no `unsafeDepth_` in the marking).

### R15 - the control flow

A move inside a branch or a loop body is visible after it.

* corpus: `48_move_in_if_branch.ivy`, `49_move_in_for_body.ivy`,
  `50_move_in_while_body.ivy`, `51_move_in_switch_case.ivy`,
  `52_move_for_var_in_body.ivy` (the for-loop variable is a binding of the
  for-init scope).
* oracle: `hir_builder.cpp:1252-1261` (the compound statement is a scope),
  `:2049-2060` (the for-init scope).

### R16 - the interaction with the templates, the lifetimes and `constexpr`

The move tracking is **orthogonal** to the other features: a template
parameter, a lifetime parameter, a `constexpr` call argument and a
`constexpr` body are all tracked exactly like an ordinary local.

* corpus: `53_template_param_moved.ivy`, `54_lifetime_param_moved.ivy`,
  `55_constexpr_call_arg.ivy`, `56_constexpr_body_move.ivy`.
* oracle: `hir_builder.cpp:965` + the same check; the templates declare
  their parameters through the same `declare()` (`:934`).

### R17 - the methods and `this`

A method body is an ordinary scope; `move(this)` is **not** the builtin
(`this` is not an `IdentRef`), and a field read of `this` is never checked.

* corpus: `57_move_local_in_method.ivy` (a method-local),
  `58_move_this_then_read_field.ivy`.
* oracle: `hir_builder.cpp:2631` (`this` → `H::This`), `:2681`.

### R18 - the dump shapes

A `move(x)` that produces no diagnostic appears in the `--hir` dump as
`call 'move' : <the operand's type>` with the operand as its only argument;
the `Call` node is **not** replaced by a pass-through.

* corpus: `59_move_dump_baseline.ivy` (the clean baseline),
  `60_move_as_call_argument.ivy` (the builtin in an argument position).
* oracle: `hir_builder.cpp:2951-2963` (the comment "B3: Keep as Call node").

---

## 4. The diagnostics

The two HIR-stage diagnostics of the move tracking, with their **exact**
message bytes:

| # | message (byte-exact) | location | trigger | stage |
|---|---|---|---|---|
| 1 | `use of moved-out variable '<name>'` | the **identifier's** own location (`e.loc` of the `IdentRef` expression) | a read of a binding whose move state is `MovedOut` | HIR |
| 2 | `'<name>' is already moved-out` | the **call's** location (the `(` token) | a `move()` of a binding whose move state is already `MovedOut` | HIR |

The two messages are emitted in the order **(1) then (2)** for a double move,
because the operand is built before the builtin branch runs
(`hir_builder.cpp:2904-2925` before `:2937`).  The captured evidence
(`09_move_twice.ivy`, oracle):

```
selfhost/corpus/hir/move/09_move_twice.ivy:5:10: error: use of moved-out variable 'x'
selfhost/corpus/hir/move/09_move_twice.ivy:5:9: error: 'x' is already moved-out
```

Note the two different columns: `5:10` is the **operand** `x`, `5:9` is the
**call** `move(`.

Neither message is a *warning*; both are `error:` and both make the HIR build
fail (the oracle exits 1 and prints **no** dump - `src/app/main.cpp:1311`).

The **MIR** stage has the same two message texts
(`src/mir/mir_builder.cpp:477`, `:570-571`), but they are a *different stage*
and are **not** part of H4.6 (see §1.2 and §12.2).

The **pre-existing** diagnostics that the corpus files also produce are the
ordinary H4.1/H4.2 ones - `undeclared identifier 'move'`,
`call to undeclared function 'ivy::move'`,
`function 'g' cannot be used as a value (no function pointers)` - and they
are covered by the `hir/name` suite; they appear here only because the
`move()` builtin **loses** to the ordinary resolution in those shapes.

---

## 5. The dump representation

The oracle's `move()` builtin keeps the `Call` node (it does **not** replace
it with a pass-through), so the `--hir` dump shows an ordinary call whose
callee is the literal name `move` and whose result type is the **operand's**
type.

`selfhost/corpus/hir/move/59_move_dump_baseline.ivy`:

```ivy
// H4.6 rule: the clean baseline: the dump shows call 'move' : int32_t (phase-1 fixture: m47_move_ok_baseline)
void f() {
    int32 x = 1;
    int32 y = move(x);
    int32 z = y;
}
```

the oracle's `--hir` output (byte-exact):

```
function f
  return void()
  block
    decl int32_t x =
      int 1
    decl int32_t y =
      call 'move' : int32_t
        ident x
    decl int32_t z =
      ident y
```

The three things to read off the dump:

* the callee is printed as the bare name `move` (the oracle sets
  `call.callee = bareName` at `hir_builder.cpp:2961`);
* the result type is the **operand's** type (`int32_t` here) - the oracle
  copies it at `:2955-2957`;
* the argument is still printed as `ident x` - the operand is **not**
  consumed.

A `move(x)` used as a **statement** appears the same way, as an `expr` node
(`selfhost/corpus/hir/move/14_assign_lhs_bypass.ivy`):

```
    expr
      call 'move' : int32_t
        ident x
```

There is **no** move annotation in the dump: the HIR nodes
(`selfhost/hir/hir.ivy`) carry no move state, and the driver
(`selfhost/hir/hirdrive.ivy`) prints no extra line for it.  The only trace
of the move in the output is therefore the **diagnostic** - which is exactly
why the two diagnostics are the whole observable surface of the feature.

---

## 7. The port gap analysis

### 7.1 What the port lacked before phase 2

The port had **no** move tracking at all: no move state, no
`move()` builtin branch, no use-after-move check, no assignment-LHS
bypass, no re-initialisation.  `move(x)` was resolved by the ordinary
callee resolution, so the port reported `undeclared identifier 'move'` (or
`call to undeclared function 'move'`) where the oracle reported
`use of moved-out variable 'x'`, and it printed a dump with a
`call 'move'` node where the oracle printed a diagnostic.

The measured "before" baseline (the port built from the **HEAD** sources
`75332c9`, the same 140 fixtures, the same byte comparison) is in §11.8:
**52 of the 140** fixtures differed, and **all 52** were move-tracking
mismatches.

### 7.2 The change list

The port's change list (with the port line numbers) is in §11.1; the
production file is `selfhost/hir/hir_builder.ivy` only.  In summary:

| what | the port | the oracle |
|---|---|---|
| the state store | `long* smv_` (`:37`), one entry per **symbol** (not per scope), allocated in `setup` (`:296`), freed in `release` (`:464`) | `std::vector<std::unordered_map<...>> moveStates_` (`hir_builder.h:53`), one map per **scope** |
| the bypass flag | `long in_assign_lhs_` (`:41`), initialised in `setup` (`:374`) | `bool inAssignLhs_` (`hir_builder.h:77`) |
| the two diagnostics | `err_use_moved_out` (`:766-772`), `err_already_moved_out` (`:776-782`) | `:2657`, `:2945` |
| the marking | `mark_moved_out` (`:1993-2002`) | `:2941-2950` |
| the re-initialisation | `reinitialize` (`:2006-2012`) | `:3684-3695` |
| a new binding starts `Initialized` | `declare()` (`:2040`) | `:965` |
| the read check | `build_ident` (`:3616-3625`) | `:2644-2659` |
| the builtin branch | `build_call` (`:4169-4194`), **hoisted** above the callee resolution | `:2932-2963` |
| the LHS bypass | `build_assign` (`:4904-4906`) | `:3603-3605` |
| the re-initialisation | `build_assign` (`:4947-4952`) | `:3684-3695` |

### 7.3 The equivalence arguments

**The scope push/pop mapping.**  The oracle pushes and pops `moveStates_` at
**exactly** the five places where it pushes and pops `scopes_` (§2.2), and
the only insertion into a scope map is the pair `scope.emplace` (`:963`) /
`moveStates_.back().emplace` (`:965`).  The two maps of a scope therefore
always have the same key set, so:

> the oracle's "the innermost scope that declares the name" (the `scopes_`
> search) == the oracle's "the innermost scope whose move-state map has the
> name" (the `moveStates_` search).

The port has only **three** scope push/pop pairs (the function scope, the
compound statement and the for-init scope - the port has no lambdas), and it
does **not** need a scope-indexed state stack: the flat `smv_` array is
indexed by the *symbol* index and `find_sym` scans the flat symbol stack
**backwards**.  The symbols of a scope are contiguous and `leave_scope()`
truncates the stack back to the scope's start marker, so the backwards scan
finds the symbol of the innermost scope that declares the name - the same
*binding* the oracle finds.  A template instantiation saves and restores
`s_n_` (`:3070-3071` / `:3104-3105`); because the move states are indexed
by the symbol index, they follow `s_n_` automatically (the caller's symbols
keep their state, the template's own symbols get fresh states from
`declare()`).

**The per-binding (shadowing) argument.**  `smv_` has **one entry per
symbol**, not per name.  A declaration pushes a symbol *and* a fresh
`Initialized` state, so a shadowing declaration in an inner scope has its
own state and leaves the outer binding untouched.  The oracle behaves
identically because its `moveStates_` maps are per scope: the inner scope
has its own entry for the name, and the inner-to-outer search stops at the
first scope that has it.

**The `move`-branch ordering argument.**  The oracle tests the builtin at
`hir_builder.cpp:2937`, i.e. **before** the operator-overload probe
(`:2969`), before the template-id call (`:3044`) and before the ordinary
overload resolution (`:3119`).  The port's branch sits directly after the
arguments have been built (`build_call` `:4169-4194`), i.e. before the
`ca.kind == 8` block that contains the template-id handling and the
overload resolution.  The old port branch (which sat *after* the overload
resolution) was **deleted** - it is now unreachable.  The precedence is
pinned by `30_builtin_beats_user_function.ivy` (a user-defined `move`),
`33_template_named_move.ivy` (a template named `move`),
`32_builtin_beats_local_named_move.ivy` (a local variable named `move`) and
`29_move_this.ivy` / `24_move_field.ivy` (a callee that is not a bare
identifier - the branch must **not** fire).

### 7.4 The `unsafe` bypass - DECISION: NOT IMPLEMENTED

The oracle's **HIR** builder has **no** `unsafeDepth_` guard on the move
check and **no** guard on the move marking; the guard exists **only** in
the MIR builder (`src/mir/mir_builder.cpp:476`, `:568`).  The spec
(`spec/ownership.md` §2.6) is aspirational; the requirement of this
milestone is to reproduce the **code**, not the spec.  The port therefore
does **not** implement an `unsafe` bypass: the port's `unsafe_depth_` field
is not consulted by the move check or by the move marking.  The corpus
pins this with `45_unsafe_no_bypass.ivy` (the read is **inside** the
`unsafe` block and is still reported), `46_unsafe_no_bypass_then_read.ivy`
(the read is **outside** the block and is still reported) and
`47_unsafe_assign_reinitializes.ivy` (the re-assignment inside `unsafe`
re-initialises).

### 7.5 The two things the port does **not** do

* **The MIR stage.**  The port has no MIR builder; the MIR-stage move
  tracking (§2.5) is unreachable and is deferred to H5.2.  The port must
  not emit anything the oracle does not - the port's driver always exits 0
  and prints no MIR diagnostic.
* **The `unsafe` bypass** - see §7.4.

---

## 8. Open questions and how they were settled

| # | question | answer | evidence |
|---|---|---|---|
| 1 | Is the `move()` builtin a **keyword** or a **function**? | A function: it is resolved by name, it can be shadowed, and `isBuiltinFn` (`:86`) lists it with the other runtime builtins. | `30_builtin_beats_user_function.ivy`, `31_user_function_used_for_literal.ivy` |
| 2 | Does the builtin win over a user-defined `move`? | **Yes** - the branch is tested before the ordinary resolution. | `30_builtin_beats_user_function.ivy` (the user function is not called; the operand is marked) vs `31_user_function_used_for_literal.ivy` (the user function **is** used for a literal operand) |
| 3 | Does it win over a **template** named `move`? | **Yes**, when the operand is an identifier; a template-id call `move<int32>(x)` also takes the builtin (the branch does not look at `tplArgs`). | `33_template_named_move.ivy` (a literal operand → the template is used) and the phase-2 probe `p02_tplid_move.ivy` |
| 4 | Does it win over a **local variable** named `move`? | **Yes** - the callee test is on the *name*, not on the binding. | `32_builtin_beats_local_named_move.ivy` |
| 5 | Is the operand's **arity** tested syntactically or after the pack expansion? | **After** the expansion (`call.args.size()`), so the port uses the built count `e.an == 1`, not the syntactic `a.an == 1`. | `hir_builder.cpp:2937` + `:2904-2925` (the splicing) |
| 6 | Is an **undeclared** identifier a valid operand? | **Yes** - the operand test is on the HIR node kind, and an undeclared identifier is still an `IdentRef` (`:2681`); the only diagnostic is the "undeclared identifier" one. | `38_move_before_decl.ivy` |
| 7 | Is an **unscoped enum constant** a valid operand? | **No** - it is folded to an `IntegerLit` (`:2670-2675`), so the branch does not fire. | `27_move_enum_const.ivy` |
| 8 | Is `this` a valid operand? | **No** - `this` becomes an `H::This` (`:2631`), not an `IdentRef`. | `29_move_this.ivy`, `58_move_this_then_read_field.ivy` |
| 9 | Is a **function name** a valid operand? | The branch **does** fire (it is an `IdentRef`), but the operand is not a variable, so nothing is marked and no move diagnostic appears; the only diagnostic is the pre-existing "function 'g' cannot be used as a value". | `28_move_function_name.ivy` |
| 10 | Does the **`unsafe` block** bypass the check? | **No** in the HIR stage (the oracle's HIR builder has no guard).  The spec asks for it; the MIR builder has it. | §7.4; `45_unsafe_no_bypass.ivy` |
| 11 | Does a **compound assignment** re-initialise? | **No** - the re-initialisation tests `as.op == "="`. | `11_compound_assign_no_reinit.ivy` |
| 12 | Is the assignment-LHS bypass **restored** after the LHS? | The oracle simply clears the flag (`:3605`) - it is not saved/restored, and it is set for **every** assignment operator.  The port does the same. | `17_assign_lhs_bypass_nested_call.ivy` (the bypass reaches into a nested call) |
| 13 | Does the bypass reach the **right-hand side**? | **No** - the RHS is built after the flag is cleared. | `08_move_in_assign_rhs.ivy`, `18_assign_rhs_is_checked.ivy` |
| 14 | Which **location** does each diagnostic carry? | The use-after-move: the **identifier's**.  The already-moved-out: the **call's** (`(` token). | `09_move_twice.ivy` (columns `5:10` and `5:9`) |
| 15 | In which **order** do the two diagnostics appear? | The use-after-move **first** (the operand is built before the branch). | `09_move_twice.ivy` |
| 16 | Is the move state **per name** or **per binding**? | **Per binding** - a shadowing declaration has its own state. | `36_shadow_inner.ivy`, `37_shadow_param_by_local.ivy` |
| 17 | Does a move in an inner block escape to the **outer** scope? | **Yes** - the oracle's states are per scope but the search walks outwards, and there is no "moved" flag on the scope. | `35_move_after_nested_block.ivy` |
| 18 | Is the **for-loop variable** still in scope after the loop? | **No** - the for-init scope is popped with the loop; the read reports `undeclared identifier 'i'`. | `39_for_var_out_of_scope.ivy` |
| 19 | Is a `const` binding movable? | **Yes** - the oracle has no const check in the move branch. | `41_move_const_param.ivy`, `43_move_const_local.ivy` |
| 20 | Is a **reference parameter** movable? | **Yes** - the branch only looks at the node kind. | `42_move_ref_param.ivy` |
| 21 | Does the dump show the move? | Only as `call 'move' : <the operand's type>` - the `Call` node survives. | §5; `59_move_dump_baseline.ivy` |
| 22 | Can a corpus file ever contain a **MIR-stage** diagnostic? | **No** - `ivy --hir` runs the whole pipeline, so the oracle prints the dump **and then** the MIR diagnostic (exit 1); the HIR-only port can never reproduce that. | §12.2; the 4 excluded fixtures with their oracle evidence |
| 23 | Does the port have to **exit 1** like the oracle? | **No** - the driver always exits 0 (as in H4.1-H4.5); `difftest.ps1` compares the **output bytes** only. | §11.8; `selfhost/difftest.ps1` |

---

## 9. The verification method

> **Note.**  The original phase-1 §9 and §10 were lost with the rest of the
> phase-1 sections when the cancelled phase-2 attempt overwrote this file
> (see §0).  The two sections below were **re-written in the phase-4
> verification** from the measurements made there; they are not a recovery of
> the lost text.  Their content is the method and the invariants - the
> material that the phase-1 sections had covered.

**The comparison convention.**  Every claim in this file is a **byte**
comparison, never a textual one:

* the oracle side is `ivy --hir <file>` with the **stdout and the stderr**
  captured as raw bytes (`cmd /c "... > out 2> err"`); the two streams are
  concatenated in the order the harness uses, because the oracle prints the
  HIR dump on stdout and the diagnostics on stderr, while the selfhost
  driver prints **both** on stdout;
* the port side is the `hirdrive` driver, which reads the file list from
  stdin;
* the only normalisation is `CR LF -> LF` (a platform artefact of the
  Windows console, not a content difference) - the same normalisation that
  `selfhost/difftest.ps1` applies;
* the port's `=== <path> ===` marker line is prepended to the oracle's bytes
  before the comparison, so the two sides are aligned line for line.

**The four checks.**  A change to the move semantics is proved by four
independent measurements, none of which trusts the others:

| # | check | what it proves |
|---|---|---|
| 1 | the **corpus** suite (`selfhost/corpus/hir/move/`) - 60 files, byte-identical | the rules R1-R18 of §3 on the *positive* cases (the diagnostic fires, the diagnostic does not fire, the dump shape) |
| 2 | the **phase-1 fixtures** - the 140 files of §6, compared against the **HEAD** build and the current build | that the milestone changed exactly the move-tracking cases and **nothing else**: a file whose output is identical in the two builds is a pre-existing gap (§12.2) |
| 3 | the **25 existing suites** (620 files) - all byte-identical | that no other feature regressed (the HIR builder is shared by every HIR suite) |
| 4 | an **independent probe set** written from the oracle sources (not from the corpus) | that the rules also hold on the cases the corpus does not cover |

**The HEAD baseline.**  The "before" measurement is always a port built from
the **HEAD** sources (`75332c9`) in a scratch directory, so that the
comparison is against the same code with the milestone removed.  The
baseline is what makes the claim "pre-existing gap" checkable: a difference
that is **present in both** builds is not caused by the milestone.

**The probe set.**  The probes are written from the oracle sources, one
rule per probe, and are compared with the same byte convention.  A probe
that the oracle cannot answer (e.g. it crashes) is recorded as such and is
excluded - it is not a port defect.

---

## 10. The invariants and the failure modes

> **Note.**  See the note at the head of §9: this section was re-written in
> the phase-4 verification.

### 10.1 The invariants the implementation must preserve

| # | invariant | pinned by |
|---|---|---|
| 1 | the move state belongs to the **binding**, not to the name | `36_shadow_inner.ivy`, `37_shadow_param_by_local.ivy` |
| 2 | a new binding starts `Initialized`, a popped scope's slot is **re-initialised** on reuse | `29_move_after_nested_block.ivy`, `36_shadow_inner.ivy` |
| 3 | the move state of an **outer** binding is visible from an inner scope | `34_move_in_nested_block.ivy`, `35_move_after_nested_block.ivy` |
| 4 | the use-after-move diagnostic carries the **identifier's** location | `09_move_twice.ivy` (column 10 of the operand) |
| 5 | the already-moved-out diagnostic carries the **call's** location (the `(`) | `09_move_twice.ivy` (column 9 of the call) |
| 6 | the use-after-move is reported **before** the already-moved-out | `09_move_twice.ivy` (the order of the two lines) |
| 7 | the assignment LHS is bypassed **at any depth**, including inside a nested call | `14` - `17_assign_lhs_bypass_*.ivy` |
| 8 | the assignment RHS is **never** bypassed | `08_move_in_assign_rhs.ivy`, `18_assign_rhs_is_checked.ivy` |
| 9 | only a plain `=` with an `IdentRef` LHS re-initialises | `10_reassign_reinitializes.ivy`, `11_compound_assign_no_reinit.ivy` |
| 10 | the builtin branch fires **before** any callee resolution | `30` - `33_*move*.ivy` |
| 11 | the `unsafe` block does **not** bypass anything in the HIR stage | `45` - `47_unsafe_*.ivy` |

### 10.2 The failure modes that were considered

| # | failure mode | how it is handled | evidence |
|---|---|---|---|
| 1 | the LHS bypass flag leaks out of the LHS build and silently disables the check | the flag is set and cleared **around** the LHS build only, exactly like the oracle (`:3603-3605`); the RHS is built after the flag is cleared | `08`, `18` |
| 2 | a **nested assignment** inside the LHS clears the flag early (the oracle's own asymmetry) | the port reproduces the oracle's behaviour verbatim (the flag is cleared, not restored) - it is a *semantic* property of the oracle, not a port bug | the phase-4 probes `p38` / `p49` / `p50` (byte-identical) |
| 3 | a scope is popped and its symbol slot is reused - the stale state would be inherited | `declare()` writes `smv_[s_n_ - 1] = 0` for every new symbol, so a reused slot always starts `Initialized` | `29_move_after_block_shadow_reuse.ivy` (the phase-4 probe) |
| 4 | the arity test uses the syntactic count instead of the built count | the port uses the built count `e.an == 1`, like the oracle's `call.args.size()` (after the pack-expansion splicing) | `19_move_zero_args.ivy`, `20_move_two_args.ivy` |
| 5 | an **undeclared** identifier is accepted as the operand | the operand test is on the HIR node kind, so an undeclared identifier is still an `IdentRef` - exactly like the oracle | `38_move_before_decl.ivy` |
| 6 | an enum constant / a function name / `this` is accepted as the operand | the node kind is not an `IdentRef` (an enum constant is folded to a literal) or the operand is not a variable - the branch does not fire or marks nothing | `27`, `28`, `29` |
| 7 | the `unsafe` bypass is implemented in the port although the oracle's HIR builder has none | the port's `unsafe_depth_` is **not** consulted by the move check or by the marking | `45` - `47_unsafe_*.ivy`; §7.4 |
| 8 | the port emits a MIR-stage diagnostic | the port has no MIR builder; the corpus is MIR-clean (0 dirty files) | §12.3 |
| 9 | a template instantiation clobbers the caller's move states | the port saves and restores the symbol stack (`s_n_` / `sc_n_`); the move states are indexed by the symbol index, so they follow it | `53_template_param_moved.ivy`, the phase-4 probe `p54` |

---

## 12. Phase 3 (the corpus + the bookkeeping)

**Milestone:** H4.6, phase 3/4 (the official corpus + the bookkeeping).
**HEAD at the time of writing:** `75332c9` (unchanged - no commit was made).
**Production code changed by this phase:** **none** (the port's move
semantics are untouched; `selfhost/hir/hir_builder.ivy` is exactly as
phase 2 left it).
**Scratch dir:** `C:\Users\ASUS\AppData\Local\Temp\opencode\h46f3\`.

### 12.1 The corpus inventory

`selfhost/corpus/hir/move/` - **60 files**, grouped by topic by a global
two-digit prefix (the files of a topic occupy a contiguous range).  Every
file is self-contained, deterministic, free of any temp path, and carries
the phase-1 rule name in its first-line comment, so the mapping stays
traceable.

| # | file | the rule it covers | the phase-1 fixture |
|---|---|---|---|
| 1 | `01_move_local_read.ivy` | R2 - the basic use-after-move | `m01_move_local_read` |
| 2 | `02_move_in_call_arg.ivy` | R2 - a moved-out variable as a call argument | `m26_move_call_arg` |
| 3 | `03_move_in_return.ivy` | R2 - `return x;` after the move | `m27_move_return` |
| 4 | `04_move_in_ternary.ivy` | R2 - the use-after-move in a ternary | `m40_move_ternary` |
| 5 | `05_move_in_while_cond.ivy` | R2 - the use-after-move in a `while` condition | `m73_move_while_cond` |
| 6 | `06_move_in_prefix_inc.ivy` | R2 - `x++` after the move | `m76_move_prefix_inc` |
| 7 | `07_move_in_addr_of.ivy` | R2 - the use-after-move inside `&x` | `m25_move_addr_of` |
| 8 | `08_move_in_assign_rhs.ivy` | R5 - the right-hand side is checked | `m81_move_assign_from_moved_in_rhs` |
| 9 | `09_move_twice.ivy` | R3 - the already-moved-out (+ the order of the two diagnostics) | `m02_move_twice` |
| 10 | `10_reassign_reinitializes.ivy` | R6 - a plain `=` re-initialises | `m03_move_reassign_then_read` |
| 11 | `11_compound_assign_no_reinit.ivy` | R7 - a compound assignment does not re-initialise | `m04_move_compound_assign` |
| 12 | `12_reinit_then_move_again.ivy` | R6 - move → `=` → move again | `m93_move_after_reassign_then_move` |
| 13 | `13_param_reassign_reinitializes.ivy` | R6 - a parameter re-assigned after the move | `m57_move_param_reassign` |
| 14 | `14_assign_lhs_bypass.ivy` | R4 - the assignment left-hand side is bypassed | `m24_move_assign_lhs` |
| 15 | `15_assign_lhs_bypass_index.ivy` | R4 - `a[i] = 1` | `e02_index_lhs_bypass` |
| 16 | `16_assign_lhs_bypass_member.ivy` | R4 - `s.v = 1` | `e04_member_lhs_bypass` |
| 17 | `17_assign_lhs_bypass_nested_call.ivy` | R4 - the bypass leaks into a nested call | `q04_nested_call_in_lhs` |
| 18 | `18_assign_rhs_is_checked.ivy` | R5 + R3 - the right-hand side of an assignment is checked: a `move` in the right-hand side reports the use-after-move **and** the already-moved-out | `m70_move_assign_chain` (the plain `y = x;` form, kept by the corpus file 08), `m42_move_assign_from_move` |
| 19 | `19_move_zero_args.ivy` | R9 - the arity (0 arguments) | `m11_move_zero_args` |
| 20 | `20_move_two_args.ivy` | R9 - the arity (2 arguments) | `m12_move_two_args` |
| 21 | `21_move_qualified.ivy` | R8 - a qualified callee | `m14_move_qualified` |
| 22 | `22_move_literal.ivy` | R10 - a literal operand | `m15_move_literal` |
| 23 | `23_move_call_result.ivy` | R10 - a call-result operand | `m16_move_call_result` |
| 24 | `24_move_field.ivy` | R10 - a member operand | `m17_move_field` |
| 25 | `25_move_index.ivy` | R10 - an index operand | `m18_move_index` |
| 26 | `26_move_nested_move.ivy` | R10 - a nested-call operand | `m41_move_nested_move` |
| 27 | `27_move_enum_const.ivy` | R10 - an enum-constant operand (folded to a literal) | `f01_move_enum_const` |
| 28 | `28_move_function_name.ivy` | R10 - a function-name operand | `f02_move_function_name` |
| 29 | `29_move_this.ivy` | R10 - `this` is not an `IdentRef` | `m38_move_this` |
| 30 | `30_builtin_beats_user_function.ivy` | R1 + R11 - a user-defined `move` does not shadow the builtin | `e01_user_defined_move` |
| 31 | `31_user_function_used_for_literal.ivy` | R11 - the user function **is** used for a literal | `e08_user_move_non_ident` |
| 32 | `32_builtin_beats_local_named_move.ivy` | R11 - a local variable named `move` | `q02_local_named_move` |
| 33 | `33_template_named_move.ivy` | R11 - a template named `move` | `q06_template_named_move` |
| 34 | `34_move_in_nested_block.ivy` | R12 - the move is visible in the nested block | `m08_move_nested_block` |
| 35 | `35_move_after_nested_block.ivy` | R12 - the move is visible after the block | `m21_move_after_block` |
| 36 | `36_shadow_inner.ivy` | R12 - the inner binding has its own state | `m09_move_shadow_inner` |
| 37 | `37_shadow_param_by_local.ivy` | R12 - a parameter shadowed by a body local | `f03_move_param_shadow_body` |
| 38 | `38_move_before_decl.ivy` | R10 + R12 - the operand is not a variable yet | `m10_move_before_decl` |
| 39 | `39_for_var_out_of_scope.ivy` | R12 - the for variable is gone after the loop | `m89_move_for_var_after_loop` |
| 40 | `40_move_param.ivy` | R13 - a by-value parameter | `m05_move_param` |
| 41 | `41_move_const_param.ivy` | R13 - a `const` parameter | `e18_move_of_const_param` |
| 42 | `42_move_ref_param.ivy` | R13 - a reference parameter | `m44_move_ref_param` |
| 43 | `43_move_const_local.ivy` | R13 - a `const` local | `m06_move_const` |
| 44 | `44_move_struct_value.ivy` | R13 - a struct value | `m31_move_struct_value` |
| 45 | `45_unsafe_no_bypass.ivy` | R14 - the `unsafe` block does not bypass the check | `m36_move_in_unsafe` |
| 46 | `46_unsafe_no_bypass_then_read.ivy` | R14 - the read is outside the `unsafe` block | `m49_move_in_unsafe_then_read` |
| 47 | `47_unsafe_assign_reinitializes.ivy` | R14 + R6 - the re-assignment inside `unsafe` | `m102_move_unsafe_bypass_spec` |
| 48 | `48_move_in_if_branch.ivy` | R15 - the move in the if-branch | `f06_move_in_if_branch` |
| 49 | `49_move_in_for_body.ivy` | R15 - the move in the for body | `m39_move_in_for_body_after_loop` |
| 50 | `50_move_in_while_body.ivy` | R15 - the move in the while body | `e11_move_in_while_body` |
| 51 | `51_move_in_switch_case.ivy` | R15 - the move in a switch case | `m30_move_in_switch` |
| 52 | `52_move_for_var_in_body.ivy` | R15 - the for-loop variable is a binding | `m07_move_for_var` |
| 53 | `53_template_param_moved.ivy` | R16 - a template parameter | `m32_move_template` |
| 54 | `54_lifetime_param_moved.ivy` | R16 - a lifetime parameter | `m33_move_lifetime_param` |
| 55 | `55_constexpr_call_arg.ivy` | R16 - a `constexpr` call argument | `e14_move_in_constexpr_call` |
| 56 | `56_constexpr_body_move.ivy` | R16 - a `constexpr` body | `m34_move_constexpr` |
| 57 | `57_move_local_in_method.ivy` | R17 - a method-local | `m86_move_local_in_method` |
| 58 | `58_move_this_then_read_field.ivy` | R17 - `move(this)` + a field read | `m99_move_this_field_read` |
| 59 | `59_move_dump_baseline.ivy` | R18 - the clean dump shape | `m47_move_ok_baseline` |
| 60 | `60_move_as_call_argument.ivy` | R18 - the builtin in an argument position | `m69_move_in_callarg_position` |

### 12.2 The excluded candidates

The phase-1 corpus had **140** fixtures; **60** are in the corpus above and
**80** are excluded.  The exclusions, with the reason and the **oracle
evidence** (measured again in this phase - the oracle's stdout and stderr
were captured separately for each fixture):

#### (a) A MIR-stage diagnostic - never matchable (4 files)

`ivy --hir` runs the **whole** pipeline (`src/app/main.cpp:1303-1321`), so
these files print the HIR dump **and then** a MIR diagnostic (exit 1).  The
selfhost driver stops after the HIR stage (`selfhost/hir/hirdrive.ivy:503-535`),
so it can never reproduce the extra line.  The evidence is the oracle's
**stdout** (the dump, non-empty) together with its **stderr** (the
diagnostic, non-empty) - a file with a dump **and** a diagnostic is
MIR-dirty by construction:

| fixture | the oracle's stdout | the oracle's stderr (after the dump) |
|---|---|---|
| `m37_move_lambda_capture.ivy` | 440 bytes (the HIR dump) | `4:14: error: use of moved-out variable 'x'` (the MIR builder re-builds the capture's initialiser, `src/mir/mir_builder.cpp:676-678`) |
| `m51_move_mir_only_ret_ref.ivy` | 124 bytes (the HIR dump) | `3:5: error: returning a reference to a local variable (dangling reference)` |
| `m83_move_lambda_no_return.ivy` | 445 bytes (the HIR dump) | `4:14: error: use of moved-out variable 'x'` |
| `m84_move_lambda_explicit_ret.ivy` | 438 bytes (the HIR dump) | `4:14: error: use of moved-out variable 'x'` |

(Three of the four also depend on the **lambda** gap - see (c).)

#### (b) A pre-existing port gap - the struct construction (4 files)

`S s(1);` - the direct initialisation of a struct with a user-declared
constructor - is not supported by the port's HIR builder.  The port reports
`undeclared identifier 'S'` where the oracle builds the constructor.  The
**move** tracking itself is correct in both: the port emits the use-after-move
at exactly the oracle's location, but the surrounding diagnostics differ.

| fixture | the oracle | the port |
|---|---|---|
| `m35_move_struct_ctor.ivy` | `9:15: error: use of moved-out variable 's'` | `7:9: error: undeclared identifier 'S'` + `7:9: error: call to undeclared function 'S'` + `7:5: error: cannot initialize variable 's' of type 'S' with a value of type 'int'` + `9:15: error: use of moved-out variable 's'` |
| `e09_move_struct_with_dtor.ivy` | `11:15: error: use of moved-out variable 'r'` | the same three extra diagnostics at `9:11` + `11:15: error: use of moved-out variable 'r'` |
| `f10_move_struct_ctor_then_use.ivy` | a dump (450 bytes, no diagnostic) | `7:9`/`7:5` and `9:9`/`9:5` "undeclared identifier 'S'" etc. |
| `m96_move_struct_ctor_param.ivy` | a dump (450 bytes, no diagnostic) | the same as `f10` |

This is the **RAII/DTOR** milestone's gap (`PLAN.md`: "struct construction
(can khoi tao mau `Res` co destructor) - canh RAII/DTOR milestone"), **not**
a move gap.

#### (c) A pre-existing port gap - the lambda syntax (2 files)

The port's **parser** has no lambda syntax at all.

| fixture | the oracle | the port |
|---|---|---|
| `e07_lambda_body_use.ivy` | a dump (355 bytes, no diagnostic) | `4:14: error: unexpected token in expression` + `4:15: error: expected ';' after declaration` + ... |
| `m61_move_lambda_capture_ok.ivy` | a dump (384 bytes, no diagnostic) | `3:14: error: unexpected token in expression` + ... |

#### (d) A pre-existing port gap - the C-style cast (1 file)

| fixture | the oracle | the port |
|---|---|---|
| `e03_deref_lhs_bypass.ivy` | `2:5: error: raw pointer variable declaration requires an unsafe block` + `2:16: error: C-style cast requires an unsafe block` | `2:17: error: unexpected token in expression` + `2:23: ...` + `2:24: error: expected ')' to close parenthesized expression` |

The port's parser does not implement the C-style cast `(int32*)0`.  The
**move** part of the fixture is unaffected - the deref left-hand side is
bypassed like every other left-hand side (`inAssignLhs_` is set around the
*whole* left-hand side expression, `hir_builder.cpp:3603-3605`), and the
oracle's own diagnostics for this file are the two `unsafe` ones, not a move
one.  The difference is entirely the parser's missing C-style cast.

#### (e) A pre-existing port gap - the global variable (2 files)

The port's parser has **no global variables**; the file is a parse error in
**both** (the oracle included - the global syntax is not in Ivy 0.5 yet),
so the fixture does not exercise any move rule.

| fixture | the oracle's diagnostics (the port produces the same ones) |
|---|---|
| `m20_move_global.ivy` | `1:11: error: expected '(' after function name 'g_x'` + `1:13` / `1:14: error: expected a type (void, bool, int8, ...)` + `1:13` / `1:14: error: expected ',' between parameters` + `2:7: error: expected ',' between parameters` |
| `m97_move_global_var_parse.ivy` | the same five diagnostics |

#### (f) A pre-existing port gap - the multi-declarator statement (2 files)

| fixture | the oracle's diagnostics (the port produces the same ones) |
|---|---|
| `m45_move_multi_decl.ivy` | `3:10: error: undeclared identifier 'x'` + `4:15: error: undeclared identifier 'x'` |
| `m64_move_multidecl_only.ivy` | `3:15: error: undeclared identifier 'x'` |

The statement `int32 x = 1, y = 2;` is not supported by the port's parser
(it is a parser/aggregate case, H3.3/H3.4), so the file never reaches the
move tracking.

#### (g) A pre-existing port gap - the namespace qualifier (1 file)

| fixture | the oracle's diagnostics (the port produces the same ones) |
|---|---|
| `q01_qualified_global_move.ivy` | `3:5: error: unexpected token in expression` + `3:7: error: expected ';' after expression` |

`::move(x)` - the **global** qualifier - is not in Ivy 0.5.  The
namespace-*qualified* form that **is** in Ivy 0.5 (`ivy::move(x)`) is in the
corpus as `21_move_qualified.ivy`.

#### (h) Not a move rule (5 files)

| fixture | the oracle's diagnostics (the port produces the same ones) | why it is out |
|---|---|---|
| `m85_move_in_method.ivy` | `3:17: error: expected '{' for method body or ';' for a declaration` + `3:26: error: expected field or method name` + `6:1: error: expected ';' after struct definition` + `6:2: error: expected a type (void, bool, int8, ...)` | the **trailing return type of a method** (`int32 get() -> int32 {`) is not legal Ivy; the file is a parse error in both, not a move rule |
| `m43_move_redeclare_same_scope.ivy` | `4:5: error: redefinition of variable 'x'` | an H4.1 (name resolution) rule, not a move rule |
| `m92_move_assign_to_const.ivy` | `4:5: error: cannot assign to const variable` | an H4.2 (type checking) rule, not a move rule |
| `m91_move_call_builtin.ivy` | `4:5: error: undeclared identifier 'printf'` | an H4.1 rule (`printf` is a *runtime* builtin, `isBuiltinFn` `:88-91`, but it is not registered in the HIR function table) - not a move rule |
| `m52_move_mir_only_borrow.ivy` | **no diagnostic at all** (exit 0, a 122-byte dump) | a **borrow** fixture (a reference bound to a borrowed value) - it produces no HIR diagnostic at all, so it exercises no move rule |

#### (i) Near-duplicates of a corpus file (59 files)

The remaining 59 excluded fixtures are the phase-1 **controls** - the
cases that must **not** change - and they cover a rule that the corpus file
named in the last column already pins (or a rule that the corpus pins as a
*control*: a case where the move state must **not** leak into an unrelated
expression).  The mapping:

| phase-1 fixture | the corpus file that covers the same rule |
|---|---|
| `e05_move_in_for_init` | `22_move_literal` (the operand is a literal; the for-init position is not a move rule) |
| `e06_move_in_switch_case` | `51_move_in_switch_case` |
| `e10_sibling_scopes_same_name` | `36_shadow_inner` |
| `e12_move_then_use_in_condition` | `05_move_in_while_cond` |
| `e13_move_of_param_in_method` | `40_move_param` |
| `e15_move_of_array_var` | `14_assign_lhs_bypass` (the dump shape of a `move()` that produces no diagnostic) |
| `e16_move_lhs_reinit_then_move` | `12_reinit_then_move_again` |
| `e17_move_in_nested_block_shadow` | `34_move_in_nested_block` |
| `e19_move_call_result_move` | `23_move_call_result` |
| `e20_move_in_unsafe_then_use` | `46_unsafe_no_bypass_then_read` |
| `f04_move_this_read` | `58_move_this_then_read_field` |
| `f05_move_in_nested_template` | `53_template_param_moved` |
| `f07_move_in_else_branch` | `48_move_in_if_branch` |
| `f08_move_of_var_in_unsafe_only` | `45_unsafe_no_bypass` |
| `f09_move_assign_lhs_in_unsafe` | `47_unsafe_assign_reinitializes` |
| `m100_move_qualified_ivy_move` | `21_move_qualified` |
| `m101_move_after_nested_move_ok` | `10_reassign_reinitializes` |
| `m13_move_no_call` | `19_move_zero_args` (the callee is not a call: the ordinary resolution reports `undeclared identifier 'move'`) |
| `m19_move_binary` | `22_move_literal` (the operand is not an identifier) |
| `m22_move_sibling_block` | `35_move_after_nested_block` |
| `m23_move_read_expr` | `01_move_local_read` |
| `m28_move_sizeof_type` | `14_assign_lhs_bypass` (a control: the move state must not leak into a type expression - `sizeof(int32)` is not a variable read) |
| `m29_move_in_while` | `50_move_in_while_body` |
| `m42_move_assign_from_move` | `09_move_twice` |
| `m46_move_switch_case_block` | `51_move_in_switch_case` |
| `m48_move_dump_only` | `14_assign_lhs_bypass` (the two files are identical) |
| `m50_move_method_this_field` | `29_move_this` |
| `m53_move_in_do_while` | `50_move_in_while_body` |
| `m54_move_initializer` | `22_move_literal` |
| `m55_move_conditional_arg` | `22_move_literal` (the operand is not an identifier) |
| `m56_move_in_exprstmt` | `14_assign_lhs_bypass` (the dump shape of a `move()` as an expression statement) |
| `m58_move_scope_after_loop` | `39_for_var_out_of_scope` |
| `m59_move_struct_ptr` | `16_assign_lhs_bypass_member` |
| `m60_move_array_elem_write` | `25_move_index` |
| `m62_move_template_move_ok` | `53_template_param_moved` (a template that does not move anything) |
| `m63_move_lifetime_ok` | `54_lifetime_param_moved` (a lifetime that is not moved) |
| `m65_move_this_in_method` | `29_move_this` |
| `m66_move_in_unsafe_only` | `45_unsafe_no_bypass` |
| `m67_move_nested_block_only` | `34_move_in_nested_block` |
| `m68_move_exprstmt_only` | `14_assign_lhs_bypass` (the dump shape) |
| `m71_move_ternary_assign` | `04_move_in_ternary` |
| `m72_move_in_if_cond` | `05_move_in_while_cond` |
| `m74_move_in_switch_cond` | `05_move_in_while_cond` |
| `m75_move_in_for_cond` | `05_move_in_while_cond` |
| `m77_move_postfix_dec` | `06_move_in_prefix_inc` |
| `m78_move_deref` | `07_move_in_addr_of` (the diagnostic is on the `&x` of the same file) |
| `m79_move_struct_field_write` | `16_assign_lhs_bypass_member` |
| `m80_move_array_write` | `15_assign_lhs_bypass_index` |
| `m82_move_move_of_param_reexpr` | `13_param_reassign_reinitializes` |
| `m87_move_outer_scope_from_inner` | `36_shadow_inner` |
| `m88_move_before_decl_outer` | `38_move_before_decl` |
| `m90_move_in_constexpr_body` | `56_constexpr_body_move` |
| `m94_move_ref_param_assign` | `13_param_reassign_reinitializes` (the reference-parameter variant) |
| `m95_move_in_nested_fn_call` | `02_move_in_call_arg` |
| `m98_move_method_this` | `29_move_this` |
| `q05_move_param_then_read` | `40_move_param` |
| `q07_move_in_lhs_of_ternary` | `04_move_in_ternary` |
| `q08_move_reinit_by_compound` | `11_compound_assign_no_reinit` |
| `q09_move_again_after_reinit` | `12_reinit_then_move_again` |

### 12.3 The verification

**The new suite:**

```
PS> powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/hir/move -Kinds all -Oracle --hir `
     -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder `
     -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive
[1/4] building selfhost modules ...
[2/4] collecting corpus ...
      files: 60
[3/4] running C++ oracle + Ivy port ...
[4/4] comparing (kinds: all) ...
PASS: identical (filtered) output on 60 files
```

**The MIR-cleanliness of the corpus** was measured separately (the oracle's
stdout and stderr captured per file): **60 MIR-clean, 0 MIR-dirty** - no
corpus file prints a dump **and** a diagnostic.

**The 25 existing suites** (run **sequentially** - the lexer and the
preprocessor suites share one temporary directory):

| suite | files | verdict | suite | files | verdict |
|---|---|---|---|---|---|
| `lexer/ident` | 10 | **PASS** | `parser/aggregate` | 14 | **PASS** |
| `lexer/num` | 8 | **PASS** | `parser/template` | 16 | **PASS** |
| `lexer/str` | 7 | **PASS** | `parser/module` | 16 | **PASS** |
| `lexer/op` | 5 | **PASS** | `parser/err` | 19 | **PASS** |
| `lexer/trivia` | 7 | **PASS** | `hir/name` | 27 | **PASS** |
| `lexer/err` | 9 | **PASS** | `hir/type` | 120 | **PASS** |
| `pp/define_obj` | 10 | **PASS** | `hir/constexpr` | 70 | **PASS** |
| `pp/define_fn` | 12 | **PASS** | `hir/template` | 99 | **PASS** |
| `pp/cond` | 16 | **PASS** | `hir/borrow` | 45 | **PASS** |
| `pp/expand` | 16 | **PASS** | | | |
| `pp/predef` | 20 | **PASS** | | | |
| `pp/line` | 22 | **PASS** | | | |
| `pp/no_include` | 9 | **PASS** | | | |
| `parser/decl` | 15 | **PASS** | | | |
| `parser/expr` | 13 | **PASS** | | | |
| `parser/stmt` | 15 | **PASS** | | | |

**25 suites, 620 files, 25 PASS / 0 FAIL.**  With the new suite:
**26 suites, 680 files, all byte-identical.**

**The phase-1 fixtures were re-verified from scratch** in this phase (a
freshly built port in `h46f3\port\`, built from *copies* of
`selfhost/parsing/*.ivy` and `selfhost/hir/*.ivy`, so that no build product
was written into the tree): **129 of the 140** fixtures are byte-identical,
**11** differ - the 4 MIR-dirty ones (§12.2a), the 4 struct-construction ones
(§12.2b), the 2 lambda ones (§12.2c) and the 1 C-style-cast one (§12.2d).
This confirms §11.8's numbers independently.

### 12.4 The bookkeeping record

* **Created**: `selfhost/corpus/hir/move/` - 60 files (the corpus).
* **Appended**: `selfhost/hir/MOVE_NOTES.md` - §1, §2, §3, §4, §5, §7, §8
  and §12 (this block).  §0, §6 and §11 are **untouched**.
* **Modified (comment only)**: `selfhost/difftest.ps1` - the **header
  comment block** gained the H4.6 invocation (the HIR feature list).  **No
  behaviour change**: the parameters, the logic and the output of the script
  are untouched.
* **Modified**: `PLAN.md` -
  * the H4.6 row of the mapping table (§5) is now
    `| H4.6 | Move / use-after-move | `hir_builder.cpp` | `hir/hir_builder.ivy` | `corpus/hir/move` | ✅ byte-identical 60 files |`;
  * a new **H4.6 note** in the "Hiện trạng" section describes the whole
    feature set (the `move()` builtin recognition rules, the move-state
    store, the use-after-move check, the assignment-LHS bypass, the
    re-initialisation, the two diagnostics, the builtin-vs-user-defined
    precedence), the corpus, the invocation, the MIR-cleanliness constraint
    and the deferred scope;
  * the "Còn lại (để milestone sau)" lists of the H4.2 and the H4.3 notes
    no longer list the **move tracking** as "not ported"; they now name the
    deferred items instead (the MIR move tracking → H5.2, the `unsafe`
    bypass, the lambdas, the struct constructors → the RAII/DTOR milestone).
* **Unchanged**: `src/**` (the oracle was **not** rebuilt - `build\Release\ivy.exe`
  keeps its mtime `2026-10-21 16:41:44`), `selfhost/parsing/**`,
  `selfhost/hir/hir_builder.ivy` (phase 2's implementation is untouched),
  the other corpora, `.gitignore`, `examples/`, `opencode.json`.
* **No commit** was made.

### 12.5 Phase 4 (the independent verification) - the record

**Milestone:** H4.6, phase 4/4 (the independent verification).
**HEAD at the time of writing:** `75332c9` (unchanged - no commit was made).

The verification re-measured **everything** from scratch (the corpus, the
phase-1 fixtures, the 25 other suites and an independent probe set) and
found **no defect in the port's move semantics**.  Two **bookkeeping**
defects were found and fixed:

* **the corpus file `18_assign_rhs_is_checked.ivy` was a verbatim copy of
  `08_move_in_assign_rhs.ivy`** (the two phase-1 fixtures it came from,
  `m70_move_assign_chain` and `m81_move_assign_from_moved_in_rhs`, are
  themselves byte-identical programs).  A byte-hash of the corpus does not
  reveal it - the two files differ only in their first-line comment - but
  the *programs* are the same, so the file added no differential coverage.
  The file was **replaced** (not deleted) by a program that covers the same
  rule in a combination no other corpus file covers: the right-hand side of
  an assignment contains a `move` of an **already moved-out** variable, so
  the file now reports **both** diagnostics (the use-after-move at the
  operand and the already-moved-out at the call).  The oracle's output and
  the port's output are byte-identical on it.  §12.1 (row 18) and §12.2(i)
  were updated accordingly.
* **the file:line source map of §11.3 was off by one** for the oracle's
  `declare()` call sites (it cited `:933` / `:1297` / ... instead of
  `:934` / `:1298` / ...).  The map of §2.3 was already correct; §11.3 was
  corrected.  No other section of the file was changed, and no section was
  removed, truncated or duplicated.  §0 gained one paragraph that records
  this phase's corrections; §6 (the phase-1 inventory) is untouched.

**§9 and §10 were re-written** in this phase (see their head notes): the
original phase-1 §9 and §10 were lost with the rest of the phase-1
sections when the cancelled phase-2 attempt overwrote this file (§0), so
the file did not contain the sections §0-§12.  The re-written sections
carry the verification method (§9) and the invariants / failure modes
(§10); they are clearly marked as a phase-4 re-write, not as a recovery of
the lost text.

**The re-measured numbers** (all of them reproduce the phase-2/phase-3
figures exactly):

| measurement | phase-2/3 figure | the phase-4 re-measurement |
|---|---|---|
| the corpus suite | 60/60 byte-identical | 60/60 byte-identical |
| the MIR-cleanliness of the corpus | 60 clean / 0 dirty | 0 dirty (14 files reach the MIR stage and are clean, 46 fail in the HIR stage and never reach it) |
| the 25 other suites | 620 files, 25 PASS | 620 files, 25 PASS |
| the 26 suites together | 680 files, all byte-identical | 680 files, all byte-identical |
| the phase-1 HIR class (83) | 81 byte-identical, 2 differ | 81 byte-identical, 2 differ (`m35`, `e09` - the struct-constructor gap) |
| the phase-1 CTRL class (39) | 39 byte-identical | 39 byte-identical |
| the phase-1 MIR class (4) | 0 byte-identical (out of scope) | 0 (unchanged from the HEAD build) |
| the phase-1 OUT class (14) | 9 byte-identical | 9 (unchanged from the HEAD build) |
| the phase-1 fixtures together (140) | 129 byte-identical, 11 differ | 129 byte-identical, 11 differ - the 11 are the **same** files, and each is byte-identical in the HEAD build and in the current build (a pre-existing gap, never a move-tracking regression) |
| the independent probes | - | 79 written, 77 byte-identical, 1 differ (the pre-existing struct-constructor gap), 1 not answerable (the **oracle crashes** on `&<struct>` - a pre-existing oracle bug, reproducible without any `move` in the file) |

**The `unsafe` bypass** was verified to be **absent** from the port's HIR
move tracking, by fixture: `45_unsafe_no_bypass.ivy` (the read is inside
the block and is reported), `46_unsafe_no_bypass_then_read.ivy` (the read
is outside the block and is reported) and
`47_unsafe_assign_reinitializes.ivy` (the re-assignment inside the block
re-initialises) - the oracle's HIR builder has no `unsafeDepth_` guard
(only the MIR builder has one, `mir_builder.cpp:476` / `:568`).

**No production code was changed in this phase**: the port's move
semantics are byte-identical to what phase 2 left.
