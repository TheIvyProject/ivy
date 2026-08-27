# Kế hoạch hoàn thiện MIR — IvyInterpret v0.2

- **Trạng thái**: ✅ Hoàn thành — 6/6 bước, 14 test pass
- **Ngày**: 2026-08-22
- **Phiên bản**: v0.2 (MIR-based, safety guarantee)
- **Tài liệu liên quan**: [PLAN.md](file:///d:/project/Ivy/ivyc/PLAN.md) (P4.6)

---

## 1. Bối cảnh & Vấn đề

### Pipeline Ivy hiện tại

```
Lexer → Preprocessor → Parser → HIR → MIR ─┬─→ LLVM IR → native code
                                           └─→ IvyInterpret v0.2 (--run)
```

### Tại sao thông dịch MIR?

IvyInterpret v0.2 thông dịch **MIR** — tầng IR sau HIR, trước LLVM IR. Thông dịch MIR đảm bảo:

- **Lifetime checker chạy** (chỉ chạy ở MIR level) — bắt dangling pointer, use-after-free
- **Unsafe enforcement** — MIR mark `inUnsafe`, code unsafe phải trong `[[ivy::unsafe]]` block
- → **Giữ ý nghĩa "Ivy an toàn"** khi chạy `--run` — không như C++ interpreter thường

---

## 2. Tham khảo kiến trúc

### Rust Miri (POPL 2026)

Miri là MIR interpreter trưởng thành nhất, tham chiếu chính cho thiết kế IvyInterpret v0.2.

| Khía cạnh | Miri (Rust) | IvyInterpret v0.2 (Ivy) |
|---|---|---|
| **IR level** | MIR (sau HIR, trước LLVM IR) | MIR (sau HIR, trước LLVM IR) — **giống Miri** |
| **Value representation** | Hybrid: Scalar / ScalarPair / Indirect | Hybrid: Int/Float/Ptr/Struct + Provenance |
| **Memory model** | `HashMap<AllocId, Allocation>` với provenance + initMask | Tương tự: `Memory` table với `Allocation` |
| **Safety policy** | `Machine` trait — inject qua hooks | `Machine` base class — virtual hooks |
| **Alias tracking** | Stacked Borrows / Tree Borrows | Provenance tracking (đơn giản hơn ban đầu) |
| **Control flow** | Block + stmt index pointer | Block* + inst index pointer |

**Bài học chính từ Miri**:
1. MIR là "sweet spot" — CFG đã build, type còn nguyên vẹn
2. `Machine` trait cho phép inject safety policy mà không entangle core interpreter
3. Provenance (pointer origin tracking) là khác biệt cốt lõi với C++ interpreter thường
4. `before_memory_access` là central choke point — mọi load/store đều qua đây

### General CFG interpreter patterns

- **Register-based** (không stack VM) — MIR đã có named allocas, map trực tiếp vào env map
- **Block + stmt pointer** — `(curBlock, curInst)` pair, CondBranch/Jump cập nhật block
- **Switch-dispatch** — `switch(inst.kind)` trên `Inst::Kind`

---

## 3. Phân tích gap MIR hiện tại

### MIR hiện tại có gì

| Structure | Location | Contents |
|---|---|---|
| `Lifetime` | `mir.h:21-31` | `{None, Named, Static, Local, Unknown}` — gắn trên mỗi `Expr` |
| `Expr` | `mir.h:36-76` | Tree-form, 17 node types (Binary, Unary, Call, Member, etc.) |
| `Inst` | `mir.h:87-101` | 6 kinds: `Alloca, Store, Eval, Ret, CondBranch, Jump` + `inUnsafe` flag |
| `Block` | `mir.h:103-105` | `vector<unique_ptr<Inst>>` — basic block |
| `Function` | `mir.h:107-118` | `name, params, lifetimes, returnLifetime, blocks` |
| `TranslationUnit` | `mir.h:145-149` | `functions, enums, structs` |
| CFG edges | `mir.h:94-95` | `CondBranch::thenBlock/elseBlock` + `Jump::target` — **raw Block* đã resolve** |

### MIR hiện thiếu gì

| Gap | Location | Mức độ | Fix |
|---|---|---|---|
| **Call target là `string_view`** | `mir.h:48` | **Blocker** | Thêm `const Function* target` + back-fill pass |
| **Không có Value model** | (absent) | **Blocker** | Tạo `interp_value.h` — runtime Value type |
| **Không có Memory model** | (absent) | **Blocker** | Tạo `interp_memory.h` — Allocation table |
| **String/Char lit raw (có quotes)** | `mir.h:39-40` | Minor | Reuse `decodeString`/`decodeChar` từ `codegen.cpp:337-370` |
| **Lifetime check chỉ check `return`** | `mir_builder.cpp:71-94` | Gap | Mở rộng: check store, arg, null deref |
| **`new`/`delete` không unsafe-gated** | `hir_builder.cpp:1356-1374` | Gap | Gate trong interpreter qua `inUnsafe` |
| **Codegen callee scan bỏ qua namespace** | `codegen.cpp:825-828` | Latent bug | Fix trong back-fill pass |

### MIR đã sẵn sàng (không cần thay đổi)

| Yếu tố | Chi tiết |
|---|---|
| **Expression form** | Tree-form — tree-walking OK, không cần linearize |
| **CFG edges** | `Block*` raw pointers — interpreter duyệt trực tiếp |
| **Phi nodes** | Không có — dùng named allocas, env map `name → Cell` phù hợp |
| **`inUnsafe` flag** | Đã set bởi MIR builder — dùng để gate runtime safety checks |
| **`Lifetime` annotations** | Đã gắn trên mỗi Expr — dùng cho runtime provenance |

---

## 4. Thiết kế Data Structures

### 4.1. Runtime Value type (`mir/interp_value.h`)

```cpp
struct Provenance {
    enum Kind { None, Local, Static, Heap, Param } kind = Kind::None;
    uint32_t allocId = 0;       // ID allocation (cho Heap/Local)
    std::string_view ltName;   // Lifetime name (cho Param, từ MIR Lifetime)
};

struct Value {
    enum Kind { Int, Float, Ptr, Struct, Void } kind = Void;
    long long i = 0;
    double f = 0.0;
    struct PtrVal {
        Cell cell;              // shared_ptr<Value> — pointee
        Provenance prov;        // origin tracking
    } ptr;
    struct StructVal {
        std::string typeName;
        std::unordered_map<std::string, Cell> fields;
    } strct;

    // Queries
    bool isInt()    const { return kind == Int; }
    bool isFloat()  const { return kind == Float; }
    bool isPtr()    const { return kind == Ptr; }
    bool isStruct() const { return kind == Struct; }
    bool isVoid()   const { return kind == Void; }
    long long  asInt()    const;
    double     asFloat()  const;
    PtrVal&    asPtr();
    StructVal& asStruct();
    std::string toString() const;
};

using Cell = std::shared_ptr<Value>;  // heap-allocated, shared ownership
Cell makeCell(Value v);               // helper: create heap cell
```

**Lý do thiết kế**:
- `Cell = shared_ptr<Value>` — pointer/reference aliasing tự nhiên (chia sẻ cell)
- `Provenance` — theo dõi origin pointer, borrow từ Miri (không phải chỉ số nguyên)
- Hybrid representation — Int/Float trực tiếp (không box), Ptr/Struct qua Cell

### 4.2. Allocation / Memory model (`mir/interp_memory.h`)

```cpp
struct Allocation {
    uint32_t id;
    std::vector<std::byte> bytes;
    std::vector<bool> initMask;   // per-byte init tracking
    bool isLive = true;           // use-after-free detection
    bool isHeap = false;          // new vs alloca
};

class Memory {
    std::unordered_map<uint32_t, Allocation> allocs_;
    uint32_t nextId_ = 1;
public:
    uint32_t allocate(size_t size, bool isHeap = false);
    void     deallocate(uint32_t id);  // mark dead
    bool     isLive(uint32_t id) const;
    bool     isInitialized(uint32_t id, size_t offset) const;
    void     writeByte(uint32_t id, size_t offset, std::byte val);
    std::byte readByte(uint32_t id, size_t offset);
};
```

**Lý do thiết kế**:
- Mọi memory access đều qua `Memory` table → `Machine` hook có thể intercept
- `initMask` — catch read-before-init (UB detection)
- `isLive` — catch use-after-free (dangling pointer)
- `isHeap` — distinguish `new` (cần `delete`) vs `alloca` (auto)

### 4.3. Machine trait — policy hook (`mir/interp_machine.h`)

```cpp
class Machine {
public:
    virtual ~Machine() = default;
    // Called on EVERY memory access (load/store)
    virtual void beforeMemoryAccess(const Value::PtrVal& ptr, bool isWrite,
                                     const Memory& mem) {}
    // Called on pointer creation (&x, new, etc.)
    virtual void onPointerCreate(const Value::PtrVal& ptr) {}
    // Called on function call
    virtual void onCall(std::string_view fnName,
                        const std::vector<Value>& args) {}
    // Called on return — check dangling
    virtual void onReturn(const Value& retVal, std::string_view retLt) {}
};

// Concrete machines — incremental safety layers:
class NoOpMachine      : public Machine {};              // plain interp
class BoundsMachine    : public Machine { /* OOB check */ };
class InitMachine      : public Machine { /* uninit read */ };
class LifetimeMachine  : public Machine { /* dangling ptr */ };
class FullSafetyMachine: public Machine { /* all checks */ };
```

**Lý do thiết kế**:
- Core interpreter stays dumb — safety policy inject qua `Machine`
- Bắt đầu với `NoOpMachine`, thêm checks dần (bounds, init, provenance, aliasing)
- Đây là decision có leverage cao nhất — cho phép mở rộng incremental

### 4.4. Interpreter engine (`mir/interpreter.h`, `mir/interpreter.cpp`)

```cpp
class Interpreter {
public:
    explicit Interpreter(const mir::TranslationUnit& tu,
                          Machine* machine = nullptr);
    Value callMain();
    Value call(std::string_view fn, std::vector<Value> args);
    const std::vector<InterpDiag>& diagnostics() const;
    bool failed() const;
    void setOutput(std::ostream& os);
private:
    struct FrameCtx {
        const mir::Function* fn;
        std::unordered_map<std::string_view, Cell> locals;
        const mir::Block* curBlock;
        size_t curInst;
        bool returned = false;
        Value retVal;
    };

    const mir::TranslationUnit& tu_;
    std::vector<FrameCtx> frames_;
    Memory memory_;
    std::unique_ptr<Machine> machine_;
    std::vector<InterpDiag> diags_;
    std::ostream* out_ = nullptr;

    // Block-level execution
    Value execFunction(const mir::Function& fn, std::vector<Value> args);
    void execBlock(FrameCtx& frame);
    void execInst(FrameCtx& frame, const mir::Inst& inst);

    // Expression evaluation (tree-walk MIR Expr)
    Value evalExpr(const mir::Expr& e, FrameCtx& frame);
    Value evalBinary(const mir::Expr::Binary& b, FrameCtx& frame);
    Value evalUnary(const mir::Expr::Unary& u, FrameCtx& frame);
    Value evalCall(const mir::Expr::Call& c, FrameCtx& frame);
    Value evalMember(const mir::Expr::Member& m, FrameCtx& frame);
    Value evalAssign(const mir::Expr::Assign& a, FrameCtx& frame);
    Value evalInitList(const mir::Expr::InitList& il, const mir::Type& ty, FrameCtx& frame);
    Cell lvalueCell(const mir::Expr& e, FrameCtx& frame);

    // Builtins
    Value callBuiltin(std::string_view name, const std::vector<Value>& args);
    bool isBuiltin(std::string_view name) const;

    // Helpers
    void error(SourceLoc loc, std::string msg);
    void declareLocal(FrameCtx& frame, std::string_view name, Cell c);
    Cell lookupLocal(FrameCtx& frame, std::string_view name);
    Value defaultStruct(std::string_view structName);
};
```

**Lý do thiết kế**:
- `FrameCtx` theo dõi `(curBlock, curInst)` — instruction pointer cho CFG
- `locals` = env map `name → Cell` — phù hợp named allocas (không cần SSA)
- `memory_` + `machine_` — memory access luôn qua Machine hook
- Tree-walking `evalExpr` — MIR Expr chưa linearize, đệ quy trực tiếp

### 4.5. Block execution loop

```
execFunction(fn, args):
    push frame
    bind params → locals
    curBlock = fn.blocks[0]
    curInst = 0
    while (!returned):
        inst = curBlock->insts[curInst]
        execInst(frame, inst)
        curInst++
        // execInst có thể set curBlock (Jump/CondBranch) hoặc returned (Ret)

execInst(frame, inst):
    switch (inst.kind):
        Alloca:     declareLocal(frame, inst.var, makeCell(evalExpr(init)))
        Store:      Cell c = lvalueCell(inst.target)
                    *c = evalExpr(inst.value)
                    machine->beforeMemoryAccess(c->ptr, isWrite=true)
        Eval:       evalExpr(inst.value)  // discard result
        Ret:        frame.retVal = evalExpr(inst.value)
                    frame.returned = true
        CondBranch: if (evalExpr(cond).asInt()) curBlock = thenBlock
                    else curBlock = elseBlock
                    curInst = 0  // reset cho block mới
        Jump:       curBlock = target
                    curInst = 0
```

---

## 5. Giai đoạn triển khai (6 bước)

### Bước 1 — Hoàn thiện MIR data structures (`mir.h`) ✅

| Thay đổi | File | Chi tiết | Trạng thái |
|---|---|---|---|
| `Call::target` | `mir.h:54-55` | Thêm `const Function* target = nullptr` | ✅ |
| `StringLit::decoded` | `mir.h:41` | Thêm `std::string decoded` | ✅ |
| `CharLit::decoded` | `mir.h:45` | Thêm `long long decoded` | ✅ |

### Bước 2 — Back-fill pass trong MirBuilder (`mir_builder.cpp`) ✅

Thêm post-pass ở cuối `MirBuilder::build()`:

1. **Resolve `Call::target`** — `walkExpr` visitor scan `mir_->functions` để match `name` + `namespacePrefix`
2. **Decode `StringLit`/`CharLit`** — `decodeString`/`decodeChar` helpers (copy từ codegen)
3. **Walk tất cả instructions** — `Alloca.init`, `Store.target/value`, `Eval.value`, `Ret.value`, `CondBranch.cond`

### Bước 3 — Mở rộng lifetime checker (`mir_builder.cpp`) ✅

| Check mới | Vị trí | Logic | Trạng thái |
|---|---|---|---|
| **Store dangling** | `checkStore()` | Nếu `value->lifetime == Local` và target là pointer slot → error (trừ khi `inUnsafe`) | ✅ |
| **Store lifetime tracking** | `Store` handler | Update variable lifetime trong scope sau khi store | ✅ |

### Bước 4 — Runtime value + memory model ✅

Tạo 3 file mới trong `src/mir/`:

| File | Nội dung | Trạng thái |
|---|---|---|
| `interp_value.h` | `Value` (Int/Float/Ptr/Struct/Str/Void), `Provenance`, `Cell = shared_ptr<Value>`, `makeCell`/`makeStr` | ✅ |
| `interp_memory.h` | `Allocation` (initMask, isLive), `Memory` class | ✅ |
| `interp_machine.h` | `Machine` base + `NoOpMachine` | ✅ |

### Bước 5 — MIR Interpreter engine ✅

Tạo 2 file mới trong `src/mir/`:

| File | Nội dung | Trạng thái |
|---|---|---|
| `interpreter.h` | `Interpreter` class declaration, `FrameCtx` với `std::deque` | ✅ |
| `interpreter.cpp` | Full implementation: CFG traversal, tree-walk evalExpr, builtins | ✅ |

**Bugs fixed trong quá trình**:
1. **Str Value**: Thêm `Str` kind vào `Value` — xóa hack `__str__` struct
2. **Deque fix**: `std::vector<FrameCtx>` → `std::deque<FrameCtx>` — tránh dangling reference khi push frame
3. **execBlock infinite loop**: Thoát while loop khi `frame.curBlock != blk` (Jump/CondBranch đổi block)
4. **Compound assignment**: Expand `lhs op= rhs` → `lhs = lhs op rhs` trong MirBuilder trước khi tạo Store
5. **printf precision**: Rebuild format spec từ flags/width/precision thay vì hardcode `%f`

### Bước 6 — Tích hợp vào main.cpp + test ✅

| Thay đổi | File | Chi tiết | Trạng thái |
|---|---|---|---|
| `--run` flag | `src/app/main.cpp` | `--run` dùng IvyInterpret v0.2 (MIR-based) | ✅ |
| CMakeLists | `CMakeLists.txt` | Thêm `src/mir/interpreter.cpp` vào sources | ✅ |

**Regression test — 14 examples pass**:
- `test_mir_simple` — x+y=30
- `test_mir_call` — z=30 (function call)
- `test_interp_simple` — result: 30
- `test_struct_init` — all init types (init, ctor, partial, empty, zero, reassign)
- `test_interp_lambda` — no-capture: 6
- `test_interp_lambda_cap` — by-value: 15
- `test_mir_for` — sum=6 (for loop + compound assignment)
- `test_mir_loops` — sum=55, x=0, y=160 (for/while/do-while)
- `test_interp_v02` — comprehensive: recursion, loops, break/continue, ternary, short-circuit, bitwise, float
- `test_struct` — struct, class, pointer field, nested, namespace
- `test_interp_struct` — struct + loop + comparison
- `test_struct_defaults` — default member initializers
- `test_lambda_capture` — by-value, by-ref, no-capture
- `test_lambda_parse` — lambda parse + call

**Safety guarantees verified**:
- `test_struct.cpp:71` — `n.next = &nextVal` bị reject (dangling pointer store) → fix bằng `[[ivy::unsafe]]` block

---

## 6. Lưu ý quan trọng

1. **Machine trait từ ngày đầu** — dù ban đầu chỉ `NoOpMachine`, có hook sẵn để thêm safety checks dần (bounds, init, provenance, aliasing). Đây là decision có leverage cao nhất.

2. **Provenance tracking** — pointer không chỉ là số nguyên, mà mang origin (allocId, lifetime). Đây là điểm khác biệt cốt lõi với C++ interpreter thường.

3. **initMask** — track per-byte initialization, catch read-before-init (UB detection).

4. **Thread safety** — sau này nếu Ivy có thread, thêm vector clocks vào `Allocation` (giống Miri `AllocExtra`).

5. **Codegen callee scan bug** — `codegen.cpp:825-828` bỏ qua namespace prefix khi resolve callee. Fix trong back-fill pass (Bước 2) để cả codegen và interpreter đều受益.

---

## 7. Liên kết

- [PLAN.md](file:///d:/project/Ivy/ivyc/PLAN.md) — P4.6 (IvyInterpret roadmap)
- [README.md](file:///d:/project/Ivy/ivyc/README.md) — IvyInterpret section
- [src/mir/mir.h](file:///d:/project/Ivy/ivyc/src/mir/mir.h) — MIR data structures hiện tại
- [src/mir/mir_builder.cpp](file:///d:/project/Ivy/ivyc/src/mir/mir_builder.cpp) — MIR builder (lifetime check, CFG)
- [src/codegen/codegen.cpp](file:///d:/project/Ivy/ivyc/src/codegen/codegen.cpp) — codegen (decodeString/decodeChar source)

---

## 8. Tham khảo

- [Miri: Practical UB Detection for Rust (POPL 2026)](https://dl.acm.org/doi/pdf/10.1145/3776690)
- [Miri DeepWiki](https://deepwiki.com/rust-lang/rust/6.2-miri:-mir-interpreter-and-ub-detector)
- [Miri slides — Scott Olson](https://files.solson.me/miri-slides.pdf)
- [Tree Borrows — PLDI 2025](https://pldi25.sigplan.org/details/pldi-2025-papers/42/Tree-Borrows)
- [Virtual Machine Showdown: Stack vs Registers](https://www.tara.tcd.ie/tara8/server/api/core/bitstreams/9610949a-818f-406d-9c2c-73365ad61108/content)
