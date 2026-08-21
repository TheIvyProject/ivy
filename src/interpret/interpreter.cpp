#include "interpret/interpreter.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

namespace ivy {
namespace interp {

// ============================================================
// Construction
// ============================================================

Interpreter::Interpreter(const hir::TranslationUnit& tu)
    : tu_(tu), out_(&std::cout) {}

// ============================================================
// Public entry points
// ============================================================

Value Interpreter::callMain() { return call("main", {}); }

Value Interpreter::call(std::string_view name, std::vector<Value> args) {
    const hir::Function* fn = nullptr;
    for (const auto& f : tu_.functions) {
        if (f->name == name && f->body) { fn = f.get(); break; }
    }
    if (!fn) {
        // Try full qualified name "ns::func".
        for (const auto& f : tu_.functions) {
            std::string full = std::string(f->namespacePrefix) + std::string(f->name);
            if (full == name && f->body) { fn = f.get(); break; }
        }
    }
    if (!fn) {
        diags_.push_back({0, 0,
            "IvyInterpret: function '" + std::string(name) + "' not found"});
        failed_ = true;
        return Value{};
    }
    return execFn(*fn, std::move(args));
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
        auto found = it->find(std::string(name));
        if (found != it->end()) return found->second;
    }
    return nullptr;
}

void Interpreter::declareCell(std::string_view name, Cell c) {
    topFrame()[std::string(name)] = std::move(c);
}

// ============================================================
// Function execution
// ============================================================

Value Interpreter::execFn(const hir::Function& fn, std::vector<Value> args) {
    frames_.push_back({});

    // Bind parameters.
    for (std::size_t i = 0; i < fn.params.size() && i < args.size(); ++i) {
        const auto& p = fn.params[i];
        if (p.name.empty()) continue;
        if (p.type.isReference || p.type.pointerDepth > 0) {
            // Reference param: if arg is already a Ptr share its cell.
            if (args[i].isPtr())
                topFrame()[std::string(p.name)] = args[i].asPtr();
            else
                topFrame()[std::string(p.name)] = makeCell(std::move(args[i]));
        } else {
            topFrame()[std::string(p.name)] = makeCell(std::move(args[i]));
        }
    }

    Signal sig = execCompound(*fn.body);
    frames_.pop_back();

    if (std::holds_alternative<ReturnSignal>(sig))
        return std::move(std::get<ReturnSignal>(sig).value);
    return Value{};
}

// ============================================================
// Statement execution
// ============================================================

Interpreter::Signal Interpreter::execCompound(const hir::Stmt::Compound& c) {
    frames_.push_back({});  // inner scope
    Signal sig;
    for (const auto& s : c.stmts) {
        sig = execStmt(*s);
        if (!std::holds_alternative<std::monostate>(sig)) break;
    }
    frames_.pop_back();
    return sig;
}

Interpreter::Signal Interpreter::execStmt(const hir::Stmt& s) {
    using S = hir::Stmt;
    return std::visit([&](const auto& v) -> Signal {
        using V = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<V, S::Compound>) {
            return execCompound(v);

        } else if constexpr (std::is_same_v<V, S::Decl>) {
            Value init;
            if (v.type.isReference && v.init) {
                // For reference declarations, bind the cell directly from
                // the initializer's lvalue (so the local aliases the
                // original variable).  This handles `T& r = *ptr;` etc.
                Cell c = lvalueCell(*v.init);
                if (c) { declareCell(v.name, c); return std::monostate{}; }
                // Fallback: if the init is an rvalue Ptr, share its cell.
                Value iv = evalExpr(*v.init);
                if (iv.isPtr() && iv.asPtr()) { declareCell(v.name, iv.asPtr()); return std::monostate{}; }
                init = std::move(iv);
            } else if (v.init) {
                init = evalExpr(*v.init);
            } else {
                // No initializer. For struct types, synthesize a default-
                // constructed struct (all fields zero/default-initialized).
                // For other types this leaves a Void value (zero).
                if (v.type.pointerDepth == 0) {
                    for (const auto& s : tu_.structs)
                        if (s.name == v.type.base) { init = defaultStructValue(v.type.base); break; }
                } else {
                    // Pointer with no init → null.
                    Value pv; pv.data = Value::Ptr{nullptr};
                    init = std::move(pv);
                }
            }
            if (v.type.isReference || v.type.pointerDepth > 0) {
                if (init.isPtr())
                    declareCell(v.name, init.asPtr());
                else
                    declareCell(v.name, makeCell(std::move(init)));
            } else {
                declareCell(v.name, makeCell(std::move(init)));
            }
            return std::monostate{};

        } else if constexpr (std::is_same_v<V, S::If>) {
            Value cond = evalExpr(*v.cond);
            bool taken = cond.isInt() ? cond.asInt() != 0
                       : cond.isFloat() ? cond.asFloat() != 0.0 : false;
            if (taken)           return execStmt(*v.thenBranch);
            if (v.elseBranch)    return execStmt(*v.elseBranch);
            return std::monostate{};

        } else if constexpr (std::is_same_v<V, S::While>) {
            while (true) {
                Value c = evalExpr(*v.cond);
                if (!(c.isInt() ? c.asInt() != 0 : c.isFloat() ? c.asFloat() != 0.0 : false)) break;
                Signal sig = execStmt(*v.body);
                if (std::holds_alternative<ReturnSignal>(sig)) return sig;
                if (std::holds_alternative<BreakSignal>(sig))  break;
            }
            return std::monostate{};

        } else if constexpr (std::is_same_v<V, S::DoWhile>) {
            while (true) {
                Signal sig = execStmt(*v.body);
                if (std::holds_alternative<ReturnSignal>(sig)) return sig;
                if (std::holds_alternative<BreakSignal>(sig))  break;
                Value c = evalExpr(*v.cond);
                if (!(c.isInt() ? c.asInt() != 0 : c.isFloat() ? c.asFloat() != 0.0 : false)) break;
            }
            return std::monostate{};

        } else if constexpr (std::is_same_v<V, S::For>) {
            frames_.push_back({});  // init-scope
            if (v.init) execStmt(*v.init);
            Signal result;
            while (true) {
                if (v.cond) {
                    Value c = evalExpr(*v.cond);
                    if (!(c.isInt() ? c.asInt() != 0 : c.isFloat() ? c.asFloat() != 0.0 : false)) break;
                }
                Signal sig = execStmt(*v.body);
                if (std::holds_alternative<ReturnSignal>(sig)) { result = sig; break; }
                if (std::holds_alternative<BreakSignal>(sig))  break;
                if (v.incr) evalExpr(*v.incr);
            }
            frames_.pop_back();
            return result;

        } else if constexpr (std::is_same_v<V, S::Return>) {
            Value val;
            if (v.value) val = evalExpr(*v.value);
            return ReturnSignal{std::move(val)};

        } else if constexpr (std::is_same_v<V, S::Break>) {
            return BreakSignal{};

        } else if constexpr (std::is_same_v<V, S::Continue>) {
            return ContinueSignal{};

        } else if constexpr (std::is_same_v<V, S::ExprStmt>) {
            if (v.value) evalExpr(*v.value);
            return std::monostate{};

        } else if constexpr (std::is_same_v<V, S::Unsafe>) {
            return execStmt(*v.body);

        } else {  // Null
            return std::monostate{};
        }
    }, s.node);
}

// ============================================================
// Expression evaluation
// ============================================================

Value Interpreter::evalExpr(const hir::Expr& e) {
    using E = hir::Expr;
    return std::visit([&](const auto& v) -> Value {
        using V = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<V, E::IntegerLit>) {
            return Value{v.value};

        } else if constexpr (std::is_same_v<V, E::FloatLit>) {
            return Value{v.value};

        } else if constexpr (std::is_same_v<V, E::BoolLit>) {
            return Value{static_cast<long long>(v.value ? 1 : 0)};

        } else if constexpr (std::is_same_v<V, E::StringLit>) {
            // Strip surrounding quotes and unescape.
            std::string s(v.raw);
            if (s.size() >= 2 && s.front() == '"') s = s.substr(1, s.size() - 2);
            std::string out;
            for (std::size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char nx = s[++i];
                    switch (nx) {
                        case 'n': out += '\n'; break;  case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;  case '\\':out += '\\'; break;
                        case '"': out += '"';  break;  case '0': out += '\0'; break;
                        default:  out += nx;   break;
                    }
                } else { out += s[i]; }
            }
            return Value{std::move(out)};

        } else if constexpr (std::is_same_v<V, E::CharLit>) {
            std::string s(v.raw);
            if (s.size() >= 2 && s.front() == '\'') s = s.substr(1, s.size() - 2);
            char c = s.empty() ? 0
                   : (s[0] == '\\' && s.size() > 1
                      ? (s[1] == 'n' ? '\n' : s[1] == 't' ? '\t' : s[1])
                      : s[0]);
            return Value{static_cast<long long>(static_cast<unsigned char>(c))};

        } else if constexpr (std::is_same_v<V, E::NullptrLit>) {
            Value nv; nv.data = Value::Ptr{nullptr}; return nv;

        } else if constexpr (std::is_same_v<V, E::IdentRef>) {
            Cell c = lookupCell(v.name);
            if (!c) { error(e.loc, "undefined variable '" + std::string(v.name) + "'"); return Value{}; }
            // If the variable is a pointer, return a Ptr value (don't deref).
            if (e.type.pointerDepth > 0 && !e.type.isReference) {
                Value out; out.data = Value::Ptr{c}; return out;
            }
            // Reference-transparent: if cell holds a Ptr (reference), load through.
            if (e.type.isReference && c->isPtr() && c->asPtr()) return *c->asPtr();
            return *c;

        } else if constexpr (std::is_same_v<V, E::Unary>) {
            return evalUnary(v, e.loc);

        } else if constexpr (std::is_same_v<V, E::Binary>) {
            return evalBinary(v, e.loc);

        } else if constexpr (std::is_same_v<V, E::Ternary>) {
            Value cond = evalExpr(*v.cond);
            bool taken = cond.isInt() ? cond.asInt() != 0
                       : cond.isFloat() ? cond.asFloat() != 0.0 : false;
            return evalExpr(taken ? *v.thenBranch : *v.elseBranch);

        } else if constexpr (std::is_same_v<V, E::Assign>) {
            return evalAssign(v, e.loc);

        } else if constexpr (std::is_same_v<V, E::Call>) {
            return evalCall(v, e.loc);

        } else if constexpr (std::is_same_v<V, E::Member>) {
            return evalMember(v, e.loc);

        } else if constexpr (std::is_same_v<V, E::InitList>) {
            return evalInitList(v, e.type, e.loc);

        } else if constexpr (std::is_same_v<V, E::Lambda>) {
            // Represent closure as a tagged Struct holding capture values.
            // Field names come from the closure struct definition (in tu_).
            Value::Struct sv;
            sv.typeName = std::string(v.closureType);
            // Find the closure struct definition to get field names.
            const StructDecl* sd = nullptr;
            for (const auto& s : tu_.structs)
                if (s.name == v.closureType) { sd = &s; break; }
            for (std::size_t i = 0; i < v.captureInits.size(); ++i) {
                std::string fieldName = sd && i < sd->fields.size()
                    ? std::string(sd->fields[i].name)
                    : std::to_string(i);
                sv.fields[fieldName] =
                    makeCell(v.captureInits[i] ? evalExpr(*v.captureInits[i]) : Value{});
            }
            Value out; out.data = std::move(sv); return out;

        } else if constexpr (std::is_same_v<V, E::Index>) {
            error(e.loc, "IvyInterpret: array index not yet supported"); return Value{};
        } else if constexpr (std::is_same_v<V, E::New>) {
            error(e.loc, "IvyInterpret: 'new' not yet supported"); return Value{};
        } else if constexpr (std::is_same_v<V, E::Delete>) {
            error(e.loc, "IvyInterpret: 'delete' not yet supported"); return Value{};
        } else {
            error(e.loc, "IvyInterpret: unsupported expression"); return Value{};
        }
    }, e.node);
}

// ============================================================
// Binary
// ============================================================

Value Interpreter::evalBinary(const hir::Expr::Binary& b, SourceLoc loc) {
    // Short-circuit logical.
    if (b.op == "&&") {
        Value lv = evalExpr(*b.lhs);
        if (lv.isInt() && lv.asInt() == 0) return Value{0LL};
        Value rv = evalExpr(*b.rhs);
        return Value{(rv.isInt() ? rv.asInt() != 0 : rv.asFloat() != 0.0) ? 1LL : 0LL};
    }
    if (b.op == "||") {
        Value lv = evalExpr(*b.lhs);
        if (lv.isInt() && lv.asInt() != 0) return Value{1LL};
        Value rv = evalExpr(*b.rhs);
        return Value{(rv.isInt() ? rv.asInt() != 0 : rv.asFloat() != 0.0) ? 1LL : 0LL};
    }

    Value lv = evalExpr(*b.lhs);
    Value rv = evalExpr(*b.rhs);
    const auto& op = b.op;

    if (lv.isInt() && rv.isInt()) {
        long long l = lv.asInt(), r = rv.asInt();
        if (op == "+")  return Value{l + r};
        if (op == "-")  return Value{l - r};
        if (op == "*")  return Value{l * r};
        if (op == "/")  { if (!r) { error(loc, "division by zero"); return Value{0LL}; } return Value{l / r}; }
        if (op == "%")  { if (!r) { error(loc, "modulo by zero");   return Value{0LL}; } return Value{l % r}; }
        if (op == "==") return Value{l == r ? 1LL : 0LL};
        if (op == "!=") return Value{l != r ? 1LL : 0LL};
        if (op == "<")  return Value{l <  r ? 1LL : 0LL};
        if (op == "<=") return Value{l <= r ? 1LL : 0LL};
        if (op == ">")  return Value{l >  r ? 1LL : 0LL};
        if (op == ">=") return Value{l >= r ? 1LL : 0LL};
        if (op == "&")  return Value{l & r};
        if (op == "|")  return Value{l | r};
        if (op == "^")  return Value{l ^ r};
        if (op == "<<") return Value{l << r};
        if (op == ">>") return Value{l >> r};
    }
    if (lv.isFloat() || rv.isFloat()) {
        double l = lv.isFloat() ? lv.asFloat() : static_cast<double>(lv.asInt());
        double r = rv.isFloat() ? rv.asFloat() : static_cast<double>(rv.asInt());
        if (op == "+")  return Value{l + r};
        if (op == "-")  return Value{l - r};
        if (op == "*")  return Value{l * r};
        if (op == "/")  return Value{l / r};
        if (op == "==") return Value{l == r ? 1LL : 0LL};
        if (op == "!=") return Value{l != r ? 1LL : 0LL};
        if (op == "<")  return Value{l <  r ? 1LL : 0LL};
        if (op == "<=") return Value{l <= r ? 1LL : 0LL};
        if (op == ">")  return Value{l >  r ? 1LL : 0LL};
        if (op == ">=") return Value{l >= r ? 1LL : 0LL};
    }
    if (lv.isStr() && rv.isStr() && op == "+") return Value{lv.asStr() + rv.asStr()};

    // Pointer comparisons (== / != with null or another pointer).
    if (lv.isPtr() || rv.isPtr()) {
        bool lnull = lv.isPtr() ? !lv.asPtr() : lv.isInt() ? lv.asInt() == 0 : false;
        bool rnull = rv.isPtr() ? !rv.asPtr() : rv.isInt() ? rv.asInt() == 0 : false;
        if (op == "==") return Value{(lnull == rnull) ? 1LL : 0LL};
        if (op == "!=") return Value{(lnull != rnull) ? 1LL : 0LL};
    }

    error(loc, "IvyInterpret: unsupported binary op '" + std::string(op) + "'");
    return Value{};
}

// ============================================================
// Unary
// ============================================================

Value Interpreter::evalUnary(const hir::Expr::Unary& u, SourceLoc loc) {
    const auto& op = u.op;

    if (op == "&") {
        // Special case: &lambda → return Ptr to a cell holding the closure struct.
        if (std::holds_alternative<hir::Expr::Lambda>(u.operand->node)) {
            Value v = evalExpr(*u.operand);
            Cell c = makeCell(std::move(v));
            Value out; out.data = Value::Ptr{c}; return out;
        }
        Cell c = lvalueCell(*u.operand);
        if (!c) { error(loc, "cannot take address"); return Value{}; }
        Value out; out.data = Value::Ptr{c}; return out;
    }
    if (op == "*") {
        Value v = evalExpr(*u.operand);
        if (!v.isPtr()) { error(loc, "dereference of non-pointer"); return Value{}; }
        if (!v.asPtr()) { error(loc, "null pointer dereference");   return Value{}; }
        return *v.asPtr();
    }

    // Prefix ++/--.
    if (u.isPrefix && (op == "++" || op == "--")) {
        Cell c = lvalueCell(*u.operand);
        if (c && c->isInt()) {
            c->data = Value::Int{c->asInt() + (op == "++" ? 1LL : -1LL)};
            return *c;
        }
    }
    // Postfix ++/--.
    if (!u.isPrefix && (op == "++" || op == "--")) {
        Cell c = lvalueCell(*u.operand);
        if (c && c->isInt()) {
            Value old = *c;
            c->data = Value::Int{c->asInt() + (op == "++" ? 1LL : -1LL)};
            return old;
        }
    }

    Value v = evalExpr(*u.operand);
    if (op == "-") {
        if (v.isInt())   return Value{-v.asInt()};
        if (v.isFloat()) return Value{-v.asFloat()};
    }
    if (op == "!" || op == "not") {
        long long bv = v.isInt() ? v.asInt() : (v.isFloat() ? (v.asFloat() != 0.0 ? 1LL : 0LL) : 0LL);
        return Value{bv == 0 ? 1LL : 0LL};
    }
    if (op == "~" && v.isInt()) return Value{~v.asInt()};
    if (op == "+")              return v;

    error(loc, "IvyInterpret: unsupported unary op '" + std::string(op) + "'");
    return Value{};
}

// ============================================================
// Assignment
// ============================================================

Value Interpreter::evalAssign(const hir::Expr::Assign& a, SourceLoc loc) {
    Cell c = lvalueCell(*a.lhs);
    if (!c) { error(loc, "IvyInterpret: cannot assign to expression"); return Value{}; }

    // Write-through references.
    Cell target = (c->isPtr() && c->asPtr()) ? c->asPtr() : c;

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
        else { error(loc, "IvyInterpret: unsupported compound op '" + std::string(op) + "'"); return Value{}; }
        lv.data = Value::Int{res};
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
        else { error(loc, "IvyInterpret: unsupported compound float op"); return Value{}; }
        lv.data = Value::Float{res};
        return lv;
    }

    error(loc, "IvyInterpret: unsupported compound assignment");
    return Value{};
}

// ============================================================
// lvalue → Cell
// ============================================================

Value Interpreter::defaultStructValue(std::string_view structName) {
    const StructDecl* sd = nullptr;
    for (const auto& s : tu_.structs)
        if (s.name == structName) { sd = &s; break; }
    if (!sd) return Value{};

    Value::Struct sv;
    sv.typeName = std::string(structName);
    for (const auto& f : sd->fields) {
        Value fv;
        // Recursively default-construct nested struct fields.
        if (f.type.pointerDepth == 0) {
            for (const auto& s : tu_.structs)
                if (s.name == f.type.base) { fv = defaultStructValue(f.type.base); break; }
        } else {
            // Pointer fields default to null.
            Value pv; pv.data = Value::Ptr{nullptr};
            fv = std::move(pv);
        }
        sv.fields[std::string(f.name)] = makeCell(std::move(fv));
    }
    Value out; out.data = std::move(sv); return out;
}

Cell Interpreter::lvalueCell(const hir::Expr& e) {
    using E = hir::Expr;
    if (const auto* id = std::get_if<E::IdentRef>(&e.node)) {
        Cell c = lookupCell(id->name);
        // If cell IS itself a reference (Ptr), return the pointed-to cell.
        if (c && c->isPtr() && c->asPtr()) return c->asPtr();
        return c;
    }
    if (const auto* u = std::get_if<E::Unary>(&e.node)) {
        if (u->op == "*") {
            Value v = evalExpr(*u->operand);
            if (v.isPtr() && v.asPtr()) return v.asPtr();
        }
    }
    if (const auto* m = std::get_if<E::Member>(&e.node)) {
        // Recursively get the base cell, then look up the field.
        Cell base = lvalueCell(*m->base);
        if (!base) {
            // Maybe base is an rvalue (e.g. arrow on a pointer).  Evaluate
            // the base expression and look up the field on the result.
            Value bv = evalExpr(*m->base);
            if (m->isArrow) {
                if (!bv.isPtr() || !bv.asPtr()) return nullptr;
                bv = *bv.asPtr();
            }
            if (!bv.isStruct()) return nullptr;
            auto it = bv.asStruct().fields.find(std::string(m->name));
            return it != bv.asStruct().fields.end() ? it->second : nullptr;
        }
        // If the base is a pointer (arrow), deref first.
        Value bv = *base;
        if (m->isArrow) {
            if (!bv.isPtr() || !bv.asPtr()) return nullptr;
            bv = *bv.asPtr();
        }
        if (!bv.isStruct()) return nullptr;
        auto it = bv.asStruct().fields.find(std::string(m->name));
        return it != bv.asStruct().fields.end() ? it->second : nullptr;
    }
    return nullptr;
}

// ============================================================
// Member access
// ============================================================

Value Interpreter::evalMember(const hir::Expr::Member& m, SourceLoc loc) {
    Value base = evalExpr(*m.base);
    if (m.isArrow) {
        if (!base.isPtr() || !base.asPtr()) {
            error(loc, "IvyInterpret: arrow on null/non-pointer"); return Value{};
        }
        base = *base.asPtr();
    }
    if (!base.isStruct()) {
        error(loc, "IvyInterpret: member access on non-struct"); return Value{};
    }
    auto& fields = base.asStruct().fields;
    auto it = fields.find(std::string(m.name));
    if (it == fields.end()) {
        error(loc, "IvyInterpret: no field '" + std::string(m.name) + "'"); return Value{};
    }
    return it->second ? *it->second : Value{};
}

// ============================================================
// InitList
// ============================================================

Value Interpreter::evalInitList(const hir::Expr::InitList& il,
                                 const hir::Type& ty, SourceLoc loc) {
    (void)loc;  // source location not yet used for error reporting here.
    const StructDecl* sd = nullptr;
    for (const auto& s : tu_.structs)
        if (s.name == ty.base) { sd = &s; break; }

    if (!sd) {
        if (!il.elements.empty() && il.elements[0]) return evalExpr(*il.elements[0]);
        return Value{};
    }

    Value::Struct sv;
    sv.typeName = std::string(ty.base);
    for (std::size_t i = 0; i < sd->fields.size(); ++i) {
        Value fv;
        if (i < il.elements.size() && il.elements[i]) {
            fv = evalExpr(*il.elements[i]);
        } else {
            // Missing initializer: default-construct the field.
            const hir::Type& ft = sd->fields[i].type;
            if (ft.pointerDepth == 0) {
                bool nestedStruct = false;
                for (const auto& s : tu_.structs)
                    if (s.name == ft.base) { fv = defaultStructValue(ft.base); nestedStruct = true; break; }
                if (!nestedStruct) fv = Value{0LL};  // zero for int/float
            } else {
                Value pv; pv.data = Value::Ptr{nullptr};
                fv = std::move(pv);
            }
        }
        sv.fields[std::string(sd->fields[i].name)] = makeCell(std::move(fv));
    }
    Value out; out.data = std::move(sv); return out;
}

// ============================================================
// Function call
// ============================================================

Value Interpreter::evalCall(const hir::Expr::Call& c, SourceLoc loc) {
    std::vector<Value> args;
    args.reserve(c.args.size());
    for (const auto& a : c.args) args.push_back(evalExpr(*a));

    if (isBuiltin(c.callee)) return callBuiltin(c.callee, args, loc);

    if (c.target && c.target->body)
        return execFn(*c.target, std::move(args));

    return call(c.callee, std::move(args));
}

// ============================================================
// Built-ins (printf, putchar, puts, exit, abort)
// ============================================================

bool Interpreter::isBuiltin(std::string_view name) const {
    return name == "printf" || name == "puts"  || name == "putchar" ||
           name == "exit"   || name == "abort" || name == "malloc"  || name == "free";
}

Value Interpreter::callBuiltin(std::string_view name,
                                const std::vector<Value>& args,
                                SourceLoc loc) {
    (void)loc;  // source location not yet used for built-in calls.
    if (name == "puts") {
        std::string s = args.empty() ? "" : (args[0].isStr() ? args[0].asStr() : args[0].toString());
        *out_ << s << "\n";
        return Value{static_cast<long long>(s.size() + 1)};
    }
    if (name == "putchar") {
        char c = args.empty() ? 0 : static_cast<char>(args[0].isInt() ? args[0].asInt() : 0);
        *out_ << c;
        return args.empty() ? Value{0LL} : args[0];
    }
    if (name == "printf") {
        if (args.empty()) return Value{0LL};
        std::string fmt = args[0].isStr() ? args[0].asStr() : args[0].toString();
        std::string out;
        std::size_t ai = 1;
        for (std::size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%') { out += fmt[i]; continue; }
            ++i; if (i >= fmt.size()) break;
            // Skip flags / width / precision / length modifiers.
            while (i < fmt.size() && (std::string("-+ #0").find(fmt[i]) != std::string::npos ||
                                      (fmt[i] >= '0' && fmt[i] <= '9') ||
                                      fmt[i] == '.' || fmt[i] == 'l' || fmt[i] == 'h' ||
                                      fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't')) ++i;
            if (i >= fmt.size()) break;
            char spec = fmt[i];
            if (spec == '%') { out += '%'; continue; }
            Value arg = (ai < args.size()) ? args[ai++] : Value{};
            char buf[64] = {};
            switch (spec) {
                case 'd': case 'i':
                    std::snprintf(buf, sizeof(buf), "%lld",
                        arg.isInt() ? arg.asInt()
                        : arg.isFloat() ? static_cast<long long>(arg.asFloat())
                        : 0LL);
                    out += buf; break;
                case 'u':
                    std::snprintf(buf, sizeof(buf), "%llu",
                        static_cast<unsigned long long>(
                            arg.isInt() ? arg.asInt() : arg.isFloat() ? static_cast<long long>(arg.asFloat()) : 0LL));
                    out += buf; break;
                case 'f':
                    std::snprintf(buf, sizeof(buf), "%f",
                        arg.isFloat() ? arg.asFloat() : static_cast<double>(
                            arg.isInt() ? arg.asInt() : 0));
                    out += buf; break;
                case 'g':
                    std::snprintf(buf, sizeof(buf), "%g",
                        arg.isFloat() ? arg.asFloat() : static_cast<double>(
                            arg.isInt() ? arg.asInt() : 0));
                    out += buf; break;
                case 'e':
                    std::snprintf(buf, sizeof(buf), "%e",
                        arg.isFloat() ? arg.asFloat() : static_cast<double>(
                            arg.isInt() ? arg.asInt() : 0));
                    out += buf; break;
                case 's':
                    out += arg.isStr() ? arg.asStr() : arg.toString(); break;
                case 'c':
                    out += static_cast<char>(arg.isInt() ? arg.asInt() : 0); break;
                case 'p':
                    out += "(ptr)"; break;
                default:
                    out += '%'; out += spec; break;
            }
        }
        *out_ << out;
        return Value{static_cast<long long>(out.size())};
    }
    if (name == "exit") {
        std::exit(static_cast<int>((!args.empty() && args[0].isInt()) ? args[0].asInt() : 0));
    }
    if (name == "abort") { std::abort(); }
    // malloc/free: stub
    return Value{};
}

}  // namespace interp
}  // namespace ivy
