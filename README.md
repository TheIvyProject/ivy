<div align="center">

# 🌿 Ivy Programming Language

**A C++ successor with Rust-style memory safety — familiar syntax, native ABI compatibility, and zero-cost performance.**

</div>

---

## What is Ivy?

Ivy is a modern systems programming language built on the syntax, features, ABI
and libraries of C++. It is a successor to C++ in the spirit of Carbon: it keeps
the language developers already know instead of asking them to start over.
Around 80–90% of Ivy syntax is taken directly from C++.

If C3 is a modern C, Ivy is a modern C++.

**Why Ivy?**

- **Memory safety on a C++ foundation.** Ivy applies Rust-style ownership,
  borrowing and lifetime checking to C++ semantics — no garbage collector and no
  runtime overhead.
- **Zero-FFI C++ interoperability.** Unlike Rust or Zig, Ivy shares the native
  C++ ABI (Itanium C++ ABI on Unix-like systems, MSVC ABI on Windows), so Ivy
  and C++ code call each other directly, without bindings or glue layers.
- **A familiar language.** Declarations, expressions, operators and much of the
  standard vocabulary come straight from C++, so existing C++ developers are
  productive on day one.

---

## History

Ivy began as a safe subset of C++. Strict C++ backwards compatibility soon
became the limiting factor: ISO/IEEE standardization constraints blocked syntax
modernizations and deeper compile-time safety guarantees. Ivy therefore grew
into an independent language, keeping C++'s ergonomics, ABI and library model
while defining its own specification.

The compiler became fully self-hosted on 22 November 2026: the C++
implementation was retired and `ivy` now builds itself (see
[Self-Hosted Compiler](#self-hosted-compiler-22-november-2026)).

---

## Core Language Features

- **Native Lifetime Annotations:** Express reference validity cleanly without awkward attribute wrappers.

```ivy
lifetime<$a, $b>
const int32& $a foo(
    const int32& $a x,
    const int32& $b y
) {
    return x; // ✅ OK: lifetime $a satisfies return lifetime $a
    // return y; // ❌ Error: lifetime $b cannot satisfy return lifetime $a
}
```

- **Zero-Cost Abstractions:** Pure systems performance compiled directly to native code via LLVM.
- **Deterministic RAII & Explicit Moves:** Resources destruct automatically upon exiting scope; moved-out variables are tracked and verified at compile-time.

---

## Modern Compiler Pipeline

The `ivy` compiler architecture draws strong architectural inspiration from modern compilers (such as rustc):

```
Source (.ivy) ──► Lexer ──► Preprocessor ──► Parser ──► AST ──► HIR ──► MIR ──► LLVM IR ──► Native Binary
```

- **HIR (High-level IR):** Performs rigorous type inference, template instantiation, and semantic validation.
- **MIR (Mid-level IR):** Control Flow Graph (CFG) representation where borrow checking, lifetime verification, and safety validations occur.
- **LLVM IR:** Emits optimized intermediate representation to leverage world-class LLVM optimization and code-generation pipelines.

---

## IvyInterpret

Unlike the restricted `constexpr` mechanisms in standard C++, Ivy features **IvyInterpret**—a built-in, MIR-level tree-walking interpreter embedded directly within `ivy`.

- Executes MIR in memory with zero backend compilation overhead (`ivy --run <file.ivy>`).
- Enables advanced compile-time evaluation: pure code paths and side-effect-free calculations can run during compilation to embed direct values into output binaries.
- Guarantees full memory safety validation before execution.

---

## Self-Hosted Compiler (22 November 2026)

On **22 November 2026** Ivy reached full self-hosting: the `ivy` compiler is
written in Ivy and builds itself. The C++ implementation that hosted the
language until then has been retired from the repository; it remains in git
history at tag `0.5-baseline` together with its parity harnesses.

- **Bootstrap chain:** checked-in seed (`selfhost/bootstrap/ivy-stage0.exe`) →
  stage 1 → stage 2, with byte-identical objects, module interfaces and
  executables (`selfhost/build.ps1`).
- **Gates:** `selfhost/build.ps1 -SelfLink -Golden` (F2 + self-link + 1052-fixture
  golden parity) and `selfhost/check_repro.ps1` (reproducible build).
- **CI:** `.github/workflows/selfhost-dogfood.yml` runs the whole chain with no
  C++ compiler and no CMake.

Requirements: Windows x64, PowerShell, and LLVM 21.1.6 (`clang++` on `PATH`, or
`LLVM_ROOT` pointing at the toolchain). The runtime object is built from
`std/rt.ivy` by the build script.

```powershell
# Build and verify the compiler with itself (no oracle, no CMake).
pwsh -NoProfile -File selfhost/build.ps1 -SelfLink -Golden
```

---

## Status & Documentation

Ivy is actively establishing its independent language specifications and foundational documentation following its departure from strict C++ conformance.

- **Compiler Status:** self-hosted as of 22 November 2026 (`selfhost/`).
- **Official Documentation:** Explore language fundamentals, syntax, and memory safety rules in [`docs/`](docs/).
- **Legacy Archive:** Historical documentation and previous C++-subset specifications are archived at [TheIvyProject/ivy (archive-2026-4-9)](https://github.com/TheIvyProject/ivy/tree/archive-2026-4-9).

---

## License

Ivy is distributed under the [Apache License 2.0](LICENSE).
