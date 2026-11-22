<div align="center">

# 🌿 Ivy Programming Language

**A modern systems programming language built on the foundations of C++, fortified with compile-time memory safety, zero-cost abstractions, and clean ergonomics.**

</div>

---

## What is Ivy?

Ivy originally started as a safe subset of C++. However, maintaining strict C++ backwards compatibility imposed severe limitations: ISO/IEEE standardization constraints prevented critical syntax modernizations and deeper compile-time safety guarantees.

To eliminate undefined behavior while retaining high performance and developer ergonomics, Ivy evolved into an **independent systems programming language**. It gives developers the familiar power and expressiveness of C++ while introducing modern ownership, lifetime checking, and clean language mechanics.

---

## Core Language Features

- **Compile-Time Memory Safety:** Smartly integrates Rust-proven ownership, borrowing, and lifetime mechanics without garbage collection runtime overhead.
- **Familiar Ergonomics (~80% C++ Syntax):** Designed so C++ developers feel instantly at home. The transition learning curve is drastically reduced.
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
