#include "mir/interpreter.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace ivy {
namespace mir {

// ============================================================
// Construction
// ============================================================

Interpreter::Interpreter(const TranslationUnit& tu, Machine* machine)
    : tu_(tu),
      machine_(machine ? machine : new NoOpMachine()),
      out_(&std::cout) {}

// ============================================================
// Public entry points
// ============================================================

Value Interpreter::callMain() { return call("main", {}); }

Value Interpreter::call(std::string_view name, std::vector<Value> args) {
    const Function* fn = nullptr;
    for (const auto& f : tu_.functions) {
        if (f->name == name && f->hasBody) { fn = f.get(); break; }
    }
    if (!fn) {
        for (const auto& f : tu_.functions) {
            std::string full = std::string(f->namespacePrefix) + std::string(f->name);
            if (full == name && f->hasBody) { fn = f.get(); break; }
        }
    }
    if (!fn) {
        diags_.push_back({0, 0,
            "IvyInterpret v0.2: function '" + std::string(name) + "' not found"});
        failed_ = true;
        return Value{};
    }
    return execFunction(*fn, std::move(args));
}

// ============================================================
// Diagnostics
// ============================================================

void Interpreter::error(SourceLoc loc, std::string msg) {
    diags_.push_back({loc.line, loc.col, std::move(msg)});
    failed_ = true;
}

// ============================================================
// Variable lookup / declaration
// ============================================================

Cell Interpreter::lookupCell(std::string_view name) {
    for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
        auto found = it->locals.find(std::string(name));
        if (found != it->locals.end()) return found->second;
    }
    return nullptr;
}

void Interpreter::declareCell(std::string_view name, Cell c) {
    frames_.back().locals[std::string(name)] = std::move(c);
}

// ============================================================
// Function execution (CFG traversal)
// ============================================================

Value Interpreter::execFunction(const Function& fn, std::vector<Value> args) {
    frames_.push_back({});
    FrameCtx& frame = frames_.back();
    frame.fn = &fn;

    // Bind parameters.
    for (std::size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
        const auto& p = fn.params[i];
        if (p.name.empty()) continue;
        if (p.type.isReference || p.type.pointerDepth > 0) {
            // Reference/pointer param: share cell if arg is Ptr, else wrap.
            if (args[i].isPtr() && !args[i].ptr.isNull)
                frame.locals[std::string(p.name)] = args[i].ptr.cell;
            else
                frame.locals[std::string(p.name)] = makeCell(std::move(args[i]));
        } else {
            frame.locals[std::string(p.name)] = makeCell(std::move(args[i]));
        }
    }

    // CFG traversal: walk blocks via curBlock/curInst pointer pair.
    if (!fn.blocks.empty()) {
        frame.curBlock = fn.blocks[0].get();
        frame.curInst = 0;
        while (!frame.returned && frame.curBlock) {
            execBlock(frame);
        }
    }

    Value ret = std::move(frame.retVal);
    frames_.pop_back();
    return ret;
}

void Interpreter::execBlock(FrameCtx& frame) {
    const Block* blk = frame.curBlock;
    if (!blk) { frame.returned = true; return; }

    while (frame.curInst < blk->insts.size() && !frame.returned) {
        const Inst& inst = *blk->insts[frame.curInst];
        execInst(frame, inst);
        // If the inst changed block (Jump/CondBranch) or returned, stop
        // processing this block — the caller (execFunction loop) will pick
        // up the new curBlock/curInst.
        if (frame.curBlock != blk || frame.returned)
            break;
        ++frame.curInst;
    }
    // If we fell off the end without a terminator, treat as implicit return.
    if (!frame.returned && frame.curBlock == blk)
        frame.returned = true;
}

void Interpreter::execInst(FrameCtx& frame, const Inst& inst) {
    using K = Inst::Kind;
    switch (inst.kind) {
        case K::Alloca: {
            const auto& a = std::get<Inst::Alloca>(inst.node);
            Value init;
            if (a.type.arraySize > 0) {
                // Array: create a Struct-based representation where each
                // element is a field named by its index ("0", "1", ...).
                Value arr;
                arr.kind = Value::Struct;
                arr.strct.typeName = "__array";
                for (std::uint32_t i = 0; i < a.type.arraySize; ++i) {
                    arr.strct.fields[std::to_string(i)] = makeCell(makeInt(0));
                }
                declareCell(a.var, makeCell(std::move(arr)));
                break;
            }
            if (a.init) {
                init = evalExpr(*a.init);
            } else {
                // Default: zero/null init.
                if (a.type.pointerDepth > 0) {
                    init = makeNullPtr();
                } else {
                    // Check if struct type — default-construct.
                    bool isStruct = false;
                    for (const auto& s : tu_.structs)
                        if (s.name == a.type.base) { init = defaultStructValue(a.type.base); isStruct = true; break; }
                    if (!isStruct) init = makeInt(0);
                }
            }
            if (a.type.isReference || a.type.pointerDepth > 0) {
                if (init.isPtr() && !init.ptr.isNull)
                    declareCell(a.var, init.ptr.cell);
                else
                    declareCell(a.var, makeCell(std::move(init)));
            } else {
                declareCell(a.var, makeCell(std::move(init)));
            }
            break;
        }
        case K::Store: {
            const auto& st = std::get<Inst::Store>(inst.node);
            Cell c = lvalueCell(*st.target);
            if (!c) { error(inst.loc, "cannot assign to expression"); break; }
            // Write-through references.
            Cell target = (c->isPtr() && !c->ptr.isNull) ? c->ptr.cell : c;
            Value rhs = evalExpr(*st.value);
            *target = std::move(rhs);
            break;
        }
        case K::Eval: {
            const auto& ev = std::get<Inst::Eval>(inst.node);
            if (ev.value) evalExpr(*ev.value);
            break;
        }
        case K::Ret: {
            const auto& rt = std::get<Inst::Ret>(inst.node);
            if (rt.value) frame.retVal = evalExpr(*rt.value);
            frame.returned = true;
            break;
        }
        case K::CondBranch: {
            const auto& cb = std::get<Inst::CondBranch>(inst.node);
            Value cond = evalExpr(*cb.cond);
            bool taken = cond.isInt() ? cond.asInt() != 0
                        : cond.isFloat() ? cond.asFloat() != 0.0 : false;
            frame.curBlock = taken ? cb.thenBlock : cb.elseBlock;
            frame.curInst = 0;
            break;
        }
        case K::Jump: {
            const auto& j = std::get<Inst::Jump>(inst.node);
            frame.curBlock = j.target;
            frame.curInst = 0;
            break;
        }
        case K::Switch: {
            const auto& sw = std::get<Inst::Switch>(inst.node);
            long long condInt = 0;
            if (sw.cond) {
                Value cv = evalExpr(*sw.cond);
                condInt = cv.isInt() ? cv.asInt() : 0LL;
            }
            Block* dest = sw.defaultBlock;
            for (const auto& arm : sw.arms) {
                if (arm.value == condInt) { dest = arm.block; break; }
            }
            frame.curBlock = dest;
            frame.curInst = 0;
            break;
        }
    }
}

// ============================================================
// Expression evaluation
// ============================================================

Value Interpreter::evalExpr(const Expr& e) {
    using E = Expr;
    return std::visit([&](const auto& v) -> Value {
        using V = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<V, E::IntegerLit>) {
            return makeInt(v.value);
        } else if constexpr (std::is_same_v<V, E::FloatLit>) {
            return makeFloat(v.value);
        } else if constexpr (std::is_same_v<V, E::BoolLit>) {
            return makeInt(v.value ? 1 : 0);
        } else if constexpr (std::is_same_v<V, E::StringLit>) {
            // Use decoded bytes from back-fill pass — stored as Str Value.
            return makeStr(v.decoded);
        } else if constexpr (std::is_same_v<V, E::CharLit>) {
            return makeInt(v.decoded);
        } else if constexpr (std::is_same_v<V, E::NullptrLit>) {
            return makeNullPtr();
        } else if constexpr (std::is_same_v<V, E::IdentRef>) {
            Cell c = lookupCell(v.name);
            if (!c) { error(e.loc, "undefined variable '" + std::string(v.name) + "'"); return makeVoid(); }
            // Array: return the array value directly (no decay in interpreter).
            if (e.type.arraySize > 0) return *c;
            // If the variable is a pointer, return a Ptr value (don't deref).
            if (e.type.pointerDepth > 0 && !e.type.isReference) {
                return makePtr(c);
            }
            // Reference-transparent: if cell holds a Ptr (reference), load through.
            if (e.type.isReference && c->isPtr() && !c->ptr.isNull)
                return *c->ptr.cell;
            return *c;
        } else if constexpr (std::is_same_v<V, E::Unary>) {
            return evalUnary(v, e);
        } else if constexpr (std::is_same_v<V, E::Binary>) {
            return evalBinary(v, e);
        } else if constexpr (std::is_same_v<V, E::Ternary>) {
            Value cond = evalExpr(*v.cond);
            bool taken = cond.isInt() ? cond.asInt() != 0
                        : cond.isFloat() ? cond.asFloat() != 0.0 : false;
            return evalExpr(taken ? *v.thenBranch : *v.elseBranch);
        } else if constexpr (std::is_same_v<V, E::Assign>) {
            return evalAssign(v, e);
        } else if constexpr (std::is_same_v<V, E::Call>) {
            return evalCall(v, e);
        } else if constexpr (std::is_same_v<V, E::Member>) {
            return evalMember(v, e);
        } else if constexpr (std::is_same_v<V, E::InitList>) {
            return evalInitList(v, e.type, e);
        } else if constexpr (std::is_same_v<V, E::Lambda>) {
            // Represent closure as a tagged Struct holding capture values.
            Value::StructVal sv;
            sv.typeName = std::string(v.closureType);
            // Find the closure struct info to get field names.
            const StructInfo* si = nullptr;
            for (const auto& s : tu_.structs)
                if (s.name == v.closureType) { si = &s; break; }
            for (std::size_t i = 0; i < v.captureInits.size(); ++i) {
                std::string fieldName = si && i < si->fields.size()
                    ? std::string(si->fields[i].name)
                    : std::to_string(i);
                sv.fields[fieldName] =
                    makeCell(v.captureInits[i] ? evalExpr(*v.captureInits[i]) : Value{});
            }
            Value out;
            out.kind = Value::Struct;
            out.strct = std::move(sv);
            return out;
        } else if constexpr (std::is_same_v<V, E::Index>) {
            // Array indexing: base is an array (Struct __array) or pointer.
            // For the interpreter we only handle the array case.
            Value baseVal = evalExpr(*v.base);
            Value idxVal = evalExpr(*v.index);
            long long idx = idxVal.isInt() ? idxVal.asInt() : 0LL;
            if (baseVal.isStruct() && baseVal.strct.typeName == "__array") {
                // Bounds check (always done in interpreter, even in unsafe).
                const std::uint32_t sz = static_cast<std::uint32_t>(baseVal.strct.fields.size());
                if (idx < 0 || static_cast<std::uint64_t>(idx) >= sz) {
                    error(e.loc, "index out of bounds: index " + std::to_string(idx) +
                                 " out of range [0, " + std::to_string(sz) + ")");
                    return makeVoid();
                }
                auto it = baseVal.strct.fields.find(std::to_string(idx));
                if (it != baseVal.strct.fields.end() && it->second) return *it->second;
                return makeInt(0);
            }
            error(e.loc, "IvyInterpret v0.2: pointer index not yet supported");
            return makeVoid();
        } else if constexpr (std::is_same_v<V, E::New>) {
            error(e.loc, "IvyInterpret v0.2: 'new' not yet supported");
            return makeVoid();
        } else if constexpr (std::is_same_v<V, E::Delete>) {
            error(e.loc, "IvyInterpret v0.2: 'delete' not yet supported");
            return makeVoid();
        } else {
            error(e.loc, "IvyInterpret v0.2: unsupported expression");
            return makeVoid();
        }
    }, e.node);
}

// ============================================================
// Binary
// ============================================================

Value Interpreter::evalBinary(const Expr::Binary& b, const Expr& e) {
    const auto& op = b.op;
    SourceLoc loc = e.loc;

    // Short-circuit logical.
    if (op == "&&") {
        Value lv = evalExpr(*b.lhs);
        if (lv.isInt() && lv.asInt() == 0) return makeInt(0);
        Value rv = evalExpr(*b.rhs);
        return makeInt((rv.isInt() ? rv.asInt() != 0 : rv.asFloat() != 0.0) ? 1 : 0);
    }
    if (op == "||") {
        Value lv = evalExpr(*b.lhs);
        if (lv.isInt() && lv.asInt() != 0) return makeInt(1);
        Value rv = evalExpr(*b.rhs);
        return makeInt((rv.isInt() ? rv.asInt() != 0 : rv.asFloat() != 0.0) ? 1 : 0);
    }

    Value lv = evalExpr(*b.lhs);
    Value rv = evalExpr(*b.rhs);

    if (lv.isInt() && rv.isInt()) {
        long long l = lv.asInt(), r = rv.asInt();
        if (op == "+")  return makeInt(l + r);
        if (op == "-")  return makeInt(l - r);
        if (op == "*")  return makeInt(l * r);
        if (op == "/")  { if (!r) { error(loc, "division by zero"); return makeInt(0); } return makeInt(l / r); }
        if (op == "%")  { if (!r) { error(loc, "modulo by zero"); return makeInt(0); } return makeInt(l % r); }
        if (op == "==") return makeInt(l == r ? 1 : 0);
        if (op == "!=") return makeInt(l != r ? 1 : 0);
        if (op == "<")  return makeInt(l <  r ? 1 : 0);
        if (op == "<=") return makeInt(l <= r ? 1 : 0);
        if (op == ">")  return makeInt(l >  r ? 1 : 0);
        if (op == ">=") return makeInt(l >= r ? 1 : 0);
        if (op == "&")  return makeInt(l & r);
        if (op == "|")  return makeInt(l | r);
        if (op == "^")  return makeInt(l ^ r);
        if (op == "<<") return makeInt(l << r);
        if (op == ">>") return makeInt(l >> r);
    }
    if (lv.isFloat() || rv.isFloat()) {
        double l = lv.isFloat() ? lv.asFloat() : static_cast<double>(lv.asInt());
        double r = rv.isFloat() ? rv.asFloat() : static_cast<double>(rv.asInt());
        if (op == "+")  return makeFloat(l + r);
        if (op == "-")  return makeFloat(l - r);
        if (op == "*")  return makeFloat(l * r);
        if (op == "/")  return makeFloat(l / r);
        if (op == "==") return makeInt(l == r ? 1 : 0);
        if (op == "!=") return makeInt(l != r ? 1 : 0);
        if (op == "<")  return makeInt(l <  r ? 1 : 0);
        if (op == "<=") return makeInt(l <= r ? 1 : 0);
        if (op == ">")  return makeInt(l >  r ? 1 : 0);
        if (op == ">=") return makeInt(l >= r ? 1 : 0);
    }

    // String comparisons.
    if (lv.isStr() || rv.isStr()) {
        if (op == "==") return makeInt(lv.asStr() == rv.asStr() ? 1 : 0);
        if (op == "!=") return makeInt(lv.asStr() != rv.asStr() ? 1 : 0);
    }

    // Pointer comparisons (== / != with null or another pointer).
    if (lv.isPtr() || rv.isPtr()) {
        bool lnull = lv.isPtr() ? lv.ptr.isNull : lv.isInt() ? lv.asInt() == 0 : false;
        bool rnull = rv.isPtr() ? rv.ptr.isNull : rv.isInt() ? rv.asInt() == 0 : false;
        if (op == "==") return makeInt(lnull == rnull ? 1 : 0);
        if (op == "!=") return makeInt(lnull != rnull ? 1 : 0);
    }

    error(loc, "IvyInterpret v0.2: unsupported binary op '" + std::string(op) + "'");
    return makeVoid();
}

// ============================================================
// Unary
// ============================================================

Value Interpreter::evalUnary(const Expr::Unary& u, const Expr& e) {
    const auto& op = u.op;
    SourceLoc loc = e.loc;

    if (op == "&") {
        // Special case: &lambda → return Ptr to a cell holding the closure struct.
        if (std::holds_alternative<Expr::Lambda>(u.operand->node)) {
            Value v = evalExpr(*u.operand);
            Cell c = makeCell(std::move(v));
            return makePtr(c);
        }
        Cell c = lvalueCell(*u.operand);
        if (!c) { error(loc, "cannot take address"); return makeVoid(); }
        return makePtr(c);
    }
    if (op == "*") {
        Value v = evalExpr(*u.operand);
        if (!v.isPtr()) { error(loc, "dereference of non-pointer"); return makeVoid(); }
        if (v.ptr.isNull) { error(loc, "null pointer dereference"); return makeVoid(); }
        return *v.ptr.cell;
    }

    // Prefix ++/--.
    if (u.isPrefix && (op == "++" || op == "--")) {
        Cell c = lvalueCell(*u.operand);
        if (c && c->isInt()) {
            *c = makeInt(c->asInt() + (op == "++" ? 1LL : -1LL));
            return *c;
        }
    }
    // Postfix ++/--.
    if (!u.isPrefix && (op == "++" || op == "--")) {
        Cell c = lvalueCell(*u.operand);
        if (c && c->isInt()) {
            Value old = *c;
            *c = makeInt(c->asInt() + (op == "++" ? 1LL : -1LL));
            return old;
        }
    }

    Value v = evalExpr(*u.operand);
    if (op == "-") {
        if (v.isInt())   return makeInt(-v.asInt());
        if (v.isFloat()) return makeFloat(-v.asFloat());
    }
    if (op == "!" || op == "not") {
        long long bv = v.isInt() ? v.asInt() : (v.isFloat() ? (v.asFloat() != 0.0 ? 1LL : 0LL) : 0LL);
        return makeInt(bv == 0 ? 1 : 0);
    }
    if (op == "~" && v.isInt()) return makeInt(~v.asInt());
    if (op == "+")              return v;

    error(loc, "IvyInterpret v0.2: unsupported unary op '" + std::string(op) + "'");
    return makeVoid();
}

// ============================================================
// Assignment
// ============================================================

Value Interpreter::evalAssign(const Expr::Assign& a, const Expr& e) {
    SourceLoc loc = e.loc;
    Cell c = lvalueCell(*a.lhs);
    if (!c) { error(loc, "cannot assign to expression"); return makeVoid(); }

    // Write-through references.
    Cell target = (c->isPtr() && !c->ptr.isNull) ? c->ptr.cell : c;

    Value rhs = evalExpr(*a.rhs);
    const auto& op = a.op;

    if (op == "=") {
        *target = std::move(rhs);
        return *target;
    }

    // Compound assignment.
    Value& lv = *target;
    std::string plain(op.data(), op.size() - 1);  // "+=" → "+"

    if (lv.isInt() && rhs.isInt()) {
        long long l = lv.asInt(), r = rhs.asInt();
        long long res = l;
        if (plain == "+")  res = l + r;
        else if (plain == "-")  res = l - r;
        else if (plain == "*")  res = l * r;
        else if (plain == "/" && r) res = l / r;
        else if (plain == "%" && r) res = l % r;
        else if (plain == "&")  res = l & r;
        else if (plain == "|")  res = l | r;
        else if (plain == "^")  res = l ^ r;
        else if (plain == "<<") res = l << r;
        else if (plain == ">>") res = l >> r;
        else { error(loc, "unsupported compound op"); return makeVoid(); }
        lv = makeInt(res);
        return lv;
    }
    if (lv.isFloat() || rhs.isFloat()) {
        double l = lv.isFloat() ? lv.asFloat() : static_cast<double>(lv.asInt());
        double r = rhs.isFloat() ? rhs.asFloat() : static_cast<double>(rhs.asInt());
        double res = l;
        if (plain == "+") res = l + r;
        else if (plain == "-") res = l - r;
        else if (plain == "*") res = l * r;
        else if (plain == "/") res = l / r;
        else { error(loc, "unsupported compound float op"); return makeVoid(); }
        lv = makeFloat(res);
        return lv;
    }

    error(loc, "unsupported compound assignment");
    return makeVoid();
}

// ============================================================
// lvalue → Cell
// ============================================================

Cell Interpreter::lvalueCell(const Expr& e) {
    using E = Expr;
    if (const auto* id = std::get_if<E::IdentRef>(&e.node)) {
        Cell c = lookupCell(id->name);
        // If cell IS itself a reference (Ptr), return the pointed-to cell.
        if (c && c->isPtr() && !c->ptr.isNull) return c->ptr.cell;
        return c;
    }
    if (const auto* idx = std::get_if<E::Index>(&e.node)) {
        // Array element lvalue: get the array cell, find element by index.
        Cell arrCell = lvalueCell(*idx->base);
        Value idxVal = evalExpr(*idx->index);
        long long i = idxVal.isInt() ? idxVal.asInt() : 0LL;
        if (arrCell && arrCell->isStruct() && arrCell->strct.typeName == "__array") {
            const std::uint32_t sz = static_cast<std::uint32_t>(arrCell->strct.fields.size());
            if (i < 0 || static_cast<std::uint64_t>(i) >= sz) {
                error(e.loc, "index out of bounds: index " + std::to_string(i) +
                             " out of range [0, " + std::to_string(sz) + ")");
                return nullptr;
            }
            auto it = arrCell->strct.fields.find(std::to_string(i));
            if (it != arrCell->strct.fields.end()) return it->second;
        }
        return nullptr;
    }
    if (const auto* u = std::get_if<E::Unary>(&e.node)) {
        if (u->op == "*") {
            Value v = evalExpr(*u->operand);
            if (v.isPtr() && !v.ptr.isNull) return v.ptr.cell;
        }
    }
    if (const auto* m = std::get_if<E::Member>(&e.node)) {
        // Recursively get the base cell, then look up the field.
        Cell base = lvalueCell(*m->base);
        if (!base) {
            Value bv = evalExpr(*m->base);
            if (m->isArrow) {
                if (!bv.isPtr() || bv.ptr.isNull) return nullptr;
                bv = *bv.ptr.cell;
            }
            if (!bv.isStruct()) return nullptr;
            auto it = bv.strct.fields.find(std::string(m->name));
            return it != bv.strct.fields.end() ? it->second : nullptr;
        }
        Value bv = *base;
        if (m->isArrow) {
            if (!bv.isPtr() || bv.ptr.isNull) return nullptr;
            bv = *bv.ptr.cell;
        }
        if (!bv.isStruct()) return nullptr;
        auto it = bv.strct.fields.find(std::string(m->name));
        return it != bv.strct.fields.end() ? it->second : nullptr;
    }
    return nullptr;
}

// ============================================================
// Member access
// ============================================================

Value Interpreter::evalMember(const Expr::Member& m, const Expr& e) {
    SourceLoc loc = e.loc;
    Value base = evalExpr(*m.base);
    if (m.isArrow) {
        if (!base.isPtr() || base.ptr.isNull) {
            error(loc, "arrow on null/non-pointer");
            return makeVoid();
        }
        base = *base.ptr.cell;
    }
    if (!base.isStruct()) {
        error(loc, "member access on non-struct");
        return makeVoid();
    }
    auto& fields = base.strct.fields;
    auto it = fields.find(std::string(m.name));
    if (it == fields.end()) {
        error(loc, "no field '" + std::string(m.name) + "'");
        return makeVoid();
    }
    return it->second ? *it->second : Value{};
}

// ============================================================
// InitList
// ============================================================

Value Interpreter::evalInitList(const Expr::InitList& il, const Type& ty, const Expr& e) {
    (void)e;
    const StructInfo* si = nullptr;
    for (const auto& s : tu_.structs)
        if (s.name == ty.base) { si = &s; break; }

    if (!si) {
        if (!il.elements.empty() && il.elements[0]) return evalExpr(*il.elements[0]);
        return makeVoid();
    }

    Value::StructVal sv;
    sv.typeName = std::string(ty.base);
    for (std::size_t i = 0; i < si->fields.size(); ++i) {
        Value fv;
        if (i < il.elements.size() && il.elements[i]) {
            fv = evalExpr(*il.elements[i]);
        } else {
            // Missing initializer: default-construct the field.
            const Type& ft = si->fields[i].type;
            if (ft.pointerDepth == 0) {
                bool nestedStruct = false;
                for (const auto& s : tu_.structs)
                    if (s.name == ft.base) { fv = defaultStructValue(ft.base); nestedStruct = true; break; }
                if (!nestedStruct) fv = makeInt(0);
            } else {
                fv = makeNullPtr();
            }
        }
        sv.fields[std::string(si->fields[i].name)] = makeCell(std::move(fv));
    }
    Value out;
    out.kind = Value::Struct;
    out.strct = std::move(sv);
    return out;
}

// ============================================================
// Default struct value (zero-init)
// ============================================================

Value Interpreter::defaultStructValue(std::string_view structName) {
    const StructInfo* si = nullptr;
    for (const auto& s : tu_.structs)
        if (s.name == structName) { si = &s; break; }
    if (!si) return makeVoid();

    Value::StructVal sv;
    sv.typeName = std::string(structName);
    for (const auto& f : si->fields) {
        Value fv;
        if (f.type.pointerDepth == 0) {
            bool nestedStruct = false;
            for (const auto& s : tu_.structs)
                if (s.name == f.type.base) { fv = defaultStructValue(f.type.base); nestedStruct = true; break; }
            if (!nestedStruct) fv = makeInt(0);
        } else {
            fv = makeNullPtr();
        }
        sv.fields[std::string(f.name)] = makeCell(std::move(fv));
    }
    Value out;
    out.kind = Value::Struct;
    out.strct = std::move(sv);
    return out;
}

// ============================================================
// Function call
// ============================================================

Value Interpreter::evalCall(const Expr::Call& c, const Expr& e) {
    (void)e;
    std::vector<Value> args;
    args.reserve(c.args.size());
    for (const auto& a : c.args) args.push_back(evalExpr(*a));

    if (isBuiltin(c.callee)) return callBuiltin(c.callee, args);

    if (c.target && c.target->hasBody)
        return execFunction(*c.target, std::move(args));

    // Fallback: look up by name.
    return call(c.callee, std::move(args));
}

// ============================================================
// Builtins (printf, putchar, puts, exit, abort)
// ============================================================

bool Interpreter::isBuiltin(std::string_view name) const {
    return name == "printf" || name == "puts" || name == "putchar" ||
           name == "exit" || name == "abort";
}

namespace {

// Extract a C string from a Value (string literal or pointer to string).
std::string extractString(const Value& v) {
    if (v.isStr()) return v.asStr();
    if (v.isPtr() && !v.ptr.isNull && v.ptr.cell) {
        const Value& pt = *v.ptr.cell;
        if (pt.isStr()) return pt.asStr();
    }
    return {};
}

}  // namespace

Value Interpreter::callBuiltin(std::string_view name,
                                const std::vector<Value>& args) {
    if (name == "puts") {
        std::string s = !args.empty() ? extractString(args[0]) : "";
        *out_ << s << "\n";
        return makeInt(static_cast<long long>(s.size() + 1));
    }
    if (name == "putchar") {
        char c = args.empty() ? 0 : static_cast<char>(args[0].isInt() ? args[0].asInt() : 0);
        *out_ << c;
        return args.empty() ? makeInt(0) : args[0];
    }
    if (name == "printf") {
        if (args.empty()) return makeInt(0);
        // Extract format string from args[0].
        std::string fmt = extractString(args[0]);
        std::string out;
        std::size_t ai = 1;
        for (std::size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%') { out += fmt[i]; continue; }
            ++i; if (i >= fmt.size()) break;
            // Collect flags / width / precision / length modifiers to rebuild
            // the format spec for snprintf.
            std::string spec_prefix;
            while (i < fmt.size() && (std::string("-+ #0").find(fmt[i]) != std::string::npos ||
                                      (fmt[i] >= '0' && fmt[i] <= '9') ||
                                      fmt[i] == '.' || fmt[i] == 'l' || fmt[i] == 'h' ||
                                      fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't')) {
                spec_prefix += fmt[i++];
            }
            if (i >= fmt.size()) break;
            char spec = fmt[i];
            if (spec == '%') { out += '%'; continue; }
            Value arg = (ai < args.size()) ? args[ai++] : Value{};
            char buf[64] = {};
            std::string fmt_spec = "%" + spec_prefix;
            switch (spec) {
                case 'd': case 'i':
                    fmt_spec += "lld";
                    std::snprintf(buf, sizeof(buf), fmt_spec.c_str(),
                        arg.isInt() ? arg.asInt()
                        : arg.isFloat() ? static_cast<long long>(arg.asFloat())
                        : 0LL);
                    out += buf; break;
                case 'u':
                    fmt_spec += "llu";
                    std::snprintf(buf, sizeof(buf), fmt_spec.c_str(),
                        static_cast<unsigned long long>(
                            arg.isInt() ? arg.asInt() : arg.isFloat() ? static_cast<long long>(arg.asFloat()) : 0LL));
                    out += buf; break;
                case 'f':
                    fmt_spec += 'f';
                    std::snprintf(buf, sizeof(buf), fmt_spec.c_str(),
                        arg.isFloat() ? arg.asFloat() : static_cast<double>(
                            arg.isInt() ? arg.asInt() : 0));
                    out += buf; break;
                case 'g':
                    fmt_spec += 'g';
                    std::snprintf(buf, sizeof(buf), fmt_spec.c_str(),
                        arg.isFloat() ? arg.asFloat() : static_cast<double>(
                            arg.isInt() ? arg.asInt() : 0));
                    out += buf; break;
                case 'e':
                    fmt_spec += 'e';
                    std::snprintf(buf, sizeof(buf), fmt_spec.c_str(),
                        arg.isFloat() ? arg.asFloat() : static_cast<double>(
                            arg.isInt() ? arg.asInt() : 0));
                    out += buf; break;
                case 's':
                    out += extractString(arg);
                    break;
                case 'c':
                    out += static_cast<char>(arg.isInt() ? arg.asInt() : 0); break;
                case 'p':
                    out += "(ptr)"; break;
                default:
                    out += '%'; out += spec_prefix; out += spec; break;
            }
        }
        *out_ << out;
        return makeInt(static_cast<long long>(out.size()));
    }
    if (name == "exit") {
        std::exit(static_cast<int>((!args.empty() && args[0].isInt()) ? args[0].asInt() : 0));
    }
    if (name == "abort") { std::abort(); }
    return makeVoid();
}

// ============================================================
// Value::toString
// ============================================================

std::string Value::toString() const {
    switch (kind) {
        case Void:   return "(void)";
        case Int:    return std::to_string(i);
        case Float: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", f);
            return std::string(buf);
        }
        case Ptr:
            if (ptr.isNull) return "(nullptr)";
            return "(ptr)";
        case Struct:
            return "(" + strct.typeName + ")";
        case Str:
            return str;
    }
    return "(unknown)";
}

}  // namespace mir
}  // namespace ivy
