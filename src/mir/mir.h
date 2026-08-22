#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

#include "hir/hir.h"  // ivy::Type, ivy::SourceLoc

namespace ivy {
namespace mir {

using Type = ivy::Type;
using SourceLoc = ivy::SourceLoc;

struct Function;
struct Block;

// Lifetime annotation attached to every expression by the lifetime analysis.
struct Lifetime {
    enum class Kind {
        None,     // non-pointer value (or nullptr): no lifetime to track
        Named,    // tied to a [[ivy::lt_def]] lifetime; name is valid
        Static,   // string literal: lives forever
        Local,    // points into a local variable's storage
        Unknown,  // opaque (extern "C" call result, unannotated pointer param)
    };
    Kind kind = Kind::None;
    std::string_view name;  // valid when kind == Kind::Named
};

// Low-level expression tree. Mirrors hir::Expr but carries the computed
// lifetime annotation. Member access is kept (P4.4: structs) — it is
// lowered to getelementptr + load/store at codegen time.
struct Expr {
    struct IntegerLit { long long value; };
    struct FloatLit { double value; };
    struct StringLit {
        std::string_view raw;     // raw, including quotes
        std::string decoded;      // decoded bytes (filled by back-fill pass)
    };
    struct CharLit {
        std::string_view raw;
        long long decoded = 0;    // decoded char value (filled by back-fill pass)
    };
    struct BoolLit { bool value; };
    struct NullptrLit {};
    struct IdentRef { std::string_view name; };
    struct Unary { std::string_view op; bool isPrefix; std::unique_ptr<Expr> operand; };
    struct Binary { std::string_view op; std::unique_ptr<Expr> lhs, rhs; };
    struct Ternary { std::unique_ptr<Expr> cond, thenBranch, elseBranch; };
    struct Call {
        std::string_view callee;
        const Function* target = nullptr;  // resolved by back-fill pass
        std::string_view returnLifetime;   // lowered [[ivy::lt_ret]] of the target ("" if none)
        std::vector<std::unique_ptr<Expr>> args;
    };
    struct Index { std::unique_ptr<Expr> base, index; };
    struct Member { std::unique_ptr<Expr> base; std::string_view name; bool isArrow; };
    struct Assign { std::string_view op; std::unique_ptr<Expr> lhs, rhs; };
    struct New { Type type; std::vector<std::unique_ptr<Expr>> args; };
    struct Delete { std::unique_ptr<Expr> operand; bool isArray; };
    // Aggregate initializer for a struct: `Point p = {1, 2};`. Elements map
    // positionally to struct fields; `type` is the struct type. Elements
    // may be fewer than the struct's fields (the rest are zero-init or use
    // default member initializers at codegen time).
    struct InitList { std::vector<std::unique_ptr<Expr>> elements; };
    // Lambda closure value — mirrors hir::Expr::Lambda.
    struct Lambda {
        std::string_view funcName;
        std::string_view closureType;
        std::vector<std::unique_ptr<Expr>> captureInits;
    };

    Type type;
    Lifetime lifetime;  // result of the lifetime analysis
    SourceLoc loc;
    std::variant<IntegerLit, FloatLit, StringLit, CharLit, BoolLit, NullptrLit, IdentRef,
                 Unary, Binary, Ternary, Call, Index, Member, Assign, New, Delete, InitList,
                 Lambda>
        node;
};

struct Param {
    Type type;
    std::string_view name;      // empty if unnamed
    std::string_view lifetime;  // lowered [[ivy::lt(a)]]
    SourceLoc loc;
};

// Flat instruction list. `inUnsafe` records that the instruction was built
// inside an [[ivy::unsafe]] block (safety checks are relaxed there).
struct Inst {
    enum class Kind { Alloca, Store, Eval, Ret, CondBranch, Jump, Switch };

    struct Alloca { std::string_view var; Type type; std::unique_ptr<Expr> init; };
    struct Store { std::unique_ptr<Expr> target; std::unique_ptr<Expr> value; };
    struct Eval { std::unique_ptr<Expr> value; };
    struct Ret { std::unique_ptr<Expr> value; };  // may be null
    struct CondBranch { std::unique_ptr<Expr> cond; Block* thenBlock; Block* elseBlock; };
    struct Jump { Block* target; };
    // switch: one arm per case label. defaultBlock is the block to jump to
    // when no case matches (may equal exitBlock if there is no 'default').
    struct SwitchArm { long long value; Block* block; };
    struct Switch {
        std::unique_ptr<Expr> cond;
        std::vector<SwitchArm> arms;
        Block* defaultBlock;  // never null
    };

    Kind kind;
    bool inUnsafe = false;
    SourceLoc loc;
    std::variant<Alloca, Store, Eval, Ret, CondBranch, Jump, Switch> node;
};

struct Block {
    std::vector<std::unique_ptr<Inst>> insts;
};

struct Function {
    std::string_view name;
    std::string_view namespacePrefix;  // "ns1::ns2::" or "" for global scope
    Type returnType;
    std::vector<Param> params;
    std::vector<Lifetime> lifetimes;  // lowered [[ivy::lt_def(a)]]
    std::string_view returnLifetime;  // lowered [[ivy::lt_ret(a)]]
    std::vector<std::unique_ptr<Block>> blocks;
    bool isExternC = false;
    bool hasBody = false;  // false = declaration only (extern "C" prototype)
    bool isConstexpr = false;
    bool isConsteval = false;
    SourceLoc loc;
};

// Minimal enum info for codegen: name, underlying type, and namespace
// context for proper ABI mangling. (The full EnumDecl with enumerator
// values isn't needed past HIR because enum constants are folded to
// IntegerLit at the HIR stage.)
struct EnumInfo {
    std::string_view name;
    std::string_view namespacePrefix;  // "ns1::ns2::" or "" for global scope
    bool isScoped = false;
    std::string underlyingBase;  // e.g. "int", "int32_t"
};

// Minimal struct info for codegen: name, namespace context, and
// the field list (for layout + GEP index resolution). The full
// StructDecl with default initializers isn't needed past HIR —
// we only carry the field name + type (no `init` unique_ptr).
struct StructField {
    std::string_view name;
    Type type;
};
struct StructInfo {
    std::string_view name;
    std::string_view namespacePrefix;  // "ns1::ns2::" or "" for global scope
    std::vector<StructField> fields;  // ordered — index in vector = GEP field index
};

struct TranslationUnit {
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<EnumInfo> enums;  // for codegen type resolution
    std::vector<StructInfo> structs;  // for codegen type + layout resolution
};

}  // namespace mir
}  // namespace ivy
