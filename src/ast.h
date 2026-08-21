#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

namespace ivy {

struct SourceLoc {
    std::uint32_t line = 0;  // 1-based
    std::uint32_t col = 0;   // 1-based, in bytes
};

// [[ivy::name(arg, ...)]] — name excludes the "ivy::" namespace.
struct Attribute {
    std::string_view name;
    std::vector<std::string_view> args;
    SourceLoc loc;
};

struct Type {
    std::string_view base;  // "void", "int", "char", "long long", ...
    bool isUnsigned = false;
    bool isConst = false;
    bool isReference = false;  // T&
    std::uint32_t pointerDepth = 0;
};

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
    struct Call { std::unique_ptr<Expr> callee; std::vector<std::unique_ptr<Expr>> args; };
    struct Index { std::unique_ptr<Expr> base, index; };
    struct Member { std::unique_ptr<Expr> base; std::string_view name; bool isArrow; };
    struct Assign { std::string_view op; std::unique_ptr<Expr> lhs, rhs; };
    struct New { Type type; std::vector<std::unique_ptr<Expr>> args; };  // unsafe only
    struct Delete { std::unique_ptr<Expr> operand; bool isArray; };      // unsafe only
    // Braced aggregate initializer: `Point p = {1, 2};` or `Point p{1, 2};`.
    // Used for struct types — elements map positionally to struct fields.
    struct InitList { std::vector<std::unique_ptr<Expr>> elements; };

    SourceLoc loc;
    std::variant<IntegerLit, FloatLit, StringLit, CharLit, BoolLit, NullptrLit, IdentRef,
                 Unary, Binary, Ternary, Call, Index, Member, Assign, New, Delete, InitList>
        node;
};

// Deep-copy an expression tree. Default member initializers are AST
// subtrees owned by `Field::init` (unique_ptr); cloning lets us hand a
// fresh copy to the HIR builder so the same default can be applied to
// multiple variables of the same struct type.
inline std::unique_ptr<Expr> cloneExpr(const Expr& e) {
    auto out = std::make_unique<Expr>();
    out->loc = e.loc;
    std::visit([&]<typename V>(const V& v) {
        if constexpr (std::is_same_v<V, Expr::Unary>) {
            out->node.emplace<Expr::Unary>(Expr::Unary{v.op, v.isPrefix,
                v.operand ? cloneExpr(*v.operand) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::Binary>) {
            out->node.emplace<Expr::Binary>(Expr::Binary{v.op,
                v.lhs ? cloneExpr(*v.lhs) : nullptr,
                v.rhs ? cloneExpr(*v.rhs) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::Ternary>) {
            out->node.emplace<Expr::Ternary>(Expr::Ternary{
                v.cond ? cloneExpr(*v.cond) : nullptr,
                v.thenBranch ? cloneExpr(*v.thenBranch) : nullptr,
                v.elseBranch ? cloneExpr(*v.elseBranch) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::Call>) {
            Expr::Call c{v.callee ? cloneExpr(*v.callee) : nullptr, {}};
            for (const auto& a : v.args) c.args.push_back(cloneExpr(*a));
            out->node.emplace<Expr::Call>(std::move(c));
        } else if constexpr (std::is_same_v<V, Expr::Index>) {
            out->node.emplace<Expr::Index>(Expr::Index{
                v.base ? cloneExpr(*v.base) : nullptr,
                v.index ? cloneExpr(*v.index) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::Member>) {
            out->node.emplace<Expr::Member>(Expr::Member{
                v.base ? cloneExpr(*v.base) : nullptr, v.name, v.isArrow});
        } else if constexpr (std::is_same_v<V, Expr::Assign>) {
            out->node.emplace<Expr::Assign>(Expr::Assign{v.op,
                v.lhs ? cloneExpr(*v.lhs) : nullptr,
                v.rhs ? cloneExpr(*v.rhs) : nullptr});
        } else if constexpr (std::is_same_v<V, Expr::New>) {
            Expr::New n{v.type, {}};
            for (const auto& a : v.args) n.args.push_back(cloneExpr(*a));
            out->node.emplace<Expr::New>(std::move(n));
        } else if constexpr (std::is_same_v<V, Expr::Delete>) {
            out->node.emplace<Expr::Delete>(Expr::Delete{
                v.operand ? cloneExpr(*v.operand) : nullptr, v.isArray});
        } else if constexpr (std::is_same_v<V, Expr::InitList>) {
            Expr::InitList il;
            for (const auto& el : v.elements) il.elements.push_back(cloneExpr(*el));
            out->node.emplace<Expr::InitList>(std::move(il));
        } else {
            out->node = v;  // POD variants: IntegerLit, FloatLit, etc.
        }
    }, e.node);
    return out;
}

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
    struct ExprStmt { std::unique_ptr<ivy::Expr> value; };
    struct Unsafe { std::unique_ptr<Stmt> body; };
    struct Null {};

    SourceLoc loc;
    std::variant<Compound, Decl, If, While, DoWhile, For, Return, Break, Continue, ExprStmt,
                 Unsafe, Null>
        node;
};

struct Param {
    Type type;
    std::string_view name;  // empty if unnamed
    std::vector<Attribute> attrs;
    SourceLoc loc;
};

struct Function {
    std::vector<Attribute> attrs;  // [[ivy::lt_def(a)]], [[ivy::lt_ret(a)]]
    Type returnType;
    std::string_view name;          // qualified name (e.g. "ns::func")
    std::string_view namespacePrefix;  // "ns1::ns2::" or "" for global scope
    std::vector<Param> params;
    std::unique_ptr<Stmt::Compound> body;  // null => declaration only
    bool isExternC = false;
    SourceLoc loc;
};

// A single enumerator: `NAME = value` or `NAME` (implicit value).
struct Enumerator {
    std::string_view name;
    std::unique_ptr<Expr> value;  // may be null — implicit (prev + 1, or 0)
    SourceLoc loc;
};

// A struct/class field: `Type name;` or `Type name = default;`.
// Default member initializers are supported syntactically (parsed
// into `init`) but Ivy currently ignores them at codegen time — the
// field is zero-initialized via `alloca` like everything else.
struct Field {
    Type type;
    std::string_view name;
    std::unique_ptr<Expr> init;  // may be null — no default initializer
    SourceLoc loc;
};

// An `enum` (or `enum class`) declaration. C++ unscoped enums have
// implicit underlying type `int` and their enumerators leak into the
// enclosing scope. `enum class` (scoped) enumerators are accessed as
// `EnumName::Value`. Ivy currently uses `int` as the underlying type
// for all enums (matching C++ default for unscoped; `enum class`
// defaults to `int` too). An optional explicit underlying type
// (`enum Name : int32_t`) is accepted but must be an integer type.
struct EnumDecl {
    std::string_view name;          // qualified (e.g. "ns::Color")
    std::string_view namespacePrefix;  // "ns::" or "" for global scope
    std::vector<Enumerator> enumerators;
    bool isScoped = false;  // `enum class` / `enum struct`
    Type underlyingType;     // resolved; defaults to `int` if not specified
    SourceLoc loc;
};

// A `struct` (or `class`) declaration. Ivy treats `struct` and
// `class` identically — both are aggregates with public members.
// No inheritance, no methods, no access specifiers — Ivy is a
// subset and structs are plain C-style aggregates.
struct StructDecl {
    std::string_view name;          // qualified (e.g. "ns::Point")
    std::string_view namespacePrefix;  // "ns::" or "" for global scope
    std::vector<Field> fields;
    bool isClass = false;  // `class` vs `struct` (cosmetic only)
    SourceLoc loc;
};

struct TranslationUnit {
    std::vector<Function> functions;
    std::vector<EnumDecl> enums;
    std::vector<StructDecl> structs;
};

}  // namespace ivy