# Cấu trúc C++ bị Ivy từ bỏ

- **Trạng thái**: Draft — cần review
- **Ngày**: 2026-08-20

---

## 1. Tổng quan

Ivy là subset của C++: chỉ giữ những gì tinh hoa nhất, cấu trúc nào không thể
đảm bảo an toàn — hoặc bị thay thế hoàn toàn bởi cú pháp khác — đều bị loại khỏi
ngôn ngữ. Tài liệu này ghi lại từng quyết định loại bỏ, lý do, và cách thay thế.

Mỗi mục loại bỏ đều an toàn nhờ **one-way source compatibility**: Ivy ⊂ C++,
nên ai thực sự cần cấu trúc đã loại thì compile file đó bằng clang/g++ — không
mất khả năng nào, chỉ mất sự tiện.

---

## 2. Danh sách loại bỏ

### 2.1. `T a(x)` — Direct-initialization bằng ngoặc tròn

**Loại bỏ hoàn toàn — kể cả trong `[[ivy::unsafe]]`.** Dạng khởi tạo biến bằng
ngoặc tròn không được phép ở bất kỳ đâu. Không giống con trỏ thô hay C-style
cast (chỉ cấm ngoài unsafe), cấu trúc này không có ngoại lệ nào cả:

```cpp
int a(0);              // ❌ error
int b(a);              // ❌ error
std::vector<int> v(10, 5);  // ❌ error (khi nào Ivy có template)
```

**Thay thế:**

```cpp
int a = 0;             // ✅ copy-init
int a{0};              // ✅ list-init (không narrowing)
int b = a;             // ✅
std::vector<int> v{};  // ✅ tạo rỗng
v.assign(10, 5);       // ✅ thay cho v(10, 5)
std::vector<int> v = make_filled(10, 5);  // ✅ builder/factory
```

#### Lý do loại bỏ

1. **Most vexing parse.** Trong C++, `T x();` là khai báo hàm, không phải biến —
   một trong những cạm bẫy kinh điển nhất của ngôn ngữ. `T x{}` luôn khai báo
   biến. Bỏ dạng ngoặc tròn loại toàn bộ lớp ambiguity này → đúng triết lý
   "safe by construction".

2. **Narrowing.** `int a{0.5}` bị báo lỗi lúc compile; `int a(0.5)` truncate
   im lặng. List-init nghiêm hơn, bắt lỗi mất dữ liệu từ lúc biên dịch.

3. **Thừa với scalar type.** `T x = e` và `T x{e}` bao phủ mọi trường hợp khởi
   tạo giá trị vô hướng. Không có tình huống nào chỉ paren-init mới làm được.

4. **Ambiguity với container (tương lai).** `std::vector<int> v(10, 5)` (10 phần
   tử giá trị 5) vs `v{10, 5}` (2 phần tử 10 và 5) chọn constructor khác nhau.
   Khi Ivy có template, hai cách thay thế, không cần paren-init:

   ```cpp
   std::vector<int> v{};   // tạo rỗng rồi gán
   v.assign(10, 5);        // assign() — tiện ích kế thừa, Ivy không cần giữ
   ```

   ```cpp
   std::vector<int> v = make_filled(10, 5);  // factory
   ```

   Guaranteed copy elision (C++17) / move semantics đảm bảo không mất hiệu năng
   so với `v(10, 5)`. Không cần constexpr/consteval — một hàm thường là đủ.
   Cả hai pattern đều không yêu cầu thêm cú pháp nào vào grammar.

5. **Escape hatch luôn tồn tại.** Ai cần đúng ngữ nghĩa paren-init thì compile
   bằng C++ compiler thật. Ivy không cần giữ lại cú pháp chỉ để vừa một thiểu số.

#### Vì sao không có ngoại lệ `unsafe`?

`[[ivy::unsafe]]` tồn tại để cấp **năng lực** mà Ivy không thể kiểm chứng nhưng
đôi khi người dùng cần mua bằng trách nhiệm của họ:

- C-style cast — bỏ qua kiểm tra, tăng hiệu năng khi người dùng biết chắc không lỗi
- `malloc` / `free`, con trỏ thô, pointer cast — raw memory, giao tiếp C API
- Trong unsafe, cast vẫn có tác dụng thật: `(uint64_t)&buf` để đọc byte...

`T a(x)` thì ngược lại — **không bán năng lực nào**: `T a = e` / `T a{e}` cho
cùng kết quả, cùng hiệu năng, không mất gì để viết. Nó chỉ đơn giản là thừa:

- Không bỏ qua kiểm tra nào (không narrowing check — đây là khuyết điểm, không
  phải ưu điểm như cast)
- Không làm được việc gì mà `=` / `{}` không làm được
- Còn mang theo most vexing parse — cạm bẫy, không phải công cụ

Cấu trúc thừa → loại **vô điều kiện**; unsafe không hồi sinh nó. Nguyên tắc:
*unsafe chỉ cấp năng lực, không hồi sinh cú pháp.*

#### Phạm vi

- Chỉ áp dụng cho **declaration**: `T name(args)`.
- **Không** áp dụng cho functional cast trong expression: `int(x)`, `double(y)`
  — đây là cấu trúc khác, quyết định riêng (xem [cast.md](cast.md), tạm thời
  chỉ trong `[[ivy::unsafe]]`).

#### Triển khai

- Parser hiện tại chỉ nhận `=` làm initializer ([parseDeclarator](file:///d:/project/Ivy/ivyc/src/parser.cpp#L585-L598));
  `T a(0)` lẫn `T a{0}` đều chưa được hỗ trợ. Quyết định này chốt: **không thêm
  dạng ngoặc tròn, kể cả bên trong `[[ivy::unsafe]]`** — unsafe chỉ thay đổi
  *năng lực* được phép, không thay đổi *cú pháp* được phép.
- Cải thiện error message khi gặp `(` sau declarator name:

  ```
  error: direct-initialization `T a(x)` is not supported in Ivy;
         use `=` or `{ }` instead
  ```

---

## 3. Các mục tiếp theo

Danh sách sẽ mở rộng. Ứng viên tiếp theo:

- [ ] `new` / `delete` ngoài `[[ivy::unsafe]]` (đã cấm — README)
- [ ] C-style cast ngoài `[[ivy::unsafe]]` (đã cấm — [cast.md](cast.md))
- [ ] Templates / exceptions (decide per feature)
- [ ] C++ compatibility types (`int`, `float`, `double`) — chỉ khi `#pragma ivy cnumber` ([types.md](types.md))
