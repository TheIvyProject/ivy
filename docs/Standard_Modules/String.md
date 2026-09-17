# ivy::string

Kiểu chuỗi ký tự owned, UTF-8, an toàn bộ nhớ — thay thế `char*` và `std::string`.

```ivy
import ivy.string;
```

---

### 1. Tạo string

```ivy
import ivy.string;

ivy::string s1 = "Hello, Ivy!";      // Từ string literal
ivy::string s2;                       // Empty string (len == 0)
ivy::string s3 = s1;                  // Copy
ivy::string s4 = move(s1);           // Move (s1 giờ moved-out)
```

---

### 2. Properties

| Property | Kiểu | Mô tả |
|----------|------|-------|
| `len()` | `size` | Số byte (UTF-8) trong chuỗi |
| `empty()` | `bool` | `true` nếu `len() == 0` |

```ivy
ivy::string name = "Ivy";
io::println(name.len());    // 3
io::println(name.empty());  // false
```

---

### 3. Methods

#### Truy cập

```ivy
ivy::string s = "Hello";

char c = s[0];          // 'H' — bounds-checked, panic nếu out-of-range
char last = s[s.len() - 1]; // 'o'

// Slice — trả ivy::string_view (borrow, không copy)
auto sub = s.substr(0, 3); // "Hel"
```

#### Nối chuỗi

```ivy
ivy::string a = "Hello";
ivy::string b = " World";

a.append(b);              // a = "Hello World" (in-place)
ivy::string c = a + b;    // Operator + tạo string mới

a.append('!');             // Append single char
```

#### Tìm kiếm

```ivy
ivy::string path = "/home/user/file.txt";

bool found = path.contains("user");       // true
ivy::optional<size> pos = path.find("file"); // optional{11}
bool starts = path.startsWith("/home");    // true
bool ends = path.endsWith(".txt");         // true
```

#### So sánh

```ivy
ivy::string a = "abc";
ivy::string b = "abc";

bool eq = (a == b);  // true
bool ne = (a != b);  // false
bool lt = (a < b);   // false — lexicographic comparison
```

---

### 4. Tất cả methods

| Method | Signature | Mô tả |
|--------|-----------|-------|
| `len()` | `size len() const` | Số byte UTF-8 |
| `empty()` | `bool empty() const` | `true` nếu rỗng |
| `operator[]` | `char operator[](size index) const` | Truy cập byte (bounds-checked) |
| `substr(pos, count)` | `ivy::string substr(size pos, size count) const` | Chuỗi con (copy) |
| `append(s)` | `void append(const ivy::string& s)` | Nối chuỗi in-place |
| `append(c)` | `void append(char c)` | Nối ký tự in-place |
| `operator+` | `ivy::string operator+(const ivy::string& rhs) const` | Tạo chuỗi mới |
| `contains(s)` | `bool contains(const ivy::string& s) const` | Có chứa chuỗi con? |
| `find(s)` | `ivy::optional<size> find(const ivy::string& s) const` | Vị trí đầu tiên, hoặc `nullopt` |
| `startsWith(s)` | `bool startsWith(const ivy::string& s) const` | Bắt đầu bằng? |
| `endsWith(s)` | `bool endsWith(const ivy::string& s) const` | Kết thúc bằng? |
| `trim()` | `ivy::string trim() const` | Bỏ whitespace đầu/cuối (tạo string mới) |
| `toLower()` | `ivy::string toLower() const` | Chuyển thường (ASCII) |
| `toUpper()` | `ivy::string toUpper() const` | Chuyển hoa (ASCII) |
| `split(delim)` | `ivy::vector<ivy::string> split(char delim) const` | Tách theo ký tự |

---

### 5. Ownership & RAII

`ivy::string` là **owned type** — tự quản lý bộ nhớ:

```ivy
void example() {
    ivy::string s = "Hello";
    // s sở hữu buffer heap chứa "Hello"

    ivy::string t = s;     // Copy — t có buffer riêng
    ivy::string u = move(s); // Move — u lấy buffer của s, s moved-out

    // u ra scope → ~string() tự free buffer
    // t ra scope → ~string() tự free buffer
}
```

- **Copy**: Deep copy (allocate buffer mới, memcpy nội dung)
- **Move**: Transfer ownership (lấy pointer, zero-out source)
- **RAII**: Destructor `~string()` tự free buffer khi ra scope

---

### 6. String literals & encoding

```ivy
ivy::string utf8 = "Xin chào 🌍";  // UTF-8 encoded
size byteLen = utf8.len();           // Byte count (không phải character count)

// Raw string (không xử lý escape sequences)
ivy::string raw = R"(C:\Users\path\to\file)";
```

> **Lưu ý**: `len()` trả về byte count, không phải character/codepoint count.
> UTF-8 string có thể có byte count > character count.

---

### 7. So sánh với các ngôn ngữ khác

| Ivy | Rust | C++ | Go |
|-----|------|-----|----|
| `ivy::string` | `String` | `std::string` | `string` |
| `ivy::string_view` (planned) | `&str` | `std::string_view` | `string` (immutable) |
| `len()` | `.len()` | `.size()` | `len(s)` |
| `find()` → `optional<size>` | `.find()` → `Option<usize>` | `.find()` → `npos` | `strings.Index()` → `-1` |
