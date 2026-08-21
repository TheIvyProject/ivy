# Ivy Lang

> A safer subset of C++, compiled to LLVM IR by its own compiler (`ivyc`).

Ivy Lang is a language **within** C++. Source files are ordinary C++ (`.cpp`, `.cxx`, `.cc`) — the same syntax, the same file extensions — but the set of accepted programs is a curated subset chosen so the language can be made safe by construction, and safety semantics are expressed through standard C++ attributes (`[[ivy::...]]`).

`ivyc` is a full compiler frontend with a classic pipeline:

```
Lexer → Preprocessor → Parser → HIR → MIR → LLVM IR
```

It accepts only the restricted C++ subset; anything outside it is a compile error. By default, Ivy uses its own fixed-width integer and floating-point types (`int8_t`…`int64_t`, `uint8_t`…`uint64_t`, `float16_t`…`float128_t`, `bfloat16_t`, `size_t`, `ptrdiff_t`, `bool`, `void`) and rejects C-style number types (`int`, `unsigned`, `long`, `short`, `char`, `float`, `double`, `long long`); `#pragma ivy cnumber` opts into the C-style types. Hex/octal/binary integer literals (`0xFF`, `017`, `0b1010`) are always allowed regardless of the pragma. The output is LLVM IR, which the LLVM backend lowers to native code. The `Preprocessor` stage currently expands `#include` (quoted and angle-bracket forms), object-like macros (`#define NAME body...`), function-like macros (`#define NAME(params) body...`, including variadic `...`/`__VA_ARGS__`), conditional compilation (`#ifdef`/`#ifndef`/`#if EXPR`/`#elif EXPR`/`#else`/`#endif`/`#undef`, with `#if`/`#elif` evaluating C++ integral constant expressions over integer literals, `defined()`, macro expansion, arithmetic, bitwise, logical, comparison, and ternary operators), predefined macros (`__LINE__`, `__FILE__`, `__DATE__`, `__TIME__`, `__cplusplus`), and `#pragma ivy cnumber`, and is invoked between the lexer and the parser; `ivyc -o file.i` (or `.ii`) dumps the preprocessed C++ source text instead of continuing down the pipeline, mirroring `g++ -E`.

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
- **Control flow**: `if`/`else`, `while`, `do-while`, `for`, `break`, `continue`, `return`, ternary `?:`.
- **Operators**: arithmetic, comparison, logical, bitwise, assignment, compound assignment.
- **Preprocessor**: `#define` (object-like, function-like, variadic), `#include`, `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif` with constant-expression evaluation, `#undef`, `#pragma ivy cnumber`, predefined macros (`__LINE__`, `__FILE__`, `__DATE__`, `__TIME__`, `__cplusplus`).
- **Safety**: `[[ivy::unsafe]]` blocks for pointer arithmetic, `malloc`/`free`, casts; `[[ivy::lt_def]]`/`[[ivy::lt_ret]]` lifetime annotations.

Each forbidden construct is either rejected outright or requires an explicit `[[ivy::unsafe]]` opt-out. The goal is: **if it compiles, it is safe by construction** — no annotation required for the common path.

## Roadmap

- [ ] Subset definition: the exact set of accepted C++ features and restrictions
- [ ] Lexer: token definitions incl. attributes
- [ ] Parser: AST for the restricted grammar
- [ ] HIR: type checking, attribute lowering
- [ ] MIR: CFG construction, lifetime checker, `[[ivy::unsafe]]` enforcement
- [ ] LLVM IR emission (LLVM infrastructure)
- [ ] Test suite: safe programs compile clean, unsafe programs are rejected

## Status

Early design phase — nothing is implemented yet. This document is the design contract.

# License
[Apache License 2.0](LICENSE)
