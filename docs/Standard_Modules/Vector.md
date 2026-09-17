# ivy::vector\<T\>

Mảng động (dynamic array), generic, an toàn bộ nhớ — thay thế `T*` + `malloc`/`realloc`.

```ivy
import ivy.vector;
```

---

### 1. Tạo vector

```ivy
import ivy.vector;

ivy::vector<int32> v1;                    // Empty vector
ivy::vector<int32> v2 = {1, 2, 3, 4, 5}; // Initializer list
ivy::vector<int32> v3 = v2;              // Copy
ivy::vector<int32> v4 = move(v2);        // Move (v2 giờ moved-out)
```

---

### 2. Thêm / xóa phần tử

```ivy
ivy::vector<int32> v;

v.push(10);       // [10]
v.push(20);       // [10, 20]
v.push(30);       // [10, 20, 30]

int32 last = v.pop();  // last = 30, v = [10, 20]
// pop() trả ivy::optional<T> nếu vector có thể rỗng (xem §5)
```

---

### 3. Truy cập phần tử

```ivy
ivy::vector<int32> v = {10, 20, 30};

int32 a = v[0];        // 10 — bounds-checked, panic nếu out-of-range
int32 b = v[2];        // 30
int32 c = v.front();   // 10 — phần tử đầu
int32 d = v.back();    // 30 — phần tử cuối

// Unsafe: bỏ bounds check
unsafe {
    int32 e = v.unsafeGet(100); // Không bounds check — UB nếu out-of-range
}
```

> Bounds check trong safe zone: truy cập `v[i]` với `i >= v.len()` gọi `__ivy_panic` — chương trình dừng ngay với error message rõ ràng.

---

### 4. Properties

```ivy
ivy::vector<int32> v = {1, 2, 3};

size length = v.len();       // 3
size cap = v.capacity();     // >= 3 (có thể lớn hơn do pre-allocation)
bool empty = v.empty();      // false

v.reserve(100);              // Pre-allocate capacity cho ít nhất 100 phần tử
v.clear();                   // Xóa tất cả, len = 0, capacity giữ nguyên
```

---

### 5. Tất cả methods

| Method | Signature | Mô tả |
|--------|-----------|-------|
| `len()` | `size len() const` | Số phần tử |
| `capacity()` | `size capacity() const` | Dung lượng đã cấp phát |
| `empty()` | `bool empty() const` | `true` nếu rỗng |
| `push(val)` | `void push(T val)` | Thêm phần tử cuối |
| `pop()` | `T pop()` | Lấy & xóa phần tử cuối. Panic nếu rỗng |
| `operator[]` | `T& operator[](size index)` | Truy cập bounds-checked |
| `front()` | `T& front()` | Phần tử đầu. Panic nếu rỗng |
| `back()` | `T& back()` | Phần tử cuối. Panic nếu rỗng |
| `insert(pos, val)` | `void insert(size pos, T val)` | Chèn tại vị trí |
| `remove(pos)` | `T remove(size pos)` | Xóa tại vị trí, trả phần tử đã xóa |
| `clear()` | `void clear()` | Xóa tất cả phần tử |
| `reserve(n)` | `void reserve(size n)` | Cấp phát trước |
| `contains(val)` | `bool contains(const T& val) const` | Có chứa giá trị? (cần `operator==`) |
| `iter()` | `Iterator<T> iter() const` | Iterator dùng cho `for` loop |
| `slice()` | `ivy::slice<T> slice() const` | View vào toàn bộ vector |
| `slice(from, to)` | `ivy::slice<T> slice(size from, size to) const` | View vào phạm vi |

---

### 6. Iteration

```ivy
ivy::vector<int32> nums = {10, 20, 30};

// Range-based for (immutable borrow)
for (const int32& n : nums) {
    io::println(n);
}

// Range-based for (mutable borrow)
for (int32& n : nums) {
    n *= 2; // [20, 40, 60]
}

// Index-based
for (size i = 0; i < nums.len(); i += 1) {
    io::println(nums[i]);
}
```

---

### 7. Ownership & RAII

```ivy
void example() {
    ivy::vector<ivy::string> names = {"Alice", "Bob"};
    // names sở hữu buffer heap + mỗi string sở hữu buffer riêng

    names.push("Charlie");
    // Grow: realloc nếu cần, move existing elements

    ivy::vector<ivy::string> copy = names;      // Deep copy — copy tất cả strings
    ivy::vector<ivy::string> moved = move(names); // Move — transfer buffer ownership
    // names giờ moved-out

    // moved ra scope → ~vector() gọi ~string() cho mỗi element, rồi free buffer
}
```

- **Copy**: Deep copy (allocate buffer mới, copy/clone mỗi phần tử)
- **Move**: Transfer ownership (lấy pointer + len + capacity, zero-out source)
- **RAII**: Destructor gọi `~T()` cho mỗi phần tử, rồi free buffer
- **Grow**: Khi `len == capacity`, allocate buffer mới (2x), move phần tử sang

---

### 8. Vector + Slice

`ivy::vector<T>` có thể tạo `ivy::slice<T>` (view, không copy):

```ivy
ivy::vector<int32> v = {1, 2, 3, 4, 5};

ivy::slice<int32> all = v.slice();        // View toàn bộ [1..5]
ivy::slice<int32> mid = v.slice(1, 4);    // View [2, 3, 4]

// Slice borrow từ vector — vector không được thay đổi khi slice còn sống
// v.push(6); // ❌ Error: cannot mutate while immutably borrowed by slice
```

---

### 9. So sánh với các ngôn ngữ khác

| Ivy | Rust | C++ | Go |
|-----|------|-----|----|
| `ivy::vector<T>` | `Vec<T>` | `std::vector<T>` | `[]T` slice |
| `push(val)` | `.push(val)` | `.push_back(val)` | `append(s, val)` |
| `pop()` | `.pop()` | `.pop_back()` | `s[:len-1]` |
| `v[i]` (bounds-checked) | `v[i]` (panic) | `v[i]` (UB) / `v.at(i)` | `v[i]` (panic) |
| `len()` | `.len()` | `.size()` | `len(v)` |
| `slice(a, b)` | `&v[a..b]` | — | `v[a:b]` |
