# 🧱 Basic Types

Ivy provides a set of built-in primitive types for representing boolean values, characters, numbers, and platform-dependent pointers.

> **Note on Naming (`_t` suffix):**  
> Ivy omits the `_t` suffix found in standard C/C++ types (such as `int32_t`, `size_t`) to keep type names clean, concise, and convenient to write. Furthermore, this design prevents naming collisions with standard C++ types (`std::*` / `<cstdint>`) when importing or interoperating C++ code into Ivy.

---

### Boolean & Character & Void

| Type | Size | Bits | Min | Max |
| :--- | :--- | :--- | :--- | :--- |
| `bool` | 1 byte | 8 (1 bit value) | `false` (0) | `true` (1) |
| `char` | 1 byte | 8 | `0` | `255` |
| `void` | 0 bytes | 0 | N/A | N/A |

---

### Signed Integers

| Type | Size | Bits | Min | Max |
| :--- | :--- | :--- | :--- | :--- |
| `int8` | 1 byte | 8 | `-128` | `127` |
| `int16` | 2 bytes | 16 | `-32,768` | `32,767` |
| `int32` | 4 bytes | 32 | `-2,147,483,648` ($-2^{31}$) | `2,147,483,647` ($2^{31}-1$) |
| `int64` | 8 bytes | 64 | `-9,223,372,036,854,775,808` ($-2^{63}$) | `9,223,372,036,854,775,807` ($2^{63}-1$) |
| `int128` | 16 bytes | 128 | $-2^{127}$ | $2^{127}-1$ |

---

### Unsigned Integers

| Type | Size | Bits | Min | Max |
| :--- | :--- | :--- | :--- | :--- |
| `uint8` | 1 byte | 8 | `0` | `255` |
| `uint16` | 2 bytes | 16 | `0` | `65,535` |
| `uint32` | 4 bytes | 32 | `0` | `4,294,967,295` ($2^{32}-1$) |
| `uint64` | 8 bytes | 64 | `0` | `18,446,744,073,709,551,615` ($2^{64}-1$) |
| `uint128` | 16 bytes | 128 | `0` | $2^{128}-1$ |

---

### Floating-Point Numbers

| Type | Size | Bits | Precision | Approximate Range |
| :--- | :--- | :--- | :--- | :--- |
| `float16` | 2 bytes | 16 | Half precision | $\approx \pm 6.5504 \times 10^{4}$ |
| `float32` | 4 bytes | 32 | Single precision | $\approx \pm 3.4028 \times 10^{38}$ |
| `float64` | 8 bytes | 64 | Double precision | $\approx \pm 1.7977 \times 10^{308}$ |
| `float128` | 16 bytes | 128 | Quadruple precision | $\approx \pm 1.1897 \times 10^{4932}$ |

---

### Pointer-Sized & Architecture Types

| Type | Description | Size (32-bit / 64-bit) |
| :--- | :--- | :--- |
| `iptr` | Signed integer matching pointer width (`intptr_t`) | 4 bytes / 8 bytes |
| `uptr` | Unsigned integer matching pointer width (`uintptr_t`) | 4 bytes / 8 bytes |
| `size` | Unsigned size type for memory and lengths (`size_t`) | 4 bytes / 8 bytes |

