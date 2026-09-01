#pragma once

// IvyInterpret v0.2 — MIR tree-walking interpreter.
// Module contract:
//   - Input : const mir::TranslationUnit&  (fully type-checked + lifetime-checked MIR)
//   - Output: mir::Value                   (result of calling a function)
//   - Depends only on mir/ (not parsing/ or codegen/).
//
// Safety: because it runs on MIR, all lifetime checks and unsafe enforcement
// have already been applied by the MIR builder.  The interpreter additionally
// performs runtime checks (null deref, use-after-free) via the Machine hook.
//
// Usage (--run mode):
//   Interpreter interp(tu);
//   interp.callMain();

#include <cstdint>
#include <deque>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mir/mir.h"
#include "mir/interp_value.h"
#include "mir/interp_memory.h"
#include "mir/interp_machine.h"

namespace ivy {
namespace mir {

// Diagnostic produced by the interpreter (runtime errors).
struct InterpDiag {
    std::uint32_t line = 0;
    std::uint32_t col  = 0;
    std::string   message;
};

class Interpreter {
public:
    explicit Interpreter(const TranslationUnit& tu,
                          Machine* machine = nullptr);

    // Call the `main` function. Returns the exit code (as Int).
    Value callMain();

    // Call any function by name with pre-built args.
    Value call(std::string_view name, std::vector<Value> args);

    const std::vector<InterpDiag>& diagnostics() const { return diags_; }
    bool failed() const { return failed_; }
    void setOutput(std::ostream& os) { out_ = &os; }

private:
    using Frame = std::unordered_map<std::string, Cell>;

    struct FrameCtx {
        const Function* fn = nullptr;
        Frame locals;
        const Block* curBlock = nullptr;
        std::size_t curInst = 0;
        bool returned = false;
        Value retVal;
    };

    const TranslationUnit& tu_;
    std::deque<FrameCtx> frames_;
    Memory memory_;
    std::unique_ptr<Machine> machine_;
    std::vector<InterpDiag> diags_;
    bool failed_ = false;
    std::ostream* out_;

    // Helpers
    void error(SourceLoc loc, std::string msg);
    Frame& topFrame() { return frames_.back().locals; }
    Cell lookupCell(std::string_view name);
    void declareCell(std::string_view name, Cell c);

    // Block-level execution
    Value execFunction(const Function& fn, std::vector<Value> args);
    void execBlock(FrameCtx& frame);
    void execInst(FrameCtx& frame, const Inst& inst);

    // Expression evaluation (tree-walk MIR Expr)
    Value evalExpr(const Expr& e);
    Value evalBinary(const Expr::Binary& b, const Expr& e);
    Value evalUnary(const Expr::Unary& u, const Expr& e);
    Value evalCall(const Expr::Call& c, const Expr& e);
    Value evalMember(const Expr::Member& m, const Expr& e);
    Value evalAssign(const Expr::Assign& a, const Expr& e);
    Value evalInitList(const Expr::InitList& il, const Type& ty, const Expr& e);
    Cell  lvalueCell(const Expr& e);
    Value defaultStructValue(std::string_view structName);

    // Builtins
    Value callBuiltin(std::string_view name, const std::vector<Value>& args);
    bool isBuiltin(std::string_view name) const;

    // 8.5: Print a Value with type-aware formatting.
    void printValue(const Value& v, const mir::Type& t);
};

}  // namespace mir
}  // namespace ivy
