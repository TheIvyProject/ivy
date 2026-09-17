# ivy::math

Module toán học chuẩn — hàm số học, lượng giác, hằng số.

```ivy
import ivy.math;
```

---

### 1. Hằng số

```ivy
import ivy.math;

float64 pi = math::PI;       // 3.14159265358979323846
float64 e = math::E;         // 2.71828182845904523536
float64 inf = math::INF;     // Positive infinity
float64 nan = math::NAN;     // Not a Number
```

| Hằng số | Kiểu | Giá trị |
|---------|------|---------|
| `math::PI` | `float64` | π ≈ 3.14159265358979 |
| `math::E` | `float64` | e ≈ 2.71828182845905 |
| `math::INF` | `float64` | +∞ |
| `math::NAN` | `float64` | NaN |

---

### 2. Hàm số học cơ bản

```ivy
int32 a = math::abs(-42);        // 42
int32 mn = math::min(3, 7);      // 3
int32 mx = math::max(3, 7);      // 7
int32 cl = math::clamp(15, 0, 10); // 10 (clamp vào [0, 10])
```

| Hàm | Signature | Mô tả |
|-----|-----------|-------|
| `math::abs(x)` | `T abs(T x)` | Giá trị tuyệt đối |
| `math::min(a, b)` | `T min(T a, T b)` | Nhỏ hơn |
| `math::max(a, b)` | `T max(T a, T b)` | Lớn hơn |
| `math::clamp(x, lo, hi)` | `T clamp(T x, T lo, T hi)` | Giới hạn x vào [lo, hi] |

> Tất cả hàm trên là **generic** — hoạt động cho mọi kiểu số (`int32`, `float64`,...).
> Đồng thời là **constexpr** — có thể dùng tại compile-time.

---

### 3. Lũy thừa & căn

```ivy
float64 s = math::sqrt(16.0);    // 4.0
float64 c = math::cbrt(27.0);    // 3.0
float64 p = math::pow(2.0, 10.0); // 1024.0
float64 l = math::log(math::E);   // 1.0
float64 l2 = math::log2(8.0);    // 3.0
float64 l10 = math::log10(100.0); // 2.0
```

| Hàm | Signature | Mô tả |
|-----|-----------|-------|
| `math::sqrt(x)` | `float64 sqrt(float64 x)` | Căn bậc 2 |
| `math::cbrt(x)` | `float64 cbrt(float64 x)` | Căn bậc 3 |
| `math::pow(base, exp)` | `float64 pow(float64 base, float64 exp)` | Lũy thừa |
| `math::log(x)` | `float64 log(float64 x)` | Logarit tự nhiên (ln) |
| `math::log2(x)` | `float64 log2(float64 x)` | Logarit cơ số 2 |
| `math::log10(x)` | `float64 log10(float64 x)` | Logarit cơ số 10 |

---

### 4. Lượng giác

```ivy
float64 s = math::sin(math::PI / 2.0);  // 1.0
float64 c = math::cos(0.0);              // 1.0
float64 t = math::tan(math::PI / 4.0);  // ≈ 1.0

float64 as = math::asin(1.0);            // π/2
float64 ac = math::acos(1.0);            // 0.0
float64 at = math::atan(1.0);            // π/4
float64 at2 = math::atan2(1.0, 1.0);    // π/4
```

| Hàm | Signature | Mô tả |
|-----|-----------|-------|
| `math::sin(x)` | `float64 sin(float64 x)` | Sin (radians) |
| `math::cos(x)` | `float64 cos(float64 x)` | Cos (radians) |
| `math::tan(x)` | `float64 tan(float64 x)` | Tan (radians) |
| `math::asin(x)` | `float64 asin(float64 x)` | Arcsin |
| `math::acos(x)` | `float64 acos(float64 x)` | Arccos |
| `math::atan(x)` | `float64 atan(float64 x)` | Arctan |
| `math::atan2(y, x)` | `float64 atan2(float64 y, float64 x)` | Arctan2 (2 args) |

---

### 5. Làm tròn

```ivy
float64 f = math::floor(3.7);   // 3.0
float64 c = math::ceil(3.2);    // 4.0
float64 r = math::round(3.5);   // 4.0
float64 t = math::trunc(3.9);   // 3.0 (cắt phần thập phân)
```

| Hàm | Signature | Mô tả |
|-----|-----------|-------|
| `math::floor(x)` | `float64 floor(float64 x)` | Làm tròn xuống |
| `math::ceil(x)` | `float64 ceil(float64 x)` | Làm tròn lên |
| `math::round(x)` | `float64 round(float64 x)` | Làm tròn gần nhất |
| `math::trunc(x)` | `float64 trunc(float64 x)` | Cắt phần thập phân |

---

### 6. Kiểm tra đặc biệt

```ivy
bool inf = math::isInf(math::INF);   // true
bool nan = math::isNaN(math::NAN);   // true
bool fin = math::isFinite(3.14);     // true
```

| Hàm | Signature | Mô tả |
|-----|-----------|-------|
| `math::isInf(x)` | `bool isInf(float64 x)` | Kiểm tra infinity |
| `math::isNaN(x)` | `bool isNaN(float64 x)` | Kiểm tra NaN |
| `math::isFinite(x)` | `bool isFinite(float64 x)` | Kiểm tra hữu hạn |

---

### 7. Constexpr

Hầu hết hàm math hỗ trợ **constexpr** — dùng tại compile-time:

```ivy
constexpr float64 HALF_PI = math::PI / 2.0;
constexpr int32 MAX_VAL = math::max(100, 200);  // 200 tại compile-time
```

---

### 8. So sánh với các ngôn ngữ khác

| Ivy | Rust | C++ | Go | Python |
|-----|------|-----|----|--------|
| `math::sqrt(x)` | `x.sqrt()` | `std::sqrt(x)` | `math.Sqrt(x)` | `math.sqrt(x)` |
| `math::PI` | `std::f64::consts::PI` | `M_PI` | `math.Pi` | `math.pi` |
| `math::abs(x)` | `x.abs()` | `std::abs(x)` | `math.Abs(x)` | `abs(x)` |
| `math::min(a,b)` | `a.min(b)` | `std::min(a,b)` | `min(a,b)` (Go 1.21) | `min(a,b)` |
