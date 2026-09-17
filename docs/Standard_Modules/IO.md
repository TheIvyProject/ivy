# ivy::io

Module I/O chuẩn — đọc/ghi stdout, stderr, stdin và file.

```ivy
import ivy.io;
```

---

### 1. Output — in ra console

```ivy
import ivy.io;

// print — in không xuống dòng
io::print("Hello");
io::print(42);
io::print(3.14f);

// println — in + xuống dòng
io::println("Hello, Ivy!");
io::println(42);

// Format string với placeholder {}
io::println("Name: {}, Age: {}", "Alice", 30);
// Output: Name: Alice, Age: 30

// eprint / eprintln — in ra stderr
io::eprintln("Error: file not found");
```

---

### 2. Input — đọc từ stdin

```ivy
import ivy.io;

// Đọc một dòng từ stdin
ivy::string line = io::readLine();
io::println("You typed: {}", line);

// Đọc với prompt
ivy::string name = io::readLine("Enter your name: ");
```

---

### 3. Print functions

| Hàm | Signature | Mô tả |
|-----|-----------|-------|
| `io::print(args...)` | `void print(Args... args)` | In ra stdout, không xuống dòng |
| `io::println(args...)` | `void println(Args... args)` | In ra stdout + newline |
| `io::eprint(args...)` | `void eprint(Args... args)` | In ra stderr, không xuống dòng |
| `io::eprintln(args...)` | `void eprintln(Args... args)` | In ra stderr + newline |
| `io::readLine()` | `ivy::string readLine()` | Đọc một dòng từ stdin |
| `io::readLine(prompt)` | `ivy::string readLine(const ivy::string& prompt)` | In prompt rồi đọc |

---

### 4. Format string

`io::print` / `io::println` hỗ trợ **type-safe format string** với placeholder `{}`:

```ivy
int32 x = 42;
float64 pi = 3.14159;
ivy::string name = "Ivy";
bool flag = true;

io::println("x = {}", x);           // x = 42
io::println("pi = {}", pi);         // pi = 3.14159
io::println("name = {}", name);     // name = Ivy
io::println("flag = {}", flag);     // flag = true
io::println("{} + {} = {}", 1, 2, 3); // 1 + 2 = 3
```

**Quy tắc format**:
- `{}` — format mặc định cho kiểu T (gọi `T::format()` nếu có)
- Số lượng `{}` phải khớp số arguments — **compile-time check**
- Kiểu T phải triển khai concept `Formattable` (tất cả built-in types đã triển khai)

---

### 5. File I/O

```ivy
import ivy.io;

// Đọc toàn bộ file
ivy::expected<ivy::string, io::Error> content = io::readFile("data.txt");
if (content.hasValue) {
    io::println(content.value);
} else {
    io::eprintln("Error: {}", content.error);
}

// Ghi file (tạo mới hoặc ghi đè)
ivy::expected<void, io::Error> result = io::writeFile("output.txt", "Hello!");

// Append vào file
ivy::expected<void, io::Error> result2 = io::appendFile("log.txt", "New entry\n");
```

---

### 6. io::File — file handle

```ivy
import ivy.io;

// Mở file
ivy::expected<io::File, io::Error> f = io::File::open("data.txt", io::Mode::Read);

if (f.hasValue) {
    io::File& file = f.value;

    // Đọc toàn bộ
    ivy::expected<ivy::string, io::Error> content = file.readAll();

    // Đọc từng dòng
    ivy::expected<ivy::string, io::Error> line = file.readLine();

    // file ra scope → RAII tự đóng
}
```

| Method | Signature | Mô tả |
|--------|-----------|-------|
| `File::open(path, mode)` | `expected<File, Error>` | Mở file |
| `readAll()` | `expected<string, Error>` | Đọc toàn bộ nội dung |
| `readLine()` | `expected<string, Error>` | Đọc một dòng |
| `write(data)` | `expected<size, Error>` | Ghi data, trả số byte đã ghi |
| `flush()` | `expected<void, Error>` | Flush buffer |

---

### 7. io::Mode

```ivy
enum class io::Mode {
    Read,           // Đọc (file phải tồn tại)
    Write,          // Ghi (tạo mới hoặc ghi đè)
    Append,         // Ghi tiếp cuối file
    ReadWrite       // Đọc + ghi
};
```

---

### 8. io::Error

```ivy
enum class io::Error {
    NotFound,           // File không tồn tại
    PermissionDenied,   // Không có quyền
    AlreadyExists,      // File đã tồn tại (khi tạo exclusive)
    IoError,            // Lỗi I/O chung
    InvalidPath         // Đường dẫn không hợp lệ
};
```

---

### 9. RAII — tự đóng file

```ivy
void process() {
    auto f = io::File::open("data.txt", io::Mode::Read);
    if (!f.hasValue) return;

    io::File& file = f.value;
    // ... dùng file ...

    // Không cần gọi file.close() — destructor ~File() tự đóng
} // ~File() gọi OS close() tại đây
```

---

### 10. So sánh với các ngôn ngữ khác

| Ivy | Rust | C++ | Go | Python |
|-----|------|-----|----|--------|
| `io::println(...)` | `println!(...)` | `std::cout << ...` | `fmt.Println(...)` | `print(...)` |
| `io::readFile(path)` | `fs::read_to_string(path)` | `std::ifstream` | `os.ReadFile(path)` | `open(path).read()` |
| `io::File::open(...)` | `File::open(...)` | `std::fstream` | `os.Open(...)` | `open(...)` |
| RAII close | RAII (Drop) | RAII (dtor) | `defer f.Close()` | `with` statement |
| `expected<T, Error>` | `Result<T, io::Error>` | exception / error_code | `(T, error)` | exception |
