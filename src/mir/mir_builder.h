#pragma once

#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/diagnostic.h"
#include "hir/hir.h"
#include "mir/mir.h"

namespace ivy {

// Builds the CFG-based MIR from the type-checked HIR and runs the safety
// analyses on it:
//   - lifetime checker: a pointer returned from a function must match its
//     [[ivy::lt_ret(a)]] (a parameter with [[ivy::lt(a)]], a static string
//     literal, or nullptr). Returning pointers to locals (dangling) or
//     pointers with unknown lifetimes is rejected, unless the return
//     statement sits inside an [[ivy::unsafe]] block.
//   - [[ivy::unsafe]] is recorded on each instruction (Inst::inUnsafe) so
//     later passes know which code opted out of safety checks.
class MirBuilder {
public:
    explicit MirBuilder(const hir::TranslationUnit& hir);

    // Returns nullptr if any error was reported.
    std::unique_ptr<mir::TranslationUnit> build();

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

private:
    const hir::TranslationUnit& hir_;
    std::unique_ptr<mir::TranslationUnit> mir_;
    std::vector<Diagnostic> diagnostics_;
    bool failed_ = false;

    mir::Function* current_ = nullptr;
    mir::Block* cur_ = nullptr;  // block being filled
    int unsafeDepth_ = 0;

    // Storage-lifetime of parameters (for '&param').
    std::unordered_map<std::string_view, mir::Lifetime> params_;
    // Value-lifetime of local variables, innermost scope last.
    std::vector<std::unordered_map<std::string_view, mir::Lifetime>> scopes_;

    // Loop context for break/continue. incr may be null (while/do-while).
    struct LoopCtx { mir::Block* cond; mir::Block* incr; mir::Block* exit; };
    std::vector<LoopCtx> loops_;

    void error(SourceLoc loc, std::string message);

    mir::Block* newBlock();
    mir::Inst* emit(mir::Inst::Kind kind, SourceLoc loc);
    void jumpTo(mir::Block* target);
    mir::Lifetime lookup(std::string_view name) const;
    void declare(std::string_view name, mir::Lifetime lt);

    std::unique_ptr<mir::Expr> buildExpr(const hir::Expr& e);
    void buildStmt(const hir::Stmt& s);
    void checkReturn(const mir::Function& fn, const mir::Lifetime& lt, SourceLoc loc);
    void checkStore(const mir::Expr& target, const mir::Expr& value, SourceLoc loc);
    void buildFunction(mir::Function& fn, const hir::Function& hf);

    // Post-pass: resolve Call::target pointers and decode string/char literals.
    void resolveCalls();
    void decodeLiterals();
    void walkExpr(mir::Expr& e);  // visitor for back-fill

    // Deep-clone a MIR expression (used for compound-assignment expansion).
    static std::unique_ptr<mir::Expr> cloneExpr(const mir::Expr& e);
};

}  // namespace ivy
