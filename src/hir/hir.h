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
    struct Unary { std::string_view op; bool isPrefix; std::unique_ptr<Expr> operand; };
    struct Binary { std::string_view op; std::unique_ptr<Expr> lhs, rhs; };
    struct Ternary { std::unique_ptr<Expr> cond, thenBranch, elseBranch; };
    struct Call {
        std::string_view callee;
        const Function* target = nullptr;  // resolved by the builder
        std::vector<std::unique_ptr<Expr>> args;
    };
    struct Index { std::unique_ptr<Expr> base, index; };
    struct Member { std::unique_ptr<Expr> base; std::string_view name; bool isArrow; };
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
    std::variant<IntegerLit, FloatLit, StringLit, CharLit, BoolLit, NullptrLit, IdentRef,
                 Unary, Binary, Ternary, Call, Index, Member, Assign, New, Delete, InitList,
                 Lambda>
        node;
};

struct Stmt {
    struct Compound { std::vector<std::unique_ptr<Stmt>> stmts; };
    struct Decl { Type type; std::string_view name; std::unique_ptr<Expr> init; };
    struct If { std::unique_ptr<Expr> cond; std::unique_ptr<Stmt> thenBranch, elseBranch; };
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

    SourceLoc loc;
    std::variant<Compound, Decl, If, While, DoWhile, For, Return, Break, Continue, ExprStmt,
                 Unsafe, Null>
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
    SourceLoc loc;
};

struct TranslationUnit {
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<EnumDecl> enums;  // resolved enums (from ast::EnumDecl)
    std::vector<StructDecl> structs;  // resolved structs (from ast::StructDecl)
};

}  // namespace hir
}  // namespace ivy
