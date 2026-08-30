#include "mir/mir_builder.h"

#include <string>

namespace ivy {
namespace {

// Check if a type base name is a numeric type (integer or float).
bool isNumericM(const mir::Type& t) {
    if (t.pointerDepth > 0 || t.base == "void" || t.base == "nullptr") return false;
    if (t.arraySize > 0) return false;
    static const std::string_view intBases[] = {
        "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t",
        "uint32_t","uint64_t","size_t","ptrdiff_t","int","long",
        "long long","short","unsigned","char","unsigned char",
        "signed char","bool"
    };
    static const std::string_view floatBases[] = {
        "float16_t","float32_t","float64_t","float128_t","bfloat16_t",
        "float","double","long double"
    };
    for (auto b : intBases) if (t.base == b) return true;
    for (auto b : floatBases) if (t.base == b) return true;
    return false;
}

// Merges the lifetimes of the two branches of a ternary expression.
mir::Lifetime mergeLifetime(const mir::Lifetime& a, const mir::Lifetime& b) {
    if (a.kind == mir::Lifetime::Kind::Named && b.kind == mir::Lifetime::Kind::Named &&
        a.name == b.name)
        return a;
    if (a.kind == mir::Lifetime::Kind::Local || b.kind == mir::Lifetime::Kind::Local)
        return mir::Lifetime{mir::Lifetime::Kind::Local, {}};
    if (a.kind == b.kind) return a;
    return mir::Lifetime{mir::Lifetime::Kind::Unknown, {}};
}

// Decode escape sequences in a string/char literal body (without quotes).
// Copied from codegen.cpp — shared logic for literal decoding.
bool decodeBody(std::string_view body, std::string& bytes) {
    bytes.clear();
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c != '\\') {
            bytes.push_back(c);
            continue;
        }
        if (++i >= body.size()) return false;
        switch (body[i]) {
            case 'n': bytes.push_back('\n'); break;
            case 't': bytes.push_back('\t'); break;
            case 'r': bytes.push_back('\r'); break;
            case 'a': bytes.push_back('\a'); break;
            case 'b': bytes.push_back('\b'); break;
            case 'f': bytes.push_back('\f'); break;
            case 'v': bytes.push_back('\v'); break;
            case '\\': bytes.push_back('\\'); break;
            case '\'': bytes.push_back('\''); break;
            case '"': bytes.push_back('"'); break;
            case '?': bytes.push_back('?'); break;
            case 'x': {
                long long v = 0;
                int count = 0;
                while (++i < body.size() && count < 2) {
                    char h = body[i];
                    if (h >= '0' && h <= '9') v = v * 16 + (h - '0');
                    else if (h >= 'a' && h <= 'f') v = v * 16 + (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v = v * 16 + (h - 'A' + 10);
                    else { --i; break; }
                    ++count;
                }
                bytes.push_back(static_cast<char>(v));
                break;
            }
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                long long v = body[i] - '0';
                int count = 1;
                while (++i < body.size() && count < 3) {
                    char o = body[i];
                    if (o >= '0' && o <= '7') { v = v * 8 + (o - '0'); ++count; }
                    else { --i; break; }
                }
                bytes.push_back(static_cast<char>(v));
                break;
            }
            default:
                bytes.push_back(body[i]);
                break;
        }
    }
    return true;
}

bool decodeString(std::string_view raw, std::string& bytes) {
    if (raw.size() >= 3 && raw[0] == 'R' && raw[1] == '"') {
        std::size_t open = raw.find('(');
        std::size_t close = raw.rfind(')');
        if (open == std::string_view::npos || close == std::string_view::npos) return false;
        bytes = std::string(raw.substr(open + 1, close - open - 1));
        return true;
    }
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return false;
    return decodeBody(raw.substr(1, raw.size() - 2), bytes);
}

bool decodeChar(std::string_view raw, long long& value) {
    if (raw.size() < 2 || raw.front() != '\'' || raw.back() != '\'') return false;
    std::string bytes;
    if (!decodeBody(raw.substr(1, raw.size() - 2), bytes)) return false;
    if (bytes.empty()) return false;
    value = static_cast<unsigned char>(bytes[0]);
    return true;
}

}  // namespace

MirBuilder::MirBuilder(const hir::TranslationUnit& hir) : hir_(hir) {}

void MirBuilder::error(SourceLoc loc, std::string message) {
    diagnostics_.push_back(Diagnostic{loc.line, loc.col, std::move(message)});
    failed_ = true;
}

mir::Block* MirBuilder::newBlock() {
    auto b = std::make_unique<mir::Block>();
    mir::Block* raw = b.get();
    current_->blocks.push_back(std::move(b));
    return raw;
}

mir::Inst* MirBuilder::emit(mir::Inst::Kind kind, SourceLoc loc) {
    auto inst = std::make_unique<mir::Inst>();
    inst->kind = kind;
    inst->inUnsafe = unsafeDepth_ > 0;
    inst->loc = loc;
    mir::Inst* raw = inst.get();
    cur_->insts.push_back(std::move(inst));
    return raw;
}

void MirBuilder::jumpTo(mir::Block* target) {
    // A block that already ends in a return must not get a trailing jump
    // (the jump would be emitted after the ret, which is invalid).
    if (!cur_->insts.empty() && cur_->insts.back()->kind == mir::Inst::Kind::Ret) {
        return;
    }
    auto* inst = emit(mir::Inst::Kind::Jump, {0, 0});
    inst->node = mir::Inst::Jump{target};
}

mir::Lifetime MirBuilder::lookup(std::string_view name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        const auto hit = it->find(name);
        if (hit != it->end()) return hit->second;
    }
    const auto p = params_.find(name);
    if (p != params_.end()) return p->second;
    return {};  // kind None; error already reported by the HIR
}

void MirBuilder::declare(std::string_view name, mir::Lifetime lt) {
    scopes_.back().emplace(name, lt);
}

// --- lifetime checker ---

void MirBuilder::checkReturn(const mir::Function& fn, const mir::Lifetime& lt, SourceLoc loc) {
    if (fn.returnType.pointerDepth == 0) return;
    if (fn.returnLifetime.empty()) return;  // declaration only, e.g. extern "C"
    if (unsafeDepth_ > 0) return;           // [[ivy::unsafe]] opts out
    switch (lt.kind) {
        case mir::Lifetime::Kind::Named:
            if (lt.name != fn.returnLifetime) {
                error(loc, "returned pointer has lifetime '" + std::string(lt.name) +
                               "' but the function declares [[ivy::lt_ret(" +
                               std::string(fn.returnLifetime) + ")]]");
            }
            return;
        case mir::Lifetime::Kind::Static:  // string literal: valid forever
        case mir::Lifetime::Kind::None:    // nullptr
            return;
        case mir::Lifetime::Kind::Local:
            error(loc, "returned pointer to a local variable (dangling)");
            return;
        case mir::Lifetime::Kind::Unknown:
            error(loc, "returned pointer has no known lifetime; tie it to a parameter with "
                       "[[ivy::lt(...)]] or return nullptr");
            return;
    }
}

// --- store lifetime checker: catch dangling pointer stores ---

void MirBuilder::checkStore(const mir::Expr& target, const mir::Expr& value, SourceLoc loc) {
    if (unsafeDepth_ > 0) return;  // [[ivy::unsafe]] opts out
    // Only check pointer-typed stores.
    if (target.type.pointerDepth == 0) return;
    // Reject storing a Local (dangling) pointer into a named-lifetime slot.
    if (value.lifetime.kind == mir::Lifetime::Kind::Local) {
        error(loc, "storing a pointer to a local variable into a pointer slot (dangling)");
    }
}

// --- expressions ---

std::unique_ptr<mir::Expr> MirBuilder::buildExpr(const hir::Expr& e) {
    auto out = std::make_unique<mir::Expr>();
    out->loc = e.loc;
    out->type = e.type;
    const auto& n = e.node;
    using H = hir::Expr;

    if (std::holds_alternative<H::IntegerLit>(n)) {
        out->node = mir::Expr::IntegerLit{std::get<H::IntegerLit>(n).value};
        return out;  // lifetime: None (default)
    }
    if (std::holds_alternative<H::FloatLit>(n)) {
        out->node = mir::Expr::FloatLit{std::get<H::FloatLit>(n).value};
        return out;
    }
    if (std::holds_alternative<H::StringLit>(n)) {
        out->node = mir::Expr::StringLit{std::get<H::StringLit>(n).raw};
        out->lifetime.kind = mir::Lifetime::Kind::Static;
        return out;
    }
    if (std::holds_alternative<H::CharLit>(n)) {
        out->node = mir::Expr::CharLit{std::get<H::CharLit>(n).raw};
        return out;
    }
    if (std::holds_alternative<H::BoolLit>(n)) {
        out->node = mir::Expr::BoolLit{std::get<H::BoolLit>(n).value};
        return out;
    }
    if (std::holds_alternative<H::NullptrLit>(n)) {
        out->node = mir::Expr::NullptrLit{};
        return out;
    }
    if (std::holds_alternative<H::This>(n)) {
        // `this` — lowered as an IdentRef to the "this" parameter.
        out->node = mir::Expr::This{};
        return out;
    }
    if (std::holds_alternative<H::IdentRef>(n)) {
        const std::string_view name = std::get<H::IdentRef>(n).name;
        out->node = mir::Expr::IdentRef{name};
        out->lifetime = lookup(name);
        return out;
    }
    if (std::holds_alternative<H::Unary>(n)) {
        const H::Unary& v = std::get<H::Unary>(n);
        auto& un = out->node.emplace<mir::Expr::Unary>();
        un.op = v.op;
        un.isPrefix = v.isPrefix;
        un.operand = buildExpr(*v.operand);
        if (!un.operand) return out;
        if (v.op == "&") {
            // Address of a variable: the storage must outlive the function.
            if (std::holds_alternative<mir::Expr::IdentRef>(un.operand->node)) {
                const std::string_view name =
                    std::get<mir::Expr::IdentRef>(un.operand->node).name;
                bool local = false;
                for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
                    if (it->contains(name)) {
                        local = true;
                        break;
                    }
                }
                if (local) {
                    out->lifetime.kind = mir::Lifetime::Kind::Local;
                } else {
                    out->lifetime = params_.contains(name) ? params_.at(name)
                                                           : mir::Lifetime{};
                }
            } else {
                out->lifetime.kind = mir::Lifetime::Kind::Unknown;
            }
            return out;
        }
        if (v.op == "*") {
            out->lifetime = un.operand->lifetime;  // the pointee borrows the pointer's lifetime
            return out;
        }
        if (v.op == "++" || v.op == "--") {
            out->lifetime = un.operand->type.pointerDepth > 0 ? un.operand->lifetime
                                                              : mir::Lifetime{};
            return out;
        }
        return out;  // - + ! ~ : None
    }
    if (std::holds_alternative<H::Binary>(n)) {
        const H::Binary& v = std::get<H::Binary>(n);
        auto& bin = out->node.emplace<mir::Expr::Binary>();
        bin.op = v.op;
        bin.lhs = buildExpr(*v.lhs);
        bin.rhs = buildExpr(*v.rhs);
        if (!bin.lhs || !bin.rhs) return out;
        if (v.op == "+" || v.op == "-") {
            out->lifetime = bin.lhs->type.pointerDepth > 0 ? bin.lhs->lifetime
                             : bin.rhs->type.pointerDepth > 0 ? bin.rhs->lifetime
                                                              : mir::Lifetime{};
        }
        return out;
    }
    if (std::holds_alternative<H::Ternary>(n)) {
        const H::Ternary& v = std::get<H::Ternary>(n);
        auto& ter = out->node.emplace<mir::Expr::Ternary>();
        ter.cond = buildExpr(*v.cond);
        ter.thenBranch = buildExpr(*v.thenBranch);
        ter.elseBranch = buildExpr(*v.elseBranch);
        if (ter.thenBranch && ter.elseBranch) {
            out->lifetime = mergeLifetime(ter.thenBranch->lifetime, ter.elseBranch->lifetime);
        }
        return out;
    }
    if (std::holds_alternative<H::Call>(n)) {
        const H::Call& v = std::get<H::Call>(n);
        auto& call = out->node.emplace<mir::Expr::Call>();
        call.callee = v.callee;
        call.isVirtual = v.isVirtual;        // 7.7
        call.vtableSlot = v.vtableSlot;      // 7.7
        call.methodName = v.methodName;      // 7.7
        call.returnLifetime = v.target ? v.target->returnLifetime : std::string_view{};
        for (const auto& a : v.args) {
            if (a) call.args.push_back(buildExpr(*a));
            else call.args.push_back(nullptr);
        }
        if (e.type.pointerDepth > 0) {
            if (!call.returnLifetime.empty()) {
                out->lifetime.kind = mir::Lifetime::Kind::Named;
                out->lifetime.name = call.returnLifetime;
            } else {
                out->lifetime.kind = mir::Lifetime::Kind::Unknown;  // opaque, e.g. malloc
            }
        }
        out->lifetime.kind = mir::Lifetime::Kind::Unknown;  // virtual: unknown
        return out;
    }
    if (std::holds_alternative<H::Index>(n)) {
        const H::Index& v = std::get<H::Index>(n);
        auto& idx = out->node.emplace<mir::Expr::Index>();
        idx.base = buildExpr(*v.base);
        idx.index = buildExpr(*v.index);
        if (idx.base) out->lifetime = idx.base->lifetime;
        return out;
    }
    if (std::holds_alternative<H::Member>(n)) {
        const H::Member& v = std::get<H::Member>(n);
        auto& mem = out->node.emplace<mir::Expr::Member>();
        mem.base = buildExpr(*v.base);
        mem.name = v.name;
        mem.isArrow = v.isArrow;
        mem.isScope = v.isScope;
        // Member access inherits the lifetime of the base (if it's a
        // pointer field, it borrows the base's lifetime).
        if (mem.base) out->lifetime = mem.base->lifetime;
        return out;
    }
    if (std::holds_alternative<H::Assign>(n)) {
        const H::Assign& v = std::get<H::Assign>(n);
        auto& as = out->node.emplace<mir::Expr::Assign>();
        as.op = v.op;
        as.lhs = buildExpr(*v.lhs);
        as.rhs = buildExpr(*v.rhs);
        if (as.rhs) out->lifetime = as.rhs->lifetime;
        return out;
    }
    if (std::holds_alternative<H::New>(n)) {
        const H::New& v = std::get<H::New>(n);
        auto& nw = out->node.emplace<mir::Expr::New>();
        nw.type = v.type;
        for (const auto& a : v.args) nw.args.push_back(buildExpr(*a));
        out->lifetime.kind = mir::Lifetime::Kind::Unknown;
        return out;
    }
    if (std::holds_alternative<H::Delete>(n)) {
        const H::Delete& v = std::get<H::Delete>(n);
        auto& dl = out->node.emplace<mir::Expr::Delete>();
        dl.isArray = v.isArray;
        dl.operand = buildExpr(*v.operand);
        return out;
    }
    if (std::holds_alternative<H::InitList>(n)) {
        const H::InitList& v = std::get<H::InitList>(n);
        auto& il = out->node.emplace<mir::Expr::InitList>();
        for (const auto& el : v.elements) {
            if (el) il.elements.push_back(buildExpr(*el));
            else il.elements.push_back(nullptr);
        }
        return out;
    }
    if (std::holds_alternative<H::Lambda>(n)) {
        const H::Lambda& v = std::get<H::Lambda>(n);
        auto& lam = out->node.emplace<mir::Expr::Lambda>();
        lam.funcName = v.funcName;
        lam.closureType = v.closureType;
        for (const auto& ci : v.captureInits) {
            if (ci) lam.captureInits.push_back(buildExpr(*ci));
            else lam.captureInits.push_back(nullptr);
        }
        // A lambda value is a closure struct — it lives as long as the
        // enclosing function (it's a local aggregate). No pointer lifetime.
        return out;
    }

    out->lifetime.kind = mir::Lifetime::Kind::Unknown;
    return out;
}

// --- statements (CFG construction) ---

void MirBuilder::buildStmt(const hir::Stmt& s) {
    const auto& n = s.node;
    using H = hir::Stmt;

    if (std::holds_alternative<H::Compound>(n)) {
        for (const auto& st : std::get<H::Compound>(n).stmts) buildStmt(*st);
        return;
    }
    if (std::holds_alternative<H::Null>(n)) {
        return;
    }
    if (std::holds_alternative<H::Decl>(n)) {
        const H::Decl& d = std::get<H::Decl>(n);
        auto* inst = emit(mir::Inst::Kind::Alloca, s.loc);
        auto& a = inst->node.emplace<mir::Inst::Alloca>();
        a.var = d.name;
        a.type = d.type;
        if (d.init) {
            a.init = buildExpr(*d.init);
            declare(d.name, a.init ? a.init->lifetime : mir::Lifetime{});
        } else {
            declare(d.name, {});
        }
        return;
    }
    if (std::holds_alternative<H::If>(n)) {
        const H::If& v = std::get<H::If>(n);
        mir::Block* cont = newBlock();
        // For `if constexpr` that folded to a constant, one branch may
        // be null.  If both are null (e.g. `if constexpr (false)` with
        // no else), the If is a no-op — just fall through to cont.
        if (!v.thenBranch && !v.elseBranch) {
            jumpTo(cont);
            cur_ = cont;
            return;
        }
        mir::Block* thenB = v.thenBranch ? newBlock() : cont;
        mir::Block* elseB = v.elseBranch ? newBlock() : cont;

        auto* inst = emit(mir::Inst::Kind::CondBranch, s.loc);
        auto& cb = inst->node.emplace<mir::Inst::CondBranch>();
        cb.cond = buildExpr(*v.cond);
        cb.thenBlock = thenB;
        cb.elseBlock = elseB;

        if (v.thenBranch) {
            cur_ = thenB;
            buildStmt(*v.thenBranch);
            jumpTo(cont);
        }
        if (v.elseBranch) {
            cur_ = elseB;
            buildStmt(*v.elseBranch);
            jumpTo(cont);
        }
        cur_ = cont;
        return;
    }
    if (std::holds_alternative<H::While>(n)) {
        const H::While& v = std::get<H::While>(n);
        mir::Block* condB = newBlock();
        mir::Block* bodyB = newBlock();
        mir::Block* exitB = newBlock();
        jumpTo(condB);

        cur_ = condB;
        auto* inst = emit(mir::Inst::Kind::CondBranch, s.loc);
        auto& cb = inst->node.emplace<mir::Inst::CondBranch>();
        cb.cond = buildExpr(*v.cond);
        cb.thenBlock = bodyB;
        cb.elseBlock = exitB;

        cur_ = bodyB;
        loops_.push_back(LoopCtx{condB, nullptr, exitB});
        buildStmt(*v.body);
        loops_.pop_back();
        jumpTo(condB);
        cur_ = exitB;
        return;
    }
    if (std::holds_alternative<H::DoWhile>(n)) {
        const H::DoWhile& v = std::get<H::DoWhile>(n);
        mir::Block* bodyB = newBlock();
        mir::Block* condB = newBlock();
        mir::Block* exitB = newBlock();
        jumpTo(bodyB);

        cur_ = bodyB;
        loops_.push_back(LoopCtx{condB, nullptr, exitB});
        buildStmt(*v.body);
        loops_.pop_back();
        jumpTo(condB);

        cur_ = condB;
        auto* inst = emit(mir::Inst::Kind::CondBranch, s.loc);
        auto& cb = inst->node.emplace<mir::Inst::CondBranch>();
        cb.cond = buildExpr(*v.cond);
        cb.thenBlock = bodyB;
        cb.elseBlock = exitB;
        cur_ = exitB;
        return;
    }
    if (std::holds_alternative<H::For>(n)) {
        const H::For& v = std::get<H::For>(n);
        scopes_.push_back({});
        if (v.init) buildStmt(*v.init);  // decl lives in the current (pre-cond) block

        mir::Block* condB = newBlock();
        mir::Block* bodyB = newBlock();
        mir::Block* incrB = newBlock();
        mir::Block* exitB = newBlock();
        jumpTo(condB);

        cur_ = condB;
        if (v.cond) {
            auto* inst = emit(mir::Inst::Kind::CondBranch, s.loc);
            auto& cb = inst->node.emplace<mir::Inst::CondBranch>();
            cb.cond = buildExpr(*v.cond);
            cb.thenBlock = bodyB;
            cb.elseBlock = exitB;
        } else {
            jumpTo(bodyB);
        }

        cur_ = bodyB;
        loops_.push_back(LoopCtx{condB, incrB, exitB});
        buildStmt(*v.body);
        loops_.pop_back();
        jumpTo(incrB);

        cur_ = incrB;
        if (v.incr) {
            auto* inst = emit(mir::Inst::Kind::Eval, s.loc);
            inst->node.emplace<mir::Inst::Eval>().value = buildExpr(*v.incr);
        }
        jumpTo(condB);
        cur_ = exitB;
        scopes_.pop_back();
        return;
    }
    if (std::holds_alternative<H::Return>(n)) {
        const H::Return& v = std::get<H::Return>(n);
        auto* inst = emit(mir::Inst::Kind::Ret, s.loc);
        auto& r = inst->node.emplace<mir::Inst::Ret>();
        if (v.value) {
            r.value = buildExpr(*v.value);
            if (r.value) checkReturn(*current_, r.value->lifetime, s.loc);
        }
        return;
    }
    if (std::holds_alternative<H::Break>(n)) {
        if (!loops_.empty()) jumpTo(loops_.back().exit);
        return;
    }
    if (std::holds_alternative<H::Continue>(n)) {
        if (!loops_.empty()) {
            const LoopCtx& l = loops_.back();
            jumpTo(l.incr ? l.incr : l.cond);
        }
        return;
    }
    if (std::holds_alternative<H::ExprStmt>(n)) {
        const H::ExprStmt& v = std::get<H::ExprStmt>(n);
        std::unique_ptr<mir::Expr> e = buildExpr(*v.value);
        if (!e) return;
        if (std::holds_alternative<mir::Expr::Assign>(e->node)) {
            auto* inst = emit(mir::Inst::Kind::Store, s.loc);
            auto& st = inst->node.emplace<mir::Inst::Store>();
            auto& as = std::get<mir::Expr::Assign>(e->node);
            // Expand compound assignment: `lhs op= rhs` → `lhs = lhs op rhs`.
            // This avoids needing a separate op field on Store.
            if (as.op != "=" && as.op.size() >= 2 && as.op.back() == '=') {
                std::string_view plainOp = as.op.substr(0, as.op.size() - 1);
                auto lhsCopy = cloneExpr(*as.lhs);
                // Build a fresh Binary expr: lhs_copy <plainOp> rhs
                auto binExpr = std::make_unique<mir::Expr>();
                binExpr->loc = as.rhs ? as.rhs->loc : s.loc;
                binExpr->type = as.rhs ? as.rhs->type : as.lhs->type;
                auto& bin = binExpr->node.emplace<mir::Expr::Binary>();
                bin.op = plainOp;
                bin.lhs = std::move(lhsCopy);
                bin.rhs = std::move(as.rhs);
                // Recompute lifetime for the new binary expr.
                if (bin.lhs && bin.rhs) {
                    if (bin.lhs->type.pointerDepth > 0)
                        binExpr->lifetime = bin.lhs->lifetime;
                    else if (bin.rhs->type.pointerDepth > 0)
                        binExpr->lifetime = bin.rhs->lifetime;
                }
                as.rhs = std::move(binExpr);
                as.op = "=";
            }
            st.target = std::move(as.lhs);
            st.value = std::move(as.rhs);
            // Track the variable's value lifetime for later reads.
            if (st.target && st.value &&
                std::holds_alternative<mir::Expr::IdentRef>(st.target->node)) {
                const std::string_view name =
                    std::get<mir::Expr::IdentRef>(st.target->node).name;
                for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
                    auto hit = it->find(name);
                    if (hit != it->end()) {
                        hit->second = st.value->lifetime;
                        break;
                    }
                }
            }
            // Safety check: reject dangling pointer stores (outside unsafe).
            if (st.target && st.value) {
                checkStore(*st.target, *st.value, s.loc);
            }
        } else {
            auto* inst = emit(mir::Inst::Kind::Eval, s.loc);
            inst->node.emplace<mir::Inst::Eval>().value = std::move(e);
        }
        return;
    }
    if (std::holds_alternative<H::Unsafe>(n)) {
        ++unsafeDepth_;
        buildStmt(*std::get<H::Unsafe>(n).body);
        --unsafeDepth_;
        return;
    }
    if (std::holds_alternative<H::Switch>(n)) {
        const H::Switch& v = std::get<H::Switch>(n);
        mir::Block* exitB = newBlock();

        // Create one block per case clause.
        std::vector<mir::Block*> caseBlocks;
        caseBlocks.reserve(v.cases.size());
        for (std::size_t i = 0; i < v.cases.size(); ++i)
            caseBlocks.push_back(newBlock());

        // Find default block (or use exitB if no default clause).
        mir::Block* defaultBlock = exitB;
        for (std::size_t i = 0; i < v.cases.size(); ++i) {
            if (!v.cases[i].value) { defaultBlock = caseBlocks[i]; break; }
        }

        // Emit the switch instruction in the current block.
        auto* swInst = emit(mir::Inst::Kind::Switch, s.loc);
        auto& sw = swInst->node.emplace<mir::Inst::Switch>();
        sw.cond = v.cond ? buildExpr(*v.cond) : nullptr;
        sw.defaultBlock = defaultBlock;
        for (std::size_t i = 0; i < v.cases.size(); ++i) {
            if (!v.cases[i].value) continue;  // skip default — handled above
            // Evaluate the case value constant.
            const hir::Expr& cv = *v.cases[i].value;
            long long intVal = 0;
            if (std::holds_alternative<hir::Expr::IntegerLit>(cv.node))
                intVal = std::get<hir::Expr::IntegerLit>(cv.node).value;
            else if (std::holds_alternative<hir::Expr::BoolLit>(cv.node))
                intVal = std::get<hir::Expr::BoolLit>(cv.node).value ? 1 : 0;
            sw.arms.push_back({intVal, caseBlocks[i]});
        }

        // Build each case body. break → jumpTo(exitB).
        for (std::size_t i = 0; i < v.cases.size(); ++i) {
            cur_ = caseBlocks[i];
            loops_.push_back(LoopCtx{nullptr, nullptr, exitB});
            for (const auto& st : v.cases[i].stmts) buildStmt(*st);
            loops_.pop_back();
            // If the block isn't already terminated (break emitted a Jump),
            // add a fall-through jump to exitB (safety net for return/continue).
            jumpTo(exitB);
        }
        cur_ = exitB;
        return;
    }
}

// --- functions ---

void MirBuilder::buildFunction(mir::Function& fn, const hir::Function& hf) {
    current_ = &fn;
    unsafeDepth_ = 0;
    params_.clear();
    scopes_.clear();
    loops_.clear();

    for (const hir::Lifetime& l : hf.lifetimes) {
        fn.lifetimes.push_back(mir::Lifetime{mir::Lifetime::Kind::Named, l.name});
    }
    fn.returnLifetime = hf.returnLifetime;
    for (const hir::Param& hp : hf.params) {
        mir::Lifetime lt;
        if (!hp.lifetime.empty()) {
            lt = mir::Lifetime{mir::Lifetime::Kind::Named, hp.lifetime};
        } else if (hp.type.pointerDepth > 0) {
            lt.kind = mir::Lifetime::Kind::Unknown;
        }
        if (!hp.name.empty()) params_[hp.name] = lt;
    }

    scopes_.push_back({});
    cur_ = newBlock();
    for (const auto& st : hf.body->stmts) buildStmt(*st);
    scopes_.pop_back();
}

std::unique_ptr<mir::TranslationUnit> MirBuilder::build() {
    mir_ = std::make_unique<mir::TranslationUnit>();
    // Populate enum info for codegen type resolution.
    for (const auto& ed : hir_.enums) {
        mir_->enums.push_back({ed.name, ed.namespacePrefix, ed.isScoped,
                               std::string(ed.underlyingType.base)});
    }
    // Populate struct info for codegen type + layout resolution.
    // 7.7: include base class info, polymorphic flag, and vtable entries.
    for (const auto& sd : hir_.structs) {
        mir::StructInfo si;
        si.name = sd.name;
        si.namespacePrefix = sd.namespacePrefix;
        // 7.7: base subobject fields come first (after vptr), so that
        // GEP field indices line up with the LLVM struct layout.
        for (const auto& bc : sd.bases) {
            for (const auto& bs : hir_.structs) {
                if (bs.name != bc.type.base) continue;
                for (const auto& bf : bs.fields)
                    si.fields.push_back({bf.name, bf.type});
                break;
            }
        }
        for (const auto& f : sd.fields) si.fields.push_back({f.name, f.type});
        // Base classes.
        for (const auto& bc : sd.bases) {
            si.bases.push_back({bc.type.base});
        }
        // Polymorphic: check if any function has vtableOf == this struct.
        for (const auto& hf : hir_.functions) {
            if (hf->vtableOf == sd.name) {
                si.isPolymorphic = true;
                break;
            }
            // Also check if any base is polymorphic.
            if (!si.isPolymorphic) {
                for (const auto& bc : sd.bases) {
                    for (const auto& bs : hir_.structs) {
                        if (bs.name == bc.type.base) {
                            // Check if base has virtual methods.
                            // We'll just check functions list.
                        }
                    }
                }
            }
        }
        // Also polymorphic if any base is polymorphic (check recursively).
        // Simple approach: polymorphic if has bases that are polymorphic
        // or has virtual methods. We check functions list for any
        // isVirtual function with name starting with this struct name.
        if (!si.isPolymorphic) {
            for (const auto& hf : hir_.functions) {
                if (hf->isVirtual) {
                    // Check if function belongs to this struct or a base.
                    std::string fnName(hf->name);
                    std::string prefix(sd.name);
                    prefix += "::";
                    if (fnName.substr(0, prefix.size()) == prefix) {
                        si.isPolymorphic = true;
                        break;
                    }
                }
            }
            // Check bases recursively (simple: any base is polymorphic).
            if (!si.isPolymorphic) {
                for (const auto& bc : sd.bases) {
                    for (const auto& bs : mir_->structs) {
                        if (bs.name == bc.type.base && bs.isPolymorphic) {
                            si.isPolymorphic = true;
                            break;
                        }
                    }
                    if (si.isPolymorphic) break;
                }
            }
        }
        // Vtable entries: collect virtual methods in order.
        // For now, populate from function list: functions with
        // isVirtual and name starting with structName::.
        // Override entries from derived take priority.
        if (si.isPolymorphic) {
            // First, collect from bases (in order).
            for (const auto& bc : sd.bases) {
                for (const auto& bs : mir_->structs) {
                    if (bs.name == bc.type.base) {
                        for (const auto& vte : bs.vtable) {
                            si.vtable.push_back(vte);
                        }
                        break;
                    }
                }
            }
            // Then, add/override from this struct's methods.
            for (const auto& hf : hir_.functions) {
                if (!hf->isVirtual) continue;
                std::string fnName(hf->name);
                std::string prefix(sd.name);
                prefix += "::";
                if (fnName.substr(0, prefix.size()) != prefix) continue;
                std::string bareName = fnName.substr(prefix.size());
                // 7.7: destructors share a single vtable slot. Normalize
                // `~ClassName` → `~dtor` so derived `~Dog` overrides base
                // `~Animal` (matches HIR builder normalization).
                if (hf->isDtor) bareName = "~dtor";
                // Check if overrides an existing entry.
                bool overridden = false;
                for (auto& vte : si.vtable) {
                    if (vte.methodName == bareName) {
                        vte.funcName = hf->name;
                        overridden = true;
                        break;
                    }
                }
                if (!overridden) {
                    si.vtable.push_back({bareName, hf->name});
                }
            }
        }
        mir_->structs.push_back(std::move(si));
    }
    for (const auto& hf : hir_.functions) {
        auto fn = std::make_unique<mir::Function>();
        fn->name = hf->name;
        fn->namespacePrefix = hf->namespacePrefix;
        fn->returnType = hf->returnType;
        for (const auto& p : hf->params) {
            fn->params.push_back({p.type, p.name, p.lifetime, p.loc});
        }
        fn->isExternC = hf->isExternC;
        fn->hasBody = hf->body != nullptr;
        fn->isConstexpr = hf->isConstexpr;
        fn->isConsteval = hf->isConsteval;
        fn->isCtor = hf->isCtor;
        fn->isDtor = hf->isDtor;
        fn->isVirtual = hf->isVirtual;
        fn->isPureVirtual = hf->isPureVirtual;
        fn->vtableOf = hf->vtableOf;
        fn->loc = hf->loc;
        mir::Function* raw = fn.get();
        mir_->functions.push_back(std::move(fn));
        if (hf->body) buildFunction(*raw, *hf);
    }
    if (failed_) return nullptr;
    // Post-pass: resolve call targets and decode string/char literals.
    for (const auto& fn : mir_->functions) {
        for (const auto& blk : fn->blocks) {
            for (const auto& inst : blk->insts) {
                if (auto* al = std::get_if<mir::Inst::Alloca>(&inst->node); al && al->init)
                    walkExpr(*al->init);
                if (auto* st = std::get_if<mir::Inst::Store>(&inst->node)) {
                    if (st->target) walkExpr(*st->target);
                    if (st->value) walkExpr(*st->value);
                }
                if (auto* ev = std::get_if<mir::Inst::Eval>(&inst->node); ev && ev->value)
                    walkExpr(*ev->value);
                if (auto* rt = std::get_if<mir::Inst::Ret>(&inst->node); rt && rt->value)
                    walkExpr(*rt->value);
                if (auto* cb = std::get_if<mir::Inst::CondBranch>(&inst->node); cb && cb->cond)
                    walkExpr(*cb->cond);
            }
        }
    }
    return std::move(mir_);
}

// --- post-pass visitor: resolve Call::target + decode string/char literals ---

void MirBuilder::walkExpr(mir::Expr& e) {
    using M = mir::Expr;
    std::visit([&](auto& v) {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, M::StringLit>) {
            decodeString(v.raw, v.decoded);
        } else if constexpr (std::is_same_v<V, M::CharLit>) {
            decodeChar(v.raw, v.decoded);
        } else if constexpr (std::is_same_v<V, M::Call>) {
            // Resolve target by matching name + param types (overload
            // resolution at MIR level — HIR already resolved the correct
            // overload, so we just need to find the MIR function with
            // the same name and matching param signature).
            for (const auto& fn : mir_->functions) {
                if (fn->name != v.callee) continue;
                // Match param count (variadic extern "C" accepts extra).
                const std::size_t np = fn->params.size();
                const std::size_t na = v.args.size();
                if (np > na) continue;
                if (!fn->isExternC && np != na) continue;
                bool sigMatch = true;
                for (std::size_t i = 0; i < np && sigMatch; ++i) {
                    mir::Type p = fn->params[i].type; p.isReference = false;
                    mir::Type a = v.args[i] ? v.args[i]->type : mir::Type{};
                    a.isReference = false;
                    if (!(p == a)) {
                        // Allow numeric promotion (overload resolution
                        // already done at HIR; here we just pick the
                        // best-effort match).
                        if (isNumericM(p) && isNumericM(a)) continue;
                        sigMatch = false;
                    }
                }
                if (sigMatch) { v.target = fn.get(); break; }
            }
            // Fallback: if no signature match, try name-only (legacy).
            if (!v.target) {
                for (const auto& fn : mir_->functions) {
                    if (fn->name == v.callee) { v.target = fn.get(); break; }
                }
            }
            for (auto& a : v.args) if (a) walkExpr(*a);
        } else if constexpr (std::is_same_v<V, M::Unary>) {
            if (v.operand) walkExpr(*v.operand);
        } else if constexpr (std::is_same_v<V, M::Binary>) {
            if (v.lhs) walkExpr(*v.lhs);
            if (v.rhs) walkExpr(*v.rhs);
        } else if constexpr (std::is_same_v<V, M::Ternary>) {
            if (v.cond) walkExpr(*v.cond);
            if (v.thenBranch) walkExpr(*v.thenBranch);
            if (v.elseBranch) walkExpr(*v.elseBranch);
        } else if constexpr (std::is_same_v<V, M::Index>) {
            if (v.base) walkExpr(*v.base);
            if (v.index) walkExpr(*v.index);
        } else if constexpr (std::is_same_v<V, M::Member>) {
            if (v.base) walkExpr(*v.base);
        } else if constexpr (std::is_same_v<V, M::Assign>) {
            if (v.lhs) walkExpr(*v.lhs);
            if (v.rhs) walkExpr(*v.rhs);
        } else if constexpr (std::is_same_v<V, M::New>) {
            for (auto& a : v.args) if (a) walkExpr(*a);
        } else if constexpr (std::is_same_v<V, M::Delete>) {
            if (v.operand) walkExpr(*v.operand);
        } else if constexpr (std::is_same_v<V, M::InitList>) {
            for (auto& el : v.elements) if (el) walkExpr(*el);
        } else if constexpr (std::is_same_v<V, M::Lambda>) {
            for (auto& c : v.captureInits) if (c) walkExpr(*c);
        }
    }, e.node);
}

void MirBuilder::resolveCalls() { /* inlined into build() post-pass */ }
void MirBuilder::decodeLiterals() { /* inlined into build() post-pass */ }

// Deep-clone a MIR expression tree. Used when expanding compound assignments
// (`lhs += rhs` → `lhs = lhs + rhs`): the lhs must appear twice (once as the
// store target, once as the binary operand), so we need an independent copy.
std::unique_ptr<mir::Expr> MirBuilder::cloneExpr(const mir::Expr& e) {
    auto out = std::make_unique<mir::Expr>();
    out->loc = e.loc;
    out->type = e.type;
    out->lifetime = e.lifetime;
    std::visit([&](const auto& v) {
        using V = std::decay_t<decltype(v)>;
        using M = mir::Expr;
        if constexpr (std::is_same_v<V, M::IntegerLit>) {
            out->node = M::IntegerLit{v.value};
        } else if constexpr (std::is_same_v<V, M::FloatLit>) {
            out->node = M::FloatLit{v.value};
        } else if constexpr (std::is_same_v<V, M::StringLit>) {
            M::StringLit c; c.raw = v.raw; c.decoded = v.decoded;
            out->node = std::move(c);
        } else if constexpr (std::is_same_v<V, M::CharLit>) {
            M::CharLit c; c.raw = v.raw; c.decoded = v.decoded;
            out->node = std::move(c);
        } else if constexpr (std::is_same_v<V, M::BoolLit>) {
            out->node = M::BoolLit{v.value};
        } else if constexpr (std::is_same_v<V, M::NullptrLit>) {
            out->node = M::NullptrLit{};
        } else if constexpr (std::is_same_v<V, M::This>) {
            out->node = M::This{};
        } else if constexpr (std::is_same_v<V, M::IdentRef>) {
            out->node = M::IdentRef{v.name};
        } else if constexpr (std::is_same_v<V, M::Unary>) {
            auto& c = out->node.emplace<M::Unary>();
            c.op = v.op; c.isPrefix = v.isPrefix;
            if (v.operand) c.operand = cloneExpr(*v.operand);
        } else if constexpr (std::is_same_v<V, M::Binary>) {
            auto& c = out->node.emplace<M::Binary>();
            c.op = v.op;
            if (v.lhs) c.lhs = cloneExpr(*v.lhs);
            if (v.rhs) c.rhs = cloneExpr(*v.rhs);
        } else if constexpr (std::is_same_v<V, M::Ternary>) {
            auto& c = out->node.emplace<M::Ternary>();
            if (v.cond) c.cond = cloneExpr(*v.cond);
            if (v.thenBranch) c.thenBranch = cloneExpr(*v.thenBranch);
            if (v.elseBranch) c.elseBranch = cloneExpr(*v.elseBranch);
        } else if constexpr (std::is_same_v<V, M::Call>) {
            auto& c = out->node.emplace<M::Call>();
            c.callee = v.callee; c.target = v.target;
            c.returnLifetime = v.returnLifetime;
            for (const auto& a : v.args) c.args.push_back(a ? cloneExpr(*a) : nullptr);
        } else if constexpr (std::is_same_v<V, M::Index>) {
            auto& c = out->node.emplace<M::Index>();
            if (v.base) c.base = cloneExpr(*v.base);
            if (v.index) c.index = cloneExpr(*v.index);
        } else if constexpr (std::is_same_v<V, M::Member>) {
            auto& c = out->node.emplace<M::Member>();
            c.name = v.name; c.isArrow = v.isArrow; c.isScope = v.isScope;
            if (v.base) c.base = cloneExpr(*v.base);
        } else if constexpr (std::is_same_v<V, M::Assign>) {
            auto& c = out->node.emplace<M::Assign>();
            c.op = v.op;
            if (v.lhs) c.lhs = cloneExpr(*v.lhs);
            if (v.rhs) c.rhs = cloneExpr(*v.rhs);
        } else if constexpr (std::is_same_v<V, M::New>) {
            auto& c = out->node.emplace<M::New>();
            c.type = v.type;
            for (const auto& a : v.args) c.args.push_back(a ? cloneExpr(*a) : nullptr);
        } else if constexpr (std::is_same_v<V, M::Delete>) {
            auto& c = out->node.emplace<M::Delete>();
            c.isArray = v.isArray;
            if (v.operand) c.operand = cloneExpr(*v.operand);
        } else if constexpr (std::is_same_v<V, M::InitList>) {
            auto& c = out->node.emplace<M::InitList>();
            for (const auto& el : v.elements) c.elements.push_back(el ? cloneExpr(*el) : nullptr);
        } else if constexpr (std::is_same_v<V, M::Lambda>) {
            auto& c = out->node.emplace<M::Lambda>();
            c.funcName = v.funcName; c.closureType = v.closureType;
            for (const auto& ci : v.captureInits) c.captureInits.push_back(ci ? cloneExpr(*ci) : nullptr);
        }
    }, e.node);
    return out;
}

}  // namespace ivy
