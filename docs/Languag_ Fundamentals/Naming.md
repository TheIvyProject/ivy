# 🏷️ Naming Rules & Identifiers

Identifiers in Ivy are names assigned to variables, functions, structs, classes, modules, and namespaces. Ivy enforces clear lexical rules for all identifiers.

---

### 1. General Identifier Rules

- **Allowed Characters:** Must contain only letters (`A-Z`, `a-z`), digits (`0-9`), and underscores (`_`).
- **First Character:** Must begin with a letter or an underscore (`_`). **Cannot** begin with a digit.
- **Case Sensitivity:** Identifiers are strictly case-sensitive (`myVariable`, `MyVariable`, and `MYVARIABLE` are distinct identifiers).
- **Reserved Keywords:** Cannot use reserved Ivy keywords (such as `int`, `int32`, `double`, `class`, `const`, `return`, `fn`, etc.).
- **No Whitespace or Special Symbols:** Whitespaces and special characters (like `@`, `$`, `%`, `-`, `#`) are prohibited.

---

### 2. Valid vs Invalid Identifiers

| Identifier | Status | Reason |
| :--- | :--- | :--- |
| `score` | ✅ Valid | Contains only lowercase letters |
| `_tempCount` | ✅ Valid | Starts with underscore |
| `player1Position` | ✅ Valid | Alphanumeric with valid starting character |
| `MAX_LIMIT` | ✅ Valid | Uppercase letters and underscores |
| `1stPlayer` | ❌ Invalid | Starts with a digit |
| `user-name` | ❌ Invalid | Contains hyphen (`-`) |
| `total$` | ❌ Invalid | Contains special symbol (`$`) |
| `const` | ❌ Invalid | Conflicts with reserved keyword |

---

### 3. Special Scoping Identifiers

#### Modules (`.`)
Module import paths and definitions allow the dot (`.`) delimiter to represent module hierarchy:

```ivy
import ivy.io;
import math.matrix.transform;
```

#### Namespaces & Scope Resolution (`::`)
Namespaces and member scoping use the double colon (`::`) operator to access symbols across scopes:

```ivy
io::print("Hello");
math::geometry::calculateArea();
```
