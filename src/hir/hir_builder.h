#pragma once

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/diagnostic.h"
#include "hir/hir.h"
#include "parsing/ast.h"

namespace ivy {

// Builds the typed HIR from the AST:
//   - resolves names against function and variable scopes
//   - type-checks all expressions and statements
//   - lowers [[ivy::lt_def/lt_ret/lt]] into lifetime annotations
//   - enforces the Ivy subset safety rules (pointer dereference/arithmetic
//     requires [[ivy::unsafe]], uninitialized variables are errors, ...)
class HirBuilder {
public:
    explicit HirBuilder(const TranslationUnit& ast);

    // Returns nullptr if any error was reported.
    std::unique_ptr<hir::TranslationUnit> build();

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

// Promote two types using usual arithmetic conversions.
    // Returns the promoted type, or a dummyType() on error.
    static hir::Type promoteTypes(const hir::Type& a, const hir::Type& b);

private:
    const TranslationUnit& ast_;
    std::unique_ptr<hir::TranslationUnit> hir_;
    std::vector<Diagnostic> diagnostics_;
    bool failed_ = false;

    std::unordered_map<std::string_view, hir::Function*> functions_;
    std::vector<std::unordered_map<std::string_view, hir::Type>> scopes_;
    int unsafeDepth_ = 0;
    hir::Function* current_ = nullptr;
    bool hasReturnInBody_ = false;

    // Namespace prefix of the function whose body is currently being
    // built (e.g. "ns::" or "" for global scope). Used to resolve bare
    // names: when `func` is not found in `functions_`, we try
    // `ns::func` before reporting an error.
    std::string_view currentNsPrefix_;

    // Enum registry. Maps enum type name → EnumDef (underlying type +
    // constant value map). For unscoped enums, constants are also
    // registered directly in `enumConstants_` (so `Red` resolves without
    // qualification). For scoped enums (`enum class`), constants are
    // only accessible via `EnumName::Value` — resolved in `parsePrimary`
    // as a `Member` expression on an enum-typed base, but Ivy currently
    // resolves scoped enum constants here too via `EnumName::Value` →
    // the value directly.
    struct EnumDef {
        hir::Type underlyingType;
        std::unordered_map<std::string_view, long long> constants;
        bool isScoped = false;
        std::string nsPrefix;  // namespace prefix (e.g. "colors::" or "")
    };
    std::unordered_map<std::string_view, EnumDef> enums_;
    // Unscoped enum constants: name → value (for bare `Red` lookups).
    std::unordered_map<std::string_view, long long> enumConstants_;

    // Struct registry. Maps struct type name → StructDef (fields +
    // computed layout). Used by the Member handler to resolve
    // `p.x` / `p->x` field accesses.
    struct StructDef {
        std::vector<Field> fields;
        // Pointer to the AST default member initializer expression for
        // each field (null if the field has no `= init`). The AST
        // TranslationUnit outlives the HIR builder, so raw pointers are
        // safe. Used to synthesize aggregate initializers when a struct
        // variable is declared without an explicit initializer, or when
        // a braced init list has fewer elements than fields.
        std::vector<const Expr*> defaultInits;
        std::uint64_t size = 0;
        std::uint32_t align = 1;
        std::string nsPrefix;  // namespace prefix (e.g. "geom::" or "")
        // Field name → {index, offset, type} for Member resolution.
        struct FieldInfo {
            std::size_t index;
            std::uint64_t offset;
            hir::Type type;
        };
        std::unordered_map<std::string_view, FieldInfo> fieldMap;
    };
    std::unordered_map<std::string_view, StructDef> structs_;

    // Stable storage for synthesized qualified names (e.g. from
    // flattening `ns::func` Member chains in Call expressions).
    // `string_view` keys in `functions_` / `call.callee` must point
    // into stable storage; this deque provides pointer-stable nodes.
    std::deque<std::string> stringStorage_;

    void error(SourceLoc loc, std::string message);

    // signatures (pass 1)
    void buildSignature(const Function& af);
    void buildEnum(const EnumDecl& ed);
    void buildStruct(const StructDecl& sd);
    void lowerLifetimeAttributes(hir::Function& fn, const Function& af);
    std::string_view lowerParamAttribute(hir::Function& fn, const Param& ap);

    // bodies (pass 2)
    void buildBody(hir::Function& fn, const Stmt::Compound& body);
    std::unique_ptr<hir::Stmt> buildStmt(const Stmt& s);
    std::unique_ptr<hir::Stmt> buildCompound(const Stmt::Compound& c, SourceLoc loc);
    std::unique_ptr<hir::Stmt> buildDeclaration(const Stmt::Decl& d, SourceLoc loc,
                                                bool checkInit);
    std::unique_ptr<hir::Stmt> buildReturn(const Stmt::Return& r, SourceLoc loc);
    std::unique_ptr<hir::Expr> buildExpr(const Expr& e);
    // Build a struct aggregate initializer: resolve each element against the
    // corresponding field type of the target struct. Returns a hir::Expr::InitList
    // whose type is the struct type, or nullptr on error.
    std::unique_ptr<hir::Expr> buildStructInit(const Expr::InitList& il,
                                               const hir::Type& structType,
                                               [[maybe_unused]] std::string_view varName,
                                               SourceLoc loc);
    // Lower a lambda expression into a closure struct value + call-operator
    // function. The lambda's captures become fields of a synthesized
    // closure struct type; the body becomes a function that takes a
    // closure pointer as its first parameter. Returns a hir::Expr::Lambda
    // whose type is the closure struct type.
    std::unique_ptr<hir::Expr> buildLambda(const Expr::Lambda& lam, SourceLoc loc);
    // Counter for generating unique lambda names.
    int lambdaCounter_ = 0;

    // helpers
    void declare(std::string_view name, hir::Type type, SourceLoc loc);
    bool isAssignable(const hir::Type& to, const hir::Type& from) const;
    bool checkCondition(const hir::Expr& e);
    void requireUnsafe(SourceLoc loc, std::string_view what);
    void checkCall(hir::Expr::Call& call, SourceLoc loc);

    // --- constexpr evaluation ---
    // Attempts to evaluate a constexpr/consteval function call at
    // compile time.  If all arguments are compile-time constants and
    // the function body can be fully evaluated (no I/O, no dynamic
    // memory, ...), replaces the Call expression with a literal.
    // Returns true if the call was folded (out is set), false otherwise.
    bool tryEvalConstexprCall(hir::Expr& out, const hir::Expr::Call& call,
                              const hir::Function& fn, SourceLoc loc);

    // Recursive tree-walk evaluator for constexpr function bodies.
    // Returns the folded value or sets `ok=false` on the first non-
    // constant sub-expression.
    struct ConstValue {
        bool isInt = true;
        long long i = 0;
        double f = 0.0;
    };
    bool evalConstExpr(const hir::Expr& e, const hir::Function& fn,
                       ConstValue& result) const;
    bool evalConstStmt(const hir::Stmt& s, const hir::Function& fn,
                       ConstValue& result) const;

    // Namespace-aware name resolution helpers.
    // Tries `name` first, then `currentNsPrefix_ + name`.
    hir::Function* resolveFunction(std::string_view name) const;

    // Flattens a chain of `Expr::Member` (from `A::B::C`) into a
    // qualified name string. Returns false if the chain is not a
    // pure `::`-separated identifier path.
    bool flattenMemberChain(const Expr& e, std::string& out) const;
};

}  // namespace ivy
