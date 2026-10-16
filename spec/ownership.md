# Ownership & Borrow trong Ivy

- **Trạng thái**: 0.5 draft
- **Ngày**: 2026-09-15
- **Cập nhật**: 2026-10-16 — A5 native `lifetime<$a>` syntax; B5/B6 hoàn thành

---

## 1. Mục tiêu

Ivy áp dụng mô hình **Ownership + Borrow** (giống Rust nhưng mượn qua C++).
Ivy **không thay đổi ngữ nghĩa C++** — `T x = y;` vẫn là **copy** (ngữ nghĩa C++ gốc),
RAII là của C++ tuân theo quy tắc an toàn của Ivy. Ivy chỉ **thêm lớp kiểm tra**
ngữ nghĩa move/copy.

Mỗi giá trị có **một owner** tại một thời điểm. Khi gán (`T x = y;`), dữ liệu được
**copy** (ngữ nghĩa C++) — move chỉ xảy ra khi:
- Vế phải là **rvalue** (tạm thời): `x = Foo()`, `x = 10`
- Người dùng **chủ động**: `b = move(a)`

---

## 2. Ownership

### 2.1. Một owner

Mỗi vùng dữ liệu có **duy nhất một biến sở hữu**:

```ivy
int32 a = 5;           // a là owner của 5
int32 b = a;           // b là owner của bản sao (copy)
```

### 2.2. RAII

Khi biến sở hữu ra khỏi phạm vi, hàm `~T()` tự động được gọi:

```ivy
fn foo() {
    // int32* p = malloc(4)  // p là owner — unsafe only
    // ...
}   // ~free(p) — tự động gọi
```

**Trong safe zone**: chỉ dùng `free()` qua `unsafe {}`.

### 2.3. Copy semantics

Gán `T x = y;` → **copy** (ngữ nghĩa C++):

```ivy
int32 a = 5;
int32 b = a;           // b là bản sao
```

- Trong safe: `b = move(a)` là **OK** (chủ động move) — `a` trở thành moved-out.
- Đọc `a` sau `move(a)` → **lỗi compile-time**.
- Trong unsafe: `T&&` (rvalue reference) có thể dùng.

### 2.4. Move

`move()` là **hàm builtin** (không phải keyword) — có thể dùng như câu lệnh
hoặc trong biểu thức:

```ivy
// Dùng như hàm trong biểu thức:
int32 b = move(a);

// Dùng như câu lệnh (statement):
move(a);  // a → moved-out, giá trị bị discard
```

Move chỉ xảy ra trong **2 trường hợp**:

| # | Trường hợp | Ví dụ | Mô tả |
|---|-----------|-------|-------|
| 1 | **Rvalue** (tạm thời) | `x = Foo()`, `x = 10` | Không có owner cũ — tạo mới |
| 2 | **Chủ động** `move()` | `b = move(a)` | `a` → **moved-out** |

**Sau `move(a)`**:

- `a` → **Moved-out** (trạng thái `MoveState::MovedOut`)
- Đọc `a` → **lỗi** (compile-time)
- Gán lại `a = 5` → OK (tái sử dụng — `MoveState::Initialized`)

**Ví dụ**:

```ivy
int32 a = 5;
int32 b = move(a);   // a → moved-out
// int32 c = a;       // lỗi — a đã moved-out
int32 c = b;          // OK — b là owner (copy)

a = 99;               // OK — re-initialize, a trở lại Initialized
int32 d = a;          // OK — a đã được tái khởi tạo
```

### 2.5. Kiểm tra moved-out

Sau `b = move(a)`:

| Hành động | Kết quả |
|-----------|---------|
| `int32 c = a` | **lỗi** — a là moved-out |
| `a + 5` | **lỗi** — a là moved-out |
| `a = 5` | **OK** — tái sử dụng (owner mới) |
| `move(a)` | **lỗi** — đã moved-out, không move lại được |
| `move(b)` | **OK** — b là owner, chưa moved-out |

**Triển khai MIR (B3)**: `moveStates_` stack tracking — mỗi biến có trạng thái
`Initialized` hoặc `MovedOut`. Khi gặp `move(x)`:
1. Nếu `x` đã `MovedOut` → lỗi: `'<var>' is already moved-out`
2. Đánh dấu `x` = `MovedOut`

Khi đọc `x` (IdentRef, không phải lhs gán):
1. Nếu `x` = `MovedOut` → lỗi: `use of moved-out variable '<var>'`

Khi gán `x = ...` (Assign lhs):
1. `x` được re-initialize → `MoveState::Initialized`

### 2.6. unsafe bypass

Trong `unsafe {}`, move-state checking bị bỏ qua:

```ivy
int32 p = 5;
unsafe {
    int32 q = move(p);
    // Trong unsafe, đọc p sau move là OK (không kiểm tra)
    p = q + 1;  // p = 6
}
```

---

## 3. Borrow

### 3.1. Immutable borrow (`const T&`)

Nhiều `const T&` có thể tồn tại cùng lúc (read-only):

```ivy
int32 a = 5;
const int32& r1 = a;   // borrow — không thể thay đổi a qua r1
const int32& r2 = a;   // OK — nhiều immutable borrow
```

### 3.2. Mutable borrow (`T&`)

Chỉ **một** `T&` tại một thời điểm:

```ivy
int32 a = 5;
int32& r = a;           // mutable borrow — có thể thay đổi a
// const int32& r2 = a; // lỗi — đã có mutable borrow
```

### 3.3. Aliasing XOR Mutability

- Không thể có cả `T&` và `const T&` cùng lúc (nếu `T&` tồn tại).
- Trong C++ gốc: được phép — nhưng Ivy không cho phép.

```ivy
int32 a = 5;
int32& r = a;
// int32& r2 = a;        // lỗi — chỉ một mutable borrow
const int32& cr = a;    // lỗi — đã có mutable borrow (r tồn tại)
```

**Borrow Checker**: phân tích trong MIR để đảm bảo không có `T&` nào tồn tại
lâu hơn dữ liệu.

### 3.4. Lifetime annotation (A5 native syntax)

`lifetime<$a>` — giúp theo dõi thời gian sống của borrow:

```ivy
lifetime<$a>
fn get_ref(const int32& $a x) -> const int32& $a {
    return x;              // borrow → lifetime $a
}
```

Legacy attribute form (`[[ivy::lt_def]]` / `[[ivy::lt]]` / `[[ivy::lt_ret]]`)
đã xóa cùng đợt với E2 — chỉ giữ native syntax.

### 3.5. Dangling reference (B2)

B2 Lifetime verification phát hiện dangling reference — trả về reference/pointer
tới local variable:

```ivy
fn bad() -> int32& {
    int32 x = 42;
    return x;              // lỗi — returning a reference to a local variable
}
```

---

## 4. Null

### 4.1. Trong safe zone

**Không có `nullptr`** — `optional<T>` thay thế (B6):

```ivy
// Sai trong safe:
// int32* p = nullptr;      // lỗi — null pointer

// Đúng trong safe:
int32 x = 5;
ivy::optional<int32> maybe_x = x;  // B6 — thay nullable pointer
```

**Kiểu `nullptr_t`**: chỉ dùng trong `unsafe {}`.

### 4.2. Trong unsafe zone

`nullptr` được phép (C++ style):

```ivy
unsafe {
    // int32* p = nullptr; // OK
}
```

---

## 5. Xử lý lỗi

### 5.1. `expected<T, E>` (B5)

Thay thế `std::expected` (C++23):

```ivy
// B5 — ivy::expected<T, E>: struct { T value; E error; bool hasValue; }
// Pattern: if (result.hasValue) { ... } else { ... }
```

### 5.2. Exception

**Không dùng** `try/throw/catch` trong safe zone — chỉ `expected<T, E>`.

---

## 6. Triển khai

### 6.1. Pipeline

| Giai đoạn | Move semantics | Borrow | Lifetime |
|-----------|----------------|--------|----------|
| **Parser** | `move(expr)` → `Expr::Call{callee="move"}` | `T&` / `const T&` type modifier | `lifetime<$a>` annotation |
| **HIR** | Mark `MoveState::MovedOut` trong `moveStates_` | Borrow scope tracking | `lifetime<>` propagation |
| **MIR** | `moveStates_` stack — `Initialized` / `MovedOut` | `borrowScopes_` — aliasing XOR mutability | Lifetime verification (B2) |
| **Codegen** | `move()` pass-through (load value from operand) | `T&` → `ptr` | N/A |
| **Interpreter** | `move()` pass-through | `T&` → reference value | N/A |

### 6.2. B3: Move-state tracking trong MIR

```cpp
enum class MoveState { Initialized, MovedOut };
std::vector<std::unordered_map<std::string_view, MoveState>> moveStates_;
```

- **`moveStates_`**: stack of maps, push/pop cùng `scopes_` và `borrowScopes_`.
- **`markMovedOut(name)`**: đánh dấu biến = `MovedOut` trong scope gần nhất.
- **`isMovedOut(name)`**: kiểm tra biến có `MovedOut` không (search all scopes).
- **`reinitialize(name)`**: set biến về `Initialized` (sau re-assignment).
- **`inAssignLhs_`**: flag bypass use-after-move check cho lhs của assignment.
- **`unsafeDepth_`**: bypass move-state checking khi `unsafeDepth_ > 0`.

### 6.3. Thứ tự ưu tiên

1. `const T&` → immutable borrow
2. `T&` → mutable borrow
3. `Aliasing XOR Mutability` — kiểm tra trong MIR (B1)
4. `lifetime<>` annotation (A5) + verification (B2)
5. `move()` builtin + use-after-move check (A4 + B3)
6. `expected<T, E>` (B5)
7. `nullptr` → chỉ trong unsafe

---

## 7. TODO

- [x] `const T&` vào parser (type modifier)
- [x] `T&` vào parser (type modifier)
- [x] HIR — borrow scope tracking
- [x] MIR — `borrowScopes_` aliasing XOR mutability (B1)
- [x] Codegen — `T&` → `ptr`
- [x] `lifetime<>` annotation (A5) + verification (B2)
- [x] `move()` builtin + use-after-move check (A4 + B3)
- [x] Null safety (B4)
- [x] `optional<T>` (B6)
- [x] `expected<T, E>` (B5)
