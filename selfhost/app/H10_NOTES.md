# H10 - Bug-compat & sửa sau self-host

> Kế hoạch đầy đủ: [PLAN.md](../../PLAN.md) mục **H10**; danh sách bug và
> bằng chứng: [ORACLE_BUGS.md](../ORACLE_BUGS.md).

## 0. Quy ước

- Mỗi mục `H10.x` = một nhóm bug cùng cơ chế. Code + `selfhost/golden/` +
  fixture hồi quy nằm trong **cùng commit**.
- Golden trước H10.0 do oracle sinh (`oracle`); từ H10.0 trở đi do
  `selfhost/update_golden.ps1` sinh và ghi `expected:H10.x`.
- Sau mỗi fix: F2 (stage1 == stage2) + self-link phải xanh; nếu mã self-host
  đang dựa vào quirk vừa sửa thì sửa mã self-host trong cùng mục.
- Gate mỗi mục: `build.ps1 -SelfLink -Golden` (golden đã cập nhật) +
  `check_repro.ps1` ở các mốc, CI dogfood xanh.
- Mỗi mục ghi vào file này theo mẫu dưới đây.

## 1. Mẫu ghi chú

```text
### H10.x - <tên ngắn>

- **Bug**: <ID> - <triệu chứng>
- **Sửa**: <file/hàm> - <cách sửa>
- **Fixture**: selfhost/corpus/<layer>/h10_<ID>_<tên>.ivy (+ file phụ)
- **Golden**: <record thêm/đổi/xoá, provenance expected:H10.x>
- **Gate**: F2 ..., self-link SHA ..., golden <n>/0, check_repro ...
- **Ghi chú**: <ảnh hưởng, ngoại lệ, việc còn lại>
```

### H10.0 - Công cụ golden hậu-oracle

- **Mục đích**: thay `freeze_golden.ps1` sau khi oracle bị xoá. Chạy driver
  self-host đã build (stage1) trên đúng `-Fixture`/`-Modes` được chọn và chỉ
  ghi lại các record thuộc phạm vi đó.
- **Cách dùng**:
  ```powershell
  pwsh -NoProfile -File selfhost/update_golden.ps1 -Driver <stage1>\ivy_selfhost.exe `
      -Reason H10.4 -Fixture "h10_C1_*" -IvmDir <std-ivm>
  pwsh -NoProfile -File selfhost/update_golden.ps1 ... -Fixture 245_move_pointer.ivy -Modes llvm,link
  pwsh -NoProfile -File selfhost/update_golden.ps1 ... -Fixture "*" -Modes c
  pwsh -NoProfile -File selfhost/update_golden.ps1 ... -DryRun
  ```
- **Định dạng**: y hệt `freeze_golden.ps1` (`@@ <key> <nbytes> <exit>` + byte
  thô, CRLF→LF, `<OUT>`/`<RT>`/`<CLANG>`, bỏ ` -Wl,/Brepro`). Chỉ ghi các file
  thực sự đổi; record ngoài phạm vi giữ nguyên từng byte.
- **Provenance**: `manifest.txt` nhận thêm dòng
  `provenance=<fixture>|expected:H10.x` cho fixture vừa đổi; fixture không có
  dòng provenance vẫn là baseline `oracle`. `oracle_sha256` được giữ làm mốc
  lịch sử.
- **An toàn**: bắt buộc `-Reason H10.x`; fixture mới chỉ được thêm vào
  `corpus.txt` khi cập nhật đủ 10 mode; driver crash (exit âm) làm script dừng
  trước khi ghi; mọi output compiler nằm trong TEMP riêng.
- **Kiểm chứng (2026-11-22)**, stage1 dựng từ `56677e4`
  (SHA-256 `23F5BCF7…C48EA`, 27 138 560 B), golden thử nghiệm là bản copy trong
  TEMP:
  - `-DryRun` trên `01_return_const.ivy -Modes lex`: `no golden changes`,
    file golden không đổi;
  - thêm fixture tạm `zz_h10_tool_test.ivy` với đủ 10 mode: **+10 record**,
    `corpus.txt` +1, `manifest.txt` provenance `expected:H10.0`;
    `check_golden.ps1 -Filter zz_h10_tool_test.ivy` → **11 checks / 0
    failures**;
  - chạy lại cùng lệnh: `no golden changes` (idempotent);
  - sửa header exit của một record thành `1` rồi cập nhật `-Modes lex`:
    **~1 record**, sửa lại đúng; check xanh trở lại;
  - `-Modes link` trên `01_return_const.ivy` và trên fixture
    `138_new_struct_noparen.ivy` (nondet): `no golden changes`, không đụng
    `link/nondet.txt`;
  - fixture mới + `-Modes lex` bị từ chối đúng thông báo; `-Reason H9.9` bị
    từ chối;
  - golden thật trong repo không bị sửa (`git status` chỉ có script mới).

### H10.1 - `&<local struct>` và record sống qua call

- **Bug**: **B-C3** - `&<biến local kiểu struct>` làm oracle crash
  `0xC0000005`; bản port cũ báo lỗi `no matching operator '&'` vì
  `build_unary` coi mọi operand struct là cần `operator&` (7.4). **C19** -
  oracle làm hỏng khung stack khi một record local còn sống trong lúc gọi hàm
  trả record theo giá trị (`setDataLayout("")`), bug machine-code không lộ ở IR.
- **Sửa**:
  - `hir/hir_builder.ivy` `build_unary`: nhánh "struct operand cần operator
    overload" bỏ qua khi toán tử là `&` (`this.seq(a.sval, "&") == false`).
    `&` không nằm trong tập operator nạp chồng; nó đi tiếp xuống đường
    address-of bình thường và nhận địa chỉ local.
  - `codegen/codegen.ivy`: không đổi code. Đường emit hiện lấy data layout từ
    `TargetMachine` (`create_target_data_layout` + `set_module_data_layout`)
    **trước** `target_machine_emit_to_file`, nên pattern C19 không tái hiện.
    Fixture mới khoá hành vi này.
- **Fixture**:
  - `selfhost/corpus/codegen/h10_B-C3_addr_of_struct.ivy` - `S* p = &s`,
    ghi/đọc qua `p->x`; kỳ vọng exit `7`.
  - `selfhost/corpus/codegen/h10_C19_record_across_call.ivy` - ba record local
    (`keep`, `pad1..3`) còn sống qua `Pair got = make(7)`; kiểm tra cả record
    cũ lẫn record trả về; kỳ vọng exit `15`.
- **Golden**: thêm 2 fixture (10 mode mỗi fixture, 26 record; +2 `corpus.txt`)
  qua `update_golden.ps1 -Reason H10.1 -Fixture "h10_*"`; `manifest.txt`
  provenance `expected:H10.1`; `link/stdout.txt` của B-C3 không có output,
  C19 không có output (chỉ exit code).
- **Gate**: `build.ps1 -SelfLink -Golden` → bootstrap OK, self-link SHA
  `4892DB3E0D02150279B47D355BABECD4A81336FD342D98169F0BE0FDE593B021`
  (27 138 560 B), golden **12365 checks / 0 failures** (1054 fixture).
  `check_repro.ps1` → PASS. Trước khi freeze, driver stage1 chạy trực tiếp hai
  fixture: link 0, exit `7` và `15`.
- **Ghi chú**: đây là bug **loại B** (port không tái hiện oracle); sau H10.1
  port làm đúng thay vì báo lỗi, nên `ORACLE_BUGS.md` §3 cập nhật lại mô tả
  port. Không đổi hành vi của bất kỳ fixture cũ nào.

### H10.2 - RAII/Dtor: duplicate, return, jump, aggregate init

- **Bug**:
  - **H17**: `build_return` emit dtor cho mọi scope tới `dbase_` nhưng chỉ clear
    scope trong cùng; scope ngoài bị emit lần nữa ở cuối block → HIR có dtor
    trùng.
  - **H18**: lần `return` đầu clear danh sách dtor của scope, nên `return` thứ
    hai **cùng scope** không có dtor và không tạo temp `__ret`.
  - **C4**: `break`/`continue`/`nextcase` trong `if` lồng chỉ chạy dtor của
    scope trong cùng, bỏ qua local của loop body.
  - **H26**: aggregate init `T x = {...}` bypass ctor nhưng vẫn record dtor;
    port còn so sai kind (`17` = alignof thay vì `19` = init-list).
- **Sửa** (`hir/hir_builder.ivy`):
  - `build_return`: danh sách dtor là trạng thái **lexical**, không clear sau
    khi emit. Mỗi `return` tự emit dtor và tự tạo `__ret`; `build_compound`
    không emit dtor cuối block khi câu lệnh cuối cùng là terminator
    (`is_terminator`), nên không còn bản trùng của scope ngoài.
  - `wrap_scope_dtors` + `jump_target`: thêm hai stack `jscope_` (loop body) và
    `nscope_` (switch). `break` destroy tới construct sâu hơn trong hai;
    `continue` tới loop trong cùng; `nextcase` tới switch trong cùng (đi xuyên
    loop bên trong). Không truncate danh sách dtor, nên nhánh fall-through của
    cùng block vẫn còn dtor.
  - `build_decl`: nhận đúng `init-list` (`kind == 19`); khi aggregate đã bypass
    ctor, đặt `rec_dtor = 0` — không destroy object chưa được construct.
- **Fixture**:
  - `h10_H17_nested_return.ivy` - `return` trong block lồng; HIR chỉ còn một
    `Res::~Res` trên mỗi đường return; chạy in `dtor 1` hai lần, exit `15`.
  - `h10_H18_two_returns.ivy` - hai `return` trong cùng scope (switch); cả hai
    có `__ret`/`__ret0` + dtor; chạy in `box 7` hai lần, exit `17`.
  - `h10_C4_break_nested.ivy` - `break` trong `if` lồng; in `bye 0..2` (3 lần),
    exit `2`.
  - `h10_H26_agg_ctor_dtor.ivy` - `Pair p = {7, 8}` với ctor+dtor; không in
    `ctor`/`dtor`, exit `15`.
- **Golden**: `195_raii_break`, `196_raii_continue` đổi (5 record mỗi fixture:
  dtor của loop body xuất hiện trước `break`/`continue`); `196` có thêm dòng
  `dtor 1` khi chạy. Thêm 4 fixture × 10 mode; `corpus.txt` +4; provenance
  `expected:H10.2`. Fixture `194_ctor_agg_bypass` không đổi.
- **Gate**: `build.ps1 -SelfLink -Golden` → bootstrap OK, self-link SHA
  `E506FD7D94F9A154596B3408ABB19AB6AF1E5063A954BA5054C3CA7F39188C36`
  (27 140 096 B), golden **12421 checks / 0 failures** (1058 fixture).
  `check_repro.ps1` → PASS.
- **Ghi chú**: switch mà mọi case đều `return` vẫn có một dtor cuối function
  body (đường không bao giờ chạy); đây là dead code, không phải emit trùng
  trong cùng một đường thực thi. Test trực tiếp stage1: cả 4 fixture link 0 và
  cho output/exit như trên.

### H10.3 - Borrow-checker lỏng (soundness)

- **Bug**: **H19** — oracle (và port bug-compat) không chẩn đoán ba lỗ hổng
  an toàn của borrow-checker: (a) ghi vào biến đang có borrow sống
  (`stage/s14_assign_through_borrow.ivy` → exit 0); (b) buộc reference vào
  temporary rồi dùng lại, tức use-after-destroy (`stage/s9_ref_to_temp.ivy`,
  `stage/s8_use_after_destroy.ivy` → exit 0); (c) `checkStore` gần như không
  tới được vì mọi giá trị lifetime `Local` đều cần `unsafe`, mà `unsafe` tắt
  mọi check. Chi tiết: `LIFETIME_NOTES.md` §8.7, `H52_NOTES.md` §9.1.
- **Sửa** (`mir/mir_builder.ivy`):
  - `check_assign_borrow(name, line, col)`: tra borrow state từ trong ra ngoài
    (giống `check_borrow`), báo
    `cannot assign to '<n>' — it is mutably borrowed` hoặc
    `... it is immutably borrowed`.
  - Gọi từ `ex_assign` (gán `=`, `+=`, … trước khi mở rộng compound), từ
    `ex_unary` cho `++`/`--`. Chỉ áp dụng cho LHS là `IdentRef`; ghi qua
    member/index để H10.4 (slot theo scope) xử lý.
  - `check_ref_binding(vi, line, col)`: gọi từ `st_decl` cho **mọi** khai báo
    reference. Initializer phải là place — `IdentRef`, `This`, `Index`,
    `Member`, deref `*`, hoặc biểu thức đã có type reference/pointer. Nếu không,
    báo `binding a reference to a temporary value (dangling reference)` tại
    điểm khai báo; đây là chỗ chặn cả s9 (bind temp) lẫn s8 (dùng sau đó).
  - `checkStore` giữ nguyên bypass `unsafe` **có chủ ý**: runtime self-host
    buộc `&<local>` vào out-parameter trong `unsafe`
    (`mir/interpreter.ivy`: `lenptr.p = &dec_len;`), nên siết nó sẽ từ chối
    chính mã nguồn compiler. Ngoại lệ này được khoá bằng fixture.
- **Fixture** (`selfhost/corpus/mir/lifetime/`):
  - `h10_H19_assign_while_borrowed.ivy` — borrow mutable và shared; 2 lỗi
    (`x` mutably, `y` immutably borrowed), exit 1.
  - `h10_H19_write_forms.ivy` — `a++`, `b--`, `c += 2`, tham số `p` bị borrow
    rồi gán; 4 lỗi, exit 1.
  - `h10_H19_ref_to_temp.ivy` — `int32& r = 1` và `int32& r = a + b`; 2 lỗi;
    các dạng place (`T& a = x`, `T& b = get(x)`, deref) vẫn qua.
  - `h10_H19_use_after_destroy.ivy` — `Res& r = Res(1)` với destructor; lỗi ở
    binding, exit 1 (use-after-destroy bị chặn tại nguồn).
  - `h10_H19_unsafe_escape.ivy` — hai borrow mutable + ghi trong `unsafe` vẫn
    được chấp nhận (exit 0), khoá ngoại lệ có chủ ý.
- **Golden**: 5 fixture mới × 10 mode; corpus.txt +5; provenance
  `expected:H10.3`. `hir/type/66_ref_rvalue.ivy` đổi hành vi (trước đây `--hir`
  exit 0, `-c` báo "not assignable"; nay lỗi `binding a reference to a
  temporary` ở MIR), provenance `expected:H10.3`; comment của fixture ghi rõ
  quy tắc mới. Không fixture nào khác đổi.
- **Gate**: `build.ps1 -SelfLink -Golden` → bootstrap OK, F2 + self-link
  identical SHA `9AAE83B0D6CCF3C5D506EE8549A7AAFF56EA8F30E958BCA4C0508A8113717703`
  (27 141 120 B), golden **12476 checks / 0 failures** (1063 fixture).
  `check_repro.ps1` → **PASS** (83 artifacts: 42 object, 38 `.ivm`, 3 exe;
  stage1 SHA-256 `9AAE83B0…717703`).
- **Ghi chú**: `unsafe` vẫn là escape hatch cho borrow/alias (giữ nguyên hành
  vi cũ), đây là ngoại lệ có chủ ý — xem H10.17. Các lỗ hổng còn lại của
  family H19: ghi qua member/index của biến đang borrow, và use-after-destroy
  qua raw pointer trong `unsafe` (cố ý cho phép).

### H10.4 - Slot theo scope, zero-init, GEP `T**`

- **Bug**: **C1** — slot biến local khoá theo **tên**, không theo scope: hai
  local cùng tên ở scope khác nhau dùng chung một `alloca`, last-wins (ví dụ
  `MInst x` ngoài + `long x` trong block: đọc `x.kind` ra giá trị của `long`).
  **C18** — struct local chưa khởi tạo phải được zero-init tại điểm khai báo
  (port đã đúng; mục này chỉ khoá hồi quy bằng fixture IR). **H29** — store qua
  `T**` emit GEP `i8` không scale (`llvmElemType`), lệch với đường đọc
  (`llvmType`), nên `p[i] = x` ghi sai ô; các mảng `char**` của `Linker`
  (`mods_`, `extra_`, `cpph_`, `ch_`) phải đi vòng bằng `__ivy_memcpy`.
- **Sửa**:
  - `mir/mir_builder.ivy`: `declare` trả **binding id** duy nhất cho từng khai
    báo (địa chỉ tăng dần; tham số id âm `-(j+2)`), ghi vào `alloca.b` và
    `expr.b` của `IdentRef`/`This`. Pool lịch sử `hn_*` giữ mọi khai báo của
    function để `lookup_binding` còn phân giải được các destructor call mà HIR
    đặt sau khi scope đã đóng (`break` ra khỏi loop body) — fallback này chỉ
    dùng khi scope/parameter không còn tên đó.
  - `mir/mir_builder.ivy` `clone_expr`: với kind 6/7, `b` là binding id chứ
    không phải index biểu thức con, nên copy nguyên giá trị (nếu clone như
    child sẽ hỏng lookup).
  - `codegen/emitter.ivy`: bảng biến có thêm `vbid_`; `find_var_bid` /
    `is_ref_var_bid` thay cho tra theo tên trong `lower_ident`,
    `lower_lvalue`, address-of, array base và lowering `alloca`; prologue
    dùng `-(i+2)` cho tham số (`this` cũng là tham số như MIR).
  - `codegen/emitter.ivy` `lower_index_addr`: cả hai đường dùng
    `llvmType(e.type)`; xoá toàn bộ workaround `__ivy_memcpy` trong
    `codegen/codegen.ivy` (`add_module_object`, `add_extra`, `add_cpp_header`,
    `add_c_header`, mảng `hobj`) và `app/pipeline.ivy`.
  - `bootstrap/ivy-stage0.exe`: dựng lại seed (fix của compiler phải có ngay
    generation đầu): bootstrap từ seed cũ tới khi stage 1 == stage 2, rồi lấy
    stage 1 làm seed; `ivy-stage0.sha256` và `README.md` cập nhật.
- **Fixture**: `selfhost/corpus/codegen/h10_C1_slot_shadow.ivy` (shadow
  struct/long/parameter; exit 0), `h10_C18_zero_init_decl.ivy` (zero-init ở
  điểm khai báo + trong loop; exit 0), `h10_H29_pointer_store.ivy`
  (`pp[1] = p`, in `9`; exit 0).
- **Golden**: thêm 3 fixture (13+13+11 record; `corpus.txt` +3). Đổi hành vi ở
  7 fixture cũ do slot numbering/binding đúng: `229_range_for_ref`,
  `232_range_for_struct_range`, `233_range_for_struct_elem` (trước crash
  `-1073741819`, nay exit `60` — kết quả đúng), `248_std_unique_ptr`,
  `249_std_vector`, `hir/name/02_shadow` (đúng mục đích C1),
  `mir/cfg/02_local_shadowed`; thêm `h10_H17_nested_return` (slot `__ret` tách
  theo scope). Tất cả provenance `expected:H10.4`.
- **Gate**: `build.ps1 -SelfLink -Golden` → bootstrap PASS, F2 + self-link
  identical SHA `7FCFDE62EE757156C40D4A85A34CB928E82B1823B604989844B2B6380BE827FD`
  (27 159 552 B; trùng seed mới), golden **12518 checks / 0 failures**
  (1066 fixture). `check_repro.ps1` → **PASS** (83 artifact; stage1 SHA-256
  `7FCFDE62…27FD`).
- **Ghi chú**: đây là mục H10 đầu tiên phải **đổi seed** — C1/H29 nằm trong
  chính compiler, nên seed cũ sinh ra stage 1 khác stage 2. C18 vốn đã đúng
  (zero-init tại điểm khai báo); fixture chỉ khoá lại. Fallback lịch sử khai
  báo chỉ áp dụng cho destructor call do HIR tổng hợp; mọi biểu thức từ source
  đều mang binding id chính xác. `233` từ crash thành chạy đúng là hệ quả trực
  tiếp của C1 (range-for dùng cùng tên `p` ở nhiều scope).

### H10.5 - `new` đa tham số, một bảng string, pre-collect trong `new`/`delete`

- **Bug**: **C3** — `new T(args)` chỉ store `args[0]` rồi store với kiểu `T`;
  với struct đó là IR không hợp lệ (`store %struct.P 1`), nên `137_new_struct`,
  `143_new_multiarg`, `152_new_struct_i8` bị linker từ chối. **C5** — pre-pass
  và segment format dùng hai map key khác nhau nên `io::print("hi")` sinh hai
  `@.str.N` giống hệt nhau. **C16** — `collect_expr` không đi vào arg của
  `new`/operand của `delete`, nên string literal trong `new` arg bị lower thành
  `null` và io runtime có thể thiếu. **C17** — declare `@__ivy_alloc`/
  `@__ivy_free` phải chỉ xuất hiện một lần cho cả module.
- **Sửa**:
  - `codegen/emitter.ivy` `lower_new`: với struct, `store zeroinitializer` rồi
    store từng field qua GEP (đúng thứ tự arg, cast như init-list); scalar/enum
    giữ một store. Số arg nhiều hơn field bị bỏ qua.
  - `codegen/emitter.ivy`: bỏ `pm_*`/`make_prefixed_key`; chỉ còn bảng
    `sm_*` khoá theo **raw bytes** (`intern_cstring`) dùng chung cho pre-pass,
    format segment và panic message. `lower_string` tra bảng này (idempotent,
    intern tại chỗ nếu pre-pass bỏ sót), nên không còn trả `null`.
  - `codegen/emitter.ivy` `collect_expr`: kind 15 duyệt mọi arg của `new`,
    kind 16 duyệt operand của `delete` (C16).
  - C17: giữ dedup một cặp declare/module và khoá bằng fixture IR.
  - `bootstrap/ivy-stage0.exe`: dựng lại seed từ chính bản đã sửa (stage 1 ==
    stage 2), vì C5/C16 đổi IR mà seed cũ sinh ra cho chính compiler;
    `ivy-stage0.sha256` và `README.md` cập nhật.
- **Fixture**: `h10_C3_new_struct_args` (`new P(7, 9)`, in `16`),
  `h10_C5_string_dedup` (hai `print("hi")` dùng chung `@.str.0`, in `hihi`),
  `h10_C16_new_arg_string` (`new int32(strlen("hi") + 3)`, in `5`),
  `h10_C17_alloc_decl_once` (nhiều `new`/`delete` trong hai hàm, một cặp
  declare). Tất cả đều link/run được với runtime.
- **Golden**: thêm 4 fixture (đủ 10 mode). Đổi hành vi ở 12 fixture cũ:
  `137_new_struct`, `143_new_multiarg`, `152_new_struct_i8` (IR hợp lệ; `-c`
  và link/run từ fail thành pass), `17_print_str`, `18_println_mix`,
  `21_eprint`, `22_string_escape`, `248_std_unique_ptr`,
  `hir/name/11_builtins`, `interp/15_io_format`, `interp/24_exit`,
  `mir/cfg/04_call_builtin` (bảng string thống nhất nên `@.str.N` đổi số/giảm
  số global). Provenance `expected:H10.5`; `exceptions.txt` không đổi.
- **Gate**: `build.ps1 -SelfLink -Golden` → bootstrap PASS, F2 + self-link
  identical SHA `5AAD7AC5526A043DB3E9B0AFF1713C63D27F69614FAEC6082534E92A11E8E4BA`
  (27 159 552 B; trùng seed mới), golden **12580 checks / 0 failures**
  (1070 fixture). `check_repro.ps1` → **PASS** (stage1 SHA-256 `5AAD7AC5…E4BA`).
- **Ghi chú**: C3 sửa luôn 3 trong 7 fixture của **C7** (IR tự từ chối):
  `137/143/152` nay compile/link/run bình thường; `103_print_ref`,
  `104_print_ref_float`, `186_ivy_print_explicit`, `190_ivy_print_decl_order`
  vẫn để H10.6. C17 vốn đã đúng (một cặp declare cho cả module) nên chỉ thêm
  fixture khoá. Việc chạy full `update_golden` có thể xoá các dòng
  `exceptions.txt` của H27 nếu chạy `-Fixture "*"`; ở đây cập nhật theo từng
  fixture để giữ nguyên phạm vi H27.

### H10.6 - IR/ABI: reference arg, IR tự từ chối, `nullptr`, mangling

- **Bug**: **C2** - `io::print(x)` với `x` là `T&`: `lower_expr` đã load
  referent nhưng phần mở rộng vẫn dùng `llvmType(at)` nên in `sext ptr` /
  `fpext ptr` (IR không hợp lệ, `-c` từ chối). **C7** - 7 fixture mà oracle từ
  chối IR của chính nó; H10.5 đã sửa `137/143/152`, còn `103/104` (C2) và
  `186/190` (`ivy::print_*` explicit in `ivy__print_*` trong khi declare là
  `ivy_print_*`). `249_std_vector` cũng dính C2 dù không có trong danh sách
  `H63_NOTES.md` §7. **C8** - `nullptr` (không `_t`) rơi vào base lạ nên
  `llvm_type` trả `i8`; `auto a = nullptr` cấp slot số nguyên. **C10** -
  `msvcMangleType` không có nhánh array nên `int32_t[4]` mangle theo element
  (`H`), đụng overload scalar. **C11** - call không có MIR entry dùng mangle
  cố định `?<name>@@YHX@Z`; không declare nào khớp nên `-c` báo "use of
  undefined value".
- **Sửa** (chỉ `codegen/emitter.ivy`; `objdrive.ivy` không đổi):
  - `lower_io_arg`: `drop = 1` khi `at.is_ref`; nhánh float và integer dùng
    `llvm_type(at, drop)` (kiểu referent) vì `lower_expr` đã load giá trị
    (nhánh char/bool vốn không dùng `llvm_type`). IR còn `sext i32` /
    `fpext float`.
  - `llvm_type`: `nullptr` → `ptr` (như `nullptr_t`).
  - `emit_mangle_type`: array → `PAV` (array param decay thành pointer ở call
    site); đặt **sau** `is_ref`/`ptr_depth` để `T(&)[N]` vẫn là `AAV`.
  - `emit_builtin_symbol`: `::` → **một** `_` (bỏ `:` thứ hai), nên
    `ivy::print_int` → `ivy_print_int`, khớp declare/runtime.
  - `emit_unresolved_symbol(MExpr)`: mangle theo **signature call site**
    (`@Y` + kiểu trả về + từng arg; `X` cho void/không arg). Thêm
    `emit_unresolved_decl`: phát `declare` khớp (array arg → `ptr`, còn lại
    `llvm_type(a.type, 1)`). `emit()` quét `mu->exprs` sau các declare bodyless
    và trước function definitions, bỏ qua callee đặc biệt (virtual, `move`,
    `io::*`, `ivy::*`).
  - `std/rt.ivy`: thêm 8 hàm `ivy_print_int|float|str|char` và
    `ivy_println_*` (dùng lại `__ivy_fmt_*`/`__ivy_write_*`; tham số string là
    `uint64` như `__ivy_write_str`), để `rt.obj` định nghĩa đúng các symbol mà
    emitter declare cho `ivy::print_*` explicit. `lib/ivyrt.c` là runtime C cũ,
    không còn tham gia bootstrap; runtime sống là `std/rt.ivy`.
- **Fixture**: `h10_C2_print_ref` (`int32&`/`float64&` vào `io::println`, in
  `7`/`1.5`), `h10_C8_nullptr_literal` (`alloca ptr` + `store ptr null`),
  `h10_C10_array_mangle` (`?first_int32_t_@@YHPAV@Z` thay vì `YHH`),
  `h10_C11_unresolved_decl` (`declare i32 @"?Box@@YHH@Z"(i32)` + call khớp).
- **Golden**: thêm 4 fixture (đủ 10 mode). Đổi hành vi 11 fixture cũ:
  `103_print_ref`, `104_print_ref_float`, `249_std_vector` (`-c` + link/run
  fail → pass; `249` in `20 24`), `186_ivy_print_explicit`,
  `190_ivy_print_decl_order`, `191_ivy_print_enum`, `185_ivy_print_types`
  (`-c` đã pass từ trước; link/run fail vì thiếu `ivy_print_*` → nay pass:
  `186` in `423.5hiA7…`, `190` in `1x`, `191` in `2`/`1`, `185` in
  `1`,`13.14`,`A`,`s`,`5`,`1`), `hir/template/01_deduce_array` (`PAV`),
  `hir/template/05_specid_call_without_constructor`,
  `05_struct_template_wins_over_function_template` (declare + call khớp),
  `hir/type/06_lit_nullptr` (`ptr`). Provenance `expected:H10.6`;
  `exceptions.txt` không đổi.
- **Gate**: `build.ps1 -SelfLink -Golden` → bootstrap PASS, F2 + self-link
  identical SHA `2E2298C772E20330F5B9EB15D9431935E2E602519B2563BEA4FA486ECC1F8C42`
  (27 166 208 B), golden **12 644 checks / 0 failures** (1 074 fixture). Seed
  giữ nguyên `5AAD7AC5…E4BA`: stage 1 == stage 2 chứng minh IR mà compiler sinh
  cho chính nó không đổi (C2/C7/C8/C10/C11 chỉ đụng construct không xuất hiện
  trong `selfhost/**`), nên không cần dựng lại seed như H10.4/H10.5. Exe
  self-link đổi SHA so với H10.5 (27 165 696 → 27 166 208 B) chỉ vì `rt.obj`
  lớn hơn 8 hàm mới, không phải vì codegen đổi. `check_repro.ps1` → **PASS**
  (83 artifact: 42 object, 38 `.ivm`, 3 exe; stage 1 SHA-256 `2E2298C7…8C42`).
- **Ghi chú**: symbol `ivy_print_*` nay khớp đúng `declare` (một `_`) và 8 hàm
  cùng tên đã được thêm vào runtime sống `std/rt.ivy`, nên `185/186/190/191`
  link/run được và in đúng output. `lib/ivyrt.c` là runtime C cũ, không còn
  tham gia bootstrap; `std/rt.ivy` là nguồn của `rt.obj`. Tham số string của
  builtin là `uint64` (như `__ivy_write_str`) vì subset từ chối raw pointer
  parameter ngoài `unsafe`. `rt.obj` mới là 9 909 B (SHA `28539B39…66C1`,
  stage 0 và stage 1 dựng giống hệt), nên `manifest.txt` cập nhật
  `runtime_sha256`/`runtime_size` - từ đây `check_golden` không còn warning
  "runtime differs" (baseline cũ 8 711 B là runtime C thời oracle).

### H10.7 — Literal/format IR: em dash, `-0.0`, octal escape, label rỗng, struct

- **Bug**:
  - **C20** — `decodeBody`/`readOctalEscape` bắt đầu đọc từ chữ số **sau** chữ
    số mà `switch` đã tiêu thụ, nên `"\101"` ra byte `0x01` và `"\1"` ra
    `0x00` (thay vì `'A'` và `0x01`).
  - **C13** — `continue` nằm trong `switch` lấy context loop của switch
    (`incr == -1`, `cond == -1`) rồi `jump_to(-1)`; emitter in ra
    `br label %` với nhãn rỗng, làm IR không hợp lệ (`llvm-as` từ chối).
  - **C6** — `emit_struct_defs` in **mọi** struct (kể cả struct không được
    dùng), in struct rỗng thành `{  }` (hai dấu cách) và luôn thêm một dòng
    trống sau khối.
  - **C14** — literal `-0.0` giữ dấu và in `-0.000000` trong IR, trong khi
    ngôn ngữ coi `+0.0` và `-0.0` bằng nhau.
  - **H22** — em dash U+2014 bị đồn là mojibake. Kiểm tra byte thô cho thấy
    port **đã** truyền nguyên UTF-8 (`E2 80 94`) ở cả diagnostic lẫn
    `@.str.N`; mojibake trước đây đến từ lớp capture của PowerShell (`2>`),
    không phải từ compiler. H10.7 khoá hành vi đúng bằng fixture.
- **Sửa**:
  - `codegen/emitter.ivy` `decode_body` (C20): khởi tạo `v = e - '0'`,
    `cnt = 1` rồi mới đọc tối đa hai chữ số bát phân tiếp theo.
  - `mir/mir_builder.ivy` `st_continue` + `loop_continue_target` (C13): bỏ
    qua context switch (`sw_base >= 0`) và nhắm block tăng/điều kiện của
    loop thật gần nhất; không có loop thì không emit jump. Emitter thêm
    guard bỏ branch/arm/default có target âm (phòng thủ, không còn xảy ra).
  - `codegen/emitter.ivy` `emit_struct_defs` (C6): chỉ in struct được dùng;
    `mark_used_structs` quét type trong pool expr/inst, tham số, return và
    đóng closure qua field (giữ cả struct chỉ xuất hiện qua con trỏ/tham
    chiếu vì member access cần layout; struct polymorphic giữ vì vtable là
    global). Struct rỗng in `{}`, không còn dòng trống thừa.
  - `codegen/emitter.ivy` `emit_double_lit` (C14): bỏ phép dò dấu bằng
    `1.0 / v`; `-0.0` in thành `0.000000`.
- **Fixture** (`selfhost/corpus/codegen/`):
  - `h10_C20_octal_escape.ivy` — `"\101"`/`"\1"`; link stdout
    `AAB\r\n\x01\r\n` (8 B), exit 0.
  - `h10_H22_em_dash.ivy` — em dash thật trong source; IR
    `c"A\E2\80\94B\00"`, stdout `A\xE2\x80\x94B\r\n`.
  - `h10_C14_negative_zero.ivy` — `neg(0.0)` constexpr thành `-0.0`; IR
    `store double 0.000000`, runtime `z == 0.0`.
  - `h10_C13_switch_continue.ivy` — `continue` trong switch; IR không còn
    `br label %` rỗng, chạy cho exit 5.
  - `h10_C6_unused_struct.ivy` — struct `Unused` không được in, `Empty` in
    `{}`; chạy cho exit 7.
- **Golden**: 5 fixture mới × 10 mode; corpus.txt +5 (1079 fixture, 269
  link). 20 fixture cũ đổi record: `22_string_escape` (octal), `34_struct_multi`
  (`{}`), 8 `hir/constexpr` (`05,06,08,11,12,13,14_sizeof*`, `58_negative_zero`),
  4 `hir/template` (`04_template_in_method`, `05_base_dropped`,
  `05_specid_call_without_constructor`, `05_struct_template_wins_over_function_template`),
  `hir/template/h10_C11_unresolved_decl`, `hir/type/60_inherit_field`,
  `mir/cfg/03_sizeof_alignof_types`, `mir/cfg/07_deep_nesting` (nay `-c`
  exit 0), `parser/aggregate/12_base`, `parser/aggregate/14_field_init`.
  Provenance `expected:H10.7` cho cả 25 fixture. Không fixture nào khác đổi.
- **Gate**: `build.ps1 -SelfLink -Golden` → bootstrap OK, F2 + self-link
  identical SHA `B809B62811BC900B0F0C61003E4CFF0B45CEC32D7821B93E7620BEA3D2ACC1F1`
  (27 169 792 B), golden **12714 checks / 0 failures** (1079 fixture).
  `check_repro.ps1` → **PASS** (83 artifacts: 42 object, 38 `.ivm`, 3 exe;
  stage1 SHA-256 `B809B628…D2ACC1F1`).
- **Ghi chú**: C14 chọn chuẩn hoá `-0.0` → `0.000000` (giá trị runtime vẫn
  là `-0.0`); `llvm-as` chấp nhận cả `-0.000000` nên đây là thay đổi canonical
  hoá, không phải sửa lỗi IR. C13 sửa ở nguồn MIR (jump target) và giữ guard ở
  emitter để không bao giờ in nhãn rỗng. `07_deep_nesting.ivy` trước đây được
  ghi nhận là fixture `-c` **thất bại** (exit 1) đúng theo bug; nay nó qua và
  object được lưu — đây là thay đổi hành vi có chủ ý của H10.7.

### H10.8 - Runtime `fmt_float`, buffer, `_gcvt`

- **Bug**:
  - **R1** — `__ivy_fmt_float` giữ `_gcvt(v, 15)` từ H9.1 nên bỏ chữ số sau
    dấu chấm: `2.0` in thành `2.`, `100.0` thành `100.`, và dạng mũ thành
    `1.e+020` / `1.e-005`.
  - **R2** — oracle trả con trỏ vào **static buffer dùng chung**; port H9.1
    đã cấp buffer mới mỗi lần gọi nhưng chưa có fixture khoá hành vi (và
    subset không có biến file-scope/`static` local để tái hiện buffer chung).
  - **R3** — `_gcvt` được giữ vì `snprintf("%g", double)` hỏng ABI varargs
    double dưới LLJIT trên Windows x64; bản vá R1 không được đổi các chữ số
    khác của `_gcvt`.
- **Sửa**:
  - `std/rt.ivy` `__ivy_fmt_float`: sau `_gcvt`, tìm dấu `.` cuối cùng; nếu
    nó đứng ngay trước NUL hoặc `e`/`E` thì chèn `0` (dời phần đuôi sang
    phải). Buffer 64 B an toàn vì `_gcvt(..., 15)` dài tối đa ~24 ký tự.
    Mọi byte khác giữ nguyên; `__ivy_fmt_int` và cách cấp buffer mới không
    đổi.
  - Cập nhật comment đầu `std/rt.ivy`: R2 là deviation cố ý (buffer riêng
    mỗi lần gọi), R1/R3 là bản vá quanh `_gcvt`.
- **Fixture** (`selfhost/corpus/codegen/`):
  - `h10_R1_float_format.ivy` — in `2.0`, `0.0`, `1.0`, `100.0`, `2.5`,
    `1.0/3.0`, `1e20`, `1e-5`; link stdout `66 B`, exit 0.
  - `h10_R2_format_stress.ivy` — format int+float 64 vòng và một call 4
    argument; link stdout `559 B`, exit 0.
- **Golden**: 2 fixture mới × 10 mode; corpus 1079 → 1081 fixture, link
  269 → 271. Record link đổi: `95_float_array` và
  `101_struct_array_float` (`0.` → `0.0`), `247_std_math` (`3.` → `3.0`).
  Provenance `expected:H10.8` cho 5 fixture. `manifest.txt` cập nhật
  `runtime_sha256`/`runtime_size` (`2DAC59FB…C73B5D65`, `10 191 B`) vì đây
  là lần đầu runtime đổi kể từ H9.1.
- **Gate**: `build.ps1 -SelfLink -Golden` → bootstrap OK, F2 + self-link
  identical SHA
  `B3E9126586CA1DE5F47B312DFD10DF19639D3D2073D4F733944D876DA9F5AEE8`
  (27 170 304 B), golden **12742 checks / 0 failures** (1081 fixture).
  `check_repro.ps1` → **PASS** (83 artifacts: 42 object, 38 `.ivm`, 3 exe;
  stage1 SHA-256 `B3E91265.A9F5AEE8`).
- **Ghi chú**: `_gcvt` vẫn in exponent 3 chữ số (`1.0e+020`,
  `1.0e-005`) — giữ đúng quirk của oracle, chỉ vá dấu chấm treo. R2 không
  thể thành scratch buffer/arena cố định trong `std/rt.ivy` vì subset không
  có biến file-scope/`static` local, nên chấp nhận **B** (xem
  `ORACLE_BUGS.md`). `--run` vẫn dùng formatter `%g` 6 chữ số của
  interpreter (không đổi); runtime được kiểm qua link/run.

## 2. Trạng thái
| Mục | ID | Nội dung | Trạng thái |
|---|---|---|---|
| H10.0 | - | `selfhost/update_golden.ps1` (golden hậu-oracle, provenance) | ✅ |
| H10.1 | B-C3, C19 | `&<local struct>` + record local qua call | ✅ |
| H10.2 | H17, H18, C4, H26 | RAII/dtor: emit 2 lần, `return`, break/continue, aggregate init | ✅ |
| H10.3 | H19 | Borrow-checker lỏng (soundness) | ✅ |
| H10.4 | C1, C18, H29 | Slot theo scope, zero-init đúng điểm, GEP `T**`, xoá workaround memcpy | ✅ |
| H10.5 | C3, C5, C16, C17 | `new`/`delete` + global string trùng | ✅ |
| H10.6 | C2, C7, C8, C10, C11 | IR/ABI: ref arg, IR self-reject, `nullptr`, mangling | ✅ |
| H10.7 | H22, C14, C20, C13, C6 | Literal/format IR (em dash, `-0.0`, octal, label rỗng) | ✅ |
| H10.8 | R1, R2, R3 | Runtime `fmt_float`, buffer dùng chung, `_gcvt` | ✅ |
| H10.9 | H14, H15, H16, H28 | Lifetime IR: call result, `__ret`, return/NRVO, `move` | ☐ |
| H10.10 | H20, H21, H23, H24, H25, C9, C12, C15 | HIR/dump + quirk còn lại | ☐ |
| H10.11 | P1, P2, P3, P4, P7, P5 | Lexer/parser: `>>`, value arg, member template, specialization | ☐ |
| H10.12 | T1, T2, T5, T6, T11 | Template pass 1 | ☐ |
| H10.13 | T3, T4, T7, T8, T9 | Template pass 2 | ☐ |
| H10.14 | T10, T12, T13 | Template pass 3 (pack/concept/non-type) | ☐ |
| H10.15 | M1, M2 | `.ivm` escape | ☐ |
| H10.16 | I1 | Interpreter CFG self-edge | ☐ |
| H10.17 | H27, B-P5, B-R2 | Audit bug không tái hiện / ngoại lệ | ☐ |
| H10.18 | - | Audit cuối + `check_repro.ps1` + tag `0.6.0` (khi user yêu cầu) | ☐ |
