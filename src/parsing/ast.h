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

// Forward declaration so that Expr::Lambda can reference Stmt::Compound.
struct Stmt;

// Defined early because Expr::Lambda uses std::vector<Param>.
struct Param {
    Type type;
    std::string_view name;  // empty if unnamed
    std::vector<Attribute> attrs;
    SourceLoc loc;
};

// A template parameter: `typename T` (type) or `int N` / `long long N` (non-type).
// For non-type parameters, `type` holds the integer type and `name` is the
// parameter name. `isTypename` distinguishes the two kinds.
struct TemplateParam {
    bool isTypename = true;   // true => `typename T`, false => `int N` etc.
    Type type;                // for non-type: the integer type
    std::string_view name;    // parameter name (T, N, ...)
    SourceLoc loc;
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
    struct Call { std::unique_ptr<Expr> callee; std::vector<std::unique_ptr<Expr>> args; std::vector<Type> tplArgs; };
    struct Index { std::unique_ptr<Expr> base, index; };
    struct Member { std::unique_ptr<Expr> base; std::string_view name; bool isArrow; };
    struct Assign { std::string_view op; std::unique_ptr<Expr> lhs, rhs; };
    struct New { Type type; std::vector<std::unique_ptr<Expr>> args; };  // unsafe only
    struct Delete { std::unique_ptr<Expr> operand; bool isArray; };      // unsafe only
    // Braced aggregate initializer: `Point p = {1, 2};` or `Point p{1, 2};`.
    // Used for struct types — elements map positionally to struct fields.
    struct InitList { std::vector<std::unique_ptr<Expr>> elements; };
    // Lambda expression: `[cap](params) -> ret { body }`.
    // `returnType.base` is empty when `-> ret` is omitted (deduced).
    // `body` is a `Stmt::Compound` stored as `std::unique_ptr<Stmt>`
    // because `Stmt` is only forward-declared at this point.
    struct Capture { std::string_view name; bool byRef; };
    struct Lambda {
        std::vector<Capture> captures;
        std::vector<Param> params;
        Type returnType;  // base.empty() => deduced
        std::unique_ptr<Stmt> body;  // actually a Stmt::Compound
    };

    SourceLoc loc;
    std::variant<IntegerLit, FloatLit, StringLit, CharLit, BoolLit, NullptrLit, IdentRef,
                 Unary, Binary, Ternary, Call, Index, Member, Assign, New, Delete, InitList,
                 Lambda>
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
            Expr::Call c{v.callee ? cloneExpr(*v.callee) : nullptr, {}, v.tplArgs};
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
            for (const auto& el : v.elements) {
                if (el) il.elements.push_back(cloneExpr(*el));
                else il.elements.push_back(nullptr);
            }
            out->node.emplace<Expr::InitList>(std::move(il));
        } else if constexpr (std::is_same_v<V, Expr::Lambda>) {
            // Lambdas are not cloned — they are unique AST nodes owned
            // by the enclosing expression. If we reach here, just copy
            // the captures/params/returnType and leave body null.
            Expr::Lambda lam;
            lam.captures = v.captures;
            lam.params = v.params;
            lam.returnType = v.returnType;
            out->node.emplace<Expr::Lambda>(std::move(lam));
        } else {
            out->node = v;  // POD variants: IntegerLit, FloatLit, etc.
        }
    }, e.node);
    return out;
}

struct Stmt {
    struct Compound { std::vector<std::unique_ptr<Stmt>> stmts; };
    struct Decl { Type type; std::string_view name; std::unique_ptr<Expr> init; bool isConstexpr = false; };
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
    // switch/case: cond is integral; each CaseClause has a constant value
    // (null => default). Ivy forbids implicit fallthrough: every case must
    // end with break/return/continue or be provably unreachable — this is
    // checked in the HIR builder.
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

// Deep-copy a statement tree. Used by the HIR builder when prepending
// synthesized declarations (e.g. lambda capture locals) to a user-
// written body that must not be moved out of the AST.
inline std::unique_ptr<Stmt> cloneStmt(const Stmt& s) {
    auto out = std::make_unique<Stmt>();
    out->loc = s.loc;
    std::visit([&]<typename V>(const V& v) {
        if constexpr (std::is_same_v<V, Stmt::Compound>) {
            Stmt::Compound c;
            for (const auto& st : v.stmts) c.stmts.push_back(cloneStmt(*st));
            out->node.emplace<Stmt::Compound>(std::move(c));
        } else if constexpr (std::is_same_v<V, Stmt::Decl>) {
            out->node.emplace<Stmt::Decl>(Stmt::Decl{v.type, v.name,
                v.init ? cloneExpr(*v.init) : nullptr, v.isConstexpr});
        } else if constexpr (std::is_same_v<V, Stmt::If>) {
            out->node.emplace<Stmt::If>(Stmt::If{
                v.cond ? cloneExpr(*v.cond) : nullptr,
                v.thenBranch ? cloneStmt(*v.thenBranch) : nullptr,
                v.elseBranch ? cloneStmt(*v.elseBranch) : nullptr});
        } else if constexpr (std::is_same_v<V, Stmt::While>) {
            out->node.emplace<Stmt::While>(Stmt::While{
                v.cond ? cloneExpr(*v.cond) : nullptr,
                v.body ? cloneStmt(*v.body) : nullptr});
        } else if constexpr (std::is_same_v<V, Stmt::DoWhile>) {
            out->node.emplace<Stmt::DoWhile>(Stmt::DoWhile{
                v.body ? cloneStmt(*v.body) : nullptr,
                v.cond ? cloneExpr(*v.cond) : nullptr});
        } else if constexpr (std::is_same_v<V, Stmt::For>) {
            out->node.emplace<Stmt::For>(Stmt::For{
                v.init ? cloneStmt(*v.init) : nullptr,
                v.cond ? cloneExpr(*v.cond) : nullptr,
                v.incr ? cloneExpr(*v.incr) : nullptr,
                v.body ? cloneStmt(*v.body) : nullptr});
        } else if constexpr (std::is_same_v<V, Stmt::Return>) {
            out->node.emplace<Stmt::Return>(Stmt::Return{
                v.value ? cloneExpr(*v.value) : nullptr});
        } else if constexpr (std::is_same_v<V, Stmt::ExprStmt>) {
            out->node.emplace<Stmt::ExprStmt>(Stmt::ExprStmt{
                v.value ? cloneExpr(*v.value) : nullptr});
        } else if constexpr (std::is_same_v<V, Stmt::Unsafe>) {
            out->node.emplace<Stmt::Unsafe>(Stmt::Unsafe{
                v.body ? cloneStmt(*v.body) : nullptr});
        } else if constexpr (std::is_same_v<V, Stmt::Switch>) {
            Stmt::Switch sw;
            sw.cond = v.cond ? cloneExpr(*v.cond) : nullptr;
            for (const auto& c : v.cases) {
                Stmt::CaseClause cc;
                cc.value = c.value ? cloneExpr(*c.value) : nullptr;
                for (const auto& st : c.stmts) cc.stmts.push_back(cloneStmt(*st));
                sw.cases.push_back(std::move(cc));
            }
            out->node.emplace<Stmt::Switch>(std::move(sw));
        } else {
            out->node = v;  // Break, Continue, Null
        }
    }, s.node);
    return out;
}


struct Function {
    std::vector<Attribute> attrs;  // [[ivy::lt_def(a)]], [[ivy::lt_ret(a)]]
    Type returnType;
    std::string_view name;          // qualified name (e.g. "ns::func")
    std::string_view namespacePrefix;  // "ns1::ns2::" or "" for global scope
    std::vector<Param> params;
    std::vector<TemplateParam> tplParams;  // non-empty => template function
    std::unique_ptr<Stmt::Compound> body;  // null => declaration only
    bool isExternC = false;
    bool isConstexpr = false;   // `constexpr` function / variable
    bool isConsteval = false;   // `consteval` function (implies constexpr)
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