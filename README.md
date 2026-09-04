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

The `ivyc` compiler architecture draws strong architectural inspiration from modern compilers (such as rustc):

```
Source (.ivy) ──► Lexer ──► Preprocessor ──► Parser ──► AST ──► HIR ──► MIR ──► LLVM IR ──► Native Binary
```

- **HIR (High-level IR):** Performs rigorous type inference, template instantiation, and semantic validation.
- **MIR (Mid-level IR):** Control Flow Graph (CFG) representation where borrow checking, lifetime verification, and safety validations occur.
- **LLVM IR:** Emits optimized intermediate representation to leverage world-class LLVM optimization and code-generation pipelines.

---

## IvyInterpret

Unlike the restricted `constexpr` mechanisms in standard C++, Ivy features **IvyInterpret**—a built-in, MIR-level tree-walking interpreter embedded directly within `ivyc`.

- Executes MIR in memory with zero backend compilation overhead (`ivyc --run <file.ivy>`).
- Enables advanced compile-time evaluation: pure code paths and side-effect-free calculations can run during compilation to embed direct values into output binaries.
- Guarantees full memory safety validation before execution.

---

## Status & Documentation

Ivy is actively establishing its independent language specifications and foundational documentation following its departure from strict C++ conformance.

- **Official Documentation:** Explore language fundamentals, syntax, and memory safety rules in [`docs/`](docs/).
- **Legacy Archive:** Historical documentation and previous C++-subset specifications are archived at [TheIvyProject/ivy (archive-2026-4-9)](https://github.com/TheIvyProject/ivy/tree/archive-2026-4-9).

---

## License

Ivy is distributed under the [Apache License 2.0](LICENSE).
