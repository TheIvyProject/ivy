# ivy::slice\<T\>

View (borrow) vào một vùng liên tiếp của mảng hoặc vector — **không sở hữu dữ liệu**, bounds-checked, an toàn.

```ivy
import ivy.slice;
```

---

### 1. Tại sao cần `slice`?

Khi truyền mảng/vector vào hàm, thường không cần ownership — chỉ cần **đọc** (hoặc **sửa**) một phần:

```ivy
// ❌ Copy toàn bộ vector — lãng phí
void printAll(ivy::vector<int32> data) { ... }

// ✅ Borrow qua slice — zero-copy
void printAll(ivy::slice<int32> data) { ... }
```

---

### 2. Tạo slice

```ivy
import ivy.slice;
import ivy.vector;

// Từ vector
ivy::vector<int32> v = {10, 20, 30, 40, 50};
ivy::slice<int32> all = v.slice();          // [10, 20, 30, 40, 50]
ivy::slice<int32> mid = v.slice(1, 4);      // [20, 30, 40]

// Từ fixed-size array
int32[5] arr = {1, 2, 3, 4, 5};
ivy::slice<int32> s = arr;                  // Implicit conversion
ivy::slice<int32> part = arr.slice(0, 3);   // [1, 2, 3]
```

---

### 3. Truy cập phần tử

```ivy
ivy::slice<int32> s = v.slice();

int32 a = s[0];       // Bounds-checked, panic nếu out-of-range
int32 b = s.front();  // Phần tử đầu
int32 c = s.back();   // Phần tử cuối
size len = s.len();    // Số phần tử
bool e = s.empty();    // true nếu rỗng
```

---

### 4. Sub-slicing

```ivy
ivy::slice<int32> full = v.slice();         // [10, 20, 30, 40, 50]
ivy::slice<int32> sub = full.slice(1, 3);   // [20, 30]
ivy::slice<int32> tail = full.slice(3);     // [40, 50] (từ index 3 đến hết)
```

---

### 5. Tất cả methods

| Method | Signature | Mô tả |
|--------|-----------|-------|
| `len()` | `size len() const` | Số phần tử |
| `empty()` | `bool empty() const` | `true` nếu rỗng |
| `operator[]` | `const T& operator[](size index) const` | Truy cập bounds-checked |
| `front()` | `const T& front() const` | Phần tử đầu |
| `back()` | `const T& back() const` | Phần tử cuối |
| `slice(from, to)` | `ivy::slice<T> slice(size from, size to) const` | Sub-slice |
| `slice(from)` | `ivy::slice<T> slice(size from) const` | Sub-slice từ index đến hết |
| `contains(val)` | `bool contains(const T& val) const` | Có chứa giá trị? |
| `iter()` | `Iterator<T> iter() const` | Iterator cho `for` loop |

---

### 6. Mutable slice

```ivy
// Mutable borrow — có thể sửa phần tử
ivy::vector<int32> v = {1, 2, 3};
ivy::mut_slice<int32> ms = v.mutSlice();

ms[0] = 100;  // v[0] giờ = 100

// Aliasing XOR Mutability: không thể có slice + mut_slice cùng lúc
// ivy::slice<int32> s = v.slice(); // ❌ Error: already mutably borrowed
```

---

### 7. Borrowing rules

`slice<T>` là **borrow** — phải tuân thủ borrow checker:

```ivy
ivy::vector<int32> v = {1, 2, 3};
ivy::slice<int32> s = v.slice();  // Immutable borrow

// v.push(4); // ❌ Error: cannot mutate vector while borrowed by slice
io::println(s[0]); // ✅ OK

// s hết scope → borrow giải phóng
v.push(4); // ✅ OK — không còn borrow
```

---

### 8. Dùng trong hàm

```ivy
// Nhận slice — không copy, không ownership
fn sum(ivy::slice<int32> data) -> int32 {
    int32 total = 0;
    for (const int32& n : data) {
        total += n;
    }
    return total;
}

ivy::vector<int32> v = {1, 2, 3, 4, 5};
int32 s = sum(v.slice());       // 15
int32 s2 = sum(v.slice(0, 3));  // 6
```

---

### 9. Layout (nội bộ)

```
┌─────────────────────────────────┐
│  slice<T>                       │
│  ┌──────────┐  ┌─────────────┐  │
│  │  ptr      │  │  length     │  │
│  │ (T*)     │  │  (size)     │  │
│  └──────────┘  └─────────────┘  │
└─────────────────────────────────┘
Size = sizeof(pointer) + sizeof(size)  (16 bytes trên 64-bit)
```

Slice **không** sở hữu data — chỉ giữ pointer + length. Nhẹ, copy giá rẻ (copy pointer + size, không copy data).

---

### 10. So sánh với các ngôn ngữ khác

| Ivy | Rust | C++ | Go |
|-----|------|-----|----|
| `ivy::slice<T>` | `&[T]` | `std::span<T>` (C++20) | `[]T` |
| `ivy::mut_slice<T>` | `&mut [T]` | `std::span<T>` (non-const) | `[]T` |
| `s[i]` (bounds-checked) | `s[i]` (panic) | `s[i]` (UB) | `s[i]` (panic) |
| `s.slice(a, b)` | `&s[a..b]` | `s.subspan(a, b-a)` | `s[a:b]` |
