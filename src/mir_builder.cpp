#include "mir_builder.h"

#include <string>

namespace ivy {
namespace {

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
        call.returnLifetime = v.target ? v.target->returnLifetime : std::string_view{};
        for (const auto& a : v.args) call.args.push_back(buildExpr(*a));
        if (e.type.pointerDepth > 0) {
            if (!call.returnLifetime.empty()) {
                out->lifetime.kind = mir::Lifetime::Kind::Named;
                out->lifetime.name = call.returnLifetime;
            } else {
                out->lifetime.kind = mir::Lifetime::Kind::Unknown;  // opaque, e.g. malloc
            }
        }
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
        mir::Block* thenB = newBlock();
        mir::Block* elseB = v.elseBranch ? newBlock() : cont;

        auto* inst = emit(mir::Inst::Kind::CondBranch, s.loc);
        auto& cb = inst->node.emplace<mir::Inst::CondBranch>();
        cb.cond = buildExpr(*v.cond);
        cb.thenBlock = thenB;
        cb.elseBlock = elseB;

        cur_ = thenB;
        buildStmt(*v.thenBranch);
        jumpTo(cont);
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
    for (const auto& sd : hir_.structs) {
        std::vector<mir::StructField> fields;
        fields.reserve(sd.fields.size());
        for (const auto& f : sd.fields) fields.push_back({f.name, f.type});
        mir_->structs.push_back({sd.name, sd.namespacePrefix, std::move(fields)});
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
        fn->loc = hf->loc;
        mir::Function* raw = fn.get();
        mir_->functions.push_back(std::move(fn));
        if (hf->body) buildFunction(*raw, *hf);
    }
    if (failed_) return nullptr;
    return std::move(mir_);
}

}  // namespace ivy
