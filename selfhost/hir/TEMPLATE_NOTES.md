# H4.4 — Template instantiation: reconnaissance notes

**Milestone:** H4.4 (Template instantiation) — Ivy self-host port.
**Author:** RECONNAISSANCE agent (phase 1 of 6).
**Date:** 2026-10-21.
**Oracle:** `build\Release\ivy.exe` (2026-10-21 16:41:44). **Never rebuilt.** All statements below
were verified by running that binary (`ivy --hir <file>`) on throw-away fixtures in
`C:\Users\ASUS\AppData\Local\Temp\opencode\h44recon\`.
**Corpus:** `C:\Users\ASUS\AppData\Local\Temp\opencode\h44recon\corpus\` (12 canonical
fixtures `C01..C12`) + ~110 scratch fixtures in the parent directory.
Captured oracle output: `C:\Users\ASUS\AppData\Local\Temp\opencode\h44recon\corpus_oracle.txt`
(regenerate with `run_corpus.ps1` in the same directory).

**Status of this file:** specification only. No production code was changed.

---

## 0. Ground rules that follow from the oracle's code (read this first)

The C++ oracle is the specification. The three code regions that define all of H4.4:

| Concern | Oracle source |
|---|---|
| Function-template registry | `src/hir/hir_builder.cpp:825` (`templates_[af.name] = &af`) |
| Struct-template registry | `src/hir/hir_builder.cpp:553` (`structTemplates_[sd.name] = &sd`) |
| Concept registry | `src/hir/hir_builder.cpp:4343` (`concepts_[cd.name] = &cd`) |
| `lookupTemplate` | `src/hir/hir_builder.cpp:4956` |
| `substituteType` | `src/hir/hir_builder.cpp:4971` |
| `instantiateTemplate` | `src/hir/hir_builder.cpp:4995` |
| `deduceTemplateArgs` | `src/hir/hir_builder.cpp:5193` |
| `lookupStructTemplate` | `src/hir/hir_builder.cpp:5381` |
| `mangleStructSpec` | `src/hir/hir_builder.cpp:5396` |
| `instantiateStructTemplate` | `src/hir/hir_builder.cpp:5414` |
| `resolveTemplateStructType` | `src/hir/hir_builder.cpp:5590` |
| `checkConstraint` | `src/hir/hir_builder.cpp:4835` |
| Template-id call path | `src/hir/hir_builder.cpp:3043-3096` |
| Implicit-deduction call path | `src/hir/hir_builder.cpp:3120-3145` |
| HIR dump | `src/app/main.cpp:577-604` |
| Parser: template params | `src/parsing/parser.cpp:1647` |
| Parser: template args | `src/parsing/parser.cpp:1702` |
| Parser: template decl | `src/parsing/parser.cpp:1723` |
| Parser: concept decl | `src/parsing/parser.cpp:1771` |
| Parser: template-id call | `src/parsing/parser.cpp:2779-2831` |
| Parser: template-id *type* | `src/parsing/parser.cpp:388-390` and `:430-432` |
| Lexer: `>>` is ONE token | `src/parsing/lexer.cpp:115` (`{">>", TokenKind::Shr}`) |

**Re-verified (H4 follow-up, 2026-10-22):** the `>>` citation above is 1-based and
correct — `src/parsing/lexer.cpp:115` is `{">>", TokenKind::Shr}` and `:116` is
`{"+=", TokenKind::PlusAssign}` (the file was created at commit `71ff6f0` and
changed in three later commits; the `>>` entry moved between lines 77 and 115,
and `:115` is correct at the current HEAD).  The `+=` entry is `:116`, not `:115`.

**The oracle's HIR dump only prints `tu.functions`** (`main.cpp:578`). Structs and enums are
*never* printed. Therefore a class-template specialization is visible in the dump **only
through its methods**; the struct itself is invisible.

**Any diagnostic suppresses the whole dump** (`main.cpp:1307-1313`): diagnostics are printed to
stderr, `failed = true`, `return 1` — the HIR dump is *not* printed. A corpus file that
produces errors prints diagnostics only.

---

## 1. The 15 rules that matter (TL;DR)

1. **Templates are never dumped.** A template *declaration* (prototype or definition) produces
   **no** `function` entry. Only *instantiations* are dumped. (C01, C02, C08)
2. **Instantiated function name = `mangled`** = `<template name> "<" args ">"`, e.g.
   `identity<int32_t>`, `count<int32_t,int32_t>`, `ns::id<int32_t>`, `count_args<>`.
   (C01, C04, p72, examples/test_variadic.ivy)
3. **Arg mangling** = `base` + `" unsigned"`? + `" const"`? + `"*"`×ptrDepth. No spaces after
   commas. (C04, p81)
4. **Dump order:** all ordinary functions first (source order), then instantiations in
   *first-instantiation* order. A specialization used twice is emitted **once**. (C09)
5. **Struct-template specialization** name = same mangling; its methods are dumped as
   `<SpecName>::<method>` with `this` typed `SpecName&`. Constructors become
   `<SpecName>::<OriginalStructName>`. (C05, p34, p33)
6. **A template body is only type-checked when the template is instantiated.** Errors in an
   uninstantiated body are silently ignored. (C08)
7. **Only the *signature* is substituted** (return type, parameter types). **Local variable
   declarations of type `T` are NOT substituted** → they keep the literal name `T` and produce
   bogus diagnostics. (C10)
8. **The type mapping is NOT propagated to nested calls.** A template calling another template
   with an explicit argument `id<T>(x)` instantiates `id<T>` (unsusbstituted) → errors.
   (C11) — but it *does* work inside a **struct** template's method, because struct
   instantiation sets `currentTypeMapping_` (p76).
9. **Deduction**: the *only* deduction is "param type is exactly a type parameter" →
   `T = arg type`. Reference-ness is stripped. **Array/pointer decay is not modelled** except
   that a `T*` param deduces the pointee. Deduction of a *literal* yields type `int`
   (NOT `int32_t`) → `identity(1)` and `identity<int32>(1)` are **different specializations**.
   (C03, p42, p43, examples/test_template_deduction.ivy)
10. **Concepts are checked at instantiation time only** and are *permissive*: an unknown
    concept name is assumed satisfied; `bool`/`int`/… count as arithmetic and satisfy
    `a + b`; a struct needs a real `operator+` overload. (C06, C07, p21, p09)
11. **Too many explicit template arguments are silently accepted**; **too few** leave the
    unmapped parameter type as the literal parameter name (`U`) and produce a cascade of
    type errors. (p11, p14, t12)
12. **Non-type template parameters are unusable**: the parser accepts `template <int32 N>` but
    the call site cannot pass a value (`f<3>(x)` is a parse error) and `N` is never bound
    (using `N` in the body ⇒ `undeclared identifier 'N'`). (p04, p71, t03, t04)
13. **Nested template-ids do not parse** (`Box<Box<int32>>`): the lexer makes `>>` a single
    token, so the parser expects `>` and fails. (t3, t4, p38)
14. **Struct-template specializations lose their base classes** (the cloned struct never
    copies `bases`) and **`sizeof`/`alignof` of a template-id type is wrong** (falls back to
    4, it does not resolve the specialization). (p49, p48, p36)
15. **Member function templates, explicit specializations (`template <> ...`), trailing
    `requires` clauses, `static` members and non-type template arguments are all rejected by
    the parser.** (p25, p44, p23, p46, t14)

---

## 2. Syntax surface table

| Construct | Oracle behaviour | Evidence |
|---|---|---|
| `template <typename T> ret f(T x) { }` | **supported**; instantiated on call | C01, p02 |
| `template <class T> …` | **supported** (`class` is a synonym for `typename`) | corpus/parser/template/02 |
| `template <typename T, typename U> …` | **supported**; `U` unmapped ⇒ type name `U` leaks | p03, t12 |
| `template <int32 N> …` (non-type) | **parses**, never bindable; `N` ⇒ `undeclared identifier 'N'` | p71, t03 |
| `template <auto V> …` | **parses** (non-type param); unusable | p22 |
| `template <typename... Args> …` | **supported**; pack expands to N params | C04, examples/test_variadic.ivy |
| `template <typename T, typename... Args> …` | **supported**; fixed param keeps its name, pack params become `__<name>N` | examples/test_variadic.ivy (`count_after<int,int>`) |
| `template <Addable T> …` (constrained) | **supported**; constraint checked at instantiation | C06, C07 |
| `template <typename T> requires C<T> { }` (trailing) | **PARSE ERROR** (3 errors) | p23 |
| `concept Name = requires(T a, T b) { a + b; };` | **parses**; requirements kept as raw text | C06, p21 |
| `template <typename T> struct Box { … };` | **supported**; specialized on a template-id type | C05, p13 |
| `template <class T> class Holder { … };` | **supported** (same as struct) | corpus/parser/template/11 |
| `template <typename T> fn f(T x) -> T { }` | **supported** (trailing return type) | corpus/parser/template/12 |
| `template <typename T> constexpr T f(T x) { }` | **supported**; the call is const-folded | p63, corpus/parser/template/13 |
| `export template <…> …` | **supported** | p77 |
| member function template `template <typename T> T m(T) { }` inside a struct | **PARSE ERROR** `expected field or method declaration in struct body` | p25 |
| explicit specialization `template <> int32 f<int32>(int32) { }` | **PARSE ERROR** | p44 |
| explicit template args at a call `f<int32>(x)` | **supported** | C01 |
| explicit template args at a call `f<3>(x)` (value) | **PARSE ERROR** (the parser needs a *type* after `<`) | p04 |
| explicit template args at a call `f<>(x)` (empty) | **PARSE ERROR** `unexpected token in expression` | p19 |
| template-id type `Vec<int32>` | **supported** | p13, p37 |
| qualified template-id `ns::Vec<int32>` | **parses**; the *call* `ns::id<int32>(x)` ⇒ `call to undeclared function 'ns::id'` | p26 |
| nested template-id `Vec<Vec<int32>>` | **PARSE ERROR** (`>>` is one token) | t3, t4, p38 |
| template-id as a template argument `f<Vec<int32>>(x)` | **PARSE ERROR** (same `>>` problem) | p38 |
| `static` member in a struct template | **PARSE ERROR** | p46 |
| `sizeof(T)` / `alignof(T)` inside a template | compiles; **value is the 4-byte fallback**, not the real size | p30, p48 |
| `sizeof...(pack)` / `sizeof...(Pack)` | **supported** (pack size) | p62, p86 |
| fold expression `(args + ...)`, `(... + args)`, `(init + ... + args)`, `(f(args), ...)` | **supported**; expanded to a **left-nested** binary chain | examples/test_variadic.ivy, p88 |
| pack expansion in a call `f(args...)` (not a fold) | **BROKEN**: `undeclared identifier 'args'` | p85 |
| `if constexpr` inside a template | condition evaluated at instantiation; the **discarded branch is not checked** | C12, p39, p51, p52 |
| template-id on a non-template struct `Box<int32>` (plain struct) | parsed; the args are **ignored** (type stays `Box`) | p79 |
| template-id with an unknown name `Unknown<int32> b;` | **PARSE ERROR** `unexpected token in expression` | p90 |
| `using X = Box<int32>;` | UNKNOWN (not probed) | — |
| template template-parameter `template <template<class> class T> …` | **PARSE ERROR** (the parser expects a *type* or `typename` after `<`) | p22-analog; UNKNOWN precisely |

---

## 3. How the HIR dump represents templates

### 3.1 Naming / mangling

```
function-template specialization :  <tplFunc.name> "<" arg0 "," arg1 ">"
struct-template specialization    :  <tplStruct.name> "<" arg0 "," arg1 ">"
arg                                :  base [" unsigned"] [" const"] ["*" × pointerDepth]
empty pack                         :  <name>"<>"
```

* `tplFunc.name` is the *qualified* name when the template is in a namespace →
  `ns::id<int32_t>` (p72).
* The struct specialization name uses the struct's name, so a namespaced struct template
  would mangle to `ns::Box<int32_t>` — **UNKNOWN** (not probed).
* The instantiated function is registered in the overload set under the *mangled* name
  (`functions_[raw->name]`), and the call's `callee` string is the mangled name:
  `call 'identity<int32_t>' : int32_t`.
* A struct-template method is named `<SpecName>::<bareMethodName>`
  (`Box<int32_t>::get`). A constructor keeps the *original* struct name as the method
  name: `Box<int32_t>::Box` (p34).
* The `hir::Function::isTemplate` flag and `tplArgs` vector exist in `src/hir/hir.h:263-264`
  but are **not printed** by the dump. The port does not need them for byte-identity.

### 3.2 Order

`hir_->functions` is the dump order. It is filled as:

1. pass 1a: enums; pass 1b: structs (methods of *non-template* structs are appended here);
2. pass 1: free-function signatures (extern "C" first, then the rest);
3. pass 2: free-function bodies — **instantiations triggered from a body are appended here**;
4. pass 2b: method bodies of non-template structs.

Because the function pool is only appended to, **every instantiation lands after every
ordinary function**, in the order the instantiations were first triggered.
Evidence: C09 (`a`,`b`,`c`,`d` then `id<int32_t>`, `id<int64_t>`), p75
(`f`, then `Box<int32_t>::get`, then `id<int32_t>`).

Within one body, the order is the order of the triggering statements (p75: the struct is
instantiated by the `Box<int32> b;` decl, the function template by the later call).

### 3.3 De-duplication

`instantiated_[mangled]` caches the specialization; a second use returns the cached one and
**no second dump entry** is produced (C09: `id<int32_t>` appears once although used twice).
Struct specializations are cached in `instantiatedStructs_` the same way (p45).

### 3.4 Substitution actually applied

`substituteType` (`hir_builder.cpp:4971`) only rewrites the **base name** of a type and
recurses into `tplArgs`; it then re-applies the *original* type's `pointerDepth`,
`isReference` and `isConst`:

```
sub = mapping[original.base]           // the concrete type
sub.pointerDepth = original.pointerDepth
sub.isReference  = original.isReference
sub.isConst      = original.isConst
```

Consequences (all verified):

| Written | Substituted to | Note |
|---|---|---|
| `T` (param/return) | concrete type | base+unsigned come from the mapping |
| `const T&` | `const <concrete>&` | const/ref taken from the *declaration* |
| `T*` | `<concrete>*` | pointer depth taken from the declaration |
| `T` deduced as `char const*` | `char` | the concrete type's const/pointer are **lost** (p15) |
| `Box<T>` (param type) | base `Box` + `tplArgs [int32_t]` | the specialization is **not** resolved ⇒ call always mismatches (p60) |
| `Box<T>` (struct field type) | `Box<int32_t>` (resolved) | because `resolveTemplateStructType` runs on the cloned field (p61) |
| local `T y;` | **not substituted** | `currentTypeMapping_` is empty during *function* template instantiation (p28, C10) |
| local `T y;` inside a **struct** template method | substituted | `currentTypeMapping_` *is* set during struct instantiation (p82) |

### 3.5 What the specialization's body looks like

The body is the template's AST body, **re-built** with the substituted signature in scope.
Hence:

* the parameter identifiers keep their original names (`x`, `a`, `b`);
* the return type / parameter types show the substituted types;
* the body text is *not* re-type-checked against the substituted types in any special way —
  it is checked exactly as any function body, so a body that mentions `T` in a *local
  declaration* fails (C10), while a body that only *uses* the parameters works (C11, C12).

---

## 4. Verbatim oracle output (12 representative programs)

All captured with `ivy --hir <file>`; stdout+stderr merged; the exit code is the last line.
The source of each fixture is in `…\h44recon\corpus\C01..C12`; the raw capture is
`…\h44recon\corpus_oracle.txt`.

### C01 — simple type-parameter template (explicit argument)

```ivy
template <typename T> T identity(T x) { return x; }
void f() { int32 a = identity<int32>(1); }
```
```
function f
  return void()
  block
    decl int32_t a =
      call 'identity<int32_t>' : int32_t
        int 1

function identity<int32_t>
  return int32_t(int32_t x)
  block
    return
      ident x
```
exit 0.

### C02 — a template that is never instantiated

```ivy
template <typename T> T identity(T x) { return x; }
void f() { int32 a = 1; }
```
```
function f
  return void()
  block
    decl int32_t a =
      int 1
```
exit 0. **The template produces no dump entry at all** — not a prototype, not a stub.

### C03 — implicit deduction

```ivy
template <typename T> T identity(T x) { return x; }
void f() { int32 a = identity(1); }
```
```
function f
  return void()
  block
    decl int32_t a =
      call 'identity<int>' : int
        int 1

function identity<int>
  return int(int x)
  block
    return
      ident x
```
exit 0. Note the type is **`int`**, not `int32_t`: the HIR type of an integer literal is
`int` (`hir_builder.cpp:134` `intType()`), so `identity(1)` and `identity<int32>(1)` create
**two different specializations** (see `examples/test_template_deduction.ivy`, which
contains both `identity(42)` → `identity<int>` and `identity<int32>(99)` → `identity<int32_t>`).

### C04 — variadic template

```ivy
template <typename... Args> int32 count(Args... args) { return 0; }
void f() { int32 a = count<int32, int32>(1, 2); }
```
```
function f
  return void()
  block
    decl int32_t a =
      call 'count<int32_t,int32_t>' : int32_t
        int 1
        int 2

function count<int32_t,int32_t>
  return int32_t(int32_t __args0, int32_t __args1)
  block
    return
      int 0
```
exit 0. The pack parameter expands into **N parameters** named `__<funcParamName><index>`
(`__args0`, `__args1`; with `rest` → `__rest0`). The template parameter name (`Args`) is
never a scope name.

### C05 — struct template with methods

```ivy
template <typename T> struct Box {
    T value;
    T get() { return this.value; }
    void set(T v) { this.value = v; }
};
void f() { Box<int32> b; }
```
```
function f
  return void()
  block
    decl Box<int32_t> b

function Box<int32_t>::get
  return int32_t(Box<int32_t>& this)
  block
    return
      member . value
        this

function Box<int32_t>::set
  return void(Box<int32_t>& this, int32_t v)
  block
    expr
      assign '=' : int32_t
        member . value
          this
        ident v
```
exit 0. The struct itself is **not** dumped (only functions are), the `this` parameter is
`Box<int32_t>&`, and the field/method types are substituted.

### C06 — concept satisfied

```ivy
concept Addable = requires(T a, T b) { a + b; };
template <Addable T> T add(T a, T b) { return a + b; }
void f() { int32 r = add<int32>(3, 4); }
```
```
function f
  return void()
  block
    decl int32_t r =
      call 'add<int32_t>' : int32_t
        int 3
        int 4

function add<int32_t>
  return int32_t(int32_t a, int32_t b)
  block
    return
      binary '+' : int32_t
        ident a
        ident b
```
exit 0.

### C07 — concept NOT satisfied (failing file)

```ivy
concept Addable = requires(T a, T b) { a + b; };
struct P { int32 x; };
template <Addable T> T add(T a, T b) { return a; }
void f() { P p; P q; P r = add<P>(p, q); }
```
```
…\C07_concept_fail.ivy:5:31: error: concept 'Addable' not satisfied: type 'P' does not support operator '+'
…\C07_concept_fail.ivy:5:31: error: failed to instantiate template 'add'
…\C07_concept_fail.ivy:5:22: error: cannot initialize variable 'r' of type 'P' with a value of type 'int'
```
exit 1, **no HIR dump**. Three diagnostics, in that order. The third one is a knock-on: the
failed call keeps the dummy type `int` (`dummyType()`, `hir_builder.cpp:128`).

### C08 — body error in a never-instantiated template

```ivy
template <typename T> T broken(T x) { return undefined_name; }
void f() { int32 a = 1; }
```
```
function f
  return void()
  block
    decl int32_t a =
      int 1
```
exit 0. **No diagnostic** — the template body is never checked.

### C09 — ordering and de-duplication

```ivy
template <typename T> T id(T x) { return x; }
void a() { int32 v = id<int32>(1); }
void b() { int32 v = 2; }
void c() { int32 v = id<int32>(3); }
void d() { int32 v = id<int64>(4); }
```
```
function a
  return void()
  block
    decl int32_t v =
      call 'id<int32_t>' : int32_t
        int 1

function b
  return void()
  block
    decl int32_t v =
      int 2

function c
  return void()
  block
    decl int32_t v =
      call 'id<int32_t>' : int32_t
        int 3

function d
  return void()
  block
    decl int32_t v =
      call 'id<int64_t>' : int64_t
        int 4

function id<int32_t>
  return int32_t(int32_t x)
  block
    return
      ident x

function id<int64_t>
  return int64_t(int64_t x)
  block
    return
      ident x
```
exit 0.

### C10 — local declaration of `T` is NOT substituted (failing file)

```ivy
template <typename T> T f(T x) { T y = x; return y; }
void g() { int32 r = f<int32>(1); }
```
```
…\C10_local_T_not_substituted.ivy:2:34: error: cannot initialize variable 'y' of type 'T' with a value of type 'int32_t'
…\C10_local_T_not_substituted.ivy:2:43: error: cannot return value of type 'T' from function returning 'int32_t'
```
exit 1, no dump. **This is the single most important thing NOT to "fix" in the port.**

### C11 — a template calling another template (failing file)

```ivy
template <typename T> T id(T x) { return x; }
template <typename T> T outer(T x) { return id<T>(x); }
void f() { int32 a = outer<int32>(1); }
```
```
…\C11_template_calls_template.ivy:4:35: error: cannot return value of type 'T' from function returning 'T'
…\C11_template_calls_template.ivy:5:51: error: argument 1 of 'id<T>' expects 'T', got 'int32_t'
…\C11_template_calls_template.ivy:5:38: error: cannot return value of type 'T' from function returning 'int32_t'
```
exit 1, no dump. The inner template was instantiated as `id<T>` — the explicit argument was
**not** substituted, because function-template instantiation never sets
`currentTypeMapping_` (only struct-template instantiation does, `hir_builder.cpp:5460-5461`).
The same construct **inside a struct-template method works** (p76:
`call 'id<int32_t>'`).

### C12 — `if constexpr` inside a template

```ivy
template <typename T> int32 f(T x) { if constexpr (sizeof(T) == 8) { return undefined_thing; } else { return 2; } }
void g() { int32 r = f<int32>(1); }
```
```
function g
  return void()
  block
    decl int32_t r =
      call 'f<int32_t>' : int32_t
        int 1

function f<int32_t>
  return int32_t(int32_t x)
  block
    if
      binary '==' : bool
        int 4
        int 8
      block
        return
          int 2
```
exit 0. `sizeof(T)` folds to **4** (the unknown-type fallback, `hir_builder.cpp:480`), so the
condition is false, the `else` branch is kept and the `if` branch (with
`undefined_thing`) is **discarded and never checked**.

---

## 5. Substitution rules table

| Where | Written | Result | Evidence |
|---|---|---|---|
| return type | `T` | concrete type | C01 |
| return type | `T*` | `<concrete>*` | p58 (error message shows `int32_t*`) |
| parameter type | `T` | concrete type | C01 |
| parameter type | `const T&` | `const <concrete>&` | p31 |
| parameter type | `T*` | `<concrete>*` | t15 |
| parameter type | `Box<T>` | base `Box`, `tplArgs [int32_t]`, **not resolved** | p60 |
| local decl | `T y` | **not substituted** (`T` stays) | C10, p28 |
| local decl (struct-template method) | `T y` | substituted | p82 |
| struct field | `T v` | concrete type | C05 |
| struct field | `Box<T> v` | resolved to `Box<int32_t>` | p61 |
| struct base list | `T` | **base list is dropped entirely** | p49 |
| method `this` | implicit | `SpecName&` | C05 |
| default argument | `= <expr>` | **not substituted** (the default expr is cloned at the call site) | UNKNOWN (not probed) |
| `sizeof(T)` | | 4 (fallback) | p30, p48 |
| `alignof(T)` | | 4 (fallback) | p48 |
| `sizeof(T)` where T = a struct | | 4 (the specialization is *not* resolved) | p48 |
| `sizeof(Box<int32>)` | | 4 (wrong: the real size is 8) | p48 |
| non-type param `N` | | never bound ⇒ `undeclared identifier 'N'` | p71 |
| variadic pack | `Args... args` | expands to N params `__args0…` | C04 |
| pack in `sizeof...(args)` | | pack element count | p62 |
| pack in `sizeof...(Args)` | | pack element count (template-param name also works) | p86 |
| fold `(args + ...)` | | left-nested binary chain | examples/test_variadic.ivy, p88 |
| fold with an empty pack | | identity: `+`→0, `&&`→true, `||`→false, `,`→0 | examples/test_variadic.ivy |

---

## 6. Deduction rules table

`deduceTemplateArgs` (`hir_builder.cpp:5193`).

| Situation | Oracle behaviour | Evidence |
|---|---|---|
| no explicit args, param type is exactly a type parameter `T` | `T = arg type` (reference-ness stripped) | C03, p06 |
| arg is an integer literal | `T = int` (the literal's HIR type) — **not** `int32_t` | C03 |
| arg is a variable of type `int32` | `T = int32_t` | t01 |
| param is `const T&` | `T = arg type`, `isReference` stripped | p42 |
| param is `T*` | `T = pointee type`; pointer-depth mismatch ⇒ deduction fails | `hir_builder.cpp:5289-5296` |
| param is `T[N]` (array) | only handled when the arg's base already equals the param name (effectively a no-op) | `hir_builder.cpp:5310-5334` |
| param type is a concrete type (no type param) | ignored for deduction | — |
| the same type param deduced twice with different types | **deduction fails** | `hir_builder.cpp:5303` |
| fewer args than params, trailing params have defaults | allowed | `hir_builder.cpp:5250-5254` |
| fewer args than params, no defaults | **deduction fails** | p56, t12 |
| variadic template | fixed params deduce one-to-one, the pack absorbs the rest | examples/test_variadic.ivy, p87 |
| non-type params | **never deduced** ⇒ deduction fails | t04 (`cannot deduce template arguments for 'f'`) |
| explicit args given | deduction is skipped entirely (no conflict check!) | p11 (extra args ignored) |
| deduction fails | `cannot deduce template arguments for 'X'` (bare name, not mangled) | p56, t04 |
| a non-template overload with the same name exists | the **non-template wins**; the template is only used for a template-id call | p54 |

---

## 7. Concepts / constraints

`checkConstraint` (`hir_builder.cpp:4835`) is deliberately crude.

* A concept is `concept Name = requires(T a, T b) { <req>; <req>; };`. The requirements are
  stored as **raw token text** (`ast::ConceptDecl::requirements`).
* The check is performed **only at instantiation** and only for `typename` template
  parameters that carry a constraint.
* For each requirement the checker looks for a binary operator
  (`+ - * / < > == != <= >=`) in the text. If one is found:
  * the concrete type is "arithmetic" if `base` ends with `_t` or is `char`/`bool`/`int`/
    `long`/`short`/`unsigned`/`signed`/`float`/`double` ⇒ **satisfied**;
  * otherwise the type must be a struct that has a matching `operator<op>` overload whose
    first two parameters are of that exact type ⇒ otherwise **not satisfied**.
* Member-access requirements (`a.foo()`) are **not** checked at all
  (`hir_builder.cpp:4943-4946`).
* An **unknown concept name is assumed satisfied** (`hir_builder.cpp:4838-4841`) — p21 shows
  `template <UnknownConcept T>` instantiating happily.
* `bool` satisfies `a + b` (it is in the arithmetic list) — p09 shows `add<bool>` succeeding.

Diagnostics:

| Situation | Exact message |
|---|---|
| unsatisfied | `concept 'Addable' not satisfied: type 'P' does not support operator '+'` |
| then | `failed to instantiate template 'add'` (same location) |
| then (knock-on) | `cannot initialize variable 'r' of type 'P' with a value of type 'int'` |

`examples/test_concept_fail.ivy` reproduces exactly this (it is a real repo example that
fails to compile).

---

## 8. Diagnostics table

Format: `<file>:<line>:<col>: error: <message>` on **stderr**; the HIR dump is suppressed
whenever any diagnostic is produced; exit code 1.

| # | Situation | Exact message | Location | Evidence |
|---|---|---|---|---|
| D1 | call to a non-existent template with explicit args | `undeclared identifier 'nosuch'` **and** `'nosuch' is not a template` | callee ident / the call | p41 |
| D2 | deduction failed | `cannot deduce template arguments for 'id'` | the call | p56, t04 |
| D3 | instantiation aborted (constraint) | `failed to instantiate template 'add'` | the call | C07 |
| D4 | concept not satisfied | `concept 'Addable' not satisfied: type 'P' does not support operator '+'` | the call | C07 |
| D5 | wrong number of function arguments | `call to 'id<int32_t>' expects 1 argument(s), got 2` | the call | p40 |
| D6 | missing argument without default | `call to 'a<int32_t>' missing argument 2 (parameter 'y' has no default value)` | the call | p53 |
| D7 | argument type mismatch | `argument 1 of 'identity<char const*>' expects 'char', got 'const char*'` | the *argument* | p15 |
| D7b | same, with an unmapped template param | `argument 2 of 'first<int32_t>' expects 'U', got 'int'` | the argument | p14, t12 |
| D8 | local decl with an unsubstituted `T` | `cannot initialize variable 'y' of type 'T' with a value of type 'int32_t'` | the decl's type | C10, p28 |
| D9 | return with an unsubstituted `T` | `cannot return value of type 'T' from function returning 'int32_t'` | the `return` keyword | C10, p28 |
| D9b | return of an unmapped param | `cannot return value of type 'U' from function returning 'U'` | the `return` keyword | t12 |
| D10 | unknown name in an instantiated body | `undeclared identifier 'N'` / `undeclared identifier 'value'` | the identifier | p71, p07 |
| D11 | fold over an unknown pack | `fold expression references unknown parameter pack` | the fold | `hir_builder.cpp:3934` (not triggered in a corpus file) |
| D12 | template-id on a plain struct in a *type* position | *no diagnostic* — the args are ignored | — | p79 |
| D13 | too many template arguments | *no diagnostic* | — | p11, p80 |
| D14 | too few template arguments | *no diagnostic of its own*; a cascade of D7b/D9/D8 | — | p14, t12 |
| D15 | redefinition of a specialization | *no diagnostic* (the second template silently replaces the first) | — | p53, p78 |

Parse-level diagnostics (from the parser, not the HIR builder):

| Situation | Exact message(s) | Evidence |
|---|---|---|
| `f<>(x)` | `unexpected token in expression` | p19 |
| `Box<Box<int32>>` | `expected '>' to close template argument list` (×2) + `expected variable name` | t3, t4 |
| `f<Box<int32>>(x)` | `expected '>' to close template argument list` + `unexpected token in expression` | p38 |
| `Unknown<int32> b;` | `unexpected token in expression` | p90 |
| member template in a struct | `expected field or method declaration in struct body` + 2 more | p25 |
| `template <> f<int>(…)` | `expected '(' after function name 'id'` + 3 more | p44 |
| trailing `requires` clause | `expected '{' for function body or ';' for a declaration` + 2 more | p23 |
| `static` member | `expected field or method declaration in struct body` + 6 more | p46 |
| `T data[N]` field with a non-type param | `expected ';' after field declaration` + 3 more | t14 |
| `Arr<int32, 4>` (value arg) | `expected a type in template argument list` + `expected '>' to close template argument list` | t14 |
| bad template param list | `expected 'typename', a concept name, or a type in template parameter list` / `expected template parameter name` | parser.cpp:1666/1680 |

**Multiple diagnostics are printed in the order they are produced** (inner template first,
then the call site, then the enclosing statement) — see C07, C10, C11.

---

## 9. "Failing file" output shape

For any file that produces at least one diagnostic the oracle prints **only** the
diagnostics (on stderr) and **no HIR dump at all**, and exits 1:

```
$ ivy --hir failing.ivy
failing.ivy:5:31: error: concept 'Addable' not satisfied: type 'P' does not support operator '+'
failing.ivy:5:31: error: failed to instantiate template 'add'
failing.ivy:5:22: error: cannot initialize variable 'r' of type 'P' with a value of type 'int'
$ echo $?
1
```

(The dump is written to stdout, the diagnostics to stderr; the difftest harness merges
`2>&1`, so the port must reproduce the *absence* of the dump.)

---

## 10. Known oracle bugs / avoid these inputs

### 10.1 Crashes (exit code `-1073741819` = 0xC0000005, access violation)

| Input | Crash | Template-specific? | Evidence |
|---|---|---|---|
| `a + b` where `a`,`b` are structs without `operator+` | **YES** | **NO** | p64 (template) and p68 (plain) both crash |
| `obj(args)` where `obj` is a struct without `operator()` | **YES** | **NO** | p65 (template) and p69 (plain) both crash |
| `examples/test_b5_expected.ivy` (`expected<T,E>` init-list) | **YES** | **NO** | running `ivy --hir examples/test_b5_expected.ivy` crashes |

These are pre-existing oracle bugs, not template bugs. **The corpus must not contain such
files** (a crash gives empty output and a non-1 exit code, which breaks the differ).

A nested `constexpr` call inside a binary expression did **not** crash in my probe (p66,
p84) — the crash may require a different shape. **Treat it as unsafe.**

### 10.2 Semantic bugs to reproduce faithfully (do NOT "fix")

| # | Bug | Where | Evidence |
|---|---|---|---|
| B1 | A local declaration of type `T` is not substituted | `hir_builder.cpp:503-505` (`currentTypeMapping_` is empty for function templates) | C10, p28 |
| B2 | A template calling another template with an explicit argument does not substitute the argument | same root cause | C11, p27 |
| B3 | `sizeof`/`alignof` of a template-id type returns 4 (the fallback) instead of the real layout | `typeSize` fallback `hir_builder.cpp:480` | p48, p36 |
| B4 | A struct-template specialization **loses its base classes** | `instantiateStructTemplate` never copies `bases` (`hir_builder.cpp:5442-5448`) | p49 |
| B5 | The concrete type's `const`/pointer qualifiers are lost when substituting into a plain `T` parameter | `substituteType` overwrites them with the declaration's | p15 |
| B6 | `T` deduced from an integer literal is `int`, not `int32_t` | `intType()` = `"int"` | C03 |
| B7 | Extra template arguments are silently accepted; the mangled name contains them | `instantiateTemplate` maps only `min(#params,#args)` | p11, p80 |
| B8 | Two templates with the same name: the second silently replaces the first (no overloading) | `templates_` is keyed by name | p53 |
| B9 | `Box<T>` as a *parameter* type is never resolved to the specialization ⇒ every call mismatches | `instantiateTemplate` does not call `resolveTemplateStructType` on params | p60 |
| B10 | A pack expansion in a call (`f(args...)`) is broken | the pack name is not a scope name | p85 |
| B11 | The `currentTypeMapping_` is set during *struct* instantiation but not during *function* instantiation | `hir_builder.cpp:5460` vs `:4995` | C11 vs p76 |
| B12 | An unknown concept name is silently treated as satisfied | `hir_builder.cpp:4838-4841` | p21 |
| B13 | A non-type template parameter is never bound | `instantiateTemplate` only maps `isTypename` params | p71, t03 |

### 10.3 Parser limitations that the port inherits for free

* `>>` is a single token ⇒ nested template-ids cannot parse (`lexer.cpp:115`).
* A template argument must be a **type** ⇒ value arguments and `f<>()` are parse errors.
* A member function template is not a struct member ⇒ parse error.
* `template <> …` explicit specialization is not parsed.

---

## 11. Recommended implementation plan (phases 2 → 4)

### Phase 2 — parser/AST retention (no dump change)

Goal: the self-host AST must *keep* the template information that the C++ AST keeps, while
the `--ast` dump stays **byte-identical** (the C++ `--ast` dump prints neither template
parameters nor template arguments — `dumpAst`/`dumpType` in `main.cpp:331-355` ignore them;
`dumpType` prints only the base name of a template-id).

Required AST additions (self-host `selfhost/parsing/ast.ivy`):

| New record | Fields | Why |
|---|---|---|
| `AType.tpl_args` + `AType.tpl_n` | pointer into a parser-owned `AType` pool | template-id types (`Box<int32>`) — the C++ `ast::Type::tplArgs` |
| `AFunction.tpl_params` + `tpl_n` | pool of `ATemplateParam` | function templates |
| `AStruct.tpl_params` + `tpl_n` | pool of `ATemplateParam` | struct/class templates |
| `ATemplateParam` | `name`, `is_typename`, `is_variadic`, `constraint` (concept name), `type` (for non-type params) | `ast::TemplateParam` (`src/parsing/ast.h:73-80`) |
| `AConcept` + a concept pool | `name`, `param_name`, `requirements` (joined text) | `ast::ConceptDecl` (`src/parsing/ast.h:87-95`) |
| `AExpr.tpl_args` + `tpl_n` | pool of `AType` | the explicit args of a template-id **call** (`ast::Expr::Call::tplArgs`) |

Parser work (`selfhost/parsing/parser.ivy`):

* `parse_template_params` (line 3349) currently *discards* the parameters — collect them
  into a pool and return them; keep registering the `typename` names in
  `templateParamNames_` exactly as today.
* `parse_template` (line 3396) must pass the parameter list to `parse_function` /
  `parse_fn_trailing` / `parse_struct`.
* `parse_type` (line 1083) currently calls `skip_angle_args()` (line 1113) — replace with a
  real `parse_template_args()` that records the argument types (mirroring
  `parser.cpp:1702`). **Careful:** the C++ parser only treats `<` as a template-id when the
  base name is a *known* user type; the self-host `token_is_type_name` already implements
  that check, so keep the same structure.
* the template-id **call** path (the `at("<")` branch in the postfix loop) currently calls
  `this.parse_type()` and throws the result away — record the argument types on the call
  expression.
* `parse_concept` (line 3423) currently discards the concept — record name, the first
  parameter's type string (the concept's type parameter) and the requirement texts.
* the parser must keep the C++ *error* behaviour for malformed input (e.g. `expected '>' to
  close template argument list`) because the parser corpus (H3.5) is byte-compared.

Risks:
* `AType` is copied by value in ~95 places in `selfhost/hir/hir_builder.ivy` and ~20 in the
  parser; adding a pointer field means the two explicit initialisers (`parse_type` line 1084
  and `empty_type` line 1693) must initialise it, and any *value* copy of an `AType` now
  shares the pool pointer (fine — the pool is append-only, and the C++ AST does the same
  with `std::vector<Type>` by value).
* the self-host `AType` has no `arraySize`-like template slot today; check that
  `same_type` (`hir_builder.ivy:3761`) ignores template args, exactly like
  `ast::Type::operator==`… **no**: `ast::Type::operator==` *does* compare `tplArgs`
  (`src/parsing/ast.h:45`). The self-host `same_type` said "minus template arguments" —
  the port had to add the `tplArgs` comparison or the overload resolution would differ.
  **Resolved in phase 4**: `same_type` now compares `tpl_n` and the template arguments
  (`hir_builder.ivy:3767-3778`, see L5).

### Phase 3 — function templates

Implement, mirroring the oracle one-for-one:

| Oracle function | Port equivalent |
|---|---|
| `templates_` registry | a name → AST-function table in `HirBuilder` |
| `lookupTemplate` | `lookup_template(name)` (with the namespace-prefix fallback) |
| `mangle` | `mangle_args(name, args)` — base + `" unsigned"` + `" const"` + `"*"`s, comma-separated, no spaces; `name + "<>"` for zero args |
| `substituteType` | `substitute_type(t, mapping)` — rewrite the base, recurse into `tplArgs`, then re-apply the *original* `ptr_depth`/`is_ref`/`is_const` |
| `instantiateTemplate` | `instantiate_template(...)` — build the mapping, check the constraints, create the HIR function (substituted return type + params), **register it in the HIR function pool and in the overload table under the mangled name**, then build the body from a clone of the AST body |
| `deduceTemplateArgs` | `deduce_template_args(...)` — only the "param type is exactly a type param" case, plus the variadic pack |
| the template-id call path | in `build_call`: if the callee is a bare ident and the call carries template args → instantiate |
| the implicit-deduction path | in `build_call`: if there is **no** non-template overload for the name and the name is a template → deduce & instantiate |

Ordering: the instantiated HIR function must be **appended to the HIR function pool** at
the moment of instantiation, i.e. *after* all the ordinary functions (which are all
registered in pass 1) — this reproduces C09 / p75.

The instantiation must be **cached by the mangled name** so a second use does not emit a
second entry (C09).

The body must be built with the substituted signature in scope, and — crucially — **without**
a type mapping for locals (B1). The self-host `hir_builder.ivy` currently resolves an
`AType` by base name only; the port must reproduce "the local decl keeps the literal name
`T`" (C10) rather than substituting it.

Diagnostics: D2, D3, D5, D6, D7, D9, D10.

### Phase 4 — class/struct templates + concepts

| Oracle function | Port equivalent |
|---|---|
| `structTemplates_` registry | a name → AST-struct table |
| `lookupStructTemplate` | with the namespace-prefix fallback |
| `mangleStructSpec` | same mangling as functions |
| `instantiateStructTemplate` | clone the AST struct, substitute the field types, clone+rename the methods (`<Spec>::<method>`), build the specialization struct (layout!), build the method bodies with `this` typed `Spec&` |
| `resolveTemplateStructType` | called from decls, params, return types, field types |
| `checkConstraint` | the arithmetic/operator-overload check |

Sub-tasks / pitfalls:

* the specialization's **base list is dropped** (B4) — reproduce it;
* the specialization's methods are built **by the struct instantiation**, not by pass 2b
  (pass 2b only walks the original AST structs) — so the method bodies are built at
  instantiation time, in the order the struct is instantiated (p75);
* the constructor's method name keeps the *original* struct name: `Box<int32_t>::Box`;
* `currentTypeMapping_` **is** set during struct instantiation ⇒ local `T` decls inside a
  struct-template method *are* substituted (p82) — the opposite of phase 3;
* concepts: the concept registry is filled in pass 0a, before anything else
  (`hir_builder.cpp:4341-4343`); the constraint check runs at instantiation, per
  `typename` parameter, and an unknown concept name is *satisfied* (B12).

### Corpus for phases 2–4 (all under `selfhost/corpus/hir/tpl/`)

Minimal but **real** list (each file must be a *behavioural* test of one rule):

```
tpl/01_type_param.ivy            C01   explicit <int32>
tpl/02_uninstantiated.ivy        C02   template never instantiated -> no dump entry
tpl/03_deduce.ivy                C03   implicit deduction -> "int"
tpl/04_deduce_two_params.ivy     p57   two params deduced
tpl/05_deduce_ref.ivy            p42   const T& deduction
tpl/06_explicit_and_deduced.ivy  p50/p03  two specializations of the same template
tpl/07_variadic.ivy              C04   pack -> N params __args0..
tpl/08_variadic_mixed.ivy        p87   fixed + pack
tpl/09_variadic_empty.ivy        examples/test_variadic.ivy  empty pack, folds
tpl/10_struct_field.ivy          p13   struct template, field only
tpl/11_struct_methods.ivy        C05   struct template, methods
tpl/12_struct_ctor.ivy           p34   ctor naming
tpl/13_struct_local_T.ivy        p82   local T substituted in a struct template
tpl/14_struct_base.ivy           p49   base list dropped
tpl/15_concept_ok.ivy            C06   satisfied concept
tpl/16_concept_unknown.ivy       p21   unknown concept -> satisfied
tpl/17_order_dedup.ivy           C09   dump order + de-duplication
tpl/18_namespace.ivy             p72   ns::id<int32_t>
tpl/19_recursion.ivy             p18   recursive template
tpl/20_prototype.ivy             p74   declaration-only template
tpl/21_constexpr.ivy             p63   constexpr template (call folded)
tpl/22_overload_with_plain.ivy   p54   plain overload wins for an implicit call
tpl/23_if_constexpr.ivy          C12   discarded branch not checked
tpl/24_struct_calls_template.ivy p76   currentTypeMapping_ active in a struct template
tpl/e_01_deduce_fail.ivy         p56   cannot deduce template arguments
tpl/e_02_unknown_template.ivy    p41   'X' is not a template
tpl/e_03_concept_fail.ivy        C07   concept not satisfied
tpl/e_04_wrong_arg_count.ivy     p40   expects N argument(s), got M
tpl/e_05_arg_type_mismatch.ivy   p15   argument 1 of 'X' expects 'A', got 'B'
tpl/e_06_too_few_tpl_args.ivy    p14   'U' leaks
tpl/e_07_local_T.ivy             C10   local T not substituted
tpl/e_08_template_calls_tpl.ivy  C11   id<T> not substituted
tpl/e_09_nontype_param.ivy       p71   undeclared identifier 'N'
tpl/e_10_too_many_tpl_args.ivy   p11   extra args accepted
tpl/e_11_empty_tpl_args.ivy      p19   parse error: unexpected token in expression
tpl/e_12_nested_tplid.ivy        t3    parse error: expected '>' to close ...
tpl/e_13_member_template.ivy     p25   parse error: expected field or method ...
tpl/e_14_explicit_spec.ivy       p44   parse error
tpl/e_15_trailing_requires.ivy   p23   parse error
tpl/e_16_static_member.ivy       p46   parse error
```

Candidate real-repo sources that can be reused verbatim as corpus files (they exercise the
same paths): `examples/test_template_deduction.ivy`, `examples/test_template_struct.ivy`,
`examples/test_variadic.ivy`, `examples/test_concepts.ivy`, `examples/test_ivy_ext.ivy`
(all use `#pragma ivy cnumber`, so C-style `int`/`double` types appear — the mangled names
then contain `int`/`double` instead of `int32_t`).

The *phase 2* corpus should additionally include the existing
`selfhost/corpus/parser/template/*.ivy` (15 files, H3.5) — those must **stay** byte-identical
under `--ast` after the AST is extended.

---

## 12. Self-host code map (where to change what)

| File | What is there today | What phase 2/3/4 must add |
|---|---|---|
| `selfhost/parsing/ast.ivy` | `AType` (no `tpl_args`), `AFunction` (no template info), `AStruct` (no template info), no concept record | `AType.tpl_args/tpl_n`, `ATemplateParam`, `AConcept`, `AFunction.tpl_params/tpl_n`, `AStruct.tpl_params/tpl_n`, `AExpr.tpl_args/tpl_n` |
| `selfhost/parsing/parser.ivy` | `parse_template_params` (3349) discards; `parse_template` (3396); `parse_concept` (3423) discards; `parse_type` (1083) calls `skip_angle_args` (1113); the template-id call branch in the postfix loop discards the args | collect the params/args/concepts |
| `selfhost/hir/hir.ivy` | `HFunction` has `name`, `ret`, params, `body`; no `is_template`/`tpl_args` (not needed for the dump) | possibly nothing; the mangled name is just a `const char*` |
| `selfhost/hir/hir_builder.ivy` | `build()` (4509) = enums → structs → extern-C signatures → signatures → bodies → method bodies; `build_call` (2980); `resolve_overload` (2864); `check_call` (2926); `is_assignable` (1281) | template registries, `instantiate_template`, `deduce_template_args`, `substitute_type`, `instantiate_struct_template`, `resolve_template_struct_type`, `check_constraint`; wire them into `build_call` and into the type-resolution path (`resolveTypeAlias` equivalent) |
| `selfhost/difftest.ps1` | per-feature `-Dir`/`-Oracle`/`-Modules` | add a `-Dir selfhost/corpus/hir/tpl -Kinds all -Oracle --hir` invocation for H4.4 (same shape as the H4.1–H4.3 entries) |

**Reminder:** the self-host `--ast` dump must stay byte-identical — the C++ `--ast` dump
prints *no* template information, so all the new AST fields are invisible to it.

---

## 13. Explicitly UNKNOWN (not determined by probing)

* The behaviour of a `using X = Box<int32>;` alias (a template-id in an alias target).
* The mangled name of a struct template declared inside a namespace (`ns::Box<int32_t>`?)
  — the code path suggests it uses the qualified struct name, but it was not probed.
* The behaviour of `template <template<class> class T> …` (template template-parameters):
  the C++ parser rejects it (`expected 'typename', a concept name, or a type in template
  parameter list`), but the exact message for that specific input was not captured.
* Whether a **default argument** of a template parameter is substituted.
* Whether a **variadic pack** used as a *function argument* (not a fold) can ever work —
  p85 shows the plain `f(args...)` form is broken, but the fold-with-call form
  `(f(args), ...)` works.
* The exact behaviour of `expected<T,E>` inside a template (the plain case crashes).
* Whether a struct-template specialization with a **virtual** method produces a correct
  vtable (not probed; `isPolymorphic` is computed from the *cloned* struct, whose
  `bases` were dropped).
* The `hir::Function::isTemplate` / `tplArgs` fields are not printed by the dump, so their
  values are irrelevant for byte-identity (they were not probed).

---

## 14. Phase 2 findings (IMPLEMENTATION agent, phase 2 of 6)

**Date:** 2026-10-21. **Changed:** `selfhost/parsing/ast.ivy`,
`selfhost/parsing/parser.ivy`, this file. `selfhost/parsing/astdrive.ivy` was
**not** touched (the `--ast` dump is unchanged). Nothing under `src/**` was
touched, the oracle was not rebuilt, nothing was committed.

Phase 2 only *retains* template data in the AST. No instantiation, no
substitution, no constraint checking — those are phases 3 and 4.

### 14.1 What the AST retains now

| record | sizeof | fields added / changed |
|---|---|---|
| `AType` | **48 (unchanged)** | `AType* tpl_args` — the argument types of a template-id type (`Box<int32>`), `null` for an ordinary type; `long tpl_n` — the argument count. `is_unsigned` / `is_const` / `is_ref` changed from `long` to **`bool`** (1 byte) so that the record still fits in 48 bytes (see 14.3). |
| `ATypeList` | 16 | **new** — a slice `{ AType* items; long n; }` returned by the parser. |
| `ATemplateParam` | 96 | **new** — `name`; `type` (the type of a *non-type* parameter, e.g. `int32_t` for `template <int32 N>`); `constraint` (the concept name of a constrained parameter, `""` when unconstrained); `is_typename`; `is_variadic`; `line`; `col`. |
| `ATemplateParamList` | 16 | **new** — a slice `{ ATemplateParam* items; long n; }`. |
| `AConcept` | 48 | **new** — `name`; `param_name` (the concept's type parameter); `req_start` (first index in the requirement-text pool); `nreqs`; `line`; `col`. |
| `AFunction` | 136 → **152** | `ATemplateParam* tpl_params`; `long tpl_n`. |
| `AStruct` | 80 → **96** | `ATemplateParam* tpl_params`; `long tpl_n`. |
| `AExpr` | 152 → **168** | `AType* tpl_args`; `long tpl_n` — the explicit template arguments of a template-id **call** (`f<int32>(x)`), kind 13. |

New parser accessors: `Parser.concepts()` → `AConcept*`,
`Parser.concept_count()`, `Parser.get_concept(i)` and
`Parser.requirement_text(i)` (the raw text of the i-th requirement).
The template-parameter lists and the argument lists are reached through the
AST records themselves (`AFunction.tpl_params`, `AType.tpl_args`, …).

### 14.2 The data layout phases 3–4 must rely on

1. **"Is a template?" is `tpl_n > 0`** — exactly the oracle's
   `!tplParams.empty()` (`hir_builder.cpp:824` and `:552`). A
   `template <> int32 f(int32) { }` (an *empty* parameter list) parses in
   both the oracle and the port and is **not** a template.
2. **Template parameter** (`ATemplateParam`):
   * `is_typename == 1` → `typename T`, `class T` or `Concept T`;
   * `is_typename == 0` → non-type parameter; `type` holds its type
     (`int32_t`, `auto`, …);
   * `is_variadic == 1` → `typename... Args` / `typename Args...`;
   * `constraint` → the concept name (`""` = unconstrained). The oracle has
     **no** default template argument and **no** trailing `requires` clause,
     so neither is representable (see 14.4).
3. **Template-id type** (`AType.tpl_args` / `AType.tpl_n`): `base` is the
   template's base name, `tpl_args[0..tpl_n)` are the argument types
   (`AType*` into a parser-owned block). A nested template-id owns its own
   list, so `Box<Box<int32> >` yields *two* lists: the outer one has one
   argument (itself a template-id with one argument).
4. **Template-id call** (`AExpr.tpl_args` / `AExpr.tpl_n`, kind 13): the
   explicit template arguments of the call. `AExpr.an` is still the number of
   *call* arguments, `AExpr.tpl_n` the number of *template* arguments.
5. **Concept** (`AConcept`): `name`; `param_name` = the *text* of the first
   requires-parameter's type (e.g. `"T"`, `"const T &"` — the C++ joins the
   parameter's tokens with a single space, `ast::ConceptDecl::paramName`);
   the requirements are the raw token texts of the requires-body, one per
   `;`-terminated requirement, tokens joined with a single space (identical
   to the C++). `concept Addable = requires(T a, T b) { a + b; };` → one
   requirement, the text `"a + b"`.
6. **Ownership / mutability**: a template-argument list is an immutable block
   allocated by the parser (`__ivy_alloc`), tracked in `Parser.argblk_` and
   freed by `Parser::release()`. The C++ stores `std::vector<Type>` *by
   value*, so a consumer that substitutes into the arguments must build a
   **new** list instead of writing through the pointer. The HIR builder will
   need its own pool for that.
7. The global template-name registry (`tpnames_` / `register_template_param`)
   is **unchanged**: the parser still registers every *type* parameter name
   (including a constrained one) while the parameter list is parsed, exactly
   as in H3.5. Parsing behaviour is therefore unchanged.

### 14.3 Why `AType` still has 48 bytes

`selfhost/hir/hir_builder.ivy:295` allocates the scope-type pool with a
hard-coded 48-byte stride (`__ivy_alloc(this.s_cap_ * 48)`) and that file
must not be touched in phase 2. `AType` therefore could not grow: the three
qualifier flags became `bool` (1 byte each — this also matches
`ast::Type::isUnsigned/isConst/isReference` in the C++, which are `bool`),
which frees the 16 bytes needed for `tpl_args` + `tpl_n`. The resulting LLVM
layout is `{ ptr, ptr, i64, i1, i1, i1, i64, i64 }` = 48 bytes; the pool
strides of the parser were updated accordingly (`AFunction` 136 → 152,
`AStruct` 80 → 96, `AExpr` 152 → 168).

### 14.4 Parser divergences found (and how they were handled)

| # | Input | Oracle | Port before phase 2 | Port now |
|---|---|---|---|---|
| D1 | `Box<Box<int32>>` (nested template-id; `>>` is one token) | 3 diagnostics: `expected '>' to close template argument list` ×2 + `expected variable name` | **accepted silently** — `skip_angle_args()` skipped a balanced `< … >` | identical to the oracle: the port now parses the argument list with the oracle's algorithm (`parseTemplateArgs`) |
| D2 | `template <1 T> …` / `template <typename> …` (malformed parameter list) | `expected 'typename', a concept name, or a type in template parameter list` (or `expected template parameter name`) **plus** `expected '>' to close template parameter list` | only the first diagnostic — the port `return`ed from `parse_template_params` right after the error and skipped the `expect(">")` | identical to the oracle: the port now `break`s out of the loop (like `parser.cpp:1668/1682`) and then expects `>` |
| D3 | files whose **HIR** is invalid (C07, C10, C11, …) | `ivy --ast` also runs the HIR builder and prints its diagnostics | the port's `astdrive` is parse-only → no HIR diagnostics | unchanged — this is a phase-3/4 concern, not a parser divergence |

Questions left open by phase 1, now answered:

* **Default template argument** — **rejected** by the oracle
  (`template <typename T = int32>` → `expected '>' to close template
  parameter list` + 2 more). The port rejects it identically; there is no
  field for it.
* **Trailing `requires`** — **rejected** (`requires` is a keyword, so the
  return-type parse fails with `expected a type (…)`). The only accepted
  constraint form is a **constrained template parameter**
  `template <Concept T>`, whose concept name is stored in
  `ATemplateParam.constraint`.
* **Member function templates** — still rejected by the oracle
  (`expected field or method declaration in struct body`); the port
  reproduces this. `AFunction.tpl_params` is therefore always empty for a
  method (the port sets it to null/0 in `push_method_record`).
* **`template <> f()`** — parses in both; an empty parameter list is *not* a
  template (see 14.2.1).
* **`ns::Vec<int32>`** — the port has **no namespace support at all** (neither
  in the parser nor in the HIR builder), so a qualified template-id type
  cannot be produced by the port today. The AST representation is
  namespace-agnostic (`AType.base` is a plain string), so nothing has to
  change when namespaces are added; the qualified *mangling*
  (`ns::id<int32_t>`, p72) stays a phase-3/4 problem.

### 14.5 `--ast` byte-identity

Method: the repo harness `selfhost/difftest.ps1` compares the oracle's
`ivy --ast` with the port's `astdrive` output file by file (the harness
drops empty lines on both sides — the port has never emitted the oracle's
extra blank line after each function; that difference predates this phase
and is invisible to the harness).

* `selfhost/corpus/parser/template/*.ivy` → **15/15 PASS** (unchanged).
* Phase-1 fixtures `C01..C12` + 24 extra scratch fixtures (nested
  template-ids, default arguments, trailing `requires`, bad parameter lists,
  qualified names, concepts, non-type/variadic/constrained parameters,
  template-ids in every type position) → **31/36 identical**; the 5
  differences are D3 above (files whose HIR is invalid).
* A scratch driver confirmed that the retained data is *populated*:
  template parameters (type / non-type / variadic / constrained), template-id
  types with their argument lists, template-id call arguments, and concepts
  with their parameter name and requirement texts.

### 14.6 Per-suite results (all after the change)

| suite | files | result |
|---|---|---|
| parser/decl | 15 | PASS |
| parser/expr | 13 | PASS |
| parser/stmt | 15 | PASS |
| parser/aggregate | 14 | PASS |
| parser/template | 15 | PASS |
| parser/module | 16 | PASS |
| parser/err | 19 | PASS |
| hir/name | 27 | PASS |
| hir/type | 120 | PASS |
| hir/constexpr | 70 | PASS |
| lexer/ident, num, str, op, trivia, err | 10/8/7/5/7/9 | PASS |
| pp/define_obj, define_fn, cond, expand, predef, line, no_include | 10/12/16/16/20/22/9 | PASS |

### 14.7 What phases 3–4 must know (not stated in sections 1–13)

1. **Uninitialised-field landmine (must fix in phase 3).**
   `selfhost/hir/hir_builder.ivy` builds `AType` values through
   `ty_empty()` (line 828) and creates `AFunction`/`AField` records locally
   (lines 976, 1023, 1202–1204, 1234–1235, 1409). None of them initialise
   the new fields, so `tpl_args` / `tpl_n` / `tpl_params` are **garbage**
   there. Phase 3 must initialise them (or copy them from the AST)
   *before* reading them; reading `tpl_n` first is undefined behaviour.
   The parser side is already safe (`empty_type()`, `fresh()` and
   `push_method_record()` all initialise the new fields).
2. **The oracle does not build or type-check an uninstantiated template.**
   `buildSignature` returns early for a non-empty `tplParams`
   (`hir_builder.cpp:824`) and `buildStruct` does the same (`:552`), so the
   template's body is never checked. The port currently treats a template
   function like an ordinary one — that is why `C02`/`C08` differ under
   `--hir` today (the port reports `cannot return value of type 'T' …`).
   Phase 3 must skip functions with `tpl_n > 0` in the signature pass.
3. **The template-id arguments of a *non-template* struct are ignored by the
   oracle** (p79: `Box<int32>` on a plain `struct Box` keeps the type
   `Box`). The port keeps the arguments in the AST; phase 4 must drop them
   the same way when it resolves the type.
4. **A template-id is only produced for a *known* user type** (the oracle's
   `isTypeStart` check): `Unknown<int32> b;` is still a parse error
   (`unexpected token in expression`) — the port reproduces it.
5. **`AType.tpl_args` is not compared by the port's `same_type` yet.** The
   C++'s `ast::Type::operator==` *does* compare `tplArgs`; the port's
   `same_type` (in `hir_builder.ivy`) ignores them. Phase 3/4 must add the
   comparison, otherwise overload resolution differs from the oracle.
6. **The concept requirement texts are raw source text** (tokens joined by a
   single space), exactly like the C++'s `ast::ConceptDecl::requirements`.
   `checkConstraint` must look for the operator in that text; a
   member-access requirement (`a.foo()`) is not checked by the oracle.
7. The **parser** now rejects a nested template-id written as
   `Box<Box<int32>>` exactly like the oracle, but `Box<Box<int32> >` (with a
   space) parses in both and yields two nested argument lists.


---

## 15. Phase 3 findings (IMPLEMENTATION agent, phase 3 of 6)

**Date:** 2026-10-21. **Changed:** `selfhost/hir/hir_builder.ivy` (only),
this file. `selfhost/hir/hir.ivy` was **not** changed (the HIR model needed no
new field: the mangled name is just a `const char*` and the pack expansion is
handled entirely inside the call builder). Nothing under `src/**` was touched,
the oracle was not rebuilt, nothing was committed, and
`selfhost/parsing/**` was not touched.

**Scope:** FUNCTION template instantiation only. Struct/class templates and
concept checking are **phase 4** and are *not* implemented here (the two
fixtures that need them — `C05`, `C07` — still differ, see 15.8).

### 15.1 The algorithm as implemented

All of it lives in `selfhost/hir/hir_builder.ivy`.

**Registration (pass 1).** `build_free_signature` (line 2157) now takes the
AST function index. A function with `tpl_n > 0` is **not** registered in the
HIR function pool: it is put into the template registry
(`register_template`, line 5100) and its body is never type checked. A second
template with the same name **replaces** the first one (the oracle's
`templates_` is keyed by name). The body pass skips `tpl_n > 0` functions, so
an uninstantiated template produces **no** dump entry and **no** diagnostic.

**Instantiation on call.** `build_call` (line 3170) has two new paths for a
bare-name callee:

* *Template-id call* `f<T>(x)` — the explicit arguments are the AST argument
  types (no alias resolution, exactly like the oracle's `call.tplArgs`).
  The template is looked up by name; if it does not exist the port reports
  `undeclared identifier 'X'` (only when `X` is not a known function, symbol
  or enumerator — the oracle builds the callee expression in that case) and
  then `'X' is not a template`.
* *Implicit instantiation* `f(x)` — only when there is **no** ordinary
  overload for the name (an ordinary overload always wins) and the name is a
  template. The arguments are deduced and the specialization is built.

Both paths then run the ordinary call check (`check_call`: arity, missing
default arguments, argument types) and the `constexpr` folding on the
*instantiated* function.

**Mangling** (`mangle_args`, line 5184): `name<arg0,arg1>` where an argument
is `base` + `" unsigned"`? + `" const"`? + `"*"`×pointer depth; the
arguments are separated by a comma **without** a space. An empty pack gives
`name<>`. The name is the *template's* name (the port has no namespaces, so
the qualified form `ns::id<int32_t>` of p72 is out of reach — see 15.7).

**Substitution** (`substitute_type`, line 5220) mirrors `substituteType`:

* the base name is replaced when it is a template parameter;
* the pointer depth, the reference-ness and the const qualifier of the
  **declaration** are re-applied to the substituted type (so `T` deduced as
  `char const*` becomes a plain `char` — bug B5);
* the template arguments are substituted recursively and stored in a **new**
  list (the parser's argument list is immutable, so the port keeps its own
  chunked pool of substituted lists);
* only the **signature** (return type + parameter types) is substituted; the
  body is built **without** the mapping (bug B1 — the oracle does not set
  `currentTypeMapping_` for a function template).

**Deduction** (`deduce_template_args`, line 5254) mirrors `deduceTemplateArgs`:

* the only deduction is "the parameter type is exactly a type parameter" —
  there is no structural matching;
* the argument type is taken as it is, so an integer literal deduces `int`
  and **not** `int32_t` (bug B6): `identity(1)` and `identity<int32>(1)` are
  two different specializations;
* reference-ness is stripped; a `const T` parameter clears the const of the
  deduced type; a `T*` parameter deduces the pointee and fails on a pointer
  depth mismatch;
* the same type parameter deduced twice with different types fails the
  deduction;
* a non-type template parameter is never deduced (the deduction fails when
  the template has no type parameter at all);
* a variadic pack absorbs the arguments beyond the fixed parameters; the
  deduced list is built in the order of the template parameters.

**Explicit arguments.** The explicit arguments **replace** the deduction
(the oracle does not mix them): the mapping is built from the first
`min(#fixed-params, #args)` arguments. Extra arguments are silently accepted
and still appear in the mangled name (bug B7); too few arguments leave the
unmapped parameter's type as the literal parameter name (`U`) and the
subsequent call check reports it (bug "unmapped param name leaks").

**Variadic packs.** The pack parameter of the template expands into one
HIR parameter per pack element, named `__<funcParamName><index>`
(`__args0`, `__args1`; with `rest` → `__rest0`). The pack element types are
the argument types as they are (not substituted). An empty pack expands into
no parameter and the specialization is mangled `name<>`.

**De-duplication and ordering.** A specialization is cached by its mangled
name (`instantiated_`): a second use returns the cached function and does
**not** produce a second dump entry. The specialization is appended to the
HIR function pool *before* its body is built (so a recursive template
resolves itself) — therefore every instantiation lands **after** every
ordinary function, in first-instantiation order.

**Body building.** The body of the specialization is the template's body,
rebuilt with the substituted signature in scope. `build_body` (line 2255) was
changed so that it

* saves and restores the scope stack, the current function, the current
  return type and the `has_return` flag — a template instantiated from
  inside another function's body must not clobber its caller (this is what
  makes a nested instantiation possible at all);
* pushes the parameter scope **on top** of the caller's scope stack instead
  of resetting it, so the caller's scope markers stay intact;
* reports "may reach the end without returning a value" at the **call site**
  with the **mangled** name (the oracle uses `fn.loc`, which is the call
  site for an instantiated function).

### 15.2 The oracle behaviours that are reproduced (including the bugs)

| # | Behaviour | Where |
|---|---|---|
| B1 | A local declaration of type `T` is **not** substituted → `cannot initialize variable 'y' of type 'T' …` | `C10`, `F10`-style bodies |
| B2 | A template calling another template with an explicit argument does **not** substitute the argument (`id<T>(x)` instantiates `id<T>`) → `argument 1 of 'id<T>' expects 'T', got 'int32_t'` | `C11`, `F31` |
| B5 | The concrete type's const/pointer qualifiers are lost when substituting into a plain `T` parameter (`char const*` → `char`) | `F24`, `F36` |
| B6 | `T` deduced from an integer literal is `int`, not `int32_t` | `C03`, `F50`, `F51` |
| B7 | Extra template arguments are silently accepted; the mangled name contains them | `F12` |
| B8 | Two templates with the same name: the second silently replaces the first | `F28` |
| B10 | A pack expansion in a call (`f(args...)`) is broken: the pack name is not a scope name → `undeclared identifier 'args'` | `F47` |
| B11 | The type mapping is **not** propagated into the body of a function template (it is only propagated for *struct* templates — phase 4) | `C11`, `F31`, `F40` |
| B13 | A non-type template parameter is never bound → `undeclared identifier 'N'` in the body; an implicit call cannot deduce it | `F16`, `F17` |
| — | A template-id **parameter** type (`Box<T>`) is never resolved to the specialization — the port substitutes the base name and the argument list but does not resolve the specialization (the mismatch itself needs the phase-4 struct specialization) | phase 4 |
| — | The "may reach the end without returning a value" diagnostic of a specialization is reported at the call site with the mangled name | `F41` |
| — | The `constexpr` flag and the `extern "C"` flag are inherited by the specialization | `F27` |
| — | A template declaration without a body produces a specialization with *"(declaration only)"* | `F26` |
| — | An ordinary overload always wins over the template for an implicit call; a template-id call always uses the template | `F25` |

### 15.3 The diagnostics

| Situation | Exact message | Location |
|---|---|---|
| the name is not a template | `undeclared identifier 'X'` (only when `X` is not a function/symbol/enumerator) | the callee identifier |
| the name is not a template | `'X' is not a template` | the call |
| the deduction failed | `cannot deduce template arguments for 'X'` | the call |
| the instantiation was aborted (constraint — phase 4) | `failed to instantiate template 'X'` | the call |
| the instantiation was aborted in the deduction path (constraint — phase 4) | `failed to deduce template arguments for 'X'` | the call |
| the argument count does not match | `call to 'X<int32_t>' expects 1 argument(s), got 2` | the call |
| a missing argument has no default | `call to 'X<int32_t>' missing argument 2 (parameter 'y' has no default value)` | the call |
| the argument type does not match | `argument 1 of 'X<char const*>' expects 'char', got 'const char*'` | the argument |
| an unmapped template parameter | `argument 2 of 'first<int32_t>' expects 'U', got 'int'` | the argument |

The "undeclared identifier" of the *not a template* case is the only new
message shape: the oracle builds the callee expression (which reports the
undeclared identifier) before it reports that the name is not a template, so
**both** diagnostics appear, in that order.

As in the oracle, **any** diagnostic suppresses the whole HIR dump (the
driver prints the diagnostics only and exits 1) — the port's driver already
had that behaviour, no change was needed.

### 15.4 The phase-2 landmines that were fixed

| # | Landmine | Fix |
|---|---|---|
| L1 | `ty_empty()` left the new `AType.tpl_args` / `AType.tpl_n` fields uninitialised (`__ivy_alloc` is `malloc`) — every `AType` the port creates had a garbage `tpl_n` | `hir_builder.ivy:1168` — `ty_empty()` now sets `tpl_args = 0` and `tpl_n = 0`. Every `AType` the port builds goes through `ty_empty()` (directly or through `ty_int()`/`ty_void()`/…), so this one place covers all of them. |
| L2 | `get_expr()`'s fallback path built an `AExpr` without initialising `tpl_args` / `tpl_n` | `hir_builder.ivy:2144` — the fallback now sets them. |
| L3 | The local `AFunction` / `AField` copies (lines 976/1023/1202-1204/1234-1235/1409 of the old file) | **No change needed**: they are copies of the parser's records, and the parser initialises the new fields (`parse_type`, `empty_type`, `fresh`, `push_method_record`). Verified — the port reads `tpl_n` only on records that come from the parser. |
| L4 | `AType` in `ATemplateParam` (`ATemplateParam.type`) | **No change needed**: the parser's `parse_template_params` initialises it with `empty_type()`. |
| L5 | `same_type` ignored the template arguments (the C++'s `ast::Type::operator==` compares them) | `hir_builder.ivy:3761` — `same_type` now compares `tpl_n` and the argument lists recursively. |

### 15.5 Divergences from phase 1's notes

| # | Phase 1 said | Reality | Resolution |
|---|---|---|---|
| D1 | §2: "explicit template args at a call `f<3>(x)` (value) — **PARSE ERROR**" | **Not** a parse error: `f<3>(x)` parses as the comparison `f < 3 > (x)`, so the only diagnostic is `undeclared identifier 'f'` (p04) | The port reproduces the oracle (the parser is unchanged). The note was wrong. |
| D2 | §2: "`f<>(x)` (empty) — PARSE ERROR `unexpected token in expression`" | Confirmed (p19) — the port reproduces it (parser, phase 2). | — |
| D3 | §1 rule 1 / §3.1: "the template is never dumped" | Confirmed. The port now skips the template in the signature pass, so an uninstantiated template produces no entry and no diagnostic. | — |
| D4 | §6: "explicit args given → deduction is skipped entirely (no conflict check!)" | Confirmed. | — |
| D5 | §6: "a non-template overload with the same name exists → the non-template wins" | Confirmed (`F25`). The port only falls back to the template when there is **no** ordinary overload for the name. | — |
| D6 | §8 D1: the "not a template" case reports `undeclared identifier 'nosuch'` **and** `'nosuch' is not a template` | Confirmed, and the order matters: the undeclared identifier comes first and is at the **callee's** position, the "not a template" at the **call's** position. | Reproduced (`F15`). |
| D7 | §5: "the default argument `= <expr>` is not substituted" | The port never copies a default argument into the specialization, so the default is **lost** — a specialization of a template with a default argument has no default (`F37`, `F58` both match the oracle). | Reproduced. |
| D8 | §2: "`sizeof...(pack)` — supported" | The **oracle's parser** supports it; the **port's parser does not** (`expected '(' after 'sizeof'`). | **Blocked by the parser** — see 15.7. |
| D9 | §2: "fold expression `(args + ...)` — supported" | Same: the port's parser rejects it (`unexpected token in expression`). | **Blocked by the parser** — see 15.7. |
| D10 | §14.7.5: "`AType.tpl_args` is not compared by the port's `same_type` yet" | Fixed in phase 3 (L5). | — |

### 15.6 The pack features that are blocked by the parser

Two pack features of the oracle cannot be reproduced in phase 3 because the
**port's parser** does not accept the syntax (and `selfhost/parsing/**` is
out of scope for this phase):

| Feature | Oracle | Port (parser) | Phase-1 probe |
|---|---|---|---|
| `sizeof...(pack)` / `sizeof...(Args)` | folded to the pack size | `expected '(' after 'sizeof'` + 3 more | p62, p86, p87 |
| fold expressions `(args + ...)` | expanded to a left-nested binary chain | `unexpected token in expression` | p88 |

The pack expansion **in a call** (`f(args...)`) *is* accepted by the port's
parser (AST kind 17) and is implemented in phase 3: the pattern is built (so
the oracle's `undeclared identifier 'args'` is reproduced) and the call
splices one argument per pack element (`F47`).

Phase 4 (or a separate parser phase) should add `sizeof...(pack)` and the
fold expressions to `selfhost/parsing/parser.ivy`; the HIR side of
`sizeof...(pack)` is a one-liner once the parser produces the node (the
pack frame already knows the element count).

### 15.7 Out of scope for phase 3 (still failing)

| Fixture / probe | Why |
|---|---|
| `C05` (struct template with methods) | phase 4 (struct template instantiation) |
| `C07` (concept not satisfied) | phase 4 (concept checking) |
| p07, p12, p13, p17, p33, p34, p35, p37, p38d, p45, p48, p49, p60, p61, p75, p76, p78, p80, p82, p83, p91, t05, t06, t13, t2 | struct templates (phase 4) |
| p24, t08 | concepts (phase 4) |
| p26, p72 | **namespaces** — the port has no namespace support at all (neither in the parser nor in the HIR builder), so the qualified mangling `ns::id<int32_t>` is out of reach |
| p64, p65, p68, p69 | the **oracle crashes** (exit `-1073741819`, see §10.1) — a pre-existing oracle bug, not a template bug |
| p84, p66 | the oracle may crash (nested `constexpr` call in a binary expression) — not probed further |

### 15.8 The fixtures that were validated

The fixture set lives in
`C:\Users\ASUS\AppData\Local\Temp\opencode\h44f3\fix\` (67 files) and the
phase-1 probes were re-checked in
`C:\Users\ASUS\AppData\Local\Temp\opencode\h44f3\probes\` (115 files).
The comparison script is
`C:\Users\ASUS\AppData\Local\Temp\opencode\h44f3\cmp.ps1`; it runs the
oracle (`build\Release\ivy.exe --hir`) and the port (`hirdrive`) on every
file and compares stdout+stderr byte for byte.

**Result: 65 / 67 identical.** The two failures are `C05` and `C07`, both
phase-4 scope (15.7). The phase-1 probes: 78 / 115 identical, the 37
differences are exactly the cases listed in 15.7.

| fixture | rule it covers | result |
|---|---|---|
| `C01_type_param` | explicit `<int32>` | PASS |
| `C02_uninstantiated` | a template that is never instantiated → no entry | PASS |
| `C03_deduce` | implicit deduction → `identity<int>` | PASS |
| `C04_variadic` | the pack expands into `__args0`, `__args1` | PASS |
| `C05_struct_template` | struct template | **FAIL (phase 4)** |
| `C06_concept_ok` | a constrained parameter (the constraint is not checked in phase 3) | PASS |
| `C07_concept_fail` | the constraint check | **FAIL (phase 4)** |
| `C08_body_error_never_instantiated` | a body error in a template that is never instantiated → no diagnostic | PASS |
| `C09_order_dedup` | the dump order and the de-duplication | PASS |
| `C10_local_T_not_substituted` | bug B1 | PASS |
| `C11_template_calls_template` | bug B2 | PASS |
| `C12_if_constexpr` | `if constexpr` inside a template | PASS |
| `F04_deduce_variable` | the deduction from a variable (`int32_t`) | PASS |
| `F05_deduce_two_params` | two deduced parameters | PASS |
| `F06_deduce_const_ref` | the deduction through `const T&` | PASS |
| `F07_explicit_and_deduced` | two specializations of the same template | PASS |
| `F09_variadic_mixed` | a fixed parameter plus a pack | PASS |
| `F10_variadic_empty` | the empty pack → `count_args<>` | PASS |
| `F12_too_many_explicit` | bug B7 (extra arguments) | PASS |
| `F13_too_few_explicit` | the unmapped parameter name leaks (`U`) | PASS |
| `F14_deduce_fail` | `cannot deduce template arguments for 'id'` | PASS |
| `F15_unknown_template` | `undeclared identifier` + `'X' is not a template` | PASS |
| `F16_nontype_param` | bug B13 (the non-type parameter is never bound) | PASS |
| `F17_nontype_deduce_fail` | a template with only a non-type parameter cannot be deduced | PASS |
| `F20_recursive` | a recursive template (the specialization is registered before its body) | PASS |
| `F21_ptr_T` | the return type `T*` | PASS |
| `F22_unsigned_arg` | the `unsigned` mangling suffix | PASS |
| `F23_wrong_arg_count` | `expects 1 argument(s), got 2` | PASS |
| `F24_arg_type_mismatch` | the argument type mismatch + bug B5 | PASS |
| `F25_overload_with_plain` | an ordinary overload wins for an implicit call | PASS |
| `F26_prototype_only` | a template declaration without a body | PASS |
| `F27_constexpr_template` | a `constexpr` template (the call is folded) | PASS |
| `F28_two_templates_same_name` | bug B8 | PASS |
| `F29_deduce_array` | the deduction from an array argument | PASS |
| `F30_template_calls_normal` | a template calling an ordinary function | PASS |
| `F31_instantiated_calls_template` | bug B2 (explicit) | PASS |
| `F32_body_error_in_instantiated` | a body error in an instantiated template | PASS |
| `F33_export_template` | `export template <...>` | PASS |
| `F34_order_three_specs` | the dump order with three specializations | PASS |
| `F35_deduce_conflict` | the same parameter deduced twice with different types | PASS |
| `F36_deduce_string` | the deduction of a string (`char const*`) | PASS |
| `F37_default_arg_in_template` | a default argument in a template | PASS |
| `F38_mixed_deduce` | a fixed parameter plus a pack, deduced | PASS |
| `F39_deduce_char` | the deduction of a character literal | PASS |
| `F40_nested_template_two_levels` | a template instantiated from a template body | PASS |
| `F41_no_return` | "may reach the end" at the call site with the mangled name | PASS |
| `F42_ref_param` | a `T&` parameter | PASS |
| `F43_no_args_template` | a template without parameters | PASS |
| `F44_local_name_shadow` | a template-id call on a local variable | PASS |
| `F45_call_before_decl` | the template is used before its declaration | PASS |
| `F46_template_in_template_body` | a nested instantiation (the scope stack) | PASS |
| `F47_pack_call_expansion` | bug B10 | PASS |
| `F48_struct_arg` | the deduction from a struct argument | PASS |
| `F49_explicit_and_call_twice` | the de-duplication | PASS |
| `F50_deduce_bool` | the deduction of a boolean | PASS |
| `F51_deduce_double` | the deduction of a float | PASS |
| `F52_multiple_templates` | two different templates | PASS |
| `F53_template_in_method` | a template called from a method body | PASS |
| `F54_ptr_param_deduce` | the deduction through a `T*` parameter | PASS |
| `F55_const_param` | a `const T` parameter | PASS |
| `F56_alias_explicit_arg` | an alias as the explicit template argument | PASS |
| `F57_proto_never_instantiated` | a template declaration that is never used | PASS |
| `F58_deduce_with_default` | the deduction with a default argument | PASS |
| `F59_template_calls_template_deduced` | a nested *implicit* instantiation | PASS |
| `F60_same_name_two_specs` | two specializations of the same template | PASS |
| `F61_nested_call_in_instantiated` | a call inside an instantiated body | PASS |
| `F62_variadic_empty_deduced` | the empty pack → the mangled name `count_args<>` | PASS |


### 15.9 The per-suite results (all after the change)

| suite | files | result |
|---|---|---|
| hir/name | 27 | PASS |
| hir/type | 120 | PASS |
| hir/constexpr | 70 | PASS |
| parser/decl | 15 | PASS |
| parser/expr | 13 | PASS |
| parser/stmt | 15 | PASS |
| parser/aggregate | 14 | PASS |
| parser/template | 15 | PASS |
| parser/module | 16 | PASS |
| parser/err | 19 | PASS |
| lexer/ident, num, str, op, trivia, err | 10/8/7/5/7/9 | PASS |
| pp/define_obj, define_fn, cond, expand, predef, line, no_include | 10/12/16/16/20/22/9 | PASS |

### 15.10 Notes for the next phases

1. **The template registry and the instantiation cache live in
   `HirBuilder`**, not in the HIR model. Phase 4 adds the parallel
   `structTemplates_` registry and the struct specialization cache; the
   mangling is the same function (`mangle_args`), so the struct
   specialization should reuse it.
2. **The type mapping is a *local* variable of the instantiation** in the
   oracle. The port models this with a `base` index into the mapping array
   (`map_set`/`map_find` take the base) so that a nested instantiation does
   not disturb its caller. Phase 4 must introduce the *global*
   `currentTypeMapping_` (the oracle's) for **struct** instantiations: the
   port's `map_` is the local mapping of the function template and must
   **not** be visible in a body.
3. **The pack frame** (`push_pack_frame` / `pack_find`) mirrors the oracle's
   `currentPackMapping_`: it is a stack, so a nested instantiation pushes its
   own frame. The frame holds the template parameter name, the function
   parameter name, the element count and the index of the first element in
   the parameters of the function that is being built. Phase 4 must push a
   frame for a struct template as well (the oracle does not, but the port
   needs the element count for `sizeof...(pack)`).
4. **A large struct local inside a loop crashes the oracle's code
   generator.** The port's `build_call` originally declared an `AExpr`
   (168 bytes) in the argument loop; with two or more arguments the
   generated code crashed with an access violation. The `AExpr` local was
   moved into the helper methods `arg_pack_pattern` /
   `pack_expand_count`. **Keep large struct locals out of loops in the
   port's own source.**
5. **The oracle's `hir::Type::operator==` compares the template arguments**;
   the port's `same_type` now does too (L5). The oracle's `isAssignable`
   compares the base name only — the port keeps that behaviour.
6. **`AType` is 48 bytes** and the port's pools hard-code the stride
   (`sty_` 48, `map_type_` 48, the substituted argument pool 48). The
   phase-3 change did **not** alter the layout of `AType`, `AFunction`,
   `AExpr` or `AStmt`, so no stride had to be recomputed.
7. **The mangled name of a specialization is allocated in the string pool**
   of the builder (`sb_` + `strs_`); the pool is released in `release()`.
   The names of the synthesized pack parameters come from the same pool.

### 15.11 Appendix - the source of every phase-3 fixture

The `C01..C12` fixtures are the phase-1 ones (their source is in section 4
of this file and in the phase-1 corpus directory). The `F*` fixtures are new in phase 3:

```ivy
// F04_deduce_variable.ivy
template <typename T> T identity(T x) { return x; }
void f() { int32 a = 1; int32 b = identity(a); }

```

```ivy
// F05_deduce_two_params.ivy
template <typename T, typename U> T pair(T a, U b) { return a; }
void f() { int32 a = pair(1, 2); }

```

```ivy
// F06_deduce_const_ref.ivy
template <typename T> T id(const T& x) { return x; }
void f() { int32 a = id(1); }

```

```ivy
// F07_explicit_and_deduced.ivy
template <typename T> T identity(T x) { return x; }
void f() { int32 a = identity<int32>(1); int32 b = identity(2); }

```

```ivy
// F09_variadic_mixed.ivy
template <typename T, typename... Args> int32 count_after(T first, Args... rest) { return 0; }
void f() { int32 a = count_after<int32, int32>(10, 20); }

```

```ivy
// F10_variadic_empty.ivy
template <typename... Args> int32 count_args(Args... args) { return 0; }
void f() { int32 a = count_args<>(); }

```

```ivy
// F12_too_many_explicit.ivy
template <typename T> T identity(T x) { return x; }
void f() { int32 a = identity<int32, int32>(1); }

```

```ivy
// F13_too_few_explicit.ivy
template <typename T, typename U> T first(T a, U b) { return a; }
void f() { int32 r = first<int32>(1, 2); }

```

```ivy
// F14_deduce_fail.ivy
template <typename T> T id(T x) { return x; }
void f() { int32 a = id(); }

```

```ivy
// F15_unknown_template.ivy
void f() { int32 a = nosuch<int32>(1); }

```

```ivy
// F16_nontype_param.ivy
template <int32 N> int32 val(int32 x) { return x + N; }
void f() { int32 a = val<int32>(1); }

```

```ivy
// F17_nontype_deduce_fail.ivy
template <int32 N> int32 val(int32 x) { return x; }
void f() { int32 a = val(1); }

```

```ivy
// F20_recursive.ivy
template <typename T> T rec(T x) { return rec(x); }
void f() { int32 a = rec<int32>(1); }

```

```ivy
// F21_ptr_T.ivy
template <typename T> T* ptr(T x) { return &x; }
void f() { int32 a = ptr<int32>(1); }

```

```ivy
// F22_unsigned_arg.ivy
template <typename T> T id(T x) { return x; }
void f() { uint32 a = id<uint32>(1); }

```

```ivy
// F23_wrong_arg_count.ivy
template <typename T> T id(T x) { return x; }
void f() { int32 a = id<int32>(1, 2); }

```

```ivy
// F24_arg_type_mismatch.ivy
template <typename T> T identity(T x) { return x; }
void f() { int32 a = identity("hello"); }

```

```ivy
// F25_overload_with_plain.ivy
int32 a(int32 x) { return x; }
template <typename T> T a(T x) { return x; }
void f() { int32 r = a(1); int32 s = a<int32>(2); }

```

```ivy
// F26_prototype_only.ivy
template <typename T> T id(T x);
void f() { int32 a = id<int32>(1); }

```

```ivy
// F27_constexpr_template.ivy
template <typename T> constexpr T cst(T x) { return x; }
void f() { int32 a = cst<int32>(1); }

```

```ivy
// F28_two_templates_same_name.ivy
template <typename T> T a(T x) { return x; }
template <typename T> T a(T x, T y) { return x; }
void f() { int32 r = a<int32>(1); }

```

```ivy
// F29_deduce_array.ivy
template <typename T> int32 arr(T a) { return 0; }
void f() { int32 xs[4]; int32 a = arr(xs); }

```

```ivy
// F30_template_calls_normal.ivy
int32 helper(int32 x) { return x; }
template <typename T> T call_helper(T x) { return helper(x); }
void f() { int32 a = call_helper<int32>(1); }

```

```ivy
// F31_instantiated_calls_template.ivy
template <typename T> T id(T x) { return x; }
template <typename T> T outer(T x) { return id<T>(x); }
void f() { int32 a = outer<int32>(1); }

```

```ivy
// F32_body_error_in_instantiated.ivy
template <typename T> T broken(T x) { return undefined_name; }
void f() { int32 a = broken<int32>(1); }

```

```ivy
// F33_export_template.ivy
export template <typename T> T id(T x) { return x; }
void f() { int32 a = id<int32>(1); }

```

```ivy
// F34_order_three_specs.ivy
template <typename T> T id(T x) { return x; }
void a() { int32 v = id<int32>(1); }
void b() { int32 v = id<int64>(2); }
void c() { int32 v = id<int32>(3); }
void d() { int32 v = id<int32>(4); }

```

```ivy
// F35_deduce_conflict.ivy
template <typename T> T two(T a, T b) { return a; }
void f() { int32 a = two(1, 2); }

```

```ivy
// F36_deduce_string.ivy
template <typename T> T identity(T x) { return x; }
void f() { int32 a = identity("hello"); }

```

```ivy
// F37_default_arg_in_template.ivy
template <typename T> T withdefault(T x, int32 y = 5) { return x; }
void f() { int32 a = withdefault<int32>(1); }

```

```ivy
// F38_mixed_deduce.ivy
template <typename T, typename... Args> int32 count_after(T first, Args... rest) { return 0; }
void f() { int32 a = count_after(10, 20); }

```

```ivy
// F39_deduce_char.ivy
template <typename T> T identity(T x) { return x; }
void f() { char c = identity('a'); }

```

```ivy
// F40_nested_template_two_levels.ivy
template <typename T> T id(T x) { return x; }
template <typename T> T outer(T x) { return id<int32>(x); }
void f() { int32 a = outer<int32>(1); }

```

```ivy
// F41_no_return.ivy
template <typename T> T noret(T x) { }
void f() { int32 a = noret<int32>(1); }

```

```ivy
// F42_ref_param.ivy
template <typename T> T ref(T& x) { return x; }
void f() { int32 a = 1; int32 b = ref<int32>(a); }

```

```ivy
// F43_no_args_template.ivy
template <typename T> int32 zero() { return 0; }
void f() { int32 a = zero<int32>(); }

```

```ivy
// F44_local_name_shadow.ivy
template <typename T> T id(T x) { return x; }
void f() { int32 x = 1; int32 a = x<int32>(1); }

```

```ivy
// F45_call_before_decl.ivy
void f() { int32 a = later<int32>(1); }
template <typename T> T later(T x) { return x; }

```

```ivy
// F46_template_in_template_body.ivy
template <typename T> T id(T x) { return x; }
template <typename T> T outer(T x) { return id(x); }
void f() { int32 a = outer<int32>(1); }

```

```ivy
// F47_pack_call_expansion.ivy
void take(int32 x) { }
template <typename... Args> void pass(Args... args) { take(args...); }
void f() { pass<int32>(1); }

```

```ivy
// F48_struct_arg.ivy
struct P { int32 v; }
template <typename T> T id(T x) { return x; }
void f() { P p; P q = id(p); }

```

```ivy
// F49_explicit_and_call_twice.ivy
template <typename T> T id(T x) { return x; }
void f() { int32 a = id<int32>(1); int32 b = id<int32>(2); int32 c = id<int32>(3); }

```

```ivy
// F50_deduce_bool.ivy
template <typename T> T id(T x) { return x; }
void f() { bool a = id(true); }

```

```ivy
// F51_deduce_double.ivy
template <typename T> T id(T x) { return x; }
void f() { double a = id(1.5); }

```

```ivy
// F52_multiple_templates.ivy
template <typename T> T a(T x) { return x; }
template <typename T> T b(T x) { return x; }
void f() { int32 r = a<int32>(1); int32 s = b<int32>(2); }

```

```ivy
// F53_template_in_method.ivy
template <typename T> T g(T x) { return x; }
struct S { int32 f() { return g<int32>(1); } };
void h() { S s; int32 a = s.f(); }

```

```ivy
// F54_ptr_param_deduce.ivy
template <typename T> T* first(T* p) { return p; }
void f() { int32 v = 1; int32* p = &v; int32* q = first(p); }

```

```ivy
// F55_const_param.ivy
template <typename T> T id(const T x) { return x; }
void f() { int32 a = id<int32>(1); }

```

```ivy
// F56_alias_explicit_arg.ivy
using MyInt = int32;
template <typename T> T id(T x) { return x; }
void f() { int32 a = id<MyInt>(1); }

```

```ivy
// F57_proto_never_instantiated.ivy
template <typename T> T id(T x);
void f() { int32 a = 1; }

```

```ivy
// F58_deduce_with_default.ivy
template <typename T> T withdefault(T x, int32 y = 5) { return x; }
void f() { int32 a = withdefault(1); }

```

```ivy
// F59_template_calls_template_deduced.ivy
template <typename T> T id(T x) { return x; }
template <typename T> T outer(T x) { return id(x); }
void f() { int32 a = outer<int32>(1); }

```

```ivy
// F60_same_name_two_specs.ivy
template <typename T> T id(T x) { return x; }
void f() { int32 a = id<int32>(1); int32 b = id<int64>(2); int32 c = id<int32>(3); }

```

```ivy
// F61_nested_call_in_instantiated.ivy
template <typename T> T id(T x) { return x; }
template <typename T> T outer(T x) { int32 y = id<int32>(1); return x; }
void f() { int32 a = outer<int32>(1); }

```

```ivy
// F62_variadic_empty_deduced.ivy
template <typename... Args> int32 count_args(Args... args) { return 0; }
void f() { int32 a = count_args(); }
```

---

## 16. Phase 4 findings (IMPLEMENTATION agent, phase 4 of 6)

**Date:** 2026-10-21. **Changed:** `selfhost/hir/hir_builder.ivy` (only),
`selfhost/parsing/parser.ivy` (one 3-line location fix, see 16.9),
this file. `selfhost/hir/hir.ivy` and `selfhost/parsing/ast.ivy` were **not**
changed (the AST already carried the template data of phase 2 and the HIR
model needs no new field). Nothing under `src/**` was touched, the oracle was
not rebuilt, nothing was committed, and `selfhost/difftest.ps1` /
`PLAN.md` were not touched.

**Scope:** CLASS/STRUCT template instantiation and CONCEPTS (constraints).
Function templates were phase 3.

### 16.1 The class-template instantiation algorithm as implemented

All of it lives in `selfhost/hir/hir_builder.ivy`.

**Registration (pass 1b).** `build_structs` (line 2482) walks the AST
structs. A struct/class whose `tpl_n > 0` is **not** built: it is put into
the struct-template registry (`register_struct_template`, line 5914) and
its methods are never registered as HIR functions. A **second** template
with the same name **replaces** the first one (the oracle keys
`structTemplates_` by name) — no diagnostic (bug D15). A struct template
never occupies the struct registry, so a template and a plain struct may
share a name without a "redefinition of struct" diagnostic.

**Trigger points.** A specialization is created the moment a *template-id
type* is resolved, i.e. whenever `resolve_template_struct_type` runs on a
type that carries template arguments:

| # | Trigger | Oracle site |
|---|---|---|
| 1 | a local variable declaration `Box<int32> b;` | `buildDeclaration` (`hir_builder.cpp:1276`) |
| 2 | a field of a plain struct `struct H { Box<int32> b; }` | `buildStruct` (`:622`, `:635`, `:648`) |
| 3 | a field of a struct template | `instantiateStructTemplate` (`:5466`) |
| 4 | a function/method parameter or return type `void f(Box<int32> b)` | `buildSignature` (`:846`, `:865`), `buildStruct` (`:673`, `:687`) |
| 5 | a `using` alias target `using B = Box<int32>;` | the alias expansion, then `resolveTemplateStructType` |
| 6 | the *body-matching* pass (the AST side of the signature comparison) | `:4390`, `:4426` |

**Mangling.** `mangle_args` (the phase-3 function) is reused: the
specialization name is `<template name><arg0,arg1,...>` with the same
argument format as a function specialization (`base` + `" unsigned"`? +
`" const"`? + `"*"`×ptrDepth, comma-separated without a space). Examples:
`Box<int32_t>`, `Box<int32_t,int64_t>`, `Box<Box>` (the argument of a
nested template-id is mangled by its **base name only**, so
`Box<Box<int32> >` becomes `Box<Box>` — the inner specialization is not
resolved first).

**Substitution.** `instantiate_struct_template` (line 6200) mirrors
`instantiateStructTemplate` (`hir_builder.cpp:5414`):

* the mapping is built one-to-one: parameter *i* takes argument *i* for
  `i < min(#params, #args)`; a **non-type** parameter is never bound; a
  variadic parameter is **not** special — it takes the argument at its own
  index (the oracle maps one-to-one for structs as well);
* the **template name itself** is mapped to the specialization name, so a
  parameter of the template's own type is substituted;
* the field types are substituted (`substitute_type` + `resolve_alias`);
* the method return types and parameter types are substituted;
* the substitution replaces the pointer depth / reference / const of the
  *declaration* (bug B5) and **loses the template arguments** of the
  original type when the base name is a template parameter (the oracle's
  `resolveTypeAlias` returns the mapping entry, which has no `tplArgs`).

**Method / constructor naming.** The methods of the specialization are
named `<SpecName>::<bareMethodName>`; the constructor keeps the *original*
struct name, so it becomes `Box<int32_t>::Box` (`Holder<int32_t>::Holder`).
Overloaded methods keep the same qualified name and produce one dump entry
each (`Box<int32_t>::get` twice).

**`this`.** The implicit `this` parameter of a specialization method has
the type `<SpecName>&` (e.g. `Box<int32_t>& this`).

**Base classes are lost (bug B4).** The clone never copies the base list,
so the specialization has no bases: an inherited field is *not* found
(`struct 'Derived<int32_t>' has no member 'x'`), the base's methods are not
inherited and the base's constructor is not inherited either. Reproduced
faithfully (T03, T28, T29).

**`sizeof` / `alignof` of a template-id = 4 (bug B3).** `sizeof(Box<int32>)`
and `alignof(Box<int32>)` fold to **4**, because the *type* `Box<int32>` is
never resolved to the specialization before the size is computed (the
template name is not in the struct registry). `sizeof` of a *variable* of
the specialization type does use the real layout. Reproduced (T07).

**The type mapping IS active during a struct instantiation (bug B11, the
other half).** Unlike a function template, the mapping of a struct
template is installed while the fields, the method signatures *and* the
method bodies are built (`gmap_base_`, the port's `currentTypeMapping_`).
Consequences:

* a local declaration of type `T` **inside a method of a specialization**
  *is* substituted (`T local = ...` becomes `int32_t local = ...`);
* the explicit template arguments of a template-id **call** inside such a
  method are substituted (`id<T>(x)` instantiates `id<int32_t>`), while the
  same call inside a *function* template instantiates `id<T>` (bug B2).

**De-duplication and ordering.** A specialization is cached by its mangled
name; a second use reuses it and produces no second dump entry. The
specialization's methods are appended to the HIR function pool at the
moment the specialization is created, therefore:

* a specialization created while a **function signature** is built comes
  *before* that function (`Box<int32_t>::get` then `f`, T10);
* a specialization created while a **struct field** is laid out (pass 1b)
  comes before every free function (T08);
* a specialization created in a **function body** (pass 2) comes after
  every ordinary function;
* inside one specialization: the signatures of *all* methods are created
  first, then the bodies are built — so an instantiation triggered by the
  first method's body lands after the second method's signature (T05).

**Registration of the specialization.** The specialization is entered into
the struct registry under its mangled name (so `find_struct` finds it and
the field/method lookups work) and into the instantiation cache (so a
recursive use does not re-instantiate).

### 16.2 The concept-satisfaction rules (derived empirically)

`check_constraint` (line 6120) mirrors `checkConstraint`
(`hir_builder.cpp:4835`):

1. the check runs **only at instantiation time** — a concept that is never
   checked never fails;
2. only **type** template parameters carry a constraint. The oracle's
   parser can only attach a concept name to the `Concept T` form, which is
   always a *type* parameter (`parser.cpp:1660`), so a **non-type**
   parameter can never carry a constraint and is never checked;
3. an **unknown concept name is assumed to be satisfied** (bug B12) — the
   parameter is instantiated happily (K03);
4. the concept's requirements are the **raw source text** of the
   `requires` body, one text per `;`-terminated requirement;
5. for every requirement the checker looks for a **binary operator** in
   the text: the two-character operators `==`, `!=`, `<=`, `>=` are looked
   for **first**, over the whole text; only if none is found the
   single-character operators `+ - * / < >` are looked for (a `-` whose
   predecessor is `-` is skipped);
6. if an operator is found, the concrete type must be **arithmetic**:
   `pointerDepth == 0` and (the base name ends with `_t`, or it is one of
   `char bool int long short unsigned signed float double`). `bool`
   satisfies `a + b` (K05), `size_t`/`nullptr_t` satisfy it too (they end
   with `_t`);
7. a **non-arithmetic** type (a struct, an enum) never satisfies an
   operator requirement: the oracle looks for a function whose name
   **starts with** `operator` and whose first two parameters are the
   concrete type. A *member* operator overload is named `P::operator+` and
   therefore does **not** start with `operator`; a free `operator+` is not
   parseable. So **no struct type ever satisfies an operator
   requirement** (K02, K04, K08);
8. a requirement without a binary operator (a member access `a.bar()`, a
   `sizeof(a)`, anything else) is **not checked at all** and is always
   satisfied (K06, K07);
9. every requirement is checked; the first failing one aborts the
   instantiation (K11 — two requirements, both satisfied by `int32`).

The concept's *parameter name* (`cd.paramName`) is never used by the
checker.

### 16.3 The diagnostics

| Situation | Exact oracle text | Location | Fixture |
|---|---|---|---|
| an operator requirement is not satisfied | `concept 'Addable' not satisfied: type 'P' does not support operator '+'` | the instantiation site | K02, K08 |
| … then the function template | `failed to instantiate template 'add'` | the instantiation site | K02, K10 |
| … then the deduced function template | `failed to deduce template arguments for 'add'` | the instantiation site | K10 |
| … then the *struct* template | **no** second diagnostic — the type falls back to the **unmangled** template name, which then triggers `variable 'b' must be initialized (uninitialized variables are not allowed)` | the declaration | K08 |
| the name is not a template at all | `undeclared identifier 'X'` + `'X' is not a template` | the callee / the call | phase 3 |
| an unknown template name in a *type* position | a **parse** error (`unexpected token in expression`) — the oracle only builds a template-id for a known user type | — | phase 2 |
| a template-id on a plain (non-template) struct | **no diagnostic** — the arguments are ignored, the type stays `Box` | — | — |
| too many / too few template arguments | **no diagnostic** — the extra arguments are silently accepted, the unmapped parameter keeps its literal name (`U`) | — | T22, T23 |
| redefinition of a specialization | **no diagnostic** — the second template silently replaces the first | — | T11, T25 |

The "not satisfied" message prints the concrete type the way
`typeNameStr` does: the base name, then `" unsigned"`, then `" const"`,
then the pointer stars (no `const`/`unsigned` prefix).

**The "no HIR dump at all" rule** holds: any diagnostic suppresses the
whole dump (the driver prints the diagnostics only and exits 1). Every
failing fixture above produces *only* diagnostics.

### 16.4 The struct-template diagnostics that do **not** exist

* an unknown struct template in a type position is a *parse* error, not a
  HIR diagnostic;
* a specialization is never "redefined" — the cache returns the first one;
* the number of template arguments is never checked.

### 16.5 The fixtures that were validated

Fixture directory: `C:\Users\ASUS\AppData\Local\Temp\opencode\h44f4\fix\`
(56 files). The comparison script is
`C:\Users\ASUS\AppData\Local\Temp\opencode\h44f4\cmp.ps1` (it runs the
oracle `ivy --hir` and the port `hirdrive` on every file and compares
stdout+stderr byte for byte).

**Result: 56 / 56 identical.**

| fixture | rule it covers | result |
|---|---|---|
| `C01_type_param` | explicit `<int32>` (phase 3) | PASS |
| `C02_uninstantiated` | a template that is never instantiated | PASS |
| `C03_deduce` | implicit deduction → `identity<int>` | PASS |
| `C04_variadic` | the pack expands into `__args0`, `__args1` | PASS |
| `C05_struct_template` | **the struct template** (phase-4 scope) | PASS |
| `C06_concept_ok` | a constrained parameter | PASS |
| `C07_concept_fail` | **the constraint check** (phase-4 scope) | PASS |
| `C08_body_error_never_instantiated` | a body error in an uninstantiated template | PASS |
| `C09_order_dedup` | the dump order and the de-duplication | PASS |
| `C10_local_T_not_substituted` | bug B1 (a function template) | PASS |
| `C11_template_calls_template` | bug B2 (a function template) | PASS |
| `C12_if_constexpr` | `if constexpr` inside a template | PASS |
| `T01_struct_field` | a field of type `T` | PASS |
| `T02_struct_methods` | the methods of a specialization | PASS |
| `T03_struct_base` | the base class is lost | PASS |
| `T04_class_ctor` | `class` + a constructor (`Holder<int32_t>::Holder`) | PASS |
| `T05_struct_calls_template` | a method calls `id<T>(...)` → `id<int32_t>` | PASS |
| `T06_method_call` | `b.get()` on a specialization | PASS |
| `T07_sizeof_alignof` | `sizeof`/`alignof` of a template-id → 4 | PASS |
| `T08_field_in_struct` | a template-id as a field of a plain struct (pass 1b) | PASS |
| `T09_param_type` | a template-id as a parameter type | PASS |
| `T10_param_type_order` | the specialization precedes the function | PASS |
| `T11_two_templates` | two templates with the same name → the second wins | PASS |
| `T12_dedup` | a repeated use → one entry | PASS |
| `T13_order` | the order of two specializations | PASS |
| `T14_nested_spec` | `Box<Box<int32> >` → `Box<Box>` | PASS |
| `T15_local_T` | a local `T` **is** substituted in a struct method | PASS |
| `T16_virtual` | a virtual method in a struct template | PASS |
| `T17_field_template_id` | a field whose type is a template-id | PASS |
| `T18_return_type` | a template-id as a return type | PASS |
| `T19_alias` | `using MyBox = Box<int32>;` | PASS |
| `T20_overloaded_methods` | two methods with the same name | PASS |
| `T21_nontype_param` | a non-type parameter is never bound | PASS |
| `T22_too_few_args` | too few arguments → no diagnostic | PASS |
| `T23_too_many_args` | too many arguments → no diagnostic | PASS |
| `T24_variadic` | a variadic struct template | PASS |
| `T25_redef_template` | the redefinition of a template → no diagnostic | PASS |
| `T26_pack_in_struct` | a pack expansion in a struct-template method | PASS |
| `T27_own_type_field` | a field of the template's own type | PASS |
| `T28_base_field_lost` | an inherited field is not found | PASS |
| `T29_base_ctor_not_inherited` | the base's constructor is not inherited | PASS |
| `T30_pass_specialization` | a specialization passed as an argument | PASS |
| `T31_template_and_plain_struct` | a template and a plain struct with the same name | PASS |
| `T32_plain_struct_and_template` | the same, the other way round | PASS |
| `K01_concept_ok` | an arithmetic type satisfies `a + b` | PASS |
| `K02_concept_fail` | a struct type does not satisfy `a + b` | PASS |
| `K03_unknown_concept` | an unknown concept is satisfied | PASS |
| `K04_concept_eq` | the `==` requirement | PASS |
| `K05_concept_bool` | `bool` is arithmetic | PASS |
| `K06_concept_member` | a member-access requirement is not checked | PASS |
| `K07_concept_sizeof` | a `sizeof` requirement is not checked | PASS |
| `K08_struct_concept_fail` | a struct template with a failing constraint | PASS |
| `K09_struct_concept_ok` | a struct template with a satisfied constraint | PASS |
| `K10_concept_deduce_fail` | the deduced path → "failed to deduce …" | PASS |
| `K11_two_requirements` | two requirements | PASS |
| `K12_concept_in_method` | the check inside a struct-template method | PASS |

The phase-3 fixture set (`h44f3/fix`, 67 files) is **67 / 67** identical
after the change, and the phase-1 probe set (`h44f3/probes`, 115 files)
went from 78/115 to **105/115** — the 10 remaining differences are the
known gaps of 16.8 (namespaces, `sizeof...(pack)`, fold expressions) and
the four inputs on which **the oracle crashes** (§10.1).

The four real examples of the repository were also checked:
`examples/test_template_struct.ivy`, `examples/test_concepts.ivy`,
`examples/test_concept_fail.ivy` and `examples/test_template_deduction.ivy`
are byte-identical; `examples/test_variadic.ivy` differs because of the
`sizeof...(pack)` / fold-expression parser gap (16.8).

### 16.6 Divergences from the phases 1–3 notes

| # | Phase 1–3 said | Reality | Resolution |
|---|---|---|---|
| D1 | §14 rule 14: "the oracle **loses base classes** on specializations (reproduce that bug) — verify empirically whether base substitution happens at all and what the oracle prints" | Confirmed: the base list is dropped entirely; an inherited field is reported as missing (`struct 'Derived<int32_t>' has no member 'x'`), the base's methods and constructor are not inherited | Reproduced (T03, T28, T29) |
| D2 | §14 rule 14: "`sizeof`/`alignof` of a template-id = **4** (oracle fallback)" | Confirmed: `sizeof(Box<int32>)` = 4, `alignof(Box<int32>)` = 4. `sizeof` of a *variable* of the specialization type uses the real layout | Reproduced (T07) |
| D3 | §1 rule 5 / §3.1: "the struct specialization name = the same mangling as functions" | Confirmed, including the quirk that a nested template-id argument is mangled by its base name only (`Box<Box>`) | Reproduced (T14) |
| D4 | §8 D9: "the parameter type `Box<T>` is never resolved to the specialization ⇒ every call mismatches" (bug B9) | Confirmed for **function** templates: `instantiateTemplate` does not resolve the parameter types. A **struct** template *does* resolve them (its parameter types go through `resolveTemplateStructType`) | Reproduced (phase 3 for functions, phase 4 for structs) |
| D5 | §10.2 B11: "the `currentTypeMapping_` is set during *struct* instantiation but not during *function* instantiation" | Confirmed **and** extended: the mapping of a struct template is also applied to the explicit template arguments of a template-id **call** made inside one of its methods (`id<T>(x)` → `id<int32_t>`) | Reproduced (T05) |
| D6 | §8 D15: "redefinition of a specialization → *no diagnostic*" | Confirmed for struct templates (the second template replaces the first). The port's duplicate-struct check now **skips** the struct templates, so a template and a plain struct may share a name | Reproduced (T11, T25, T31, T32) |
| D7 | §8 D12: "a template-id on a plain struct in a type position → the arguments are ignored" | Confirmed: `Box<int32>` on a plain `struct Box` keeps the type `Box` | Reproduced (phase 2/3) |
| D8 | §7: "an unknown concept name is treated as satisfied" | Confirmed, and a **struct** type never satisfies an operator requirement (a member `operator+` is named `P::operator+` and does not start with `operator`) | Reproduced (K02, K03) |
| D9 | §13: "the behaviour of a `using X = Box<int32>;` alias" (unknown) | Now known: the alias target keeps its template arguments, the alias expands to the template-id and the specialization is created at the use site (`decl Box<int32_t> b`) | Reproduced (T19) |
| D10 | §13: "the mangled name of a struct template declared inside a namespace" (unknown) | Still unknown — the port has no namespace support at all (phase 3, 15.7) | Open |
| D11 | §5: "the default argument of a template parameter is substituted" (unknown) | The oracle has no default template arguments (the parser rejects them), so the question does not arise | — |
| D12 | §2: "a concept with a non-type parameter" | The oracle's parser cannot attach a concept to a non-type parameter, so the constraint of a non-type parameter is **never** checked | Confirmed |

### 16.7 What the port had to change (and why)

* `find_struct` (line 1648) now **skips** the struct templates (they are
  not structs) and also finds the specializations. The struct index space
  is now *global*: `0..n-1` are the AST structs, `n..n+k-1` are the
  specializations; `get_struct` (1673), `struct_size` (1717),
  `struct_align` (1733) and `struct_poly` (1749) dispatch on it. This is
  what makes `sizeof` of a *variable* of a specialization type work and
  what makes `sizeof(Box<int32>)` fall back to 4.
* `resolve_alias` now applies the **active** type mapping first (the
  oracle's `currentTypeAlias`/`currentTypeMapping_` order): the mapping
  is the port's `map_` stack, and `gmap_base_` is the base index of the
  currently active entry range.
* `build_free_signature` (2395) and `build_method_signature` (2443) now
  resolve the return type and the parameter types **before** the
  parameters are pushed and before the function is added to the pool. The
  oracle resolves them before `hir_->functions.push_back`, so a
  specialization created by a template-id parameter type is dumped
  *before* the enclosing function. The parameters are pushed as one
  contiguous run (`add_function_at`, 2363, takes the parameter index
  explicitly) because a nested instantiation pushes its own parameters
  into the same pool.
* `build_call` now substitutes the explicit template arguments of a
  template-id call with the active mapping (the oracle's
  `hir_builder.cpp:3047`) and **drops** the argument of a pack expansion
  whose pack is unknown (the oracle pushes no argument for it) — the
  latter was a phase-3 inaccuracy that a struct template exposes
  (T26).
* `instantiate_template` (the phase-3 function) now checks the concept
  constraints of the type parameters and returns -1 when a constraint
  fails, so that the caller reports `failed to instantiate template 'X'`.

### 16.8 The gaps that remain (not phase-4 scope)

The fixtures live in `C:\Users\ASUS\AppData\Local\Temp\opencode\h44f4\gaps\`.

| # | fixture | why it differs | phase |
|---|---|---|---|
| G1 | `G01_ctor_call_explicit.ivy` | `Box<int32> b = Box<int32>(1);` — the oracle resolves the **constructor** of the specialization (and injects the variable as `this`). The port has **no constructor support at all** (no ctor overload resolution, no ctor injection, no member initializer lists) — that is a general struct feature, not a template one | H4.5+ |
| G2 | `G02_tplid_call_no_ctor.ivy` | `Box<int32>(1)` where the specialization has **no** constructor: the oracle creates the specialization and leaves the call without a target (the call's type stays the dummy `int`, so only the "cannot initialize variable" diagnostic appears). The port reports `undeclared identifier 'Box'` + `'Box' is not a template` because it looks for a *function* template | H4.5+ (needs the ctor list of a specialization) |
| G3 | `G04_sizeof_pack.ivy` | `sizeof...(pack)` — the **parser** does not accept it | phase 3 gap (15.6) |
| G4 | `G05_fold_expr.ivy` | the fold expression `(args + ...)` — the **parser** does not accept it | phase 3 gap (15.6) |
| G5 | `G06_namespace.ivy` | `ns::id<int32>` — the port has no namespace support | phase 3 gap (15.7) |

`G03_ctor_call_style_init.ivy` (`Box<int32> b(1);`) is byte-identical
after the parser fix of 16.9 and is **not** a gap any more.

### 16.9 The one parser change (and its justification)

`selfhost/parsing/parser.ivy`, the constructor-style initializer
(`Type name(args)`): the **synthesized callee** of the call was created
without a location (`fresh()` leaves `line`/`col` at 0). The oracle sets
it to the location of the first token of the argument list
(`parser.cpp:2632`: `callee->loc = locOf(peek())`), so the diagnostics of
such a call were reported at `0:0` instead of the real position.

The fix is three lines:

```ivy
            AExpr callee = this.fresh(8);
            // The synthesized callee is located at the first token of the
            // argument list (the oracle's `locOf(peek())`).
            callee.line = this.peek(0).line;
            callee.col = this.peek(0).col;
```

The `--ast` dump does not print any location, so it stays
byte-identical (all seven parser suites still pass, 107 files).

The change was needed because a class template with a constructor is the
canonical user of that syntax, and the diagnostic position of the
resulting call is part of the byte-comparison.

### 16.10 The per-suite results (all after the change)

| suite | files | result |
|---|---|---|
| hir/name | 27 | PASS |
| hir/type | 120 | PASS |
| hir/constexpr | 70 | PASS |
| parser/decl | 15 | PASS |
| parser/expr | 13 | PASS |
| parser/stmt | 15 | PASS |
| parser/aggregate | 14 | PASS |
| parser/template | 15 | PASS |
| parser/module | 16 | PASS |
| parser/err | 19 | PASS |
| lexer/ident, num, str, op, trivia, err | 10/8/7/5/7/9 | PASS |
| pp/define_obj, define_fn, cond, expand, predef, line, no_include | 10/12/16/16/20/22/9 | PASS |
| phase-4 fixtures (`h44f4/fix`) | 56 | PASS |
| phase-3 fixtures (`h44f3/fix`) | 67 | PASS |
| phase-1 probes (`h44f3/probes`) | 115 | 105 PASS / 10 known gaps |

### 16.11 Notes for phase 5 (the corpus)

* The official corpus for H4.4 can be built from
  `C:\Users\ASUS\AppData\Local\Temp\opencode\h44f4\fix\` (56 files, all
  byte-identical). The `C0x` files are the phase-1 corpus (they also
  cover the phase-3 function-template rules), the `Txx` files the class
  templates and the `Kxx` files the concepts.
* The files of `C:\Users\ASUS\AppData\Local\Temp\opencode\h44f4\gaps\`
  must **not** go into the corpus (G1/G2 need the constructor feature,
  G3/G4 need the parser extensions of phase 3, G5 needs namespaces).
* The oracle crashes on four of the phase-1 probes (`p64`, `p65`, `p68`,
  `p69` — §10.1); they must not go into the corpus either.
* The corpus directory suggested by the plan is
  `selfhost/corpus/hir/template/` (phase 5 owns it; phase 4 did **not**
  create it).

---

## 17. Phase 5 findings (CORPUS agent, phase 5 of 6)

**Date:** 2026-10-21. **Created:** `selfhost/corpus/hir/template/` (95 files
in phase 5; 99 after the phase-6 verification added the four regression
files of §18.2).
**Changed:** `selfhost/difftest.ps1` (the header comment only), `PLAN.md`,
this file. `selfhost/hir/hir_builder.ivy`, `selfhost/hir/hir.ivy` and
`selfhost/parsing/**` were **not** touched — the implementation of phases
2–4 was already byte-identical, so no defect had to be fixed. Nothing under
`src/**` was touched, the oracle was not rebuilt, nothing was committed.

### 17.1 The corpus

`selfhost/corpus/hir/template/` — the official H4.4 corpus. The files are
the validated phase-1/3/4 fixtures (C/F/T/K) merged, deduplicated and
renamed; the numbering groups them by topic:

| prefix | topic | files |
|---|---|---|
| `01_` | type-parameter templates and deduction | 16 |
| `02_` | explicit template arguments | 5 |
| `03_` | variadic packs | 5 |
| `04_` | overload/template precedence, recursion, prototypes, `constexpr`, order | 14 |
| `05_` | class/struct templates | 31 |
| `06_` | concepts | 9 |
| `e_` | error cases (each triggers its intended diagnostic) | 19 |

Every file is self-contained (no `#include`, no path dependency) and covers
**one** rule. Every file starts with a comment that names the rule.

#### 17.1.1 `01_` — type-parameter templates and deduction

| file | rule it covers |
|---|---|
| `01_type_param_explicit.ivy` | a type parameter with an explicit `<int32>` argument → the specialization `identity<int32_t>` (§1 rule 2) |
| `01_never_instantiated.ivy` | a template that is never instantiated produces **no** dump entry (§1 rule 1) |
| `01_body_error_never_instantiated.ivy` | a body error is only reported when the template is instantiated (§1 rule 6) |
| `01_deduce_literal.ivy` | implicit deduction: an int literal deduces `int`, **not** `int32_t` (bug B6) |
| `01_deduce_variable.ivy` | deduction from a variable → `int32_t` |
| `01_deduce_const_ref.ivy` | deduction through `const T&` (the reference is stripped) |
| `01_deduce_ref.ivy` | a `T&` parameter keeps the reference in the specialization |
| `01_deduce_char.ivy` | a character literal deduces `char` |
| `01_deduce_bool.ivy` | a boolean literal deduces `bool` |
| `01_deduce_double.ivy` | a floating-point literal deduces `double` |
| `01_deduce_string.ivy` | a string literal deduces `char const*`; the substitution into a plain `T` drops the const and the pointer qualifiers ⇒ the argument mismatch (bug B5) |
| `01_deduce_array.ivy` | an array argument deduces the element type |
| `01_deduce_pointer.ivy` | a `T*` parameter deduces the pointee; the parameter of the specialization is `int32_t*` |
| `01_deduce_two_params.ivy` | two type parameters are deduced independently (`pair<int,int>`) |
| `01_deduce_struct.ivy` | a struct argument deduces the struct type (`id<P>`) |
| `01_const_param.ivy` | a `const T` parameter: the const comes from the *declaration*, not from the argument |

#### 17.1.2 `02_` — explicit template arguments

| file | rule it covers |
|---|---|
| `02_explicit_overrides_deduction.ivy` | an explicit argument and a deduced call produce **two** specializations (`identity<int32_t>` and `identity<int>`) |
| `02_too_many_args.ivy` | extra explicit arguments are silently accepted and appear in the mangled name (bug B7) |
| `02_too_few_args.ivy` | too few explicit arguments: the unmapped parameter keeps its literal name `U` (§8 D14) |
| `02_unsigned_arg.ivy` | the `" unsigned"` suffix of the argument mangling (§3.1) |
| `02_no_arguments.ivy` | a template without function parameters, instantiated with an explicit argument |

The "unknown template" scenario of this group is covered by the error
files of §17.1.7: `e_undeclared_identifier.ivy` (the name does not exist
at all) and `e_not_a_template.ivy` (the name exists but is not a
template). They are not duplicated here because the *rule* they pin is
the diagnostic, not the argument syntax.

#### 17.1.3 `03_` — variadic packs

| file | rule it covers |
|---|---|
| `03_pack_expansion.ivy` | the pack expands into one parameter per element, named `__args0`, `__args1` (§1 rule 3) |
| `03_empty_pack_deduced.ivy` | an empty pack is mangled `count_args<>` and the specialization has **no** parameter |
| `03_mixed_pack.ivy` | a fixed parameter followed by a pack (explicit arguments) |
| `03_mixed_pack_deduced.ivy` | the same template with the pack deduced from the call arguments |
| `03_pack_call_expansion.ivy` | a pack expansion in a call: the pack name is not a scope name ⇒ `undeclared identifier 'args'` (bug B10) |

#### 17.1.4 `04_` — overloads, recursion, prototypes, ordering

| file | rule it covers |
|---|---|
| `04_overload_wins.ivy` | an ordinary overload wins for an implicit call; a template-id call always uses the template (§6) |
| `04_recursive.ivy` | a recursive template (the specialization is registered before its body is built) |
| `04_prototype_only.ivy` | a template declared without a body ⇒ the specialization is dumped as `(declaration only)` |
| `04_prototype_never_instantiated.ivy` | a template declaration that is never used produces no entry |
| `04_constexpr_template.ivy` | a `constexpr` template: the call is folded |
| `04_same_name_templates.ivy` | two templates with the same name: the second replaces the first (bug B8) |
| `04_order_and_dedup.ivy` | the dump order and the de-duplication (§1 rule 4) |
| `04_two_specializations.ivy` | two specializations of one template; the repeated one is emitted once |
| `04_if_constexpr.ivy` | `if constexpr`: the discarded branch is not checked (`sizeof(T)` folds to 4) |
| `04_export_template.ivy` | `export template <...>` |
| `04_nested_instantiation.ivy` | a template instantiated from the body of another template |
| `04_template_calls_function.ivy` | a template calling an ordinary function |
| `04_template_in_method.ivy` | a template called from the body of a method |
| `04_call_before_declaration.ivy` | a template used before its declaration still instantiates |

#### 17.1.5 `05_` — class/struct templates

| file | rule it covers |
|---|---|
| `05_struct_field.ivy` | a field of the template parameter type |
| `05_struct_methods.ivy` | the methods of a specialization: `Box<int32_t>::get`, `this` is `Box<int32_t>&` (§1 rule 5) |
| `05_class_ctor.ivy` | a class template with a constructor ⇒ `Holder<int32_t>::Holder` |
| `05_base_dropped.ivy` | the base list of a specialization is dropped (bug B4) |
| `05_base_field_lost.ivy` | an inherited field is not found (bug B4) |
| `05_base_ctor_not_inherited.ivy` | the base constructor is not inherited (bug B4) |
| `05_explicit_too_few_args.ivy` | too few explicit arguments on a struct template |
| `05_explicit_too_many_args.ivy` | too many explicit arguments on a struct template |
| `05_nested_specialization.ivy` | `Box<Box<int32> >` is mangled `Box<Box>` (the argument is mangled by its base name only) |
| `05_template_redefinition.ivy` | the redefinition of a struct template: **no** diagnostic (§8 D15) |
| `05_template_and_plain_struct.ivy` | a struct template and a plain struct with the same name may coexist |
| `05_template_id_on_plain_struct.ivy` | a template-id on a plain struct: the arguments are ignored (§8 D12) |
| `05_method_call.ivy` | a method call on a specialization |
| `05_local_type_param.ivy` | a local `T` **is** substituted inside a struct-template method (bug B11, the other half) |
| `05_struct_calls_template.ivy` | a method calls `id<T>(...)` ⇒ `id<int32_t>` (bug B11) |
| `05_sizeof_template_id.ivy` | `sizeof`/`alignof` of a template-id = 4 (bug B3) |
| `05_field_in_struct.ivy` | a template-id as a field of a plain struct (pass 1b) |
| `05_param_type.ivy` | a template-id as a parameter type |
| `05_param_type_order.ivy` | a specialization created while a signature is built precedes that function |
| `05_return_type.ivy` | a template-id as a return type |
| `05_alias.ivy` | `using MyBox = Box<int32>;` — the alias expands to the template-id |
| `05_overloaded_methods.ivy` | two methods with the same name in a specialization |
| `05_nontype_param.ivy` | a non-type parameter of a struct template is never bound (bug B13) |
| `05_variadic.ivy` | a variadic struct template (the arguments are mapped one to one) |
| `05_own_type_field.ivy` | a field of the template's own type |
| `05_pass_specialization.ivy` | a specialization passed as an argument |
| `05_dedup.ivy` | a specialization that is used twice is emitted once |
| `05_order.ivy` | two specializations in creation order |
| `05_struct_template_wins_over_function_template.ivy` | a name that is both a struct template and a function template: the **struct template** wins for a template-id call (phase-6 regression, §18.2 D1) |
| `05_specid_call_without_constructor.ivy` | a template-id call on a struct template without a constructor: the specialization is created and the call stays unresolved (phase-6 regression, §18.2 D1) |
| `05_duplicate_template_name_implicit_call.ivy` | the same name without an explicit template-id: the **function template** is used (phase-6 regression, §18.2 D1) |

#### 17.1.6 `06_` — concepts

| file | rule it covers |
|---|---|
| `06_concept_satisfied.ivy` | an arithmetic type satisfies `a + b` |
| `06_concept_bool.ivy` | `bool` is arithmetic |
| `06_unknown_concept.ivy` | an unknown concept name is assumed to be satisfied (bug B12) |
| `06_concept_equality.ivy` | the `==` requirement: a struct type does **not** satisfy it (no free `operator==`) |
| `06_concept_member_unchecked.ivy` | a requirement without a binary operator (a member access) is not checked at all |
| `06_concept_sizeof_unchecked.ivy` | a `sizeof` requirement is not checked at all |
| `06_concept_two_requirements.ivy` | a concept with two requirements: every requirement is checked |
| `06_struct_concept_ok.ivy` | a struct template with a satisfied constraint |
| `06_concept_in_method.ivy` | the constraint check inside a struct-template method |

#### 17.1.7 `e_` — error cases (each verified against the oracle)

| file | the diagnostic(s) it must trigger |
|---|---|
| `e_undeclared_identifier.ivy` | `undeclared identifier 'nosuch'` **and** `'nosuch' is not a template` (§8 D1) |
| `e_not_a_template.ivy` | `'plain' is not a template` (§8 D1) |
| `e_cannot_deduce.ivy` | `cannot deduce template arguments for 'id'` (§8 D2) |
| `e_deduce_conflict.ivy` | `cannot deduce template arguments for 'two'` (the same type parameter deduced with two different types) |
| `e_nontype_param_deduce.ivy` | `cannot deduce template arguments for 'val'` (a non-type parameter is never deduced) |
| `e_failed_to_instantiate.ivy` | `concept 'Addable' not satisfied: type 'P' does not support operator '+'` **and** `failed to instantiate template 'add'` (§8 D3, D4) |
| `e_unsatisfied_concept.ivy` | the same two, plus the knock-on `cannot initialize variable 'r' of type 'P' with a value of type 'int'` |
| `e_concept_deduce_fail.ivy` | `concept ... not satisfied` **and** `failed to deduce template arguments for 'add'` (the deduced path) |
| `e_struct_concept_fail.ivy` | `concept ... not satisfied` **and** `variable 'b' must be initialized (uninitialized variables are not allowed)` — the struct-template **fallback** to the unmangled name |
| `e_arity_mismatch.ivy` | `call to 'id<int32_t>' expects 1 argument(s), got 2` (§8 D5) |
| `e_argument_type_mismatch.ivy` | `argument 1 of 'id<int32_t>' expects 'int32_t', got 'const char*'` (§8 D7) |
| `e_local_type_param_not_substituted.ivy` | `cannot initialize variable 'y' of type 'T' ...` **and** `cannot return value of type 'T' from function returning 'int32_t'` (bug B1, §8 D8, D9) |
| `e_template_calls_template.ivy` | `argument 1 of 'id<T>' expects 'T', got 'int32_t'` (bug B2) |
| `e_default_argument_lost.ivy` | `call to 'withdefault<int>' missing argument 2 (parameter 'y' has no default value)` (§8 D6) |
| `e_alias_template_argument.ivy` | `argument 1 of 'id<MyInt>' expects 'MyInt', got 'int'` — an alias is not resolved in an explicit template argument |
| `e_body_error_in_instantiated.ivy` | `undeclared identifier 'undefined_name'` (§8 D10) |
| `e_missing_return.ivy` | `function 'noret<int32_t>' may reach the end without returning a value` (reported at the call site with the mangled name) |
| `e_empty_template_argument_list.ivy` | `unexpected token in expression` — `f<>()` is rejected by the **parser** (an explicit template argument must be a type) |
| `e_long_diagnostic_message.ivy` | a long name in two consecutive template-id diagnostics: the message must be complete and the second message must not be prefixed with the first one (phase-6 regression, §18.2 D2) |

### 17.2 The fixtures that had to be rewritten (and why)

The phase-3/4 fixture sets contained a few files that did **not** exercise
the rule they were named after. They were rewritten (or dropped) here:

| fixture | problem | what the corpus does now |
|---|---|---|
| `F39_deduce_char` | `char` is not an Ivy type — the file was a *parse* error, not a deduction | `01_deduce_char.ivy` uses `auto c = identity('a')` (the type of a char literal is `char`) |
| `F51_deduce_double` | `double` is not an Ivy type — a *parse* error | `01_deduce_double.ivy` uses `auto d = identity(1.5)` (the type of a float literal is `double`) |
| `F48_struct_arg` | the struct was written without the trailing `;` — a *parse* error | `01_deduce_struct.ivy` uses a well-formed struct |
| `F54_ptr_param_deduce` | a raw pointer needs an `unsafe` block — a *semantic* error | `01_deduce_pointer.ivy` wraps the pointer code in `unsafe` and returns `int32` (a pointer **return** would trip the MIR lifetime check, see below) |
| `F21_ptr_T` | `&x` outside `unsafe` and `int32 a = <pointer>` — two unrelated errors | dropped; the `T*` **parameter** substitution is covered by `01_deduce_pointer.ivy` |
| `F10_variadic_empty` | `count_args<>()` is a **parse** error (`unexpected token in expression`) — the empty pack was never instantiated | dropped from `03_`; the empty pack is covered by `03_empty_pack_deduced.ivy` (the deduced empty pack *is* mangled `count_args<>`); the parse rejection is kept as `e_empty_template_argument_list.ivy` |

Two further candidates were dropped because the **oracle** emits a
diagnostic that the port cannot reproduce (the diagnostic comes from the
MIR builder, which runs *after* the `--hir` dump and is not part of this
milestone): a template with a **pointer return type** (`T*`) reports
`returned reference has unknown lifetime`. No corpus file therefore returns
a pointer or a reference from a template.

The duplicate `T11`/`T25` (two identical struct templates) and
`T31`/`T32` (a template and a plain struct, both orders — the dumps are
identical) were merged into `05_template_redefinition.ivy` and
`05_template_and_plain_struct.ivy`.

### 17.3 What is **not** in the corpus

* the known gaps of §16.8: `Box<int32>(1)` (a constructor call on a
  specialization), `sizeof...(pack)`, the fold expressions, the namespaces;
* the inputs on which the oracle **crashes** (§10.1: `a + b` on a struct
  without `operator+`, `obj(args)` on a struct without `operator()`,
  `examples/test_b5_expected.ivy`);
* the parser-level rejections that belong to the H3.5 parser corpus
  (member function templates, `template <> …`, a trailing `requires`
  clause, a `static` member, a nested template-id `Box<Box<int32>>`).

### 17.4 The verification

```
powershell -File selfhost/difftest.ps1 -Dir selfhost/corpus/hir/template -Kinds all -Oracle --hir -Modules token,lexer,preprocessor,ast,parser,hir,hir_builder -ModuleDirs selfhost/parsing,selfhost/hir -DriverDir selfhost/hir -Driver hirdrive
```

→ `PASS: identical (filtered) output on 99 files` (byte-identical, stdout +
stderr, including the diagnostic file names and positions).

### 17.5 The per-suite regression (all after the corpus was added)

| suite | files | result |
|---|---|---|
| lexer/ident, num, str, op, trivia, err | 10/8/7/5/7/9 | PASS |
| pp/define_obj, define_fn, cond, expand, predef, line, no_include | 10/12/16/16/20/22/9 | PASS |
| parser/decl | 15 | PASS |
| parser/expr | 13 | PASS |
| parser/stmt | 15 | PASS |
| parser/aggregate | 14 | PASS |
| parser/template | 15 | PASS |
| parser/module | 16 | PASS |
| parser/err | 19 | PASS |
| hir/name | 27 | PASS |
| hir/type | 120 | PASS |
| hir/constexpr | 70 | PASS |
| **hir/template (H4.4)** | **99** | **PASS** |
| **total** | **574** | **24/24 suites PASS** |

### 17.6 The remaining gaps for H4.5+

| # | gap | why | milestone |
|---|---|---|---|
| G1 | `sizeof...(pack)` and the fold expressions `(args + ...)` | the **parser** of the port does not accept them (the oracle's parser does) | a parser phase (or H4.5) |
| G2 | namespaces (`ns::id<int32>`, the qualified mangling) | the port has no namespace support at all (neither in the parser nor in the HIR builder) | H4.5+ |
| G3 | the constructor call on a specialization (`Box<int32>(1)`) | the port has **no** constructor support (no ctor overload resolution, no ctor injection, no member initializer lists) — a general struct feature | H4.5+ |
| G4 | a template whose return type is a **pointer/reference** | the oracle's MIR builder reports `returned reference has unknown lifetime` after the `--hir` dump; the port has no MIR builder yet | H4.5+ (borrow/lifetime check) |

The oracle bugs that the port reproduces **on purpose** (B1–B13 of §10.2)
are *not* gaps: they are pinned by the corpus (see 17.1.1, 17.1.5, 17.1.7).

---

## 18. Phase 6 findings (VERIFICATION agent, phase 6 of 6)

The milestone was re-verified from scratch: the pool strides were re-derived
from the record layouts, the implementation was reviewed against the oracle,
120+ independent fixtures (outside the corpus) were compared byte-for-byte
against `ivy --hir`, and every suite was re-run.

### 18.1 The pool strides (re-derived, not trusted)

Every `__ivy_alloc` in `selfhost/parsing/parser.ivy` and
`selfhost/hir/hir_builder.ivy` was checked against the record layouts of
`selfhost/parsing/ast.ivy` and `selfhost/hir/hir.ivy` (x86-64: pointer 8,
`long` 8, `double` 8, `bool` = `i1` = 1 byte with alignment 1, every other
member aligned to 8).  The sizes were also confirmed **empirically** with a
`sizeof` probe through the oracle itself (`sizeof` of a record with the same
field sequence) — every record has the size the code assumes.  The table
records the **H4.4-era** record sizes; for current sizes see `PLAN.md` and
`selfhost/mir/MIR_NOTES.md`.

| record | size | record | size |
|---|---|---|---|
| `AType` | 48 | `AStruct` | 96 |
| `ATypeList` | 16 | `AEnum` | 96 |
| `ATemplateParam` | 96 | `AAlias` | 56 |
| `ATemplateParamList` | 16 | `AExpr` | 168 |
| `AConcept` | 48 | `AStmt` | 136 |
| `AAttr` | 40 | `ACase` | 24 |
| `AParam` | 96 | `HExpr` | 144 |
| `AFunction` | 152 | `HStmt` | 120 |
| `AField` | 72 | `HParam` | 80 |
| `AEnumerator` | 32 | `HFunction` | 96 |
| | | `HCase` | 24 |

**No stride mismatch was found.**  Every pool is allocated with the size of
its element and every pool is freed with `__ivy_free` (the size does not
matter for `free`, and no pool is freed with a *different* size than it was
allocated with).

### 18.2 The defects that were found and fixed

| # | defect | where | fix |
|---|---|---|---|
| D1 | **the struct-template registry was not consulted for a template-id call**.  The oracle looks up the struct templates *before* the function templates (`buildCall` in `hir_builder.cpp`), so a name that is both a struct template and a function template is resolved as the struct template, and a template-id call on a struct template never reports "`X` is not a template".  The port only looked at the function templates: the wrong specialization was built, or a bogus "`X` is not a template" / "undeclared identifier" diagnostic was emitted. | `hir_builder.ivy` (the `a.tpl_n > 0` branch of the call builder) | the struct-template registry is now consulted first; the specialization is instantiated with the explicit arguments and the call is left unresolved (the callee keeps the bare name, the type is the dummy type) — exactly what the oracle produces for a specialization without a constructor |
| D2 | **the diagnostic messages were cut off silently**.  All the HIR diagnostics were built in one fixed scratch buffer (`dbuf_`, `ntoks * 16 + 16384` bytes) and the diagnostic kept a *pointer into it*; when the buffer was full, `db_put` dropped the text and the diagnostic showed a truncated (sometimes empty) message.  The H4.4 diagnostics (`not a template`, `cannot deduce`, `failed to instantiate`, `concept ... not satisfied`) share that buffer. | `hir_builder.ivy` (`db_end`) | the message is now copied into a stable allocation (the `strs_` pool, which grew) and the scratch buffer is reused for the next message |

D1 is pinned by three new corpus files
(`05_struct_template_wins_over_function_template.ivy`,
`05_specid_call_without_constructor.ivy`,
`05_duplicate_template_name_implicit_call.ivy`); D2 by
`e_long_diagnostic_message.ivy`.

### 18.3 The latent defects that were fixed

| # | defect | where | fix |
|---|---|---|---|
| L1 | the layout of a struct **specialization** never marked it as polymorphic: `layout_spec` looped over the *specialization's* methods, but the specialization record has no methods (`nmethods = 0`), so `spc_poly_` was always 0 and the vptr was never counted.  The oracle's `buildStruct(cloned)` computes the polymorphism from the **cloned methods**. | `hir_builder.ivy` (`layout_spec`) | `layout_spec` now takes the template and derives the polymorphism (and the vptr) from the template's methods |
| L2 | `new_stmt(kind, line, col)` ignores its `line`/`col` arguments.  `HStmt` has no `line`/`col` fields, so nothing was left uninitialised — but the dead code in `get_stmt` assigns `s.line`/`s.col`, which does not exist (it only compiles because it is unreachable). | `hir_builder.ivy` | nothing to initialise; the misleading dead assignment in `get_stmt` was left as is (it is unreachable) |

### 18.4 The gaps that stay (confirmed, not regressions)

| # | gap | confirmed by |
|---|---|---|
| G1 | `sizeof...(pack)` and the fold expressions `(args + ...)` | the oracle accepts them (and expands the pack), the **parser** of the port does not (`expected '(' after 'sizeof'`, `unexpected token in expression`) |
| G2 | namespaces | the port's parser does not accept `namespace` at all (`expected a type`) — the gap is the parser, not the template lookup |
| G3 | the constructor call on a specialization (`Box<int32>(1)`) | the oracle instantiates the specialization **and** resolves the constructor (with the ctor injection of `buildDecl`); the port has no constructor support at all.  The same gap exists for an ordinary struct (`W(1)`), so it is a general struct feature, not a template one. |

### 18.5 The corpus quality

* 99 files, 99 distinct bodies (no duplicates, no trivial files).
* All 19 `e_*` files trigger their **intended** diagnostic (each one was run
  against the oracle on its own and the diagnostic was checked).
* The 12 documented oracle bugs (B1–B13 of §10.2) are each pinned by at
  least one file.

### 18.6 The per-suite result (all after the fixes)

| suite | files | result |
|---|---|---|
| lexer/ident, num, str, op, trivia, err | 10/8/7/5/7/9 | PASS |
| pp/define_obj, define_fn, cond, expand, predef, line, no_include | 10/12/16/16/20/22/9 | PASS |
| parser/decl, expr, stmt, aggregate, template, module, err | 15/13/15/14/15/16/19 | PASS |
| hir/name, type, constexpr | 27/120/70 | PASS |
| **hir/template (H4.4)** | **99** | **PASS** |
| **total** | **574** | **24/24 suites PASS** |

