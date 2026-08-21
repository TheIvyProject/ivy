# Ivy Lang

> A safer subset of C++, compiled to LLVM IR by its own compiler (`ivyc`).

Ivy Lang is a language **within** C++. Source files are ordinary C++ (`.cpp`, `.cxx`, `.cc`) — the same syntax, the same file extensions — but the set of accepted programs is a curated subset chosen so the language can be made safe by construction, and safety semantics are expressed through standard C++ attributes (`[[ivy::...]]`).

`ivyc` is a full compiler frontend with a classic pipeline:

```
Lexer → Preprocessor → Parser → HIR → MIR → LLVM IR
```

It accepts only the restricted C++ subset; anything outside it is a compile error. By default, Ivy uses its own fixed-width integer and floating-point types (`int8_t`…`int64_t`, `uint8_t`…`uint64_t`, `float16_t`…`float128_t`, `bfloat16_t`, `size_t`, `ptrdiff_t`, `bool`, `void`) and rejects C-style number types (`int`, `unsigned`, `long`, `short`, `char`, `float`, `double`, `long long`); `#pragma ivy cnumber` opts into the C-style types. Hex/octal/binary integer literals (`0xFF`, `017`, `0b1010`) are always allowed regardless of the pragma. The output is LLVM IR, which the LLVM backend lowers to native code. The `Preprocessor` stage currently expands `#include` (quoted and angle-bracket forms), object-like macros (`#define NAME body...`), function-like macros (`#define NAME(params) body...`, including variadic `...`/`__VA_ARGS__`), conditional compilation (`#ifdef`/`#ifndef`/`#if EXPR`/`#elif EXPR`/`#else`/`#endif`/`#undef`, with `#if`/`#elif` evaluating C++ integral constant expressions over integer literals, `defined()`, macro expansion, arithmetic, bitwise, logical, comparison, and ternary operators), predefined macros (`__LINE__`, `__FILE__`, `__DATE__`, `__TIME__`, `__cplusplus`), and `#pragma ivy cnumber`, and is invoked between the lexer and the parser; `ivyc -o file.i` (or `.ii`) dumps the preprocessed C++ source text instead of continuing down the pipeline, mirroring `g++ -E`.

In addition to native compilation, `ivyc` can **interpret** a program directly from HIR via `--run` — no LLVM IR or native code generation required. This is powered by **IvyInterpret**, a self-contained tree-walking interpreter that consumes HIR and executes it in-memory. See [IvyInterpret](#ivyinterpret) below.

## Design Philosophy

**One-way source compatibility.**

```
Ivy  ⊂  C++
```

- Every valid Ivy program is valid C++ (same grammar, same extensions).
- Not every valid C++ program is valid Ivy — `ivyc` rejects what it cannot make safe.

Being source-compatible with C++ keeps the ecosystem intact: `clang-format`, IDEs, debuggers, and even other C++ compilers still understand Ivy sources. Per the standard, a conforming C++ compiler ignores unrecognized attributes (typically with a warning), so `[[ivy::...]]` never breaks a build.

But Ivy does **not** compile by delegating to a C++ compiler. `ivyc` compiles C++ natively: it lexes and parses the subset itself, builds its own IR, checks safety at the MIR level, and emits LLVM IR.

## Goals

1. **Safety** — eliminate or quarantine undefined behavior: dangling pointers, lifetime violations, unchecked bounds, raw memory management, implicit unsafe casts.
2. **Simplicity** — a curated subset of C++. Only features that can be made safe are in scope; everything else must be opted into explicitly.
3. **Backward compatibility** — source stays C++: existing editors, formatters, debuggers, and libraries work as-is. Only the compiler changes.

## Non-Goals

- Not a new syntax for C++ (that is cppfront's territory).
- Not a transpiler — Ivy is not lowered to C++ text.
- Not compatible in the C++ → Ivy direction (`ivyc` accepts only the subset).

## Compilation Pipeline

```
.cpp / .cxx / .cc
      │
      ▼
┌──────────┐
│  Lexer   │  tokens (identifiers, keywords, attributes, ...)
└──────────┘
      ▼
┌──────────────┐
│ Preprocessor │  #include + object-like & function-like macros (#define, variadic __VA_ARGS__) + conditional compilation (#ifdef/#ifndef/#if/#elif/#else/#endif/#undef) + predefined (__LINE__/__FILE__/__DATE__/__TIME__/__cplusplus); -o file.i dumps text
└──────────────┘
      ▼
┌──────────┐
│  Parser  │  AST of the restricted C++ grammar; rejects anything outside the subset
└──────────┘
      ▼
┌──────────┐
│   HIR    │  type-checked, high-level IR; Ivy attributes lowered into IR annotations
└──────────┘
      ▼
┌──────────┐
│   MIR    │  CFG-based IR; where safety analysis runs (lifetimes, unsafe blocks)
└──────────┘
      ▼
┌──────────┐
│ LLVM IR  │  emitted via the LLVM infrastructure
└──────────┘
      ▼
   LLVM backend → native binary
```

Each stage is a separate module, so the safety analyses (lifetime checking, `[[ivy::unsafe]]` enforcement, bounds checks) have a well-defined place to run: on the MIR, before LLVM IR is emitted.

## Attribute Reference

### Lifetime Attributes

| Attribute | Applies to | Meaning |
|---|---|---|
| `[[ivy::lt_def(a)]]` | function | Declares a named lifetime parameter `a` for the function. |
| `[[ivy::lt(a)]]` | parameter | The pointer must live at least as long as lifetime `a`. Enforced at call sites. |
| `[[ivy::lt_ret(a)]]` | function return | The returned pointer is valid only for lifetime `a`. Callers may not use the result beyond it. |

Lifetime attributes let a function express the same guarantees Rust expresses with lifetimes, without changing C++ syntax.

### Safety Attributes

| Attribute | Applies to | Meaning |
|---|---|---|
| `[[ivy::unsafe]]` | compound statement | Opts out of safety checks for the block: raw pointers, `malloc`/`free`, C APIs, explicit casts. The only place such code is allowed. |

## Examples

### Lifetime Attributes

```cpp
// Test Lifetime Attributes
[[ivy::lt_def(a)]] const char* [[ivy::lt_ret(a)]] select_first(
    const char* x [[ivy::lt(a)]],
    const char* y [[ivy::lt(a)]]
) {
    return x;
}
```

`select_first` declares one lifetime parameter `a`. Both `x` and `y` must outlive `a`, and the returned pointer is only guaranteed valid for `a` — the caller cannot use it past the lifetime of the arguments it passed in.

### Unsafe Block and C API Allocation

```cpp
// Test Unsafe Block and C API Allocation
void raw_alloc() {
    [[ivy::unsafe]] {
        void* p = malloc(1024);
        free(p);
    }
}
```

Raw memory management is invisible outside `[[ivy::unsafe]]` blocks. Any `malloc`, `free`, `new`, `delete`, or raw-pointer operation outside such a block is a compile error in Ivy.

## IvyInterpret

**IvyInterpret** is a self-contained tree-walking interpreter that executes Ivy programs directly from HIR — no MIR, LLVM IR, or native code generation required. It is an independent module (`src/interpret/`) that depends only on the HIR data structures and the C++ standard library.

### Usage

```
ivyc --run source.cpp
```

The `--run` flag tells `ivyc` to build HIR and then hand it to the interpreter instead of continuing down the MIR → LLVM IR pipeline. The interpreter calls `main()` and returns its exit code. Diagnostics (if any) are printed to `stderr`.

### What it supports

- **Functions**: user-defined functions, recursion, `extern "C"` built-ins (`printf`, `puts`, `putchar`, `exit`, `abort`).
- **Types**: integers, floating-point, booleans, strings, pointers, structs (including nested structs and struct copy).
- **Expressions**: integer/float/bool/string/char/nullptr literals, identifiers, unary (`+ - ! ~ & * ++ --`), binary (arithmetic, comparison, logical short-circuit, bitwise, shift), ternary, assignment (simple + compound), member access (`.` and `->`), aggregate init lists, function calls.
- **Statements**: declarations, `if`/`else`, `while`, `do-while`, `for`, `return`, `break`, `continue`, expression statements, `[[ivy::unsafe]]` blocks.
- **Structs**: aggregate initialization (`Point p = {1, 2}`), partial init (`Vec3 v = {100}` → `{100, 0, 0}`), empty init (`Vec3 v = {}`), default construction (`Point p;`), member assignment (`p.x = 10`), nested struct access (`line.start.x`), pointer-to-struct arrow (`pp->x`), struct copy assignment.
- **Lambdas**: no-capture, by-value capture (`[x]`), by-reference capture (`[&x]`). Closures are represented as runtime struct values; capture fields are initialized from the closure struct definition in the HIR translation unit.

### Runtime value model

The interpreter uses a tagged-union `Value` type (`src/interpret/value.h`) with variants for `Void`, `Int`, `Float`, `Str`, `Struct`, and `Ptr`. A `Cell` (`shared_ptr<Value>`) provides heap allocation with shared ownership so that references and pointers alias the same storage. Control flow is managed via a `Signal` variant (`Return`/`Break`/`Continue`) returned from statement execution. Function call frames are `unordered_map<string, Cell>` stacks.

### Design goals

- **Independent**: `IvyInterpret` depends only on HIR — it does not touch parsing, MIR, or codegen. This makes it usable for REPL, testing, and `constexpr`/`consteval` evaluation in the future.
- **Fast feedback**: `--run` lets you execute Ivy code immediately without a native compiler, useful for quick experiments and CI smoke tests.
- **Foundation for `constexpr`**: per the roadmap, `constexpr`/`consteval` evaluation will be built on top of IvyInterpret.

## Restrictions (The Subset)

`ivyc` accepts only a subset of C++. By default it forbids constructs that cannot be made safe, such as:

- raw pointer arithmetic and pointer-to-integer casts,
- implicit pointer conversions and `reinterpret_cast`,
- `malloc`/`free`/`new`/`delete` outside `[[ivy::unsafe]]`,
- returning a pointer whose lifetime is not tied to a parameter or to `[[ivy::lt_def]]` lifetimes,
- uninitialized variables and implicit `void*` conversions,
- language features outside the subset's grammar (templates, exceptions, and other features are either excluded or restricted — decided per feature).

### Supported features

- **Functions**: top-level function definitions, `extern "C"` declarations, parameters, return values.
- **Types**: fixed-width integer/float types (`int8_t`…`int64_t`, `uint8_t`…`uint64_t`, `float16_t`…`float128_t`, `bfloat16_t`, `size_t`, `ptrdiff_t`, `bool`, `void`). C-style types (`int`, `char`, `long`, `float`, `double`, …) require `#pragma ivy cnumber`.
- **Enums** (P4.1): unscoped (`enum`) and scoped (`enum class` / `enum struct`) enumerations with implicit/explicit values, constant-expression values (arithmetic, bitwise), explicit underlying type (`enum E : int64_t`), `EnumName::Value` for scoped enums. Enum constants are folded to integer literals at the HIR stage.
- **Namespaces** (P4.2): `namespace` blocks with nested namespaces, bare-name resolution within a namespace, qualified calls (`ns::func(args)`), qualified enum access (`ns::EnumName::Value`, `ns::enum_constant`). `extern "C"` functions are not mangled.
- **Name mangling** (P4.3): C++ symbol names are mangled according to the target platform's ABI — Itanium ABI (`_Z` prefix, `N...E` nested names, type codes) for POSIX, MSVC ABI (`?` prefix, `@`-separated scopes, type codes, `@Z` terminator) for Windows. The ABI is auto-detected from the host platform or overridden via `--target itanium|msvc`. Scoped enums (`enum class`) are mangled as nested types; unscoped enums collapse to their underlying integer type.
- **Structs / classes** (P4.4): `struct` and `class` aggregate types with field layout (sequential offsets, natural alignment, struct size rounded up to max alignment per C ABI). Member access via `.` (struct lvalue → GEP) and `->` (pointer-to-struct → load pointer then GEP). Nested struct member access (GEP chaining), namespace-qualified struct types (`ns::Struct`), struct copy assignment (LLVM aggregate load/store), zero-initialization for struct variables without explicit initializers. Named LLVM struct types (`%struct.Name = type { ... }`) emitted before function definitions. Access specifiers (`public:`/`private:`/`protected:`) are parsed and ignored (all members are public in Ivy). Variadic parameters (`...`) are accepted in `extern "C"` declarations (e.g. `printf`). `main` is never mangled to preserve the C ABI entry point. Aggregate initialization `{ }` (basic/partial/empty) and struct reassignment with init lists are supported. Default member initializers (`int x = 42;` in the struct body) are applied when a struct variable is declared without an explicit initializer and for trailing fields in partial aggregate init lists; fields without a default are zero-initialized.
- **Lambdas** (P4.5): lambda expressions `[caps](params) -> ret { body }` in primary expressions. Capture modes: `[x]` (by value), `[&x]` (by reference), `[]` (no captures). Each lambda is lowered to a closure struct type (`__lambdaN_closure`) + a call-operator function (`__lambdaN(closure_ptr, params...)`). Captured variables are injected as local declarations at the top of the body (`T cap = __closure->cap` for by-value; `T& cap = *(__closure->cap)` for by-reference) under an implicit unsafe scope so the compiler-generated closure access is always safe while user code in the body is still checked normally. Calling a lambda (`lambda_expr(args)`) passes `&closure` as the implicit first argument. Return type deduction from the first `return` statement when `-> ret` is omitted.
- **Control flow**: `if`/`else`, `while`, `do-while`, `for`, `break`, `continue`, `return`, ternary `?:`.
- **Operators**: arithmetic, comparison, logical, bitwise, assignment, compound assignment.
- **Preprocessor**: `#define` (object-like, function-like, variadic), `#include`, `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif` with constant-expression evaluation, `#undef`, `#pragma ivy cnumber`, predefined macros (`__LINE__`, `__FILE__`, `__DATE__`, `__TIME__`, `__cplusplus`).
- **Safety**: `[[ivy::unsafe]]` blocks for pointer arithmetic, `malloc`/`free`, casts; `[[ivy::lt_def]]`/`[[ivy::lt_ret]]` lifetime annotations.

Each forbidden construct is either rejected outright or requires an explicit `[[ivy::unsafe]]` opt-out. The goal is: **if it compiles, it is safe by construction** — no annotation required for the common path.

## Roadmap

- [x] Lexer: token definitions incl. attributes
- [x] Preprocessor: `#include`, `#define` (object-like, function-like, variadic), conditional compilation, predefined macros, `#pragma ivy cnumber`
- [x] Parser: AST for the restricted grammar
- [x] HIR: type checking, attribute lowering
- [x] MIR: CFG construction, lifetime checker, `[[ivy::unsafe]]` enforcement
- [x] LLVM IR emission (LLVM infrastructure)
- [x] IvyInterpret: tree-walking interpreter consuming HIR; `--run` flag for immediate execution
- [ ] `constexpr`/`consteval` evaluation via IvyInterpret
- [ ] IvyMake: C++-based build configuration system (replace CMake)
- [ ] Test suite: safe programs compile clean, unsafe programs are rejected

## Status

In active development. The core compiler pipeline (Lexer, Preprocessor, Parser, HIR, MIR, LLVM IR codegen) is implemented for the initial subset, and **IvyInterpret** provides immediate execution via `--run`. Work continues on `constexpr`/`consteval` evaluation and IvyMake.

# License
[Apache License 2.0](LICENSE)
