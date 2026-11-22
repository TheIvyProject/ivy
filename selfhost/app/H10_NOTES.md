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

## 2. Trạng thái

| Mục | ID | Nội dung | Trạng thái |
|---|---|---|---|
| H10.0 | - | `selfhost/update_golden.ps1` (golden hậu-oracle, provenance) | ☐ |
| H10.1 | B-C3, C19 | `&<local struct>` + record local qua call | ☐ |
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
