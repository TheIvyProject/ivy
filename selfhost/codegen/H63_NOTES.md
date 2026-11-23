# H6.3 - Object emission (`ObjEmit`)

## 1. Phạm vi

H6.3 port `CodeGen::emitObject` (`src/codegen/codegen.cpp:2729-2804`) sang
selfhost bằng LLVM **C** API, dựa trên binding H6.1:

1. nhận IR text (output của emitter H6.2);
2. parse IR thành `LLVMModuleRef` (`LLVMParseIRInContext`);
3. cấu hình host target y hệt oracle: triple mặc định của máy, CPU/features
   rỗng, relocation model default, code model default,
   `CodeGenOptLevel::Default` (= 2);
4. set data layout từ `TargetMachine` (`LLVMCreateTargetDataLayout`);
5. ghi COFF object (`LLVMTargetMachineEmitToFile`, `LLVMObjectFile` = 1).

Deliverables:

- `selfhost/codegen/codegen.ivy`: 2 extern mới + 13 facade method + struct
  `ObjEmit`;
- `selfhost/codegen/objdrive.ivy`: driver stdin (IR path, object path,
  source path tuỳ chọn);
- `selfhost/codegen/build_object_smoke.ps1`: harness đối chiếu object +
  link/run smoke;
- `selfhost/codegen/build_smoke.ps1`: link set đổi sang `--libs x86`;
- `selfhost/codegen/H63_NOTES.md` (file này);
- `PLAN.md` dòng H6.3.

Ngoài phạm vi: linker orchestration (H6.4), cross-compile (chỉ host triple),
debug info / optimization pipeline (oracle cũng không bật).

Không sửa `src/**`, không rebuild oracle (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`).

## 2. Binding additions (`codegen.ivy`)

`import c <llvm-c/IRReader.h>;` (declaration-only như các header khác) và 2
extern:

```ivy
extern "C" void* LLVMCreateMemoryBufferWithMemoryRangeCopy(
    const char* input_data, unsigned long long input_data_length,
    const char* buffer_name);
extern "C" int LLVMParseIRInContext(void* context_ref, void* mem_buf,
                                    void** out_m, char** out_message);
```

`LlvmC` thêm 13 pass-through method (không policy):

| method | C API |
| --- | --- |
| `memory_buffer_copy` | `LLVMCreateMemoryBufferWithMemoryRangeCopy` |
| `parse_ir_in_context` | `LLVMParseIRInContext` |
| `target_initialize` | 5 entry point `LLVMInitializeX86*` |
| `default_target_triple` | `LLVMGetDefaultTargetTriple` |
| `target_from_triple` | `LLVMGetTargetFromTriple` |
| `create_target_machine` | `LLVMCreateTargetMachine` |
| `dispose_target_machine` | `LLVMDisposeTargetMachine` |
| `set_target` | `LLVMSetTarget` |
| `set_data_layout` | `LLVMSetDataLayout` |
| `create_target_data_layout` | `LLVMCreateTargetDataLayout` |
| `set_module_data_layout` | `LLVMSetModuleDataLayout` |
| `dispose_target_data` | `LLVMDisposeTargetData` |
| `target_machine_emit_to_file` | `LLVMTargetMachineEmitToFile` |

## 3. `ObjEmit`

```ivy
export struct ObjEmit {
    long tag;
    const char* err_;
    long code_;

    int emit_text(const char* ir, unsigned long long len, const char* out_path);
    const char* err();
    long code();
};
```

`emit_text` trả code: 0 thành công, 1 context, 2 memory buffer, 3 parse IR,
4 target lookup, 5 TargetMachine, 6 emit-to-file. `err()` trả message chi tiết
do LLVM cấp (hoặc fallback ngắn); `code()` cho driver chọn tiền tố chẩn đoán.

Thứ tự mirror oracle:

1. `target_initialize()` (oracle dùng `static bool` một lần/process; các
   entry point X86 idempotent nên gọi mỗi lần là an toàn);
2. `LLVMContextCreate` → memory buffer `"ivy-module"` → `LLVMParseIRInContext`;
3. `LLVMGetDefaultTargetTriple` → `LLVMSetTarget` → `LLVMSetDataLayout(m, "")`;
4. `LLVMGetTargetFromTriple` → `LLVMCreateTargetMachine(..., 2, 0, 0)`;
5. `LLVMCreateTargetDataLayout` → `LLVMSetModuleDataLayout` → dispose target
   data;
6. `LLVMTargetMachineEmitToFile(..., 1)` → dispose TM/module/context.

Chi tiết C API:

- `LLVMParseIRInContext` **nhận quyền sở hữu** memory buffer (C API bọc
  `unwrap(MemBuf)` vào `unique_ptr`), nên buffer không được dispose — tránh
  double free.
- Message lỗi parse/target/emit được giữ nguyên con trỏ trong `err_` (không
  dispose) để driver in sau khi `emit_text` trả về. Đây là leak có chủ đích
  trong process một-lần-chạy.
- Triple lấy từ `LLVMGetDefaultTargetTriple` được dispose bằng
  `LLVMDisposeMessage` ngay sau khi tạo TargetMachine (C API copy chuỗi).

## 4. Đối chiếu oracle (P0)

Scratch: `C:\Users\ASUS\AppData\Local\Temp\opencode\h63\`. Không ghi file nào
vào repo.

Thiết lập của oracle và port trùng nhau:

| tham số | oracle | C API |
| --- | --- | --- |
| triple | `sys::getDefaultTargetTriple()` | `LLVMGetDefaultTargetTriple` |
| CPU / features | `""` / `""` | `""` / `""` |
| reloc model | `std::nullopt` (default) | `LLVMRelocDefault` = 0 |
| code model | `std::nullopt` (default) | `LLVMCodeModelDefault` = 0 |
| opt level | `CodeGenOptLevel::Default` | `LLVMCodeGenLevelDefault` = 2 |
| module id | `"ivy-module"` | `"ivy-module"` |
| data layout | `tm->createDataLayout()` | `LLVMCreateTargetDataLayout` |
| file type | `CodeGenFileType::ObjectFile` | `LLVMObjectFile` = 1 |

Vì cùng LLVM 21.1.6, cùng IR text và cùng tham số, object do port phát ra
**byte-identical** với `ivy -c`. Probe 6 fixture (01, 02, 105, 164, 235, 242)
cho hash SHA-256 trùng; ví dụ `01_return_const.obj` = 585 B ở cả hai bên.

## 5. Driver (`objdrive.ivy`)

Đọc 2 hoặc 3 dòng từ stdin:

```text
<path IR text>
<path object output>
[source path - chỉ dùng để tạo chẩn đoán giống oracle]
```

- thành công: stdout rỗng, exit 0;
- lỗi emit: exit 1;
- thiếu input/không mở được IR: exit 2.

Chẩn đoán parse-failure khớp byte oracle khi có source path:

```text
<src>:0:0: error: LLVM IR parse error: ivy: <LLVM diagnostic>
```

Hai phần được thêm thủ công để khớp `error({}, "LLVM IR parse error: " + msg)`
của oracle:

- tiền tố `<src>:0:0: error: ` (oracle in vị trí rỗng bằng file đầu vào);
- `ivy: ` trước message, vì oracle dùng `diag.print("ivy", rso)` còn C API in
  `SMDiagnostic` không có prog name.

Không có source path, driver in `ivy: error: ...` (dạng dùng khi chạy tay).

## 6. Harness (`build_object_smoke.ps1`)

1. build 11 module selfhost + `cgdrive` (như `cgdiff.ps1`);
2. build `codegen.ivy` + `objdrive.ivy` (`-c`), compile `lib/ivyrt.c`;
3. link `objdrive.exe` = objdrive + codegen + ivyrt + `llvm-config --libs x86`
   + system libs (bỏ `libxml2s.lib` không cài);
4. mỗi fixture:
   - oracle `ivy --llvm` → `*.oracle.ll`; cgdrive → `*.self.ll`; so byte IR;
   - oracle `ivy -c` → `*.oracle.obj`; objdrive → `*.self.obj`; so byte object;
   - nếu oracle `-c` **thất bại** (IR oracle tự sinh không hợp lệ — bug-compat):
     yêu cầu objdrive cũng thất bại và stderr **byte-identical**;
5. link smoke 8 fixture: `*.self.obj` + `ivyrt.obj` → exe, chạy, so
   stdout/stderr/exit với exe do oracle `ivy -o` tạo.

`-SmokeFiles` nhận danh sách phẩy (mặc định 8 fixture: 01, 02, 07, 101, 105,
164, 235, 242); `-SmokeFiles @()` để chỉ kiểm object.

## 7. Kết quả (2026-11-15)

```text
build_object_smoke.ps1:
  object PASS 246 / 246 (7 oracle-rejected IR, diagnostics match)
  run    PASS   8 / 8
cgdiff.ps1:    PASS 246 / 246
indep_verify:  INDEP PASS 246 / 246
interpdiff:    PASS 23 / 23
build_smoke:   PASS (h61 smoke ok)
```

7 fixture mà oracle `-c` từ chối IR của chính nó (đều do bug-compat
`sext ptr` của `io::print` trên reference):

```text
103_print_ref, 104_print_ref_float, 137_new_struct, 143_new_multiarg,
152_new_struct_i8, 186_ivy_print_explicit, 190_ivy_print_decl_order
```

Port báo cùng lỗi, cùng vị trí (`ivy-module:<line>:<col>`) và stderr khớp
byte. Đây là hành vi đúng *trong giai đoạn bug-compat*: oracle cũng không tạo
được object cho các file này. **H10.5/H10.6 đã sửa tận gốc** (C3 cho
`137/143/152`, C2 cho `103/104`, symbol `ivy_print_*` cho `186/190`): cả 7 nay
`-c` thành công; golden object/golden `-c` được ghi lại với provenance
`expected:H10.5/H10.6`. Xem `H10_NOTES.md` H10.5/H10.6 và `ORACLE_BUGS.md` §2.4.

Link smoke:

| fixture | exit | stdout |
| --- | --- | --- |
| `01_return_const` | 42 | 0 B |
| `02_int_ops` | 7 | 0 B |
| `07_ops` | 3 | 0 B |
| `101_struct_array_float` | 0 | 4 B |
| `105_print_ref_char` | 0 | 3 B |
| `164_printf_basic` | 0 | 8 B |
| `235_inherit_virtual_dtor` | 0 | 25 B |
| `242_move_struct` | 0 | 5 B |

## 8. Thay đổi `build_smoke.ps1` (H6.1)

`codegen.obj` giờ chứa cả `LlvmC.target_initialize` và `ObjEmit`, nên link
set phải có X86 target archives. Dòng link đổi từ

```powershell
llvm-config --libs core target irreader
```

sang

```powershell
llvm-config --libs x86
```

(`--libs x86` đã kéo theo core, target, irreader, codegen, asmprinter, MC, …).
Đây đúng là điều H61_NOTES mục 8.4 đã dự báo cho H6.3. Smoke H6.1 vẫn PASS.

## 9. Hạn chế

1. **Chỉ host target.** Oracle cũng chỉ emit object cho host; port giữ nguyên.
2. **Message lỗi ngoài parse.** Với code 5/6, câu chữ có thể khác oracle
   (`cannot open output file '...'` của oracle vs message của
   `LLVMTargetMachineEmitToFile`). Không xảy ra trong corpus; parse error
   (code 3) mới được đối chiếu byte.
3. **Buffer/message không dispose.** Có chủ đích: buffer đã bị
   `LLVMParseIRInContext` consume; message được giữ cho `err()`. Process
   driver sống ngắn nên không ảnh hưởng.
4. **Byte-parity phụ thuộc LLVM.** Object trùng byte vì oracle và port dùng
   cùng LLVM 21.1.6, cùng tham số TargetMachine. Đổi version/toolchain có thể
   đổi encoding object (vẫn đúng ngữ nghĩa).
5. **Chưa có optimization pipeline.** Oracle `emitObject` cũng không chạy
   pass tối ưu nào ngoài codegen mặc định của TargetMachine.

## 10. Trạng thái

Files thêm/sửa cho H6.3:

- `selfhost/codegen/codegen.ivy` (binding + `ObjEmit`);
- `selfhost/codegen/objdrive.ivy` (mới);
- `selfhost/codegen/build_object_smoke.ps1` (mới);
- `selfhost/codegen/build_smoke.ps1` (`--libs x86`);
- `selfhost/codegen/H63_NOTES.md` (file này);
- `PLAN.md` dòng H6.3.

Không sửa `src/**`, không rebuild oracle (27,031,552 bytes,
mtime `2026-10-21 16:41:44`, SHA-256 `FFB376CF...313E`). Không có
`.obj`, `.ivm`, `.exe` mới trong cây làm việc; mọi scratch nằm dưới
`C:\Users\ASUS\AppData\Local\Temp\opencode\h63\` (và `h62verify`, `h62r` cho
verify độc lập).
