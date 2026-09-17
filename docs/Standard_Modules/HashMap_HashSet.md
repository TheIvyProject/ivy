# ivy::hash_map\<K, V\> & ivy::hash_set\<T\>

Bảng băm (hash table) — ánh xạ key→value và tập hợp không trùng.

```ivy
import ivy.hash_map;
import ivy.hash_set;
```

---

## ivy::hash_map\<K, V\>

### 1. Tạo hash_map

```ivy
import ivy.hash_map;

ivy::hash_map<ivy::string, int32> ages;                      // Empty
ivy::hash_map<ivy::string, int32> ages2 = {
    {"Alice", 30},
    {"Bob", 25}
};
```

---

### 2. Thêm / truy cập / xóa

```ivy
ivy::hash_map<ivy::string, int32> m;

// Thêm
m.insert("Alice", 30);
m.insert("Bob", 25);

// Truy cập
ivy::optional<int32> age = m.get("Alice");  // optional{30}
ivy::optional<int32> none = m.get("Carol"); // nullopt

// Truy cập với giá trị mặc định
int32 a = m.getOr("Alice", 0);  // 30
int32 c = m.getOr("Carol", 0);  // 0

// operator[] — trả reference, insert default nếu chưa có
m["Charlie"] = 35;  // Insert Charlie → 35
int32 val = m["Charlie"]; // 35

// Xóa
bool removed = m.remove("Bob"); // true, xóa Bob
bool r2 = m.remove("Nobody");   // false, không tồn tại
```

---

### 3. Kiểm tra & kích thước

```ivy
bool has = m.contains("Alice");  // true
size len = m.len();               // Số cặp key-value
bool e = m.empty();               // true nếu rỗng
m.clear();                        // Xóa tất cả
```

---

### 4. Iteration

```ivy
ivy::hash_map<ivy::string, int32> scores = {
    {"Alice", 95},
    {"Bob", 87}
};

// Iterate qua key-value pairs
for (const auto& [key, value] : scores) {
    io::println("{}: {}", key, value);
}

// Iterate chỉ keys
for (const ivy::string& k : scores.keys()) {
    io::println(k);
}

// Iterate chỉ values
for (const int32& v : scores.values()) {
    io::println(v);
}
```

---

### 5. Tất cả methods

| Method | Signature | Mô tả |
|--------|-----------|-------|
| `insert(key, val)` | `void insert(K key, V val)` | Thêm hoặc ghi đè |
| `get(key)` | `ivy::optional<V> get(const K& key) const` | Lấy giá trị, nullopt nếu không có |
| `getOr(key, default)` | `V getOr(const K& key, V defaultVal) const` | Lấy giá trị hoặc default |
| `operator[]` | `V& operator[](const K& key)` | Truy cập (insert default nếu chưa có) |
| `contains(key)` | `bool contains(const K& key) const` | Có key? |
| `remove(key)` | `bool remove(const K& key)` | Xóa, trả true nếu tồn tại |
| `len()` | `size len() const` | Số phần tử |
| `empty()` | `bool empty() const` | Rỗng? |
| `clear()` | `void clear()` | Xóa tất cả |
| `keys()` | `Iterator<K> keys() const` | Iterator qua keys |
| `values()` | `Iterator<V> values() const` | Iterator qua values |
| `iter()` | `Iterator<Pair<K,V>> iter() const` | Iterator qua pairs |

---

### 6. Key requirements

Kiểu `K` phải triển khai:
- `operator==(const K&, const K&)` — so sánh bằng
- `hash(const K&) -> size` — hàm băm

Tất cả built-in types (`int32`, `ivy::string`,...) đã triển khai sẵn.

Cho custom type:

```ivy
struct Point {
    int32 x;
    int32 y;

    bool operator==(const Point& other) const {
        return this.x == other.x && this.y == other.y;
    }
};

// Triển khai hash cho Point
size hash(const Point& p) {
    return hash(p.x) ^ (hash(p.y) << 1);
}

ivy::hash_map<Point, ivy::string> labels;
labels.insert({1, 2}, "origin-ish");
```

---

## ivy::hash_set\<T\>

### 7. Tạo hash_set

```ivy
import ivy.hash_set;

ivy::hash_set<int32> s;
ivy::hash_set<int32> s2 = {1, 2, 3, 4, 5};
```

---

### 8. Thêm / xóa / kiểm tra

```ivy
ivy::hash_set<int32> s;

s.insert(10);
s.insert(20);
s.insert(10);  // Duplicate — không thêm

bool has = s.contains(10);  // true
bool removed = s.remove(20); // true
size len = s.len();          // 1
```

---

### 9. Set operations

```ivy
ivy::hash_set<int32> a = {1, 2, 3, 4};
ivy::hash_set<int32> b = {3, 4, 5, 6};

ivy::hash_set<int32> u = a.unionWith(b);        // {1, 2, 3, 4, 5, 6}
ivy::hash_set<int32> i = a.intersection(b);     // {3, 4}
ivy::hash_set<int32> d = a.difference(b);       // {1, 2}
bool sub = a.isSubsetOf(b);                      // false
```

---

### 10. Tất cả hash_set methods

| Method | Signature | Mô tả |
|--------|-----------|-------|
| `insert(val)` | `bool insert(T val)` | Thêm, trả true nếu mới |
| `contains(val)` | `bool contains(const T& val) const` | Có phần tử? |
| `remove(val)` | `bool remove(const T& val)` | Xóa, trả true nếu tồn tại |
| `len()` | `size len() const` | Số phần tử |
| `empty()` | `bool empty() const` | Rỗng? |
| `clear()` | `void clear()` | Xóa tất cả |
| `unionWith(other)` | `hash_set<T> unionWith(const hash_set<T>& other) const` | Hợp |
| `intersection(other)` | `hash_set<T> intersection(const hash_set<T>& other) const` | Giao |
| `difference(other)` | `hash_set<T> difference(const hash_set<T>& other) const` | Hiệu |
| `isSubsetOf(other)` | `bool isSubsetOf(const hash_set<T>& other) const` | Tập con? |
| `iter()` | `Iterator<T> iter() const` | Iterator |

---

### 11. Ownership & RAII

Cả `hash_map` và `hash_set`:
- **Copy**: Deep copy (copy tất cả entries)
- **Move**: Transfer ownership (move internal table)
- **RAII**: Destructor gọi `~K()`, `~V()` / `~T()` cho mỗi entry, rồi free table

---

### 12. So sánh với các ngôn ngữ khác

| Ivy | Rust | C++ | Go | Python |
|-----|------|-----|----|--------|
| `ivy::hash_map<K,V>` | `HashMap<K,V>` | `std::unordered_map<K,V>` | `map[K]V` | `dict` |
| `ivy::hash_set<T>` | `HashSet<T>` | `std::unordered_set<T>` | — | `set` |
| `get(k)` → `optional` | `.get(&k)` → `Option` | `.find(k)` → `iterator` | `v, ok := m[k]` | `m.get(k)` |
| `contains(k)` | `.contains(&k)` | `.contains(k)` (C++20) | `_, ok := m[k]` | `k in m` |
