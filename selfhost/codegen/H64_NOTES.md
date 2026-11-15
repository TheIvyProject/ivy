# H6.4 - Linker orchestration (`Linker`)

## 1. Phạm vi

H6.4 port `CodeGen::linkExecutable` (`src/codegen/codegen.cpp:2810-3063`) sang
selfhost, nối tiếp `ObjEmit` của H6.3:

1. tính object tạm cạnh file exe: `<dir>/<stem>__tmp.obj`;
2. emit object bằng `ObjEmit` (mirror `emitObject`);
3. tìm linker: `IVY_LINKER`, nếu không có thì dòng đầu của
   `_popen("where clang++")`;
4. chạy `"<linker>" "<obj>" -o "<exe>" "<ivyrt>"` (mọi path đều quote);
5. xoá object tạm khi thành công, giữ lại khi thất bại.

Deliverables:

- `selfhost/codegen/codegen.ivy`: 6 extern C runtime + `StrBuf` + struct
  export `Linker`;
- `selfhost/codegen/linkdrive.ivy`: driver stdin 4 dòng;
- `selfhost/codegen/build_link_smoke.ps1`: harness e2e + negative;
- `selfhost/codegen/build_smoke.ps1`: link thêm `lib/ivyrt.c`;
- `selfhost/codegen/H64_NOTES.md` (file này);
- `PLAN.md` dòng H6.4.

Ngoài phạm vi (document ở mục 11): object từ `import cpp` / `import c`, object
module của D1, `--crt-static`, cross-compile.

Không sửa `src/**`, không rebuild oracle (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`).

## 2. Binding additions (`codegen.ivy`)

```ivy
extern "C" unsigned long long __ivy_strlen(const char* s);
extern "C" void  __ivy_memcpy(void* dst, const void* src, unsigned long long n);
extern "C" char* __ivy_strdup(const char* s);

extern "C" char* getenv(const char* name);
extern "C" void* _popen(const char* command, const char* mode);
extern "C" char* fgets(char* s, int n, void* stream);
extern "C" int   _pclose(void* stream);
extern "C" int   system(const char* command);
extern "C" int   remove(const char* path);
```

`_popen`/`_pclose` là spelling MSVC CRT mà oracle dùng trong nhánh `_WIN32`
(`codegen.cpp:2840-2858`). `FILE*` được khai báo `void*` (opaque).

## 3. `StrBuf`

Ivy CLI không có `std::string`, nên command line được ghép trong buffer tăng
trưởng nội bộ module:

| method | việc |
| --- | --- |
| `setup(cap)` | cấp `__ivy_alloc`, đặt `len_ = 0`, `data_[0] = 0` |
| `ensure(extra)` | gấp đôi `cap_` tới khi đủ `len_ + extra + 1`, copy + free buffer cũ |
| `add(s)` | `__ivy_memcpy` `strlen(s)` byte, NUL-terminate |
| `add_char(c)` | thêm 1 byte |
| `add_quoted(s)` | `"` + `s` + `"` (mã 34) |
| `str()` | `const char*` NUL-terminated |
| `length()` | `long` |
| `release()` | free và zero hoá |

Dùng mã ký tự số (34 `"`, 32 space, 92 `\`, 47 `/`, 46 `.`, 10/13 newline) cho
các ký tự dễ nhầm escape.

## 4. `Linker`

```ivy
export struct Linker {
    long tag;
    long code_;
    long emit_code_;
    long link_rc_;
    const char* err_;
    char* cmd_own_;

    char* temp_obj_path(const char* exe_path);
    char* find_linker();
    int link_text(const char* ir, unsigned long long len, const char* exe_path,
                  const char* ivyrt_lib);

    long code();
    long emit_code();
    long link_rc();
    const char* err();
    const char* cmd();
};
```

`link_text` trả về và lưu `code_`:

| code | nghĩa | oracle tương ứng |
| --- | --- | --- |
| 0 | thành công, object tạm đã xoá | `linkExecutable` trả `true` |
| 1 | `ObjEmit` thất bại (`emit_code_` = 1-6, `err_` = message LLVM) | `emitObject` trả `false` |
| 2 | không tìm được linker, object tạm đã xoá | `cannot locate a linker ...` |
| 3 | linker exit != 0 (`link_rc_`), object tạm giữ lại | `linker failed (exit code N): <cmd>` |

`cmd()` trả command line đúng như oracle in trong chẩn đoán (không có cặp
quote bao ngoài dùng để chạy - xem mục 6); buffer được giữ tới lần
`link_text` kế tiếp (driver chỉ gọi một lần nên không free, tránh đọc field
chưa khởi tạo).

### 4.1 Object tạm

`temp_obj_path` mirror `exeP.parent_path() / (exeP.stem() + "__tmp.obj")`:

- chấp nhận cả `\` (92) và `/` (47); tiền tố parent (kể cả separator) copy
  nguyên văn;
- `stem` = phần sau separator cuối, cắt tại dấu `.` cuối;
- dấu `.` đầu tiên ngay sau separator **không** phải extension (giống
  `std::filesystem::path::stem()`);
- không có separator → object nằm ngay cạnh (chỉ `stem__tmp.obj`).

### 4.2 Tìm linker

```text
IVY_LINKER (khác rỗng)  ->  dùng nguyên giá trị (strdup)
ngược lại               ->  _popen("where clang++", "r") -> fgets 1024
                            -> cắt \r\n cuối -> strdup
không có               ->  remove(obj) + code 2
```

Giống oracle: chỉ lấy dòng đầu tiên; `_pclose` luôn được gọi; buffer 1024 byte
cấp bằng `__ivy_alloc` (Ivy không decay `char[1024]` thành `char*`).

## 5. Command line

```text
"<linker>" "<obj>" -o "<exe>" "<ivyrt>"
```

- thứ tự và khoảng trắng y hệt `codegen.cpp:2998-3011`;
- `"<ivyrt>"` chỉ thêm khi `IVYRT_LIB_PATH` khác rỗng (driver truyền dòng 4
  rỗng để bỏ);
- `--crt-static`, object `import cpp`/`import c`, object module D1 không có
  trong subset nên không được nối vào.

## 6. `system()` và quoting của `cmd.exe`

Oracle chạy linker bằng `CreateProcessA` trên Windows, nên command line được
truyền nguyên văn. Port dùng `system()` (một dòng, không cần bind Win32
struct). Trên Windows `system()` đi qua `cmd.exe /c`, và `cmd.exe` có quy tắc:
nếu command line bắt đầu bằng `"` và có **nhiều hơn hai** dấu quote thì nó bỏ
dấu quote đầu và dấu quote cuối cùng.

Với 4 path được quote (linker, obj, exe, ivyrt) quy tắc đó phá command line:
probe cho `rc = 1` và `cmd.exe` báo
`The filename, directory name, or volume label syntax is incorrect.` trong khi
oracle cho `rc = 2` + stderr của `where.exe`.

Cách xử lý: **bọc toàn bộ command line trong một cặp quote nữa** khi chạy:

```text
system("\"" + cmd + "\"")
```

`cmd.exe` bỏ cặp ngoài cùng, linker nhận đúng quoting bên trong. `cmd()` vẫn
giữ spelling không bọc để chẩn đoán khớp byte oracle. Probe xác nhận với cả
path có space:

```text
IVY_LINKER=C:\Windows\System32\where.exe
stderr: ERROR: Invalid pattern is specified in "path:pattern".
        <src>:0:0: error: linker failed (exit code 2): "C:\...\where.exe" ...
exit:   1 (cả oracle và port)
```

## 7. Driver (`linkdrive.ivy`)

stdin 4 dòng:

```text
<path IR text>
<path executable>
<source path - chỉ để tạo chẩn đoán>
<path libivyrt - dòng rỗng/EOF = không nối runtime>
```

Chẩn đoán (stderr), khớp byte oracle:

```text
<src>:0:0: error: LLVM IR parse error: ivy: <LLVM diagnostic>     (code 1, emit_code 3)
<src>:0:0: error: LLVM target lookup failed: <msg>                 (code 1, emit_code 4)
<src>:0:0: error: <err>                                            (code 1, emit_code khác)
<src>:0:0: error: cannot locate a linker (clang++); set IVY_LINKER or add clang++ to PATH
<src>:0:0: error: linker failed (exit code <rc>): <cmd>
```

Không có source path (dòng 3 rỗng/EOF) thì tiền tố là `ivy: error: ` như các
driver khác. Exit: 0 thành công, 1 thất bại, 2 input không hợp lệ.

## 8. Harness (`build_link_smoke.ps1`)

1. build 11 module selfhost + `cgdrive` (như `cgdiff.ps1`);
2. build `codegen.ivy` + `linkdrive.ivy` (`-c`), compile `lib/ivyrt.c`;
3. link `linkdrive.exe` = linkdrive + codegen + ivyrt + `llvm-config --libs x86`
   + system libs (bỏ `libxml2s.lib` không cài);
4. mỗi fixture trong `-SmokeFiles`:
   - cgdrive → IR text;
   - linkdrive → exe (`out/<name>.self.exe`), kiểm
     `out/<name>.self__tmp.obj` **đã bị xoá**;
   - oracle `ivy -o` → exe;
   - chạy cả hai, so stdout/stderr/exit **raw bytes**;
5. negative (a): `IVY_LINKER=where.exe`, chạy oracle `ivy -o` rồi linkdrive với
   **cùng** exe path / src arg / ivyrt path; yêu cầu cả hai exit 1, stderr
   byte-identical, object tạm **còn lại** ở cả hai;
6. negative (b): fixture `103_print_ref.ivy` (IR oracle tự sinh bị LLVM từ
   chối); yêu cầu cả hai exit 1, stderr byte-identical, **không** tạo object
   tạm;
7. edge: link fixture `-NegativeFile` vào thư mục có tên chứa space, chạy và
   so với exe oracle (bảo vệ cách quote cho `cmd.exe`, mục 6).

`-SmokeFiles` nhận danh sách phẩy; `-NegativeFile` / `-EmitFailFile` đổi
fixture negative; `-IvyrtLib` mặc định
`<repo>/build/Release/ivyrt.lib` với `/` (đúng giá trị `IVYRT_LIB_PATH` đã bake
vào oracle, cần cho byte-parity của chẩn đoán).

## 9. Kết quả (2026-11-15)

```text
build_link_smoke.ps1:
  run PASS 8 / 8
  negative PASS 2 / 2
  space PASS 1 / 1
build_object_smoke.ps1: object PASS 246 / 246 (7 oracle-rejected IR), run PASS 8 / 8
build_smoke.ps1:        PASS (h61 smoke ok)
```

Link smoke:

| fixture | exit | stdout | temp obj |
| --- | --- | --- | --- |
| `01_return_const` | 42 | 0 B | đã xoá |
| `02_int_ops` | 7 | 0 B | đã xoá |
| `07_ops` | 3 | 0 B | đã xoá |
| `101_struct_array_float` | 0 | 4 B | đã xoá |
| `105_print_ref_char` | 0 | 3 B | đã xoá |
| `164_printf_basic` | 0 | 8 B | đã xoá |
| `235_inherit_virtual_dtor` | 0 | 25 B | đã xoá |
| `242_move_struct` | 0 | 5 B | đã xoá |

Negative link failure (stderr, cả hai bên giống nhau từng byte):

```text
ERROR: Invalid pattern is specified in "path:pattern".
selfhost/corpus/codegen/01_return_const.ivy:0:0: error: linker failed (exit code 2): "C:\Windows\System32\where.exe" "<dir>\neg__link_fail__tmp.obj" -o "<dir>\neg__link_fail.exe" "D:/project/Ivy/ivyc/build/Release/ivyrt.lib"
```

Emission failure (`103_print_ref.ivy`): cả hai bên in
`<src>:0:0: error: LLVM IR parse error: ivy: ivy-module:9:19: ...` và không tạo
object.

## 10. Thay đổi `build_smoke.ps1` (H6.1)

`codegen.obj` giờ tham chiếu `__ivy_strlen`, `__ivy_strdup`, `__ivy_memcpy`,
`getenv`, `_popen`, `_pclose`, `fgets`, `system`, `remove`. Harness H6.1 trước
đây chỉ link `smoke.obj + codegen.obj + LLVM libs`, nên thêm bước compile
`lib/ivyrt.c` → `ivyrt.obj` và nối `ivyrt.obj` vào link set. Smoke vẫn PASS.

## 11. Hạn chế

1. **`system()` thay `CreateProcessA`.** Khác biệt chỉ ở các nhánh không có
   trong corpus:
   - linker không tồn tại: oracle in
     `ivy: CreateProcessA failed (error N)` và `rc = 1`; `cmd.exe` in
     `'...' is not recognized ...` và trả mã khác;
   - path chứa ký tự `cmd.exe` đặc biệt (`&`, `^`, `|`, `%`): `system()` diễn
     giải, `CreateProcessA` thì không;
   - stderr/stdout của linker vẫn đi trực tiếp ra stderr/stdout của tiến trình
     như oracle.
2. **Không nối object `import cpp` / `import c` / module D1.** Oracle thêm các
   object đó vào cùng command line; selfhost chưa có frontend tương ứng. Mọi
   fixture H6 đều đơn module, không import header.
3. **Không có `--crt-static`.** Oracle nối ` -Xclang --dependent-lib=libcmt`
   khi flag bật; port chưa có flag này (H7 sẽ thêm nếu cần).
4. **Windows-only.** `_popen`/`_pclose` và cách quote của `cmd.exe` là hành vi
   MSVC; oracle cũng chỉ chạy nhánh này trên `_WIN32`.
5. **Byte-parity của chẩn đoán phụ thuộc đường dẫn truyền vào.** Oracle in
   đúng chuỗi argv (source, exe) và `IVYRT_LIB_PATH` đã bake; harness truyền
   cùng chuỗi cho cả hai bên.
6. **Leak có chủ đích.** `cmd_own_` không được free giữa các lần gọi; driver
   gọi `link_text` một lần rồi thoát. `err_` trỏ vào message của LLVM (H6.3).

## 12. Trạng thái

Files thêm/sửa cho H6.4:

- `selfhost/codegen/codegen.ivy` (extern + `StrBuf` + `Linker`);
- `selfhost/codegen/linkdrive.ivy` (mới);
- `selfhost/codegen/build_link_smoke.ps1` (mới);
- `selfhost/codegen/build_smoke.ps1` (link thêm `ivyrt.obj`);
- `selfhost/codegen/H64_NOTES.md` (file này);
- `PLAN.md` dòng H6.4.

Không sửa `src/**`, không rebuild oracle (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h64\`.
