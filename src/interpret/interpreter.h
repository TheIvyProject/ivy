#pragma once

// IvyInterpret — HIR tree-walking interpreter.
// Module contract:
//   - Input : const hir::TranslationUnit&  (fully type-checked HIR)
//   - Output: ivy::interp::Value           (result of calling a function)
//   - No dependency on parsing/, mir/, or codegen/.
//
// Usage (--run mode):
//   Interpreter interp(tu);
//   interp.callMain();   // runs main(), prints its return value
//
// Usage (--repl mode / constexpr evaluation):
//   Value v = interp.call("funcName", {arg0, arg1});

#include <functional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "hir/hir.h"
#include "interpret/value.h"

namespace ivy {
namespace interp {

// Diagnostic produced by the interpreter (runtime errors).
struct InterpDiag {
    std::uint32_t line = 0;
    std::uint32_t col  = 0;
    std::string   message;
};

class Interpreter {
public:
    // Build an interpreter over a fully type-checked HIR translation unit.
    explicit Interpreter(const hir::TranslationUnit& tu);

    // Call the `main` function (no arguments). Returns the exit code (as Int).
    // If main is not found, records a diagnostic and returns Void.
    Value callMain();

    // Call any function by its mangled / unmangled name with pre-built args.
    Value call(std::string_view name, std::vector<Value> args);

    // Diagnostics accumulated during interpretation (runtime errors).
    const std::vector<InterpDiag>& diagnostics() const { return diags_; }
    bool failed() const { return failed_; }

    // Install an I/O callback used for `printf`-style extern "C" calls.
    // Default: writes to std::cout.
    void setOutput(std::ostream& os) { out_ = &os; }

private:
    // ---- Internal execution context ----

    // A single stack frame: maps variable name → heap Cell so that
    // references / pointer-captures share the same Cell.
    using Frame = std::unordered_map<std::string, Cell>;

    // Control-flow signals returned by statement execution.
    struct ReturnSignal  { Value value; };
    struct BreakSignal   {};
    struct ContinueSignal{};
    using Signal = std::variant<std::monostate, ReturnSignal,
                                BreakSignal, ContinueSignal>;

    // ---- Interpreter state ----
    const hir::TranslationUnit& tu_;
    std::vector<Frame> frames_;   // call stack; each function pushes one frame
    std::vector<InterpDiag> diags_;
    bool failed_ = false;
    std::ostream* out_ = nullptr; // set in constructor to &std::cout

    // ---- Helpers ----
    void        error(SourceLoc loc, std::string msg);
    Frame&      topFrame() { return frames_.back(); }
    Cell        lookupCell(std::string_view name);
    void        declareCell(std::string_view name, Cell c);

    // ---- Core eval / exec ----
    Value  evalExpr(const hir::Expr& e);
    Signal execStmt(const hir::Stmt& s);
    Signal execCompound(const hir::Stmt::Compound& c);
    Value  execFn(const hir::Function& fn, std::vector<Value> args);

    // ---- Expression helpers ----
    Value  evalBinary (const hir::Expr::Binary& b, SourceLoc loc);
    Value  evalUnary  (const hir::Expr::Unary&  u, SourceLoc loc);
    Value  evalCall   (const hir::Expr::Call&   c, SourceLoc loc);
    Value  evalMember (const hir::Expr::Member& m, SourceLoc loc);
    Value  evalAssign (const hir::Expr::Assign& a, SourceLoc loc);
    Value  evalInitList(const hir::Expr::InitList& il, const hir::Type& ty, SourceLoc loc);
    Cell   lvalueCell (const hir::Expr& e);   // lvalue → Cell (for assign / unary &)
    Value  defaultStructValue(std::string_view structName);  // zero-init a struct type

    // ---- Built-in extern "C" functions ----
    Value  callBuiltin(std::string_view name, const std::vector<Value>& args, SourceLoc loc);
    bool   isBuiltin  (std::string_view name) const;
};

}  // namespace interp
}  // namespace ivy
