# Oracle C++ — bug/quirk phải giữ khi port sang bản Ivy

> **Mục đích.** Bản self-host (`selfhost/**`) được port để **byte-identical** với
> oracle C++ (`src/**`), kể cả khi oracle làm sai. Tài liệu này liệt kê toàn bộ
> bug/quirk đã biết của oracle mà bản Ivy **cố ý tái hiện** (bug-compat), cùng
> các bug oracle mà bản Ivy **không** tái hiện, các input **cấm** dùng trong
> corpus, và các **gap** chưa port.
>
> **Vì sao giữ bug.** Sửa lẻ một bug-compat khi còn gate so với oracle sẽ làm
> gate đỏ mà không chứng minh được gì. Vì vậy mọi bug dưới đây được giữ nguyên
> cho tới khi **không còn gate nào so với oracle** (sau H9.3 golden + H9.5
> bootstrap), rồi mới sửa ở **H10**.
>
> **Trạng thái:** bản nháp 2026-11-18. Nguồn: các `*_NOTES.md` của từng tầng
> (xem cột *Bằng chứng*), đối chiếu trực tiếp `src/**` và `selfhost/**`.

---

## 0. Quy ước

| Ký hiệu | Nghĩa |
|---|---|
| **A** | Bug-compat — bản Ivy **cố ý** tái hiện bug của oracle. |
| **B** | Bản Ivy **không** tái hiện (làm đúng hoặc báo lỗi sạch). |
| **C** | Input **cấm** dùng trong corpus (làm oracle crash). |
| **D** | **Gap** — oracle có, bản Ivy chưa port (không phải bug). |

- **Oracle location**: đường dẫn trong `src/**`. Sau H9.9 thư mục này bị xoá;
  mọi tham chiếu ở đây trỏ tới tag **`0.5-baseline`** (H0.2).
- **Port location**: file trong `selfhost/**` đang giữ hành vi đó. Số dòng là
  ước lượng tại 2026-11-18, dùng tên hàm làm mốc chính.
- **Fix H10**: mục sẽ sửa sau self-host. Mọi fix **phải** cập nhật
  `selfhost/golden/` (H9.3) cùng lúc, nếu không gate golden sẽ đỏ.
- Bug ở `lib/ivyrt.c` (runtime C) cũng thuộc phạm vi: file này chỉ bị port sang
  Ivy ở H9.1, nên bug của nó được giữ tới lúc đó.

---

## 1. Bảng tổng hợp theo tầng

### 1.1 Parser / Lexer

| ID | Loại | Triệu chứng | Oracle location | Port location | Bằng chứng |
|---|---|---|---|---|---|
| **P1** | A | `>>` là **một** token, nên template-id lồng nhau (`A<B<C>>`) không parse được | `src/parsing/lexer.cpp:115` | `selfhost/parsing/lexer.ivy` (token `>>`) | `TEMPLATE_NOTES.md` §10.3 |
| **P2** | A | Template argument **phải là type**; value argument và `f<>()` là parse error | `src/parsing/parser.cpp` (`parseTemplateArgs`) | `selfhost/parsing/parser.ivy` | `TEMPLATE_NOTES.md` §10.3 |
| **P3** | A | Member function template không được coi là member của struct → parse error | `src/parsing/parser.cpp` (`parseStruct`) | `selfhost/parsing/parser.ivy` | `TEMPLATE_NOTES.md` §10.3 |
| **P4** | A | `template <> ...` (explicit specialization) không được parse | `src/parsing/parser.cpp` | `selfhost/parsing/parser.ivy` | `TEMPLATE_NOTES.md` §10.3 |
| **P5** | A | Type C-style (`char`, `double`, `long long`, …) chỉ dùng được dưới `#pragma ivy cnumber`; hậu tố `_t` (`int32_t`) là dạng deprecated nhưng parser vẫn normalize (không phải bug, nhưng corpus phải giữ đúng quy ước) | `src/parsing/parser.cpp`, `src/parsing/preprocessor.cpp` | `selfhost/parsing/parser.ivy`, `selfhost/parsing/preprocessor.ivy` | `PLAN.md` H5.1 (L720-722), `DEPRECATION_TRACKING.md` entry 2 |
| **P7** | A | Chain type C-style (`int long`) là **mutually exclusive** (`else if`), không cộng dồn keyword; chỉ vào chain khi base còn rỗng | `src/parsing/parser.cpp` | `selfhost/parsing/parser.ivy` | `H82_NOTES.md` §2 (`pp/define_obj`) |

### 1.2 HIR — template / semantic

B1–B13 là bug template của oracle, port **tái hiện nguyên văn**. Chi tiết đầy
đủ (vị trí + fixture) ở `selfhost/hir/TEMPLATE_NOTES.md` §10.2.

> Đánh số **T1–T13** (không dùng `H` để tránh trùng với milestone **H10** —
> mục sửa bug sau self-host).

| ID | Oracle bug | Oracle location | Port location | Bằng chứng |
|---|---|---|---|---|
| **T1** (B1) | Khai báo local kiểu `T` trong function template **không** được thay thế | `hir_builder.cpp:503-505` (`currentTypeMapping_` rỗng cho function template) | `selfhost/hir/hir_builder.ivy` | C10, p28 |
| **T2** (B2) | Template gọi template khác với argument tường minh **không** thay thế argument | cùng gốc T1 | `hir_builder.ivy` | C11, p27 |
| **T3** (B3) | `sizeof`/`alignof` của template-id trả **4** (fallback) thay vì layout thật | `hir_builder.cpp:480` (`typeSize` fallback) | `hir_builder.ivy` | p48, p36 |
| **T4** (B4) | Struct-template specialization **mất base class** | `hir_builder.cpp:5442-5448` | `hir_builder.ivy` | p49 |
| **T5** (B5) | `const`/pointer qualifier của type cụ thể bị mất khi thay vào tham số `T` | `substituteType` | `hir_builder.ivy` | p15 |
| **T6** (B6) | `T` suy ra từ integer literal là `int`, không phải `int32_t` | `intType()` = `"int"` | `hir_builder.ivy` | C03 |
| **T7** (B7) | Template argument thừa được chấp nhận im lặng; tên mangled chứa chúng | `instantiateTemplate` map `min(#params,#args)` | `hir_builder.ivy` | p11, p80 |
| **T8** (B8) | Hai template cùng tên: cái sau **thay** cái trước (không overload) | `templates_` key theo tên | `hir_builder.ivy` | p53 |
| **T9** (B9) | `Box<T>` là **kiểu tham số** không bao giờ resolve → mọi call mismatch | `instantiateTemplate` không gọi `resolveTemplateStructType` trên param | `hir_builder.ivy` | p60 |
| **T10** (B10) | Pack expansion trong call (`f(args...)`) hỏng | pack name không phải scope name | `hir_builder.ivy` | p85 |
| **T11** (B11) | `currentTypeMapping_` được set khi instantiate **struct** nhưng không khi **function** | `hir_builder.cpp:5460` vs `:4995` | `hir_builder.ivy` | C11 vs p76 |
| **T12** (B12) | Concept name không tồn tại được coi là **thỏa** im lặng | `hir_builder.cpp:4838-4841` | `hir_builder.ivy` | p21 |
| **T13** (B13) | Non-type template parameter **không** bao giờ được bind | `instantiateTemplate` chỉ map param `isTypename` | `hir_builder.ivy` | p71, t03 |

### 1.3 HIR — lifetime / borrow / dump

| ID | Loại | Triệu chứng | Oracle location | Port location | Bằng chứng |
|---|---|---|---|---|---|
| **H14** | A | Lifetime của **mọi** call result bị ép `Unknown` (dòng gán `Unknown` đè lên `Named`) | `src/mir/mir_builder.cpp:595-603` | `selfhost/mir/mir_builder.ivy` | `LIFETIME_NOTES.md` §8.1 |
| **H15** | A | Type của temp `__ret` là type của **biểu thức**, không phải return type (`fn widen(int32)->int64` → `decl int32_t __ret`) | `hir_builder.cpp` (`buildReturn`) | `hir_builder.ivy` `build_return` ≈L4757 | `LIFETIME_NOTES.md` §8.2, `ret/r04_widen` |
| **H16** | A | `return <local>` chỉ so **base type** (không so đủ type) → widening `int32→int64` sinh `__ret`; NRVO cũng so base nên local **không** được đánh dấu transferred | `hir_builder.cpp:1757-1765` | `hir_builder.ivy` `build_return` ≈L4791 | `LIFETIME_NOTES.md` §8.3 |
| **H17** | A | Destructor của **outer scope** bị emit **hai lần**: `emitDtorCalls()` đi hết các scope tới `dtorBaseline_`, nhưng chỉ `dtorStacks_.back()` bị clear | `hir_builder.cpp:1836-1839` | `hir_builder.ivy`: **H10.2** bỏ clear, `build_compound` không emit dtor cuối block khi câu lệnh cuối là terminator | `LIFETIME_NOTES.md` §8.5, `H10_NOTES.md` H10.2, `h10_H17_nested_return` |
| **H18** | A | `return` **đầu tiên** trong một scope clear danh sách dtor của scope đó → `return` **thứ hai** cùng scope không có dtor **và** không có temp `__ret` | `hir_builder.cpp` (`buildReturn`, `dtorStacks_.back().clear()`) | `hir_builder.ivy` `build_return`: **H10.2** giữ danh sách dtor (lexical), mỗi `return` tự emit + tạo `__ret` | `LIFETIME_NOTES.md` §8.6, `H10_NOTES.md` H10.2, `h10_H18_two_returns` |
| **H19** | A | Borrow-checker có lỗ hổng: ghi qua biến đang borrow, dùng sau destroy, bind ref vào temp **không** bị báo; `checkStore` gần như không tới được ngoài `unsafe`; `unsafe` tắt **mọi** check | `hir_builder.cpp` (borrow/lifetime) | `mir/mir_builder.ivy`: **H10.3** thêm `check_assign_borrow` (gán/`++`/`--`/compound khi đang borrow) và `check_ref_binding` (ref buộc vào temp); gọi từ `ex_assign`, `ex_unary`, `st_decl`; `checkStore` giữ nguyên bypass `unsafe` | `LIFETIME_NOTES.md` §8.7, `H10_NOTES.md` H10.3, `h10_H19_assign_while_borrowed`, `h10_H19_write_forms`, `h10_H19_ref_to_temp`, `h10_H19_use_after_destroy`, `h10_H19_unsafe_escape` |
| **H20** | A | `--ast` dump **mất thông tin**: in `fn.attrs`/`param.attrs` nhưng **không** in `declaredLifetimes`, `returnLifetime`, `param.lifetime` → native `$a` vô hình | `src/app/main.cpp:331-356` (`dumpAst`) | `selfhost/parsing/astdump` | `LIFETIME_NOTES.md` §8.8 |
| **H21** | A | Legacy `[[ivy::lt_ret(x)]]` **thắng** native `$a`, dù comment trong code nói ngược lại. Tái hiện **code**, không tái hiện comment | `hir_builder.cpp:244` | `hir_builder.ivy` | `LIFETIME_NOTES.md` §8.9, §4.2 |
| **H22** | A | Em dash (U+2014) trong output là **mojibake**: viết em dash thật vào `.ivy`, toolchain tái hiện đúng byte của oracle | `src/**` (encode output) | `selfhost/parsing/*` | `LIFETIME_NOTES.md` §8.10, §3.4 |
| **H23** | A | `consteval` **không** có runtime representation → bị bỏ qua ở cả vòng declare lẫn definition | `codegen.cpp:2674`, `:2695` | `selfhost/codegen/emitter.ivy` (`is_consteval`) | `H82_NOTES.md` §2 |
| **H24** | A | `move(x)` chỉ là pass-through khi callee **không** có MIR entry; overload `move` do người dùng định nghĩa vẫn là call thật | `codegen.cpp:1539` | `emitter.ivy` (`lower_call`) | `H82_NOTES.md` §2 |
| **H25** | A | `sizeof(<biểu thức>)` và `switch` trên `enum` bị oracle **từ chối** (không phải parse error mà là lỗi tầng sau) | `src/hir/**` | `selfhost/hir/hir_builder.ivy` | `PLAN.md` H5.1 (L720-721) |
| **H26** | A | `T x = {...}` (aggregate init) **bypass** constructor nhưng **vẫn** record destructor | `hir_builder.cpp` (`buildVarDecl`) | `hir_builder.ivy` `build_decl`: **H10.2** nhận đúng init-list (kind 19) và không `record_dtor_var` khi aggregate đã bypass ctor | `H62_NOTES.md` (RAII, L2516), `H10_NOTES.md` H10.2, `h10_H26_agg_ctor_dtor` |
| **H27** | C | `--hir`/`--mir` **crash** `0xC0000005` (`exit -1073741819`) trên init-list có phần tử `<zero-init>` do HIR builder sinh ra: oracle lưu `nullptr` trong `elements` rồi `dumpHirExpr`/`buildExpr` dereference | `src/app/main.cpp` (`dumpHirExpr` InitList), `src/mir/mir_builder.cpp` (`buildExpr` InitList) | `selfhost/hir/hir.ivy`, `selfhost/mir/mir.ivy` (in `<zero-init>` / bỏ qua, **không** crash) | `selfhost/golden/exceptions.txt` (`156_struct_partial_init`, `157_struct_empty_init`, `160_struct_designated_init`); port phải exit 0 |
| **H28** | A | `move(x)` giữ lifetime `None`: `out->lifetime = arg->lifetime` chạy **sau** `call.args.push_back(std::move(arg))`, nên `if (arg)` false và call không có `[lt: ...]` trong `--mir` | `src/mir/mir_builder.cpp:578-583` | `selfhost/mir/mir_builder.ivy` (`ex_call`): `move` để `lt_kind_ = 0`, không copy lifetime của operand | `codegen/245_move_pointer`, `hir/move/54_lifetime_param_moved` |
| **H29** | A | Ghi qua **phần tử con trỏ** (`p[i] = x` với `p` là `T**`) emit GEP **không scale**: `getelementptr i8, ptr %p, iN k` (dùng `llvmElemType`) trong khi đọc `p[i]` emit `getelementptr ptr` (dùng `llvmType`) → offset `k` thay vì `k*8`. **Sai bộ nhớ ở machine-code**, không lộ ra trong dump IR so sánh được | `codegen.cpp:1046-1063` (`lowerLValue` Index, `llvmElemType` tại `:255-260`); đọc đúng ở `:1742-1765` | **H10.4**: `selfhost/codegen/emitter.ivy` `lower_index_addr` dùng `llvmType(e.type)` cho cả hai đường; workaround `__ivy_memcpy` đã xoá khỏi `selfhost/codegen/codegen.ivy` (`add_module_object`/`add_extra`/`add_cpp_header`/`add_c_header`/`hobj`) và `selfhost/app/pipeline.ivy` | `H9_NOTES.md` §H9.4; `H10_NOTES.md` H10.4; `h10_H29_pointer_store` |

### 1.4 Codegen / IR

| ID | Loại | Triệu chứng | Oracle location | Port location | Bằng chứng |
|---|---|---|---|---|---|
| **C1** | A | Slot biến local khoá theo **tên** (không có scope): hai local cùng tên ở scope khác nhau dùng chung một `alloca`, **last-wins** | `codegen.cpp:2503-2526` | **H10.4**: `selfhost/mir/mir_builder.ivy` gán **binding id** cho từng khai báo (tham số id âm) và ghi vào `alloca.b`/`expr.b`; `selfhost/codegen/emitter.ivy` tra slot theo id (fallback theo lịch sử khai báo cho dtor do HIR tạo sau khi scope đã đóng) | `MIR_NOTES.md` §11.6; `H10_NOTES.md` H10.4; `h10_C1_slot_shadow` |
| **C2** | A | `sext ptr` / `fpext ptr` cho **reference argument** (`io::print(x)` với `x` là `T&`) — IR không hợp lệ nhưng được emit | `codegen.cpp` (`lowerCall` ref args) | `emitter.ivy` ≈L57 (`io::print` ref) | `H62_NOTES.md` |
| **C3** | A | `new T(args)` chỉ **store `args[0]`** (bỏ các arg còn lại) rồi coi là `T` | `codegen.cpp` (`lowerExpr` New) | `emitter.ivy` `lower_new` ≈L4141, store ≈L4160 | `H62_NOTES.md`; fixture `143_new_multiarg`, `152_new_struct_i8` |
| **C4** | A | `break`/`continue`/`nextcase` viết trong `if` lồng **không** chạy dtor của loop body — chỉ dtor của scope **trong cùng** | `hir_builder.cpp` (`dtorStacks_.back()`) | `hir_builder.ivy` `wrap_scope_dtors`: **H10.2** theo dõi `jscope_`/`nscope_` và destroy mọi scope tới đích của jump | `H62_NOTES.md`, `H10_NOTES.md` H10.2, `h10_C4_break_nested` |
| **C5** | A | **Global trùng lặp**: `io::print("hi")` sinh **hai** `@.str.N` (`@.str.0` từ pre-pass, `@.str.1` từ segment) vì hai map key khác nhau (`"1:"+bytes` vs raw bytes) | `codegen.cpp` (`stringList_`, `collectExpr` vs `internCString`) | `emitter.ivy` ≈L4015 (`pm_*`/`sm_*`), ≈L1093 | `H62_NOTES.md` §20 |
| **C6** | A | Quirk format IR: empty struct in `{  }` (hai dấu cách), có dòng trống, struct **không dùng** vẫn được in | `codegen.cpp` (struct/global emission) | `emitter.ivy` | `H62_NOTES.md` §28.5 |
| **C7** | A | 7 fixture mà `ivy -c` **từ chối IR do chính nó sinh ra**; port phải fail với stderr **byte-identical** | `codegen.cpp` (LLVM parse/verify) | `selfhost/codegen/objdrive.ivy` | `H63_NOTES.md` §7: `103_print_ref`, `104_print_ref_float`, `137_new_struct`, `143_new_multiarg`, `152_new_struct_i8`, `186_ivy_print_explicit`, `190_ivy_print_decl_order` |
| **C8** | A | `nullptr` literal (không có `_t`) rơi vào nhánh base lạ → **`i8`**, không phải `ptr` (`auto a = nullptr` cấp `i8`) | `codegen.cpp:227`, `:239` | `emitter.ivy` ≈L1280 (`llvm_type`) | `H82_NOTES.md` §2 |
| **C9** | A | `is_fp_ir` xét theo **spelling base**, không theo type thật | `codegen.cpp` (`isFpIr`) | `emitter.ivy` | `H82_NOTES.md` §2 |
| **C10** | A | `msvcMangleType` **không có nhánh array** → array mangle như element type (`int32_t[4]` → `H`) | `codegen.cpp` (`msvcMangleType`) | `emitter.ivy` ≈L1939 (`emit_mangle_type`) | `H82_NOTES.md` §2 |
| **C11** | A | Symbol chưa resolve dùng mangling fallback `?<name>@@YHX@Z` | `codegen.cpp` (`msvcMangleFunction`) | `emitter.ivy` | `H82_NOTES.md` §2 |
| **C12** | A | `emit()` **tiếp tục** sau lỗi (không dừng module) | `codegen.cpp` (`emit`) | `emitter.ivy` | `H82_NOTES.md` §2 |
| **C13** | A | Có trường hợp emit `br label %` với **label rỗng** | `codegen.cpp` | `emitter.ivy` | `H82_NOTES.md` §2 |
| **C14** | A | `-0.0` **giữ dấu** (`std::to_string(-0.0)` = `"-0.000000"`; `-0.0 < 0.0` là false nên phải tự dò dấu) | `codegen.cpp` (float literal) | `emitter.ivy` ≈L1701 | `H82_NOTES.md` §2 |
| **C15** | A | Biểu thức **không assignable** rơi xuống `lowerLValue` → in `expression is not assignable [nodeIdx=… type.base=… isRef=…]` rồi lower thành literal `null` (D1-debug fallthrough) | `codegen.cpp:1106-1116` | `emitter.ivy` `not_assignable_msg` ≈L584 | `H62_NOTES.md` |
| **C16** | A | `collectExpr` **không** recurse vào argument của `new` và operand của `delete` → string literal/`io::print` bên trong không được pre-collect, global intern muộn, io runtime không emit | `codegen.cpp` (`collectExpr` New/Delete) | `emitter.ivy` ≈L3738 | `H62_NOTES.md` |
| **C17** | A | `@__ivy_alloc`/`@__ivy_free` được suppress trùng: chỉ emit một lần dù nhiều `new`/`delete` | `codegen.cpp` (declare emission) | `emitter.ivy` ≈L187 | `H62_NOTES.md` |
| **C18** | A | Struct local chưa khởi tạo được **zero-init** tại điểm khai báo (không phải ở entry) | `codegen.cpp` (`alloca` hoist) | `emitter.ivy` ≈L4556; **H10.4** khoá bằng fixture IR `h10_C18_zero_init_decl` | `H62_NOTES.md`; `H10_NOTES.md` H10.4 |
| **C19** | B | Record local **còn sống** khi gọi hàm trả record theo giá trị bị codegen làm hỏng khung stack (`setDataLayout("")`) | `codegen.cpp:2763` | `selfhost/codegen/codegen.ivy`: module layout lấy từ TargetMachine **trước** `emit_to_file` (không tái hiện); fixture `h10_C19_record_across_call` (H10.1) | `MIR_NOTES.md` §11.6, `H10_NOTES.md` H10.1 |
| **C20** | A | Octal escape trong string literal: `"\101"` → byte `0x01`, `"\1"` → `0x00` (bộ đọc bắt đầu từ chữ số mà `switch` đã tiêu thụ) | `codegen.cpp` (`readOctalEscape`) | `emitter.ivy` ≈L633/L657 (`emit_escaped`) | `H62_NOTES.md` §19 |

### 1.5 Runtime (`lib/ivyrt.c` — port sang Ivy ở H9.1)

| ID | Loại | Triệu chứng | Oracle location | Port location | Bằng chứng |
|---|---|---|---|---|---|
| **R1** | A | `__ivy_fmt_float` dùng `_gcvt(v, 15)` nên `2.0` in thành `"2."` (mất chữ số thập phân) | `lib/ivyrt.c:71-82` | `std/rt.ivy` `__ivy_fmt_float` (H9.1) | `MIR_NOTES.md` §11.8 |
| **R2** | A → **B** | `__ivy_fmt_int`/`__ivy_fmt_float` trả con trỏ tới **static buffer dùng chung**, bị ghi đè ở lần gọi kế tiếp | `lib/ivyrt.c:61-63`, `:65-69` | `std/rt.ivy` (H9.1) cấp **buffer mới** mỗi lần gọi → **không** tái hiện (subset không có static); xem §3 `B-R2` | `lib/ivyrt.c` comment |
| **R3** | ghi chú | `_gcvt` được chọn vì `snprintf("%g", dbl)` hỏng ABI varargs double dưới LLJIT trên Windows x64. Khi port sang Ivy phải giữ **cùng output byte** | `lib/ivyrt.c:74-77` | `std/rt.ivy` `__ivy_fmt_float` (H9.1) giữ `_gcvt` | `lib/ivyrt.c` comment |

### 1.6 Interpreter

| ID | Loại | Triệu chứng | Oracle location | Port location | Bằng chứng |
|---|---|---|---|---|---|
| **I1** | A | CFG **self-edge**: `execInst` reset `curInst = 0`, rồi `execBlock` `++` → lần chạy lại bắt đầu ở **instruction thứ hai** của block, không phải đầu | `interpreter.cpp:162-171` | `selfhost/mir/interpreter.ivy` `set_frame_edge` ≈L1633 (`inst_index = 1`) | `MIR_NOTES.md` |

### 1.7 Module I/O, driver, linker

| ID | Loại | Triệu chứng | Oracle location | Port location | Bằng chứng |
|---|---|---|---|---|---|
| **M1** | A | `.ivm`: return type của **free function** ghi **không escape**, trong khi `type()` escape cả tên gốc → `long long` thành `long\slong` | `module_io.cpp:132` | `selfhost/parsing/module_io.ivy` `type_raw()` ≈L235-247 | `H81_NOTES.md` §6 |
| **M2** | A | `serializeType` escape **cả chuỗi type** (kể cả tên gốc), nên type C-style có dấu cách bị escape (`long\slong`) | `module_io.cpp` (`escape(serializeType(...))`) | `module_io.ivy` `type()` (`type_x(t, 1)`) | `H81_NOTES.md` §6 |
| **M3** | D | `Linker::link_text` **không** passthrough `-l`/`-L`/`-Wl,` → không thể tự link chính compiler | `codegen.cpp:2830-2860` | `selfhost/codegen/codegen.ivy` `Linker` (`set_self_link`/`collect_self_link`, H9.4) | `PLAN.md` H9.4 — **đã sửa** bằng cờ `--self-link`, không phải bug-compat |

---

## 2. Ghi chú chi tiết cho các mục phức tạp

### 2.1 H17/H18 — destructor của oracle

Oracle giữ **hai** cấu trúc song song:

- `dtorStacks_` — stack các scope, mỗi scope là danh sách local cần huỷ;
- `dtorBaseline_` — độ sâu scope của **thân hàm**.

`emitDtorCalls()` đi từ scope trong cùng ra tới `dtorBaseline_`, emit `call
'<T>::~T'` cho từng local, rồi **chỉ clear `dtorStacks_.back()`**. Hệ quả:

1. **H17**: dtor của outer scope nằm trong compound của `return` **và** lại được
   emit lần nữa khi thoát hàm. Trong dump HIR, `call 'Res::~Res'` xuất hiện
   **hai lần** (`ret/r06_nested_scope`).
2. **H18**: `buildReturn()` clear danh sách dtor của **scope trong cùng**. Một
   `return` **thứ hai** trong cùng scope thấy danh sách rỗng → không dtor, không
   `__ret` (`ret/r07_same_scope_two_returns`, `ret/r21_temp_then_early_return`).

Interpreter chỉ chạy dtor **một lần** (return short-circuit), nhưng HIR chứa
call trùng và native codegen có thể double-free. **Tái hiện code, không tái
hiện ý định.** Port: `hir_builder.ivy` `emit_dtor_calls`/`build_return`/
`wrap_scope_dtors`.

### 2.2 C1/C19 — slot biến local và record sống qua call

Hai bug codegen **liên quan** nhưng **khác nhau**:

- **C1 (slot theo tên, last-wins)** — `codegen.cpp:2503-2526` phát một `alloca`
  cho mỗi local với tên `%<name>.<n>` và lưu vào `vars_[<name>]`. Hai local
  cùng tên ở scope khác nhau **dùng chung** slot, cái sau thắng:

  ```ivy
  MInst x;  x.kind = 4;
  { long x = 5; x = x + 1; }
  io::println("kind={}", x.kind);   // oracle in 6, không phải 4
  ```

  Port tái hiện (bảng biến theo tên).

- **C19 (record local sống qua call)** — một record local **còn sống** khi gọi
  hàm trả record theo giá trị bị codegen làm hỏng khung stack, do
  `setDataLayout("")` (`codegen.cpp:2763`). Đây là bug **machine-code**, không
  lộ ra ở IR dump. Port **không** cần tái hiện vì corpus H5.1 không có mẫu;
  nhưng **không được** viết code self-host theo pattern này. Xem
  `MIR_NOTES.md` §11.6.

### 2.3 C5 — vì sao `io::print("hi")` có hai global

Oracle intern string qua **hai** đường vào **cùng** `stringList_`:

- pre-pass `collectExpr`: key `"1:" + bytes` (có tiền tố width);
- `internCString` cho segment format: key là raw `bytes`.

Key khác nhau ⇒ hai global:

```text
@.str.0 = private unnamed_addr constant [3 x i8] c"hi\00"   ; pre-pass, không dùng
@.str.1 = private unnamed_addr constant [3 x i8] c"hi\00"   ; segment, được dùng
```

Port giữ hai map riêng (`pm_*` key `"1:"+bytes`, `sm_*` key raw bytes), dedup
**trong từng map**. In cùng literal hai lần vẫn chỉ có một `@.str.0` pre-pass và
một `@.str.1` segment.

### 2.4 C7 — 7 fixture oracle từ chối IR của chính nó

Đây là **bug-compat quan trọng nhất** của tầng object: oracle sinh IR không hợp
lệ, rồi `ivy -c` từ chối chính IR đó. Port phải **fail giống hệt** (cùng exit
code, cùng stderr byte-identical), không được "sửa" IR cho hợp lệ:

| Fixture | Lý do |
|---|---|
| `103_print_ref` | reference argument → `sext ptr` (C2) |
| `104_print_ref_float` | reference float argument |
| `137_new_struct` | `new` struct + store `args[0]` (C3) |
| `143_new_multiarg` | `new P(1, 2)` — chỉ `args[0]` được store (C3) |
| `152_new_struct_i8` | `new` với base `i8` (C8) |
| `186_ivy_print_explicit` | `ivy::print_*` explicit |
| `190_ivy_print_decl_order` | thứ tự declare io runtime |

Nguồn: `H63_NOTES.md` §7.

### 2.5 M1/M2 — escape trong `.ivm`

`module_io.cpp` ghi `escape(serializeType(p.type))` — escape **cả chuỗi type**,
kể cả tên gốc. Type C-style `long long` có dấu cách nên thành `long\slong`.
Nhưng **return type của free function** lại ghi `serializeType(f.returnType)`
**không** escape (`module_io.cpp:132`). Port:

- `type()` → `type_x(t, 1)` (escape tên gốc);
- `type_raw()` → `type_x(t, 0)` cho return type free function.

Bug này chỉ lộ ra ở `codegen.ivy` (module duy nhất dùng `long long`), nên các
module khác khớp trước đó. Nguồn: `H81_NOTES.md` §6.

### 2.6 H29 — ghi qua phần tử `T**` không scale

`lowerLValue` xử lý `M::Index` bằng `llvmElemType(e.type)` (`codegen.cpp:1060`):
`llvmElemType` **bỏ một `pointerDepth`**, nên với `p[i]` mà `p` là `int8**`
(type phần tử `int8*`) nó trả `i8`, và GEP thành
`getelementptr i8, ptr %p, i64 i` — địa chỉ `p + i` byte thay vì `p + 8*i`.
Đường **đọc** (`codegen.cpp:1747`) dùng `llvmType(e.type)` = `ptr` nên scale
đúng 8. Hai đường lệch nhau ⇒ mọi `p[i] = x` ghi vào **sai ô** (thường là ô 0
với `i < 8`, ghi đè con trỏ đầu tiên).

Repro (`--llvm`, cả oracle lẫn port):

```ivy
#pragma ivy cnumber
extern "C" void* __ivy_alloc(unsigned long long n);

fn main() -> int32 {
    unsafe {
        long n = 3;
        int8** p = (int8**) __ivy_alloc(8 * n);
        p[0] = (int8*) 0;
        p[1] = (int8*) 0;
        int8* x = p[1];
    }
    return 0;
}
```

→ `%tmp.5 = getelementptr i8, ptr %tmp.6, i32 0` / `... i32 1` cho store,
nhưng `%tmp.11 = getelementptr ptr, ptr %tmp.12, i32 1` cho load.

Không nằm trong golden vì golden so **text IR** (bug nằm ở nghĩa của IR, không
phải spelling) và corpus H5.1 không có mẫu `p[i] = x` với `T**`. Nhưng **code
self-host phải tránh**: `Linker` giữ `char** mods_`/`extra_` chính là pattern
này, nên `add_module_object`/`add_extra` (`selfhost/codegen/codegen.ivy`
≈L786-812, ≈L830-855) ghi từng phần tử bằng `__ivy_memcpy(..., 8)` thay vì
`this.mods_[i] = p`.

**H10.4 — đã sửa**: `lower_index_addr` dùng `llvmType(e.type)` cho cả hai
đường, nên store qua `T**` scale đúng như load. Toàn bộ workaround
`__ivy_memcpy` ở `add_module_object`, `add_extra`, `add_cpp_header`,
`add_c_header`, mảng `hobj` (`selfhost/codegen/codegen.ivy`) và `pipeline.ivy`
đã bị xoá; các chỗ đó lại ghi trực tiếp `this.mods_[i] = p`. Fixture
`selfhost/corpus/codegen/h10_H29_pointer_store.ivy` (`pp[1] = p`, in `9`);
golden `expected:H10.4`. Vì đây là fix của chính compiler, seed
`selfhost/bootstrap/ivy-stage0.exe` được dựng lại (H10.4) để generation đầu
tiên đã có hành vi đúng — xem `H10_NOTES.md` H10.4.

---

## 3. Bug oracle mà port **không** tái hiện (loại B)

Các mục này **không** nằm trong gate bug-compat: port làm đúng, hoặc báo lỗi
sạch thay vì crash. Khi golden H9.3 được đóng băng, chúng **không** xuất hiện
trong corpus (xem §4).

| ID | Oracle | Port | Lý do |
|---|---|---|---|
| **B-P5** | `lifetime<$a> template<...> fn` consume `<` **hai lần** → parse error (`parser.cpp:789` + `:1649`) | `parser.ivy` `parse_template_params` chỉ consume `<` một lần → **parse đúng** | Port không tái hiện double-`<`; corpus **không** đặt `lifetime<...>` trên template function (`LIFETIME_NOTES.md` §8.4) |
| **B-C3** | `&<biến local kiểu struct>` làm oracle **crash** `0xC0000005` | `H10.1`: `hir_builder.ivy` `build_unary` không đòi `operator&` cho operand struct; `emitter.ivy` `lower_unary` hạ `&` về slot của local (nếu không thấy slot vẫn `fail` sạch) | Port sinh địa chỉ đúng, không crash; fixture `h10_B-C3_addr_of_struct` (`H10_NOTES.md` H10.1) |
| **B-C19** | Record local sống qua call → machine code hỏng | Port **không** viết code theo pattern này; không tái hiện | Bug machine-code, không lộ ở IR; corpus H5.1 không có mẫu |
| **B-R2** | `__ivy_fmt_int`/`__ivy_fmt_float` trả con trỏ vào **static buffer dùng chung** (`lib/ivyrt.c:61-69`); hai kết quả giữ lại cùng lúc sẽ thấy cùng nội dung | `std/rt.ivy` (H9.1) `malloc` buffer mới mỗi lần gọi → hai kết quả độc lập, nhưng **mỗi lần format là một lần cấp phát không được free** (codegen sinh ra truyền thẳng kết quả cho `_write`) | Ivy subset **không có** biến file-scope/`static` local nên không thể tái hiện buffer dùng chung. Khác biệt chỉ lộ khi caller giữ ≥ 2 kết quả, hoặc khi chương trình format trong vòng lặp dài (rò rỉ ~32/64 B mỗi lần). Không fixture nào trong corpus làm vậy; follow-up: arena/scratch buffer trong runtime hoặc để codegen tự giữ buffer (H10) |

---

## 4. Input **cấm** dùng trong corpus (loại C)

Các input dưới đây làm **oracle crash** (exit `-1073741819` = `0xC0000005`) →
output rỗng + exit code khác 1, phá differ. **Tuyệt đối không** đưa vào
`selfhost/corpus/**`, kể cả fixture "bug".

| Input | Crash | Ghi chú |
|---|---|---|
| `a + b` với `a`,`b` là struct **không** có `operator+` | Có | cả template lẫn plain đều crash (p64, p68) |
| `obj(args)` với `obj` là struct **không** có `operator()` | Có | p65, p69 |
| `examples/test_b5_expected.ivy` (`expected<T,E>` init-list) | Có | `ivy --hir` crash |
| `&<biến local kiểu struct>` | Có | xem B-C3 |

Nested `constexpr` call trong binary expression **không** crash trong probe
(p66, p84) nhưng **coi là không an toàn**. Nguồn:
`TEMPLATE_NOTES.md` §10.1, `MIR_NOTES.md` §11.7.

> **Ngoại lệ H9.3.** `selfhost/corpus/codegen/{156_struct_partial_init,157_struct_empty_init,160_struct_designated_init}.ivy`
> vẫn nằm trong corpus vì **chỉ** `--hir`/`--mir` crash; `--ast`, `--lex`,
> `--tokens`, `--llvm`, `-c`, `-o` và link đều bình thường. Ba fixture này
> được ghi ở `selfhost/golden/exceptions.txt` (H27): gate bỏ so byte nhưng
> **bắt buộc** port exit 0.

> Ghi chú: §8.11 của `LIFETIME_NOTES.md` kết luận **không** tìm được input
> lifetime nào làm crash oracle; mọi input malformed đều cho diagnostic sạch.
> Danh sách trên là các crash **đã biết** ở tầng template/codegen.

---

## 5. Gap chưa port (loại D — không phải bug)

Đây là các tính năng oracle **có** mà bản Ivy **chưa** port; chúng làm dump/MIR
khác oracle nhưng **không** phải bug-compat. Theo dõi ở `MIR_NOTES.md` §6.1
(G1–G15) và các mục H4/H5/H6 của `PLAN.md`.

| ID | Gap | Ảnh hưởng |
|---|---|---|
| **G1** | Virtual dispatch ở tầng HIR (`isVirtual`, `vtableSlot`, `methodName`) | `--hir`/`--mir` in `call` thay vì `virtual_call 'speak' slot=0` |
| **G2** | `new`/`delete` ở HIR | parser port từ chối `new`/`delete` |
| **G3** | `S s(1)` (struct construction) | parser port chưa sinh call ctor |
| **G4** | Dtor injection + `__ret` + NRVO ở HIR | xem H17/H18 (phần đã port) và `MIR_NOTES.md` §6.1 |
| **G5** | Init list `{...}` | parser port từ chối |
| **G6** | `namespace` block | parser port chưa hỗ trợ |
| **G7–G15** | `Stmt.loc`, `Member.isScope`, `Call.target`, function flags, lambda, pack/fold/sizeof-pack, `SizeOf`/`AlignOf`, `NulloptLit`, `Param.defaultValue` | xem bảng đầy đủ `MIR_NOTES.md` §6.1 |

Các gap này thuộc H4/H5/H6, **không** thuộc H10. Chỉ khi port xong mới đưa
chúng vào phạm vi gate.

---

## 6. Quy trình sửa sau self-host (H10)

Điều kiện bắt đầu H10: **không còn gate nào so với oracle C++** —

- H9.3 xong: `selfhost/golden/` đã đóng băng (1052 fixture × 7 mode + 249
  link/run + `manifest.txt`);
- H9.5 xong: bootstrap chỉ dùng binary Ivy (`selfhost/build.ps1`, stage0 →
  stage1 → stage2);
- H9.9 xong: `src/**` đã bị xoá (chỉ còn ở tag `0.5-baseline`).

Quy tắc:

1. **Mỗi bug một mục H10.x** (đánh số tuần tự), tham chiếu ID trong tài liệu
   này (ví dụ `H10.3 = H17 + H18`).
2. **Sửa code + cập nhật golden cùng một commit.** Golden là output **đã đóng
   băng của oracle**, nên fix bug-compat **sẽ** làm golden đỏ; phải ghi lại
   golden mới kèm lý do trong commit message (`H10.x: fix <ID> — <mô tả>`).
3. **Giữ test hồi quy**: mỗi bug có fixture riêng (dùng lại fixture đã ghi ở
   cột *Bằng chứng*) và một fixture "đã sửa" để chứng minh hành vi mới.
4. **Không sửa lẻ** khi còn gate so với oracle (xem đầu tài liệu).
5. **Thứ tự ưu tiên gợi ý** (sửa cái gây hại trước):
   1. Crash/UB: B-C3 (`&struct`), C19 (record qua call), H17 (double dtor).
   2. Sai kết quả chương trình: R1 (`fmt_float`), C3 (`new args[0]`), C1 (slot
      theo tên).
   3. Sai chẩn đoán/IR: C15 (`not assignable`), C5 (global trùng), C8
      (`nullptr`→`i8`).
   4. Chỉ ảnh hưởng dump/quy ước: H14–H22, I1, M1/M2; còn lại H23–H26 xếp
      theo mức ảnh hưởng thực tế.

---

## 7. Bảng tra cứu nhanh theo file port

| File port | ID liên quan |
|---|---|
| `selfhost/parsing/lexer.ivy` | P1, H22 |
| `selfhost/parsing/parser.ivy` | P1–P5, P7, B-P5 |
| `selfhost/parsing/preprocessor.ivy` | P5 |
| `selfhost/parsing/module_io.ivy` | M1, M2 |
| `selfhost/hir/hir_builder.ivy` | T1–T13, H15–H18, H21, H23, H25, H26, C4 |
| `selfhost/mir/mir_builder.ivy` | H14, H19 |
| `selfhost/mir/interpreter.ivy` | I1 |
| `selfhost/codegen/emitter.ivy` | C1–C20, H23, H24, H29 |
| `selfhost/codegen/objdrive.ivy` | C7 |
| `selfhost/codegen/codegen.ivy` | M3, H29 |
| `lib/ivyrt.c` (→ `std/rt.ivy` ở H9.1) | R1–R3 |

---

## 8. Liên kết

- [PLAN.md](../PLAN.md) — H8/H9 (loại bỏ bản C++), H10 (fix sau self-host)
- [selfhost/hir/TEMPLATE_NOTES.md](hir/TEMPLATE_NOTES.md) §10.1–10.3 — bug template + input cấm
- [selfhost/hir/LIFETIME_NOTES.md](hir/LIFETIME_NOTES.md) §8.1–8.11 — lifetime/dtor/borrow/dump
- [selfhost/mir/MIR_NOTES.md](mir/MIR_NOTES.md) §6.1, §11.6–11.8 — gap + bug codegen
- [selfhost/codegen/H62_NOTES.md](codegen/H62_NOTES.md) §19–20, §28.5 — escape, global trùng, format IR
- [selfhost/codegen/H63_NOTES.md](codegen/H63_NOTES.md) §7 — 7 fixture oracle từ chối IR
- [selfhost/app/H81_NOTES.md](app/H81_NOTES.md) §6 — `.ivm` escape
- [selfhost/app/H82_NOTES.md](app/H82_NOTES.md) §2 — 6 nhóm mismatch stage 2
- [DEPRECATION_TRACKING.md](../DEPRECATION_TRACKING.md) entry 8 — compiler C++ → Ivy
