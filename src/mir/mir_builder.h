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

    // B1: Borrow checker — production-quality aliasing XOR mutability.
    // Each BorrowScope tracks per-variable borrow state and a list of
    // active references (ref name → source variable name) so that borrows
    // are released precisely when the reference goes out of scope.
    struct BorrowEntry {
        std::string refVar;       // name of the reference variable (the borrower)
        std::string sourceVar;    // name of the borrowed variable (the borrowee)
        bool isMutable;           // true = T&, false = const T&
    };
    struct BorrowState {
        int sharedCount = 0;      // number of active const T& borrows
        bool hasMutable = false;  // an active T& borrow exists
    };
    struct BorrowScope {
        // Per-variable borrow counts (for fast aliasing XOR check).
        std::unordered_map<std::string_view, BorrowState> states;
        // Active references in this scope (for precise release on scope exit).
        std::vector<BorrowEntry> activeRefs;
    };
    std::vector<BorrowScope> borrowScopes_;
    // Check aliasing XOR mutability when creating a reference to `name`.
    // `isMutable` = true for `T&`/`T*`, false for `const T&`/`const T*`.
    // Returns true if the borrow is allowed (and records it).
    bool checkBorrow(std::string_view name, bool isMutable, SourceLoc loc);
    // Register a reference variable → source variable mapping so that
    // the borrow is released when the reference goes out of scope.
    void registerBorrower(std::string_view refVar, std::string_view sourceVar,
                          bool isMutable);
    // Release a borrow for a specific reference variable (when it goes
    // out of scope). Decrements the source variable's borrow count.
    void releaseBorrower(std::string_view refVar);
    // Release all borrows in the innermost scope (on scope pop).
    void releaseBorrowsInScope();

    // Loop context for break/continue. incr may be null (while/do-while).
    // A6: For switch contexts, caseBlocks holds the entry blocks of each
    // case (in order), and caseLabels maps label → block for `nextcase LABEL;`.
    // switchExit is the exit block (= exit for break).
    struct LoopCtx {
        mir::Block* cond;   // null inside switch
        mir::Block* incr;   // null inside switch
        mir::Block* exit;   // jump target for break
        // A6: switch-specific data
        std::vector<mir::Block*> caseBlocks;  // case entry blocks in order
        std::unordered_map<std::string_view, mir::Block*> caseLabels;  // label → block
    };
    std::vector<LoopCtx> loops_;

    // A6: Current case index within the switch being built (for unlabeled nextcase).
    int caseIndex_ = -1;

    void error(SourceLoc loc, std::string message);

    mir::Block* newBlock();
    mir::Inst* emit(mir::Inst::Kind kind, SourceLoc loc);
    void jumpTo(mir::Block* target);
    mir::Lifetime lookup(std::string_view name) const;
    void declare(std::string_view name, mir::Lifetime lt);

    std::unique_ptr<mir::Expr> buildExpr(const hir::Expr& e);
    void buildStmt(const hir::Stmt& s);
    void checkReturn(const mir::Function& fn, const mir::Expr& expr, SourceLoc loc);
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
