#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

#include "parsing/ast.h"  // ivy::Type, ivy::SourceLoc

namespace ivy {
namespace hir {

using Type = ivy::Type;
using SourceLoc = ivy::SourceLoc;

struct Function;

// Type-checked high-level IR. Mirrors the AST but every expression carries its
// resolved type, and Ivy attributes are lowered into IR annotations
// (Lifetime, Param::lifetime, returnLifetime, Unsafe statement).
struct Expr {
    struct IntegerLit { long long value; };
    struct FloatLit { double value; };
    struct StringLit { std::string_view raw; };  // raw, including quotes
    struct CharLit { std::string_view raw; };
    struct BoolLit { bool value; };
    struct NullptrLit {};
    struct IdentRef { std::string_view name; };
    struct This {};  // `this` — resolves to the implicit `this` param (6.7)
    struct Unary { std::string_view op; bool isPrefix; std::unique_ptr<Expr> operand; };
    struct Binary { std::string_view op; std::unique_ptr<Expr> lhs, rhs; };
    struct Ternary { std::unique_ptr<Expr> cond, thenBranch, elseBranch; };
    struct Call {
        std::string_view callee;
        const Function* target = nullptr;  // resolved by the builder
        std::vector<std::unique_ptr<Expr>> args;
        std::vector<Type> tplArgs;  // explicit template arguments (e.g. `add<int>`)
    };
    struct Index { std::unique_ptr<Expr> base, index; };
    // `isScope` is true for `::` (scope resolution), false for `.`/`->`.
    struct Member { std::unique_ptr<Expr> base; std::string_view name; bool isArrow; bool isScope = false; };
    struct Assign { std::string_view op; std::unique_ptr<Expr> lhs, rhs; };
    struct New { Type type; std::vector<std::unique_ptr<Expr>> args; };
    struct Delete { std::unique_ptr<Expr> operand; bool isArray; };
    // Aggregate initializer for a struct: `Point p = {1, 2};`.
    // Elements map positionally to struct fields; type is the struct type.
    struct InitList { std::vector<std::unique_ptr<Expr>> elements; };
    // Lambda closure value. After HIR lowering, a lambda expression
    // becomes an InitList that initializes a closure struct whose
    // fields are the captured variables. `funcName` is the synthesized
    // call-operator function that takes a closure pointer as its first
    // parameter. The `type` field (on the enclosing Expr) is set to
    // the closure struct type so codegen can emit the correct alloca.
    struct Lambda {
        std::string_view funcName;       // e.g. "__lambda0"
        std::string_view closureType;    // e.g. "__lambda0_closure"
        std::vector<std::unique_ptr<Expr>> captureInits;  // values for each capture field
    };

    Type type;  // resolved type
    SourceLoc loc;
    std::variant<IntegerLit, FloatLit, StringLit, CharLit, BoolLit, NullptrLit, IdentRef, This,
                 Unary, Binary, Ternary, Call, Index, Member, Assign, New, Delete, InitList,
                 Lambda>
        node;
};

// Deep-copy a HIR expression tree.  Used when the same HIR expr must
// be referenced from two places (e.g. constructor injection clones the
// already-built initializer expression so the ctor call gets a fresh
// copy while the Decl keeps its own).
inline std::unique_ptr<Expr> cloneHirExpr(const Expr& e) {
    auto out = std::make_unique<Expr>();
    out->type = e.type;
    out->loc = e.loc;
    std::visit([&]<typename V>(const V& v) {
        if constexpr (std::is_same_v<V, Expr::Unary>) {
            out->node.emplace<Expr::Unary>(Expr::Unary{v.op, v.isPrefix,
                v.operand ? cloneHirExpr(*v.operand) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::Binary>) {
            out->node.emplace<Expr::Binary>(Expr::Binary{v.op,
                v.lhs ? cloneHirExpr(*v.lhs) : nullptr,
                v.rhs ? cloneHirExpr(*v.rhs) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::Ternary>) {
            out->node.emplace<Expr::Ternary>(Expr::Ternary{
                v.cond ? cloneHirExpr(*v.cond) : nullptr,
                v.thenBranch ? cloneHirExpr(*v.thenBranch) : nullptr,
                v.elseBranch ? cloneHirExpr(*v.elseBranch) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::This>) {
            out->node.emplace<Expr::This>();
        } else if constexpr (std::is_same_v<V, Expr::Call>) {
            Expr::Call c{v.callee, v.target, {}, v.tplArgs};
            for (const auto& a : v.args) c.args.push_back(cloneHirExpr(*a));
            out->node.emplace<Expr::Call>(std::move(c));
        } else if constexpr (std::is_same_v<V, Expr::Index>) {
            out->node.emplace<Expr::Index>(Expr::Index{
                v.base ? cloneHirExpr(*v.base) : nullptr,
                v.index ? cloneHirExpr(*v.index) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::Member>) {
            out->node.emplace<Expr::Member>(Expr::Member{
                v.base ? cloneHirExpr(*v.base) : nullptr, v.name, v.isArrow, v.isScope});
        } else if constexpr (std::is_same_v<V, Expr::Assign>) {
            out->node.emplace<Expr::Assign>(Expr::Assign{v.op,
                v.lhs ? cloneHirExpr(*v.lhs) : nullptr,
                v.rhs ? cloneHirExpr(*v.rhs) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::New>) {
            Expr::New n{v.type, {}};
            for (const auto& a : v.args) n.args.push_back(cloneHirExpr(*a));
            out->node.emplace<Expr::New>(std::move(n));
        } else if constexpr (std::is_same_v<V, Expr::Delete>) {
            out->node.emplace<Expr::Delete>(Expr::Delete{
                v.operand ? cloneHirExpr(*v.operand) : nullptr, v.isArray});
        } else if constexpr (std::is_same_v<V, Expr::InitList>) {
            Expr::InitList il;
            for (const auto& el : v.elements) {
                if (el) il.elements.push_back(cloneHirExpr(*el));
                else il.elements.push_back(nullptr);
            }
            out->node.emplace<Expr::InitList>(std::move(il));
        } else if constexpr (std::is_same_v<V, Expr::Lambda>) {
            Expr::Lambda lam;
            lam.funcName = v.funcName;
            lam.closureType = v.closureType;
            for (const auto& c : v.captureInits) {
                lam.captureInits.push_back(c ? cloneHirExpr(*c) : nullptr);
            }
            out->node.emplace<Expr::Lambda>(std::move(lam));
        } else {
            out->node = v;  // POD variants: IntegerLit, FloatLit, etc.
        }
    }, e.node);
    return out;
}

struct Stmt {
    struct Compound { std::vector<std::unique_ptr<Stmt>> stmts; };
    struct Decl { Type type; std::string_view name; std::unique_ptr<Expr> init; };
    struct If { std::unique_ptr<Expr> cond; std::unique_ptr<Stmt> thenBranch, elseBranch; bool isConstexpr = false; };
    struct While { std::unique_ptr<Expr> cond; std::unique_ptr<Stmt> body; };
    struct DoWhile { std::unique_ptr<Stmt> body; std::unique_ptr<Expr> cond; };
    struct For {
        std::unique_ptr<Stmt> init;  // Decl or Null
        std::unique_ptr<Expr> cond;  // may be null
        std::unique_ptr<Expr> incr;  // may be null
        std::unique_ptr<Stmt> body;
    };
    struct Return { std::unique_ptr<Expr> value; };  // may be null
    struct Break {};
    struct Continue {};
    struct ExprStmt { std::unique_ptr<Expr> value; };
    struct Unsafe { std::unique_ptr<Stmt> body; };  // [[ivy::unsafe]] lowered
    struct Null {};
    // switch: cond is integral; default case has value == nullptr.
    // Ivy requires every case to end with break/return/continue (no fallthrough).
    struct CaseClause {
        std::unique_ptr<Expr> value;  // null => default
        std::vector<std::unique_ptr<Stmt>> stmts;
    };
    struct Switch {
        std::unique_ptr<Expr> cond;
        std::vector<CaseClause> cases;
    };

    SourceLoc loc;
    std::variant<Compound, Decl, If, While, DoWhile, For, Return, Break, Continue, ExprStmt,
                 Unsafe, Null, Switch>
        node;
};

// Lowered [[ivy::lt_def(a)]].
struct Lifetime {
    std::string_view name;
    SourceLoc loc;
};

struct Param {
    Type type;
    std::string_view name;      // empty if unnamed
    std::string_view lifetime;  // lowered [[ivy::lt(a)]]
    // Default argument: raw pointer to the AST Expr (owned by the AST
    // TranslationUnit).  At each call site that omits this argument,
    // the HIR builder clones the AST expr and builds a fresh HIR expr.
    const ivy::Expr* defaultValue = nullptr;
    SourceLoc loc;
};

struct Function {
    std::string_view name;
    std::string_view namespacePrefix;  // "ns1::ns2::" or "" for global scope
    Type returnType;
    std::vector<Param> params;
    std::vector<Lifetime> lifetimes;  // lowered [[ivy::lt_def(a)]]
    std::string_view returnLifetime;  // lowered [[ivy::lt_ret(a)]]
    std::unique_ptr<Stmt::Compound> body;  // null => declaration only
    bool isExternC = false;
    bool isConstexpr = false;
    bool isConsteval = false;
    bool isTemplate = false;       // true => template function (not instantiated)
    std::vector<Type> tplArgs;     // for instantiated templates: the concrete args
    bool isCtor = false;           // constructor
    bool isDtor = false;           // destructor
    SourceLoc loc;
};

struct TranslationUnit {
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<EnumDecl> enums;  // resolved enums (from ast::EnumDecl)
    std::vector<StructDecl> structs;  // resolved structs (from ast::StructDecl)
};

}  // namespace hir
}  // namespace ivy
