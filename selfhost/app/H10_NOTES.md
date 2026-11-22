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

## 2. Trạng thái
| Mục | ID | Nội dung | Trạng thái |
|---|---|---|---|
| H10.0 | - | `selfhost/update_golden.ps1` (golden hậu-oracle, provenance) | ✅ |
| H10.1 | B-C3, C19 | `&<local struct>` + record local qua call | ✅ |
| H10.2 | H17, H18, C4, H26 | RAII/dtor: emit 2 lần, `return`, break/continue, aggregate init | ☐ |
| H10.3 | H19 | Borrow-checker lỏng (soundness) | ☐ |
| H10.4 | C1, C18, H29 | Slot theo scope, zero-init đúng điểm, GEP `T**`, xoá workaround memcpy | ☐ |
| H10.5 | C3, C5, C16, C17 | `new`/`delete` + global string trùng | ☐ |
| H10.6 | C2, C7, C8, C10, C11 | IR/ABI: ref arg, IR self-reject, `nullptr`, mangling | ☐ |
| H10.7 | H22, C14, C20, C13, C6 | Literal/format IR (em dash, `-0.0`, octal, label rỗng) | ☐ |
| H10.8 | R1, R2, R3 | Runtime `fmt_float`, buffer dùng chung, `_gcvt` | ☐ |
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
