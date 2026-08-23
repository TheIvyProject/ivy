#include "hir/hir_builder.h"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>

namespace ivy {
namespace {

// Rank: larger = wider.  Ivy fixed-width types and C++ types share a scale
// so mixed arithmetic (`int + int32_t`) promotes correctly.
static int typeRank(std::string_view b) {
    if (b == "bool")     return 0;
    if (b == "char")     return 1;
    if (b == "int8_t" || b == "uint8_t")    return 1;
    if (b == "short")    return 2;
    if (b == "int16_t" || b == "uint16_t")  return 2;
    if (b == "int" || b == "unsigned")      return 3;
    if (b == "int32_t" || b == "uint32_t")  return 3;
    if (b == "long" || b == "int64_t" || b == "uint64_t") return 4;
    if (b == "long long") return 4;
    if (b == "size_t" || b == "ptrdiff_t")  return 4;
    if (b == "float16_t" || b == "bfloat16_t") return 5;
    if (b == "float" || b == "float32_t")  return 6;
    if (b == "double" || b == "long double" || b == "float64_t") return 7;
    if (b == "float128_t") return 8;
    return -1;
}

static bool isIvyUnsigned(std::string_view b) {
    return b == "uint8_t" || b == "uint16_t" || b == "uint32_t" || b == "uint64_t" ||
           b == "size_t";
}

static bool isFloatBase(std::string_view b) {
    return b == "float" || b == "double" || b == "long double" ||
           b == "float16_t" || b == "float32_t" || b == "float64_t" ||
           b == "float128_t" || b == "bfloat16_t";
}

static bool isIntegerBase(std::string_view b) {
    return b == "bool" || b == "char" || b == "short" || b == "int" ||
           b == "long" || b == "long long" ||
           b == "int8_t" || b == "int16_t" || b == "int32_t" || b == "int64_t" ||
           b == "uint8_t" || b == "uint16_t" || b == "uint32_t" || b == "uint64_t" ||
           b == "size_t" || b == "ptrdiff_t";
}

// Approximate bit-width of a numeric type base name.  Used for overload
// resolution ranking: narrower conversions are preferred over wider ones.
// Returns 0 for unknown types.
static int typeWidth(std::string_view b) {
    if (b == "bool" || b == "char" || b == "int8_t" || b == "uint8_t" ||
        b == "signed char" || b == "unsigned char") return 8;
    if (b == "short" || b == "int16_t" || b == "uint16_t") return 16;
    if (b == "int" || b == "int32_t" || b == "uint32_t" || b == "float" ||
        b == "float32_t" || b == "bfloat16_t" || b == "float16_t") return 32;
    if (b == "long" || b == "int64_t" || b == "uint64_t" || b == "size_t" ||
        b == "ptrdiff_t" || b == "long long" || b == "double" ||
        b == "float64_t") return 64;
    if (b == "long double" || b == "float128_t") return 128;
    return 0;
}

bool isNumeric(const hir::Type& t) {
    if (t.pointerDepth > 0 || t.base == "void" || t.base == "nullptr") return false;
    // Struct/enum types are not numeric (enums are handled separately
    // — they fold to IntegerLit at HIR build time, so an enum-typed
    // expression here means a struct, which is not usable in arithmetic).
    if (isIntegerBase(t.base) || isFloatBase(t.base)) return true;
    return false;  // unknown user-defined type (e.g. struct)
}

bool isIntegerLike(const hir::Type& t) {
    if (t.pointerDepth > 0) return false;
    return isIntegerBase(t.base);
}

// Strip reference qualifier — a reference T& behaves like T for value semantics
// (arithmetic, comparison, etc.). Reference-ness only matters for storage
// (params, locals, the lvalue itself).
hir::Type stripReference(const hir::Type& t) {
    hir::Type r = t;
    r.isReference = false;
    return r;
}

// Placeholder type used for expression recovery after an error.
hir::Type dummyType() {
    hir::Type t;
    t.base = "int";
    return t;
}

hir::Type intType() {
    hir::Type t;
    t.base = "int";
    return t;
}

hir::Type floatType() {
    hir::Type t;
    t.base = "double";
    return t;
}

hir::Type boolType() {
    hir::Type t;
    t.base = "bool";
    return t;
}

// const char*
hir::Type constCharPtrType() {
    hir::Type t;
    t.base = "char";
    t.isConst = true;
    t.pointerDepth = 1;
    return t;
}

hir::Type nullptrType() {
    hir::Type t;
    t.base = "nullptr";
    return t;
}

std::string typeToString(const hir::Type& t) {
    std::string s;
    if (t.isConst) s += "const ";
    if (t.isUnsigned) s += "unsigned ";
    s += std::string(t.base);
    for (std::uint32_t i = 0; i < t.pointerDepth; ++i) s += "*";
    return s;
}

}  // namespace

// --- promotion helper (outside anonymous-namespace) ---

hir::Type HirBuilder::promoteTypes(const hir::Type& aIn, const hir::Type& bIn) {
    // References decay to their underlying value type for arithmetic.
    hir::Type a = stripReference(aIn);
    hir::Type b = stripReference(bIn);
    if (a.pointerDepth > 0 && b.pointerDepth > 0 && a.base != b.base) return a;
    if (a.pointerDepth > 0 && b.pointerDepth == 0) return a;
    if (b.pointerDepth > 0 && a.pointerDepth == 0) return b;
    if (a.base == "nullptr") return b;
    if (b.base == "nullptr") return a;

    const bool af = isFloatBase(a.base), bf = isFloatBase(b.base);
    if (af || bf) {
        // Mixed float + integral → float of higher rank
        if (af && bf) return typeRank(a.base) >= typeRank(b.base) ? a : b;
        return af ? a : b;  // float wins over integral
    }

    if (isIntegerBase(a.base) && isIntegerBase(b.base)) {
        int ra = typeRank(a.base), rb = typeRank(b.base);
        const bool au = a.isUnsigned || isIvyUnsigned(a.base);
        const bool bu = b.isUnsigned || isIvyUnsigned(b.base);
        hir::Type r = (ra >= rb) ? a : b;
        r.isUnsigned = au || bu;
        return r;
    }
    return a;
}

HirBuilder::HirBuilder(const TranslationUnit& ast) : ast_(ast) {}

void HirBuilder::error(SourceLoc loc, std::string message) {
    diagnostics_.push_back(Diagnostic{loc.line, loc.col, std::move(message)});
    failed_ = true;
}

// --- signatures (pass 1) ---

void HirBuilder::lowerLifetimeAttributes(hir::Function& fn, const Function& af) {
    for (const Attribute& a : af.attrs) {
        if (a.name == "lt_def") {
            for (std::string_view arg : a.args) {
                if (std::any_of(fn.lifetimes.begin(), fn.lifetimes.end(),
                                [&](const hir::Lifetime& l) { return l.name == arg; })) {
                    error(a.loc, "duplicate lifetime '" + std::string(arg) + "'");
                    continue;
                }
                fn.lifetimes.push_back(hir::Lifetime{arg, a.loc});
            }
        } else if (a.name == "lt_ret") {
            if (a.args.size() != 1) {
                error(a.loc, "[[ivy::lt_ret]] expects exactly one lifetime argument");
                continue;
            }
            fn.returnLifetime = a.args[0];
        }
    }
    // Validate lt_ret references a declared lifetime.
    if (!fn.returnLifetime.empty() &&
        std::none_of(fn.lifetimes.begin(), fn.lifetimes.end(),
                     [&](const hir::Lifetime& l) { return l.name == fn.returnLifetime; })) {
        error(af.loc, "[[ivy::lt_ret(" + std::string(fn.returnLifetime) +
                          ")]]: lifetime '" + std::string(fn.returnLifetime) +
                          "' is not declared via [[ivy::lt_def(...)]]");
    }
}

std::string_view HirBuilder::lowerParamAttribute(hir::Function& fn, const Param& ap) {
    for (const Attribute& a : ap.attrs) {
        if (a.name != "lt") continue;
        if (a.args.size() != 1) {
            error(a.loc, "[[ivy::lt]] expects exactly one lifetime argument");
            continue;
        }
        const std::string_view lt = a.args[0];
        const bool declared =
            std::any_of(fn.lifetimes.begin(), fn.lifetimes.end(),
                        [&](const hir::Lifetime& l) { return l.name == lt; });
        if (!declared) {
            error(a.loc, "[[ivy::lt(" + std::string(lt) + ")]]: lifetime '" + std::string(lt) +
                             "' is not declared via [[ivy::lt_def(...)]]");
            continue;
        }
        if (ap.type.pointerDepth == 0) {
            error(a.loc, "[[ivy::lt]] requires a pointer parameter");
            continue;
        }
        return lt;
    }
    return {};
}

void HirBuilder::buildEnum(const EnumDecl& ed) {
    // Reject duplicate enum name.
    if (enums_.contains(ed.name)) {
        error(ed.loc, "redefinition of enum '" + std::string(ed.name) + "'");
        return;
    }
    EnumDef def;
    def.underlyingType = ed.underlyingType;
    def.isScoped = ed.isScoped;
    def.nsPrefix = std::string(ed.namespacePrefix);

    // Resolve each enumerator value. C++ rules:
    //   - First enumerator with no explicit value → 0.
    //   - Subsequent with no explicit value → prev + 1.
    //   - Explicit value → must be a compile-time integer constant.
    //     Ivy currently supports integer literals, unary +/-, and
    //     references to previously-defined enum constants.
    long long nextVal = 0;
    for (const Enumerator& en : ed.enumerators) {
        long long val = nextVal;
        if (en.value) {
            // Try to evaluate as a constant expression.
            const Expr& e = *en.value;
            const auto& n = e.node;
            if (std::holds_alternative<Expr::IntegerLit>(n)) {
                val = std::get<Expr::IntegerLit>(n).value;
            } else if (std::holds_alternative<Expr::Unary>(n)) {
                const auto& u = std::get<Expr::Unary>(n);
                if (u.op == "-" && u.operand &&
                    std::holds_alternative<Expr::IntegerLit>(u.operand->node)) {
                    val = -std::get<Expr::IntegerLit>(u.operand->node).value;
                } else if (u.op == "+" && u.operand &&
                           std::holds_alternative<Expr::IntegerLit>(u.operand->node)) {
                    val = std::get<Expr::IntegerLit>(u.operand->node).value;
                } else if (u.op == "-" && u.operand &&
                           std::holds_alternative<Expr::IdentRef>(u.operand->node)) {
                    const auto& ref = std::get<Expr::IdentRef>(u.operand->node);
                    auto it = def.constants.find(ref.name);
                    if (it != def.constants.end()) {
                        val = -it->second;
                    } else {
                        error(en.loc, "enumerator '" + std::string(en.name) +
                              "' value must be a constant expression");
                    }
                } else {
                    error(en.loc, "enumerator '" + std::string(en.name) +
                          "' value must be a constant expression");
                }
            } else if (std::holds_alternative<Expr::IdentRef>(n)) {
                // Reference to a previous enumerator in the same enum.
                const auto& ref = std::get<Expr::IdentRef>(n);
                auto it = def.constants.find(ref.name);
                if (it != def.constants.end()) {
                    val = it->second;
                } else {
                    error(en.loc, "enumerator '" + std::string(en.name) +
                          "' references undefined constant '" +
                          std::string(ref.name) + "'");
                }
            } else if (std::holds_alternative<Expr::Binary>(n)) {
                // Recursively evaluate a constant expression on integer
                // literals and enum constants (e.g. A | B, A + B, (A | B) | C).
                std::function<long long(const Expr&)> evalConst;
                evalConst = [&](const Expr& operand) -> long long {
                    const auto& on = operand.node;
                    if (std::holds_alternative<Expr::IntegerLit>(on)) {
                        return std::get<Expr::IntegerLit>(on).value;
                    }
                    if (std::holds_alternative<Expr::IdentRef>(on)) {
                        const auto& ref = std::get<Expr::IdentRef>(on);
                        auto it = def.constants.find(ref.name);
                        if (it != def.constants.end()) return it->second;
                        // Also check unscoped enum constants registered so far.
                        auto ec = enumConstants_.find(ref.name);
                        if (ec != enumConstants_.end()) return ec->second;
                        throw std::runtime_error("not constant");
                    }
                    if (std::holds_alternative<Expr::Unary>(on)) {
                        const auto& u = std::get<Expr::Unary>(on);
                        if (u.op == "-") return -evalConst(*u.operand);
                        if (u.op == "+") return evalConst(*u.operand);
                        if (u.op == "~") return ~evalConst(*u.operand);
                        throw std::runtime_error("unsupported unary");
                    }
                    if (std::holds_alternative<Expr::Binary>(on)) {
                        const auto& inner = std::get<Expr::Binary>(on);
                        long long lhs = evalConst(*inner.lhs);
                        long long rhs = evalConst(*inner.rhs);
                        if (inner.op == "+") return lhs + rhs;
                        if (inner.op == "-") return lhs - rhs;
                        if (inner.op == "*") return lhs * rhs;
                        if (inner.op == "/") return lhs / rhs;
                        if (inner.op == "%") return lhs % rhs;
                        if (inner.op == "|") return lhs | rhs;
                        if (inner.op == "&") return lhs & rhs;
                        if (inner.op == "^") return lhs ^ rhs;
                        if (inner.op == "<<") return lhs << rhs;
                        if (inner.op == ">>") return lhs >> rhs;
                        throw std::runtime_error("unsupported op");
                    }
                    throw std::runtime_error("not constant");
                };
                try {
                    val = evalConst(e);
                } catch (...) {
                    error(en.loc, "enumerator '" + std::string(en.name) +
                          "' value must be a constant expression");
                }
            } else {
                error(en.loc, "enumerator '" + std::string(en.name) +
                      "' value must be a constant expression");
            }
        }
        // Reject duplicate enumerator within the same enum.
        if (def.constants.contains(en.name)) {
            error(en.loc, "redefinition of enumerator '" + std::string(en.name) + "'");
            continue;
        }
        def.constants[en.name] = val;
        // For unscoped enums, register the bare name too.
        if (!ed.isScoped) {
            enumConstants_[en.name] = val;
        }
        nextVal = val + 1;
    }

    // Copy the enum declaration into the HIR TU.
    EnumDecl resolved;
    resolved.name = ed.name;
    resolved.namespacePrefix = ed.namespacePrefix;
    resolved.isScoped = ed.isScoped;
    resolved.underlyingType = ed.underlyingType;
    resolved.loc = ed.loc;
    for (const Enumerator& en : ed.enumerators) {
        Enumerator copy;
        copy.name = en.name;
        copy.loc = en.loc;
        copy.value = std::make_unique<Expr>();
        copy.value->loc = en.loc;
        copy.value->node = Expr::IntegerLit{def.constants[en.name]};
        resolved.enumerators.push_back(std::move(copy));
    }
    hir_->enums.push_back(std::move(resolved));

    enums_[ed.name] = std::move(def);
}

// Computes the size and alignment of a type for struct layout.
// Uses the same rank table as codegen's sizeofType() — Ivy fixed-width
// types and C-style types share a scale so layout matches codegen.
static std::uint64_t typeSize(const hir::Type& t) {
    if (t.pointerDepth > 0 || t.isReference) return 8;  // 64-bit pointers
    const std::string_view b = t.base;
    if (b == "bool" || b == "char" || b == "int8_t" || b == "uint8_t") return 1;
    if (b == "short" || b == "int16_t" || b == "uint16_t" || b == "float16_t" || b == "bfloat16_t") return 2;
    if (b == "int" || b == "unsigned" || b == "int32_t" || b == "uint32_t" ||
        b == "float" || b == "float32_t" || b == "size_t" || b == "ptrdiff_t") return 4;
    if (b == "long" || b == "long long" || b == "int64_t" || b == "uint64_t" ||
        b == "double" || b == "long double" || b == "float64_t" || b == "float128_t") return 8;
    return 4;  // fallback (e.g. unresolved enum → int)
}

static std::uint32_t typeAlign(const hir::Type& t) {
    // Alignment = size for all Ivy scalar types (no special alignment rules).
    return static_cast<std::uint32_t>(typeSize(t));
}

void HirBuilder::buildStruct(const StructDecl& sd) {
    // Reject duplicate struct name.
    if (structs_.contains(sd.name)) {
        error(sd.loc, "redefinition of struct '" + std::string(sd.name) + "'");
        return;
    }

    StructDef def;
    def.nsPrefix = std::string(sd.namespacePrefix);

    // Compute field layout: sequential offsets, each field aligned to
    // its natural alignment. The struct's overall alignment is the
    // max of all field alignments. The struct's size is rounded up
    // to the overall alignment (C ABI rule).
    std::uint64_t offset = 0;
    std::uint32_t structAlign = 1;
    for (std::size_t i = 0; i < sd.fields.size(); ++i) {
        const Field& f = sd.fields[i];
        const std::uint64_t sz = typeSize(f.type);
        const std::uint32_t align = typeAlign(f.type);
        // Pad to alignment.
        offset = (offset + align - 1) & ~(std::uint64_t(align - 1));
        def.fieldMap[f.name] = {i, offset, f.type};
        offset += sz;
        if (align > structAlign) structAlign = align;
    }
    def.size = (offset + structAlign - 1) & ~(std::uint64_t(structAlign - 1));
    def.align = structAlign;
    // Copy field type/name (skip `init` — not needed past HIR).
    for (const Field& f : sd.fields) {
        def.fields.push_back(Field{f.type, f.name, nullptr, f.loc});
        def.defaultInits.push_back(f.init.get());
    }

    // Copy the struct declaration into the HIR TU.  Field has
    // unique_ptr (move-only), so StructDecl is move-only.  However,
    // the original lives in ast::TranslationUnit and must remain
    // valid for later passes — we copy only the metadata needed
    // (name, namespacePrefix, isClass, loc) and rebuild the fields
    // vector by cloning each Field's type/name (skipping `init`).
    StructDecl resolved;
    resolved.name = sd.name;
    resolved.namespacePrefix = sd.namespacePrefix;
    resolved.isClass = sd.isClass;
    resolved.loc = sd.loc;
    for (const Field& f : sd.fields) {
        Field cf;
        cf.type = f.type;
        cf.name = f.name;
        cf.loc = f.loc;
        resolved.fields.push_back(std::move(cf));
    }
    hir_->structs.push_back(std::move(resolved));

    // Register member functions (methods).  Each method is registered
    // in `functions_` under its qualified name ("StructName::method")
    // with an implicit `this` parameter prepended.  The `this` param
    // is a reference to the struct type, so the method can mutate
    // the object.
    for (const Function& mf : sd.methods) {
        // Build the HIR function for this method.
        auto fn = std::make_unique<hir::Function>();
        fn->name = mf.name;  // "StructName::methodName"
        fn->namespacePrefix = mf.namespacePrefix;
        fn->isExternC = mf.isExternC;
        fn->isConstexpr = mf.isConstexpr;
        fn->isConsteval = mf.isConsteval;
        fn->returnType = mf.returnType;
        fn->loc = mf.loc;

        // Implicit `this` parameter: `StructName& this` (or
        // `const StructName& this` for const methods — Ivy doesn't
        // have const methods yet, so always non-const).
        hir::Param thisParam;
        thisParam.type.base = sd.name;
        thisParam.type.isReference = true;
        thisParam.name = "this";
        thisParam.loc = mf.loc;
        fn->params.push_back(std::move(thisParam));

        // User-declared parameters.
        for (const Param& ap : mf.params) {
            hir::Param p;
            p.type = ap.type;
            p.name = ap.name;
            p.loc = ap.loc;
            p.lifetime = lowerParamAttribute(*fn, ap);
            p.defaultValue = ap.defaultValue.get();
            fn->params.push_back(std::move(p));
        }

        // Register in functions_ under the qualified name.
        functions_[fn->name].push_back(fn.get());
        hir_->functions.push_back(std::move(fn));
    }

    // Now that def is fully built (fields + layout), move it into
    // the registry.  Method registration into def.methods/astMethods
    // happens after the move, via the reference in structs_.
    structs_[sd.name] = std::move(def);

    // Register methods in the struct's method table (under bare names).
    StructDef& defRef = structs_[sd.name];
    for (const Function& mf : sd.methods) {
        const std::string_view qualName = mf.name;
        const std::size_t pos = qualName.rfind("::");
        const std::string_view bareName =
            (pos != std::string_view::npos)
                ? qualName.substr(pos + 2)
                : qualName;
        // Find the HIR Function* we just registered.
        auto it = functions_.find(qualName);
        if (it != functions_.end() && !it->second.empty()) {
            defRef.methods[bareName].push_back(it->second.back());
        }
        defRef.astMethods.push_back(&mf);
    }
}

void HirBuilder::buildSignature(const Function& af) {
    // Template definitions are not registered in `functions_` — they are
    // stored in `templates_` and instantiated on demand.
    if (!af.tplParams.empty()) {
        templates_[af.name] = &af;
        return;
    }
    auto fn = std::make_unique<hir::Function>();
    fn->name = af.name;
    fn->namespacePrefix = af.namespacePrefix;
    fn->returnType = af.returnType;
    fn->isExternC = af.isExternC;
    fn->isConstexpr = af.isConstexpr;
    fn->isConsteval = af.isConsteval;
    fn->loc = af.loc;

    lowerLifetimeAttributes(*fn, af);

    for (const Param& ap : af.params) {
        hir::Param p;
        p.type = ap.type;
        p.name = ap.name;
        p.loc = ap.loc;
        p.lifetime = lowerParamAttribute(*fn, ap);
        p.defaultValue = ap.defaultValue.get();
        fn->params.push_back(std::move(p));
    }

    // Safety rule: a function definition that returns a pointer must declare
    // the returned pointer's lifetime. (Declarations — e.g. extern "C" C APIs
    // like malloc — are exempt.)
    if (af.body && af.returnType.pointerDepth > 0 && fn->returnLifetime.empty()) {
        error(af.loc, "function '" + std::string(af.name) +
                          "' returns a pointer but has no [[ivy::lt_ret(...)]] attribute");
    }

    // Check for redefinition and register the overload.
    auto& overloads = functions_[af.name];
    for (const auto* existing : overloads) {
        if (existing->body && af.body) {
            // Two definitions with bodies — check if they're truly the
            // same signature (same param count + types).  If so, it's a
            // redefinition error; if params differ, it's a valid overload.
            bool sameSig = (existing->params.size() == fn->params.size());
            if (sameSig) {
                for (std::size_t i = 0; i < fn->params.size() && sameSig; ++i) {
                    hir::Type a = existing->params[i].type; a.isReference = false;
                    hir::Type b = fn->params[i].type;    b.isReference = false;
                    if (!(a == b)) sameSig = false;
                }
            }
            if (sameSig) {
                error(af.loc, "redefinition of function '" + std::string(af.name) + "'");
            }
        }
    }
    hir::Function* raw = fn.get();
    hir_->functions.push_back(std::move(fn));
    overloads.push_back(raw);
}

// --- bodies (pass 2) ---

void HirBuilder::buildBody(hir::Function& fn, const Stmt::Compound& body) {
    current_ = &fn;
    hasReturnInBody_ = false;
    scopes_.push_back({});
    for (const hir::Param& p : fn.params) {
        if (!p.name.empty()) declare(p.name, p.type, p.loc);
    }
    std::unique_ptr<hir::Stmt> c = buildCompound(body, fn.loc);
    scopes_.pop_back();
    if (c) {
        fn.body = std::make_unique<hir::Stmt::Compound>(
            std::move(std::get<hir::Stmt::Compound>(c->node)));
    }
    if (!fn.returnType.isConst && fn.returnType.pointerDepth == 0 &&
        fn.returnType.base != "void" && !hasReturnInBody_) {
        error(fn.loc, "function '" + std::string(fn.name) +
                          "' may reach the end without returning a value");
    }
}

void HirBuilder::declare(std::string_view name, hir::Type type, SourceLoc loc) {
    auto& scope = scopes_.back();
    if (scope.contains(name)) {
        error(loc, "redefinition of variable '" + std::string(name) + "'");
        return;
    }
    scope.emplace(name, type);
}

bool HirBuilder::isAssignable(const hir::Type& to, const hir::Type& from) const {
    if (from.base == "nullptr") return to.pointerDepth > 0 || to.isReference;
    if (from.isReference) {
        // Reference can bind to reference of same type.
        if (to.isReference && to.base == from.base &&
            (!to.isConst || from.isConst) &&
            to.pointerDepth == from.pointerDepth) return true;
    }
    // T&  can bind to lvalue of type T (or convertible).
    // const T&  can bind to anything (incl. rvalue).
    if (to.isReference) {
        if (to.pointerDepth == 0 && from.pointerDepth == 0) {
            // Reference to scalar — allow narrowing like C++.
            if (isNumeric(to) && isNumeric(from)) return true;
            // Reference to struct — `T&` binds to `T` (lvalue).
            if (structs_.contains(to.base) && to.base == from.base)
                return true;
        }
        // Reference to pointer.
        if (to.pointerDepth == from.pointerDepth && to.base == from.base &&
            (!to.isConst || from.isConst)) return true;
    }
    // Assigning TO a reference is impossible (re-seating is not allowed in Ivy).
    // Reference-to-reference assignment not allowed.
    if (to.pointerDepth == 0 && from.pointerDepth == 0) {
        // Struct-to-struct assignment: same type only (no implicit conversions).
        if (structs_.contains(to.base) && to.base == from.base &&
            !to.isConst == !from.isConst) return true;
        // Enum-to-enum: same enum type (or enum constant folded to int).
        if (enums_.contains(to.base)) {
            // Same enum type, or int → enum (enum constants fold to int).
            return to.base == from.base || isNumeric(from);
        }
        // Enum → numeric: implicit enum-to-int conversion (e.g. returning
        // an enum variable from a function returning int32_t). Enum values
        // are represented as integers at runtime, so this is safe.
        if (enums_.contains(from.base) && isNumeric(to)) return true;
        return isNumeric(to) && isNumeric(from);
    }
    if (to.pointerDepth != from.pointerDepth) return false;
    if (to.base != from.base) return false;
    if (!to.isConst && from.isConst) return false;
    return true;
}

bool HirBuilder::checkCondition(const hir::Expr& e) {
    const hir::Type& t = e.type;
    if (isNumeric(t) || t.pointerDepth > 0 || t.base == "nullptr") return true;
    error(e.loc, "condition must be numeric, a pointer, or nullptr (got '" +
                     typeToString(t) + "')");
    return false;
}

void HirBuilder::requireUnsafe(SourceLoc loc, std::string_view what) {
    if (unsafeDepth_ == 0) {
        error(loc, std::string(what) + " requires an [[ivy::unsafe]] block");
    }
}

std::unique_ptr<hir::Stmt> HirBuilder::buildCompound(const Stmt::Compound& c, SourceLoc loc) {
    auto out = std::make_unique<hir::Stmt>();
    out->loc = loc;
    auto& compound = out->node.emplace<hir::Stmt::Compound>();
    scopes_.push_back({});
    for (const auto& s : c.stmts) compound.stmts.push_back(buildStmt(*s));
    scopes_.pop_back();
    return out;
}

std::unique_ptr<hir::Stmt> HirBuilder::buildDeclaration(const Stmt::Decl& d, SourceLoc loc,
                                                        bool checkInit) {
    auto out = std::make_unique<hir::Stmt>();
    out->loc = loc;
    auto& decl = out->node.emplace<hir::Stmt::Decl>();
    decl.type = d.type;
    decl.name = d.name;

    if (d.type.pointerDepth == 0 && d.type.base == "void") {
        error(loc, "variable '" + std::string(d.name) + "' cannot have type void");
    }

    // `auto` type deduction: infer type from the initializer expression.
    // `auto x = expr;`  →  type = expr.type (with pointer/ref from the declared auto)
    // `auto* p = &x;`   →  type = expr.type (already a pointer)
    // `auto& r = x;`    →  type = expr.type with isReference = true
    const bool isAuto = (d.type.base == "auto");
    if (isAuto) {
        if (!d.init) {
            error(loc, "'auto' variable '" + std::string(d.name) +
                           "' must have an initializer");
            declare(d.name, d.type, loc);
            return out;
        }
        decl.init = buildExpr(*d.init);
        if (!decl.init) {
            declare(d.name, d.type, loc);
            return out;
        }
        // Infer the concrete type from the initializer.
        hir::Type inferred = decl.init->type;
        // Propagate pointer/ref qualifiers from the `auto` declaration:
        // e.g. `auto* p = expr` keeps inferred pointer depth but adds declared depth.
        inferred.pointerDepth += d.type.pointerDepth;
        if (d.type.isReference) inferred.isReference = true;
        if (d.type.isConst) inferred.isConst = true;
        // Strip reference from plain `auto x = ref_expr` (copy semantics).
        if (!d.type.isReference) inferred.isReference = false;
        decl.type = inferred;
        declare(d.name, inferred, loc);
        return out;
    }

    if (d.init) {
        // Aggregate init list for a struct: `Point p = {1, 2};`
        // Resolve each element against the corresponding field type so
        // implicit conversions (e.g. int → int32_t) are applied.
        if (auto* il = std::get_if<ivy::Expr::InitList>(&d.init->node)) {
            decl.init = buildStructInit(*il, d.type, d.name, loc);
        } else {
            decl.init = buildExpr(*d.init);
            if (decl.init && !isAssignable(d.type, decl.init->type)) {
                error(loc, "cannot initialize variable '" + std::string(d.name) + "' of type '" +
                               typeToString(d.type) + "' with a value of type '" +
                               typeToString(decl.init->type) + "'");
            }
        }
    } else if (checkInit) {
        // Array variables (T[N]) are zero-initialized — no explicit
        // initializer required (like C arrays and Ivy struct variables).
        if (d.type.arraySize > 0) {
            // No error — codegen will emit zeroinitializer for the [N x T] alloca.
        } else if (structs_.contains(d.type.base)) {
            // Struct variables are zero-initialized (like C) — no explicit
            // initializer required.  All other types must be initialized.
            // Synthesize an aggregate initializer from default member
            // initializers (if any). Fields without a default are
            // zero-initialized by `lowerInitListInto` (which emits a
            // `store ... zeroinitializer` first). This preserves C
            // value-initialization semantics while honoring `= default`.
            const StructDef& def = structs_[d.type.base];
            bool anyDefault = false;
            for (auto* p : def.defaultInits) if (p) { anyDefault = true; break; }
            if (anyDefault) {
                // Build an InitList with one element per field, preserving
                // field indices: fields with a default get a clone of the
                // default expression; fields without get a nullptr
                // placeholder (zero-initialized by codegen).
                Expr::InitList il;
                for (auto* p : def.defaultInits) {
                    if (p) il.elements.push_back(cloneExpr(*p));
                    else il.elements.push_back(nullptr);
                }
                decl.init = buildStructInit(il, d.type, d.name, loc);
            }
        } else {
            error(loc, "variable '" + std::string(d.name) +
                           "' must be initialized (uninitialized variables are not allowed)");
        }
    }
    declare(d.name, d.type, loc);
    return out;
}

std::unique_ptr<hir::Stmt> HirBuilder::buildReturn(const Stmt::Return& r, SourceLoc loc) {
    auto out = std::make_unique<hir::Stmt>();
    out->loc = loc;
    auto& ret = out->node.emplace<hir::Stmt::Return>();
    hasReturnInBody_ = true;
    if (r.value) {
        ret.value = buildExpr(*r.value);
        const hir::Type& rt = current_->returnType;
        if (rt.pointerDepth == 0 && rt.base == "void") {
            error(loc, "void function cannot return a value");
        } else if (ret.value && !isAssignable(rt, ret.value->type)) {
            error(loc, "cannot return value of type '" + typeToString(ret.value->type) +
                           "' from function returning '" + typeToString(rt) + "'");
        }
    } else if (current_->returnType.pointerDepth == 0 && current_->returnType.base != "void") {
        error(loc, "function '" + std::string(current_->name) +
                       "' must return a value of type '" +
                       typeToString(current_->returnType) + "'");
    }
    return out;
}

std::unique_ptr<hir::Stmt> HirBuilder::buildStmt(const Stmt& s) {
    auto out = std::make_unique<hir::Stmt>();
    out->loc = s.loc;
    const auto& n = s.node;
    using A = Stmt;

    if (std::holds_alternative<A::Compound>(n)) {
        return buildCompound(std::get<A::Compound>(n), s.loc);
    }
    if (std::holds_alternative<A::Decl>(n)) {
        return buildDeclaration(std::get<A::Decl>(n), s.loc, /*checkInit=*/true);
    }
    if (std::holds_alternative<A::Null>(n)) {
        out->node = hir::Stmt::Null{};
        return out;
    }
    if (std::holds_alternative<A::Break>(n)) {
        out->node = hir::Stmt::Break{};
        return out;
    }
    if (std::holds_alternative<A::Continue>(n)) {
        out->node = hir::Stmt::Continue{};
        return out;
    }
    if (std::holds_alternative<A::If>(n)) {
        const A::If& v = std::get<A::If>(n);
        auto& ifs = out->node.emplace<hir::Stmt::If>();
        ifs.cond = buildExpr(*v.cond);
        if (ifs.cond) checkCondition(*ifs.cond);
        ifs.thenBranch = buildStmt(*v.thenBranch);
        if (v.elseBranch) ifs.elseBranch = buildStmt(*v.elseBranch);
        return out;
    }
    if (std::holds_alternative<A::While>(n)) {
        const A::While& v = std::get<A::While>(n);
        auto& wh = out->node.emplace<hir::Stmt::While>();
        wh.cond = buildExpr(*v.cond);
        if (wh.cond) checkCondition(*wh.cond);
        wh.body = buildStmt(*v.body);
        return out;
    }
    if (std::holds_alternative<A::DoWhile>(n)) {
        const A::DoWhile& v = std::get<A::DoWhile>(n);
        auto& dw = out->node.emplace<hir::Stmt::DoWhile>();
        dw.body = buildStmt(*v.body);
        dw.cond = buildExpr(*v.cond);
        if (dw.cond) checkCondition(*dw.cond);
        return out;
    }
    if (std::holds_alternative<A::For>(n)) {
        const A::For& v = std::get<A::For>(n);
        auto& fr = out->node.emplace<hir::Stmt::For>();
        scopes_.push_back({});
        if (v.init) fr.init = buildStmt(*v.init);
        if (v.cond) {
            fr.cond = buildExpr(*v.cond);
            if (fr.cond) checkCondition(*fr.cond);
        }
        if (v.incr) fr.incr = buildExpr(*v.incr);
        fr.body = buildStmt(*v.body);
        scopes_.pop_back();
        return out;
    }
    if (std::holds_alternative<A::Return>(n)) {
        return buildReturn(std::get<A::Return>(n), s.loc);
    }
    if (std::holds_alternative<A::ExprStmt>(n)) {
        auto& es = out->node.emplace<hir::Stmt::ExprStmt>();
        es.value = buildExpr(*std::get<A::ExprStmt>(n).value);
        return out;
    }
    if (std::holds_alternative<A::Unsafe>(n)) {
        auto& us = out->node.emplace<hir::Stmt::Unsafe>();
        ++unsafeDepth_;
        us.body = buildStmt(*std::get<A::Unsafe>(n).body);
        --unsafeDepth_;
        return out;
    }
    if (std::holds_alternative<A::Switch>(n)) {
        const A::Switch& v = std::get<A::Switch>(n);
        auto& sw = out->node.emplace<hir::Stmt::Switch>();
        sw.cond = buildExpr(*v.cond);
        // Condition must be an integer type.
        if (sw.cond) {
            const auto& t = sw.cond->type;
            bool isIntegral = (!t.base.empty() && t.pointerDepth == 0 && !t.isReference &&
                               (t.base == "bool" ||
                                t.base.find("int") != std::string_view::npos ||
                                t.base.find("char") != std::string_view::npos ||
                                t.base == "long" || t.base == "short" || t.base == "unsigned" ||
                                t.base == "long long" || t.base == "unsigned long" ||
                                t.base == "unsigned long long"));
            if (!isIntegral)
                error(s.loc, "switch condition must be an integral type");
        }
        bool hasDefault = false;
        for (const auto& ac : v.cases) {
            hir::Stmt::CaseClause cc;
            if (ac.value) {
                cc.value = buildExpr(*ac.value);
            } else {
                if (hasDefault)
                    error(s.loc, "switch has more than one 'default' case");
                hasDefault = true;
            }
            for (const auto& st : ac.stmts)
                cc.stmts.push_back(buildStmt(*st));
            // Ivy no-fallthrough: last stmt of each case must be break/return/continue/switch.
            // We emit a compile error only if the case is non-empty and doesn't end that way.
            if (!cc.stmts.empty()) {
                const hir::Stmt* last = cc.stmts.back().get();
                bool terminated = std::holds_alternative<hir::Stmt::Break>(last->node) ||
                                  std::holds_alternative<hir::Stmt::Return>(last->node) ||
                                  std::holds_alternative<hir::Stmt::Continue>(last->node);
                if (!terminated)
                    error(s.loc, "Ivy forbids implicit fallthrough: case must end with "
                                 "break, return, or continue");
            }
            sw.cases.push_back(std::move(cc));
        }
        return out;
    }
    out->node = hir::Stmt::Null{};
    return out;
}

// --- namespace helpers ---

std::vector<hir::Function*> HirBuilder::resolveOverloads(std::string_view name) const {
    std::vector<hir::Function*> result;
    // 1. Try the name as-is (works for fully-qualified and global names).
    auto it = functions_.find(name);
    if (it != functions_.end()) result = it->second;
    // 2. Try prefixing with the current namespace (bare call inside a
    //    namespace body resolving to a same-namespace function).
    if (result.empty() && !currentNsPrefix_.empty()) {
        std::string qualified;
        qualified.reserve(currentNsPrefix_.size() + name.size());
        qualified += currentNsPrefix_;
        qualified += name;
        auto it2 = functions_.find(qualified);
        if (it2 != functions_.end()) result = it2->second;
    }
    return result;
}

hir::Function* HirBuilder::resolveFunction(std::string_view name) const {
    auto overloads = resolveOverloads(name);
    return overloads.empty() ? nullptr : overloads[0];
}

hir::Function* HirBuilder::resolveOverload(
        const std::vector<hir::Function*>& candidates,
        const std::vector<hir::Type>& argTypes,
        SourceLoc loc) {
    if (candidates.empty()) return nullptr;
    if (candidates.size() == 1) {
        // Fast path: single candidate — skip ranking (checkCall does
        // the detailed arg-type checking later).
        return candidates[0];
    }
    // Rank each candidate by how well its param types match argTypes.
    // Score: 0 = no match, higher = better.
    auto rank = [this](const hir::Function* fn,
                   const std::vector<hir::Type>& args) -> int {
        const std::size_t expected = fn->params.size();
        const std::size_t actual = args.size();
        // Variadic extern "C" functions: must have at least expected args.
        if (expected > actual) {
            // Check if the missing params all have default values.
            for (std::size_t i = actual; i < expected; ++i) {
                if (!fn->params[i].defaultValue) return 0;
            }
            // OK — trailing args will be filled at call site.
        } else if (!fn->isExternC && expected != actual) {
            return 0;
        }
        int score = 0;
        for (std::size_t i = 0; i < expected; ++i) {
            const hir::Type& pt = fn->params[i].type;
            // Skip params that will be filled by default values —
            // they don't need a user-provided argument.
            if (i >= actual) {
                // Default-arg params contribute a moderate score so
                // that a candidate with fewer required args is preferred
                // over one that needs more args (but both can match).
                score += 80;  // default-arg match (less than exact)
                continue;
            }
            const hir::Type& at = args[i];
            // Strip reference-ness for comparison (reference params bind
            // to lvalues, but the type is the same).
            hir::Type p = pt; p.isReference = false;
            hir::Type a = at; a.isReference = false;
            if (p == a) {
                score += 100;  // exact match
            } else if (isNumeric(p) && isNumeric(a)) {
                // Promotion: e.g. int → int64_t, float → double.
                // Rank by width closeness: the closer the widths, the
                // better the match.  Exact width match (even if base
                // names differ, e.g. int vs int32_t) is near-exact.
                int pw = typeWidth(p.base);
                int aw = typeWidth(a.base);
                if (pw > 0 && aw > 0) {
                    int diff = (pw > aw) ? (pw - aw) : (aw - pw);
                    // Score: 90 - diff (so 0-diff = 90, 32-diff = 58, ...)
                    // This makes narrower promotions better than wider ones.
                    score += 90 - diff;
                } else {
                    score += 50;  // unknown widths — generic promotion
                }
            } else if (isAssignable(pt, at)) {
                score += 1;  // implicit conversion (lowest rank)
            } else {
                return 0;  // param type doesn't match at all
            }
        }
        return score;
    };
    int bestScore = 0;
    hir::Function* best = nullptr;
    bool ambiguous = false;
    for (hir::Function* fn : candidates) {
        int s = rank(fn, argTypes);
        if (s > bestScore) { bestScore = s; best = fn; ambiguous = false; }
        else if (s == bestScore && s > 0) { ambiguous = true; }
    }
    if (ambiguous) {
        error(loc, "ambiguous call to overloaded function");
        return nullptr;
    }
    if (!best) {
        error(loc, "no matching overload for argument types");
    }
    return best;
}

bool HirBuilder::flattenMemberChain(const Expr& e, std::string& out) const {
    // Base case: IdentRef.
    if (std::holds_alternative<Expr::IdentRef>(e.node)) {
        out = std::get<Expr::IdentRef>(e.node).name;
        return true;
    }
    // Recursive case: Member from `A::B` (scope resolution only).
    // `.`/`->` member access must NOT be treated as scope resolution —
    // otherwise `obj.method(args)` would be misclassified as a qualified
    // call `obj::method(args)`.
    if (std::holds_alternative<Expr::Member>(e.node)) {
        const auto& m = std::get<Expr::Member>(e.node);
        if (m.isArrow) return false;  // `->` is not scope resolution
        if (!m.isScope) return false;  // `.` is not scope resolution
        if (!m.base) return false;
        std::string base;
        if (!flattenMemberChain(*m.base, base)) return false;
        out = base + "::" + std::string(m.name);
        return true;
    }
    return false;
}

// --- expressions ---

void HirBuilder::checkCall(hir::Expr::Call& call, SourceLoc loc) {
    const hir::Function* fn = call.target;
    if (!fn) return;  // error already reported

    const std::size_t expected = fn->params.size();
    const std::size_t actual = call.args.size();
    const bool variadic = fn->isExternC;

    // Fill missing arguments with default values.  In C++, default
    // arguments are evaluated at the call site — so we clone the AST
    // default-value expression and build a fresh HIR expr for each
    // call site that omits trailing arguments.
    if (actual < expected && !variadic) {
        // Only trailing params can have defaults — and once a param
        // has no default, all subsequent params must also be provided.
        for (std::size_t i = actual; i < expected; ++i) {
            const hir::Param& p = fn->params[i];
            if (!p.defaultValue) {
                error(loc, "call to '" + std::string(call.callee) +
                               "' missing argument " + std::to_string(i + 1) +
                               " (parameter '" + std::string(p.name) +
                               "' has no default value)");
                return;
            }
            // Clone the AST default-value expr and build it as a HIR expr.
            auto clonedAst = ivy::cloneExpr(*p.defaultValue);
            call.args.push_back(buildExpr(*clonedAst));
        }
    }

    const std::size_t newActual = call.args.size();
    // Variadic extern "C" functions (e.g. printf) accept extra args.
    if (expected > newActual || (!variadic && expected != newActual)) {
        error(loc, "call to '" + std::string(call.callee) + "' expects " + std::to_string(expected) +
                       " argument(s), got " + std::to_string(newActual));
        return;
    }
    // Check fixed params (extra variadic args are unchecked — C ABI).
    for (std::size_t i = 0; i < expected && i < newActual; ++i) {
        if (call.args[i] && !isAssignable(fn->params[i].type, call.args[i]->type)) {
            error(call.args[i]->loc, "argument " + std::to_string(i + 1) + " of '" +
                                         std::string(call.callee) + "' expects '" +
                                         typeToString(fn->params[i].type) + "', got '" +
                                         typeToString(call.args[i]->type) + "'");
        }
    }
}

std::unique_ptr<hir::Expr> HirBuilder::buildStructInit(const Expr::InitList& il,
                                                       const hir::Type& structType,
                                                       [[maybe_unused]] std::string_view varName,
                                                       SourceLoc loc) {
    auto out = std::make_unique<hir::Expr>();
    out->loc = loc;
    // InitList is only supported for struct types (aggregate initialization).
    if (structType.pointerDepth > 0 || !structs_.contains(structType.base)) {
        error(loc, "braced initializer list can only initialize a struct variable, not '" +
                       typeToString(structType) + "'");
        out->type = dummyType();
        return out;
    }
    const StructDef& def = structs_[structType.base];

    // Empty `{}` is a value-initializer — all fields default-initialized
    // (zero for scalars, default member init if present, recursively for
    // nested structs).  We still emit an InitList node so codegen can apply
    // default member initializers when present.
    if (il.elements.size() > def.fields.size()) {
        error(loc, "too many initializers for struct '" + std::string(structType.base) +
                       "' (expected at most " + std::to_string(def.fields.size()) +
                       ", got " + std::to_string(il.elements.size()) + ")");
        out->type = structType;
        return out;
    }

    auto& hlist = out->node.emplace<hir::Expr::InitList>();
    for (std::size_t i = 0; i < il.elements.size(); ++i) {
        const auto& elem = il.elements[i];
        // A nullptr element is a placeholder for a field that has no
        // explicit initializer and no default member initializer — it
        // will be zero-initialized by codegen. Preserve the slot so
        // field indices stay aligned.
        if (!elem) {
            hlist.elements.push_back(nullptr);
            continue;
        }
        // Look up field type from the StructDef fieldMap (by index — fields
        // are sequential). Use the StructDecl field order directly.
        hir::Type fieldType = def.fields[i].type;
        auto built = buildExpr(*elem);
        if (built && !isAssignable(fieldType, built->type)) {
            error(built->loc, "field '" + std::string(def.fields[i].name) + "' of struct '" +
                                  std::string(structType.base) + "' expects '" +
                                  typeToString(fieldType) + "', got '" +
                                  typeToString(built->type) + "'");
        }
        hlist.elements.push_back(std::move(built));
    }
    // Trailing fields without an explicit initializer: apply default
    // member initializers (e.g. `Point p = {1};` where `y` has `= 0`).
    // Fields without a default are left to codegen, which zero-inits
    // them via `store ... zeroinitializer`.
    for (std::size_t i = il.elements.size(); i < def.fields.size(); ++i) {
        if (i < def.defaultInits.size() && def.defaultInits[i]) {
            auto built = buildExpr(*def.defaultInits[i]);
            hlist.elements.push_back(std::move(built));
        } else {
            // No default — push a nullptr placeholder so the MIR/codegen
            // can still map field indices correctly. Codegen skips null
            // elements (leaving the field zero-initialized).
            hlist.elements.push_back(nullptr);
        }
    }
    out->type = structType;
    return out;
}

std::unique_ptr<hir::Expr> HirBuilder::buildExpr(const Expr& e) {
    auto out = std::make_unique<hir::Expr>();
    out->loc = e.loc;
    const auto& n = e.node;
    using A = Expr;

    if (std::holds_alternative<A::IntegerLit>(n)) {
        out->node = hir::Expr::IntegerLit{std::get<A::IntegerLit>(n).value};
        out->type = intType();
        return out;
    }
    if (std::holds_alternative<A::FloatLit>(n)) {
        out->node = hir::Expr::FloatLit{std::get<A::FloatLit>(n).value};
        out->type = floatType();
        return out;
    }
    if (std::holds_alternative<A::StringLit>(n)) {
        out->node = hir::Expr::StringLit{std::get<A::StringLit>(n).raw};
        out->type = constCharPtrType();
        return out;
    }
    if (std::holds_alternative<A::CharLit>(n)) {
        out->node = hir::Expr::CharLit{std::get<A::CharLit>(n).raw};
        hir::Type t;
        t.base = "char";
        out->type = t;
        return out;
    }
    if (std::holds_alternative<A::BoolLit>(n)) {
        out->node = hir::Expr::BoolLit{std::get<A::BoolLit>(n).value};
        out->type = boolType();
        return out;
    }
    if (std::holds_alternative<A::NullptrLit>(n)) {
        out->node = hir::Expr::NullptrLit{};
        out->type = nullptrType();
        return out;
    }
    if (std::holds_alternative<A::This>(n)) {
        // `this` — resolve to the implicit `this` parameter of the
        // current method.  It's registered in the scope as "this"
        // with type `StructType&` (reference).
        out->node = hir::Expr::This{};
        // Look up "this" in variable scopes.
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto hit = it->find("this");
            if (hit != it->end()) {
                out->type = hit->second;
                return out;
            }
        }
        error(e.loc, "'this' is only valid inside a method body");
        out->type = dummyType();
        return out;
    }
    if (std::holds_alternative<A::IdentRef>(n)) {
        const std::string_view name = std::get<A::IdentRef>(n).name;
        // Check variable scopes first.
        bool found = false;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto hit = it->find(name);
            if (hit != it->end()) {
                out->node = hir::Expr::IdentRef{name};
                out->type = hit->second;
                found = true;
                break;
            }
        }
        if (!found) {
            // Check unscoped enum constants — fold to IntegerLit with
            // the enum's underlying type. This makes MIR/codegen need
            // zero changes (IntegerLit is already handled everywhere).
            auto ecIt = enumConstants_.find(name);
            if (ecIt != enumConstants_.end()) {
                out->node = hir::Expr::IntegerLit{ecIt->second};
                out->type = intType();
                found = true;
            }
        }
        if (!found) {
            // Unscoped enum constant lookup failed — report error.
            // Scoped enum constants are only accessible via `EnumName::Value`
            // (handled in the Member branch), never as bare names.
            out->node = hir::Expr::IdentRef{name};
            if (functions_.contains(name)) {
                error(e.loc, "function '" + std::string(name) +
                                 "' cannot be used as a value (no function pointers)");
            } else {
                error(e.loc, "undeclared identifier '" + std::string(name) + "'");
            }
            out->type = dummyType();
        }
        return out;
    }
    if (std::holds_alternative<A::Unary>(n)) {
        const A::Unary& v = std::get<A::Unary>(n);
        auto& un = out->node.emplace<hir::Expr::Unary>();
        un.op = v.op;
        un.isPrefix = v.isPrefix;
        un.operand = buildExpr(*v.operand);
        if (!un.operand) return out;
        const hir::Type& ot = un.operand->type;

        if (v.op == "!" ) {
            if (!isNumeric(ot) && ot.pointerDepth == 0) {
                error(e.loc, "'!' expects a numeric or pointer operand");
            }
            out->type = boolType();
            return out;
        }
        if (v.op == "~") {
            if (!isIntegerLike(ot)) {
                error(e.loc, "'~' expects an integer operand");
            }
            out->type = stripReference(ot);
            return out;
        }
        if (v.op == "-" || v.op == "+") {
            if (!isNumeric(ot)) error(e.loc, "unary '" + std::string(v.op) + "' expects a numeric operand");
            out->type = stripReference(ot);
            return out;
        }
        if (v.op == "*") {
            if (ot.pointerDepth == 0) {
                error(e.loc, "'*' expects a pointer operand");
                out->type = dummyType();
                return out;
            }
            requireUnsafe(e.loc, "pointer dereference");
            out->type = ot;
            --out->type.pointerDepth;
            return out;
        }
        if (v.op == "&") {
            if (!std::holds_alternative<hir::Expr::IdentRef>(un.operand->node)) {
                error(e.loc, "'&' is only supported on variables in the Ivy subset");
                out->type = dummyType();
                return out;
            }
            out->type = ot;
            ++out->type.pointerDepth;
            return out;
        }
        if (v.op == "++" || v.op == "--") {
            if (ot.pointerDepth > 0) {
                requireUnsafe(e.loc, "pointer increment/decrement");
            } else if (!isNumeric(ot)) {
                error(e.loc, "'" + std::string(v.op) + "' expects a numeric or pointer operand");
            }
            out->type = ot;
            return out;
        }
        out->type = dummyType();
        return out;
    }
    if (std::holds_alternative<A::Binary>(n)) {
        const A::Binary& v = std::get<A::Binary>(n);
        auto& bin = out->node.emplace<hir::Expr::Binary>();
        bin.op = v.op;
        bin.lhs = buildExpr(*v.lhs);
        bin.rhs = buildExpr(*v.rhs);
        if (!bin.lhs || !bin.rhs) return out;
        const hir::Type& lt = bin.lhs->type;
        const hir::Type& rt = bin.rhs->type;
        const bool lNum = isNumeric(lt), rNum = isNumeric(rt);
        const bool lPtr = lt.pointerDepth > 0, rPtr = rt.pointerDepth > 0;
        const bool lNull = lt.base == "nullptr", rNull = rt.base == "nullptr";

        if (v.op == "==" || v.op == "!=") {
            const bool ok = (lNum && rNum) || (lPtr && rPtr) || (lNull && rPtr) || (rNull && lPtr);
            if (!ok) error(e.loc, "'" + std::string(v.op) + "' between incompatible types");
            out->type = boolType();
            return out;
        }
        if (v.op == "<" || v.op == ">" || v.op == "<=" || v.op == ">=") {
            if (lPtr || rPtr) {
                if (!(lPtr && rPtr)) {
                    error(e.loc, "'" + std::string(v.op) + "' between pointer and non-pointer");
                }
            } else if (!(lNum && rNum)) {
                error(e.loc, "'" + std::string(v.op) + "' expects numeric operands");
            }
            out->type = boolType();
            return out;
        }
        if (v.op == "&&" || v.op == "||") {
            if (!(lNum || lPtr) || !(rNum || rPtr)) {
                error(e.loc, "'" + std::string(v.op) + "' expects numeric or pointer operands");
            }
            out->type = boolType();
            return out;
        }
        if (v.op == "<<" || v.op == ">>" || v.op == "&" || v.op == "|" || v.op == "^") {
            if (!lNum || !rNum) {
                error(e.loc, "'" + std::string(v.op) + "' expects integer operands");
            }
            out->type = promoteTypes(lt, rt);
            return out;
        }
        if (v.op == "+" || v.op == "-") {
            if (lPtr || rPtr) {
                requireUnsafe(e.loc, "pointer arithmetic");
                if (lPtr && rPtr) {
                    error(e.loc, "pointer subtraction is not supported in the Ivy subset");
                    out->type = dummyType();
                    return out;
                }
                if (lPtr && !rNum) {
                    error(e.loc, "'" + std::string(v.op) + "' with pointer and non-numeric operand");
                }
                if (rPtr && !lNum) {
                    error(e.loc, "'" + std::string(v.op) + "' with pointer and non-numeric operand");
                }
                out->type = lPtr ? lt : rt;  // result is a pointer
                return out;
            }
            if (!(lNum && rNum)) {
                error(e.loc, "'" + std::string(v.op) + "' expects numeric operands");
                out->type = dummyType();
                return out;
            }
            out->type = promoteTypes(lt, rt);
            return out;
        }
        if (v.op == "*" || v.op == "/" || v.op == "%") {
            if (!(lNum && rNum)) {
                error(e.loc, "'" + std::string(v.op) + "' expects numeric operands");
                out->type = dummyType();
                return out;
            }
            if (v.op == "%" && (!isIntegerLike(lt) || !isIntegerLike(rt))) {
                error(e.loc, "'%' expects integer operands");
            }
            out->type = promoteTypes(lt, rt);
            return out;
        }
        out->type = dummyType();
        return out;
    }
    if (std::holds_alternative<A::Ternary>(n)) {
        const A::Ternary& v = std::get<A::Ternary>(n);
        auto& ter = out->node.emplace<hir::Expr::Ternary>();
        ter.cond = buildExpr(*v.cond);
        if (ter.cond) checkCondition(*ter.cond);
        ter.thenBranch = buildExpr(*v.thenBranch);
        ter.elseBranch = buildExpr(*v.elseBranch);
        if (ter.thenBranch && ter.elseBranch) {
            // Ternary type is the promoted type of the two branches.
            out->type = promoteTypes(ter.thenBranch->type, ter.elseBranch->type);
            if (out->type.base == "char" || out->type.base == "bool" ||
                out->type.base == "void" || out->type.base == "") {
                // Fallback: if the branches are not both numeric, use the first.
                // (E.g. pointer branches -> keep as-is.)
                out->type = ter.thenBranch->type;
            }
        } else {
            out->type = dummyType();
        }
        return out;
    }
    if (std::holds_alternative<A::Call>(n)) {
        const A::Call& v = std::get<A::Call>(n);
        auto& call = out->node.emplace<hir::Expr::Call>();
        const Expr* callee = v.callee.get();
        // Copy template args (AST Type → HIR Type, same struct).
        for (const auto& ta : v.tplArgs) call.tplArgs.push_back(ta);
        // Build arguments upfront so their types are available for
        // overload resolution.  (Lambda calls add a closure-pointer arg
        // as the first element; we handle that in the lambda branch.)
        for (const auto& a : v.args) call.args.push_back(buildExpr(*a));
        // Bare name call: `func(args)` — resolve with namespace fallback.
        if (std::holds_alternative<A::IdentRef>(callee->node)) {
            const std::string_view bareName = std::get<A::IdentRef>(callee->node).name;
            // Template-id call: `func<T>(args)` — instantiate template.
            if (!call.tplArgs.empty()) {
                const Function* tplFunc = lookupTemplate(bareName);
                if (tplFunc) {
                    call.target = instantiateTemplate(*tplFunc, bareName,
                                                      call.tplArgs, e.loc);
                    if (call.target) {
                        call.callee = call.target->name;
                    } else {
                        call.callee = bareName;
                        error(e.loc, "failed to instantiate template '" + std::string(bareName) + "'");
                    }
                } else {
                    call.callee = bareName;
                    error(e.loc, "'" + std::string(bareName) + "' is not a template");
                }
            } else {
                auto overloads = resolveOverloads(bareName);
                if (overloads.empty()) {
                    call.callee = bareName;
                    error(callee->loc, "call to undeclared function '" + std::string(bareName) + "'");
                } else {
                    std::vector<hir::Type> argTypes;
                    argTypes.reserve(call.args.size());
                    for (const auto& a : call.args) {
                        argTypes.push_back(a ? a->type : dummyType());
                    }
                    call.target = resolveOverload(overloads, argTypes, e.loc);
                    if (call.target) {
                        call.callee = call.target->name;
                    } else {
                        call.callee = bareName;
                    }
                }
            }
        } else if (std::holds_alternative<A::Member>(callee->node)) {
            const auto& mem = std::get<A::Member>(callee->node);
            // Try qualified call first: `ns::func(args)` — flatten `::` chain.
            std::string qualified;
            if (flattenMemberChain(*callee, qualified)) {
                stringStorage_.push_back(std::move(qualified));
                call.callee = stringStorage_.back();
                call.target = resolveFunction(call.callee);
                if (!call.target) {
                    error(callee->loc, "call to undeclared function '" + std::string(call.callee) + "'");
                }
            } else {
                // Method call: `obj.method(args)` or `obj->method(args)`.
                // Build the base expression to determine its type.
                auto baseExpr = buildExpr(*mem.base);
                if (!baseExpr) {
                    out->type = dummyType();
                    return out;
                }
                hir::Type baseType = baseExpr->type;
                if (mem.isArrow) {
                    if (baseType.pointerDepth == 0) {
                        error(e.loc, "'->' requires a pointer operand");
                        out->type = dummyType();
                        return out;
                    }
                    --baseType.pointerDepth;
                } else {
                    if (baseType.isReference) baseType.isReference = false;
                }
                // Look up the struct type.
                auto sIt = structs_.find(baseType.base);
                if (sIt == structs_.end()) {
                    error(e.loc, "'" + typeToString(baseType) +
                          "' is not a struct type — cannot call method '" +
                          std::string(mem.name) + "'");
                    out->type = dummyType();
                    return out;
                }
                // Look up the method in the struct's method table.
                auto mIt = sIt->second.methods.find(mem.name);
                if (mIt == sIt->second.methods.end()) {
                    error(e.loc, "struct '" + std::string(baseType.base) +
                          "' has no method '" + std::string(mem.name) + "'");
                    out->type = dummyType();
                    return out;
                }
                // Inject `this` as the first argument.  The method's
                // `this` param is a reference (`StructType&`), so we pass
                // the object as an lvalue.  We wrap the base expression
                // in a Member-like node so the codegen/interpreter can
                // emit the address.  The simplest way: create an
                // hir::Expr::Member with isArrow=false and a flag that
                // marks it as "this" — but we don't have that flag.
                // Instead, we just prepend the base expression directly.
                // The method's first param is `StructType& this`, and
                // the base expression has type `StructType` (by value or
                // reference) — the codegen passes the address.
                //
                // For `.`: obj is an lvalue → pass &obj.
                // For `->`: obj is a pointer → pass obj (already an address).
                std::vector<hir::Type> argTypes;
                argTypes.push_back(baseType);  // `this` arg type
                for (const auto& a : call.args) {
                    argTypes.push_back(a ? a->type : dummyType());
                }
                // Resolve overload among the method candidates.
                call.target = resolveOverload(mIt->second, argTypes, e.loc);
                if (call.target) {
                    call.callee = call.target->name;
                    // Inject `this` as the first argument.
                    // If `->`, the base is already a pointer — we need
                    // to load it and pass it (the method expects a ref).
                    // For `.`, the base is an lvalue — pass it directly.
                    // We create an IdentRef-like wrapper by reusing the
                    // Member node: set the base expr as-is, and mark it
                    // as a "this" expression.  The codegen/interpreter
                    // treat Member as an lvalue already.
                    //
                    // The trick: we prepend the base expression as a
                    // Member expr so lowerLValue can get its address.
                    auto thisExpr = std::make_unique<hir::Expr>();
                    if (mem.isArrow) {
                        // `ptr->method(args)` — base is a pointer.
                        // We need to pass the pointer value as `this`.
                        // The method's `this` is `T&`, so passing a
                        // pointer doesn't match.  Instead, we dereference:
                        // create `*ptr` (a Member with isArrow=true).
                        thisExpr->node = hir::Expr::Member{};
                        auto& tm = std::get<hir::Expr::Member>(thisExpr->node);
                        tm.base = std::move(baseExpr);
                        tm.name = mem.name;  // not used for lvalue
                        tm.isArrow = true;
                        thisExpr->type = baseType;
                    } else {
                        // `obj.method(args)` — base is an lvalue.
                        // Pass the object directly (by reference).
                        thisExpr = std::move(baseExpr);
                    }
                    thisExpr->loc = e.loc;
                    call.args.insert(call.args.begin(), std::move(thisExpr));
                } else {
                    call.callee = mem.name;
                }
            }
        } else if (std::holds_alternative<A::Lambda>(callee->node)) {
            // Lambda callee: build the lambda expression, which yields a
            // hir::Expr::Lambda carrying the closure struct value and the
            // call-operator function name. We convert this into a direct
            // call to the call-operator with the closure pointer as the
            // first argument.
            auto lambdaExpr = buildExpr(*callee);
            if (!lambdaExpr || !std::holds_alternative<hir::Expr::Lambda>(lambdaExpr->node)) {
                out->type = dummyType();
                return out;
            }
            auto& lamNode = std::get<hir::Expr::Lambda>(lambdaExpr->node);
            // We need a call-operator function. Look it up so we get the
            // proper signature and callee name.
            call.callee = lamNode.funcName;
            call.target = resolveFunction(lamNode.funcName);
            // The first argument is a pointer to the closure struct.
            // We build it as an lvalue: first the lambda expression is an
            // rvalue of closure-struct type; we lower it to a temporary
            // alloca at codegen time. To keep things simple, we emit the
            // Lambda node as-is and let codegen allocate the closure slot.
            // The closure pointer is `&closure` — modeled here as a Unary
            // address-of on the Lambda value.
            auto closureVal = std::move(lambdaExpr);
            auto closurePtr = std::make_unique<hir::Expr>();
            closurePtr->loc = e.loc;
            closurePtr->node.emplace<hir::Expr::Unary>(
                hir::Expr::Unary{"&", true, std::move(closureVal)});
            // The closure pointer's type is the closure struct type + 1 ptr.
            // We'll fill the actual type after we know the closure type.
            closurePtr->type.pointerDepth = 1;
            // Recover closure type name from the Lambda node before move.
            closurePtr->type.base = lamNode.closureType;
            // Prepend the closure pointer as the first argument (before
            // the user-supplied args which were already built).
            call.args.insert(call.args.begin(), std::move(closurePtr));
        } else {
            error(callee->loc, "callee must be a function name");
        }
        if (call.target) {
            checkCall(call, e.loc);
            out->type = call.target->returnType;
            // --- constexpr folding ---
            // If the target is constexpr/consteval, try to evaluate the
            // call at compile time and replace the Call with a literal.
            if (call.target->isConstexpr && !call.args.empty()) {
                // All args must be literals (IntegerLit/FloatLit/BoolLit).
                bool allConst = true;
                for (const auto& a : call.args) {
                    if (!a) { allConst = false; break; }
                    if (!std::holds_alternative<hir::Expr::IntegerLit>(a->node) &&
                        !std::holds_alternative<hir::Expr::FloatLit>(a->node) &&
                        !std::holds_alternative<hir::Expr::BoolLit>(a->node)) {
                        allConst = false;
                        break;
                    }
                }
                if (allConst) {
                    // Try constexpr folding — if successful, replace
                    // the Call expression with a literal.
                    hir::Expr folded;
                    if (tryEvalConstexprCall(folded, call, *call.target, e.loc)) {
                        out = std::make_unique<hir::Expr>(std::move(folded));
                        return out;
                    }
                }
            }
        } else {
            out->type = dummyType();
        }
        return out;
    }
    if (std::holds_alternative<A::Index>(n)) {
        const A::Index& v = std::get<A::Index>(n);
        auto& idx = out->node.emplace<hir::Expr::Index>();
        idx.base = buildExpr(*v.base);
        idx.index = buildExpr(*v.index);
        if (idx.base) {
            const hir::Type& bt = idx.base->type;
            if (bt.arraySize > 0) {
                // Array indexing: safe (bounds check injected by codegen unless unsafe).
                // Result type = element type (array without the size).
                out->type = bt;
                out->type.arraySize = 0;
            } else if (bt.pointerDepth > 0) {
                requireUnsafe(e.loc, "pointer indexing");
                out->type = bt;
                if (out->type.pointerDepth > 0) --out->type.pointerDepth;
            } else {
                error(e.loc, "'[]' requires an array or pointer operand");
                out->type = dummyType();
            }
        } else {
            out->type = dummyType();
        }
        if (idx.index && !isNumeric(idx.index->type)) {
            error(idx.index->loc, "index expression must be numeric");
        }
        return out;
    }
    if (std::holds_alternative<A::Member>(n)) {
        const A::Member& v = std::get<A::Member>(n);
        // Check for scoped enum access: `EnumName::Value` or
        // `ns::EnumName::Value`.  Flatten the `::` chain into a
        // qualified name and try to resolve it as an enum.
        if (!v.isArrow && v.base) {
            // Case 1: `EnumName::Value` — base is IdentRef.
            // Case 2: `ns::EnumName::Value` — base is a Member chain.
            // In both cases, flatten the base into a qualified name,
            // then append `::Value` and check if the base (without
            // `::Value`) is an enum name.
            std::string baseQual;
            if (flattenMemberChain(*v.base, baseQual)) {
                // Try the base as an enum name (qualified or unqualified).
                auto tryEnum = [&](std::string_view enumName) -> bool {
                    auto eIt = enums_.find(enumName);
                    if (eIt != enums_.end()) {
                        auto cIt = eIt->second.constants.find(v.name);
                        if (cIt != eIt->second.constants.end()) {
                            out->node = hir::Expr::IntegerLit{cIt->second};
                            out->type = eIt->second.underlyingType;
                            return true;
                        }
                        error(e.loc, "enum '" + std::string(enumName) +
                              "' has no enumerator '" + std::string(v.name) + "'");
                        out->type = dummyType();
                        return true;  // found the enum, just bad enumerator
                    }
                    return false;
                };
                // Try qualified base name first.
                if (tryEnum(baseQual)) return out;
                // Try with namespace prefix (bare enum name inside
                // a namespace body).
                if (!currentNsPrefix_.empty()) {
                    std::string prefixed;
                    prefixed += currentNsPrefix_;
                    prefixed += baseQual;
                    if (tryEnum(prefixed)) return out;
                }
                // Try `ns::Constant` — the base is a namespace name
                // and `v.name` is an unscoped enum constant defined
                // inside that namespace.  Search all enums with a
                // matching namespace prefix.
                for (const auto& [enumName, edef] : enums_) {
                    if (edef.nsPrefix == baseQual + "::" &&
                        !edef.isScoped) {
                        auto cIt = edef.constants.find(v.name);
                        if (cIt != edef.constants.end()) {
                            out->node = hir::Expr::IntegerLit{cIt->second};
                            out->type = edef.underlyingType;
                            return out;
                        }
                    }
                }
            }
        }
        // If the `::` chain didn't resolve as an enum constant, it
        // might be a qualified struct type name (e.g. `ns::Point`)
        // used in a member access — but Ivy doesn't support static
        // member access on struct types, so fall through to the
        // runtime member access path below.
    // Runtime member access: `p.x` or `p->x` where `p` is a
    // struct-typed lvalue (or pointer-to-struct for `->`).
    {
        auto base = buildExpr(*v.base);
        if (base) {
            hir::Type baseType = base->type;
            // For `->`, the base must be a pointer-to-struct.
            // Strip one pointer level to get the struct type.
            if (v.isArrow) {
                if (baseType.pointerDepth == 0) {
                    error(e.loc, "'->' requires a pointer to struct (got '" +
                                     typeToString(baseType) + "')");
                    out->type = dummyType();
                    return out;
                }
                --baseType.pointerDepth;
            } else {
                // For `.`, the base must be a struct (or reference-to-struct).
                if (baseType.isReference) baseType.isReference = false;
            }
            // Look up the struct type.
            auto sIt = structs_.find(baseType.base);
            if (sIt == structs_.end()) {
                error(e.loc, "'" + typeToString(baseType) +
                                 "' is not a struct type — cannot access member '" +
                                 std::string(v.name) + "'");
                out->type = dummyType();
                return out;
            }
            // Look up the field.
            auto fIt = sIt->second.fieldMap.find(v.name);
            if (fIt == sIt->second.fieldMap.end()) {
                error(e.loc, "struct '" + std::string(baseType.base) +
                                 "' has no member '" + std::string(v.name) + "'");
                out->type = dummyType();
                return out;
            }
            out->node = hir::Expr::Member{};
            auto& mem = std::get<hir::Expr::Member>(out->node);
            mem.base = std::move(base);
            mem.name = v.name;
            mem.isArrow = v.isArrow;
            mem.isScope = v.isScope;
            out->type = fIt->second.type;
            return out;
        }
    }
    // Fallback: base could not be built or struct lookup failed.
    out->node = hir::Expr::Member{};
    auto& mem = std::get<hir::Expr::Member>(out->node);
    mem.base = buildExpr(*v.base);
    mem.name = v.name;
    mem.isArrow = v.isArrow;
    mem.isScope = v.isScope;
    out->type = dummyType();
    return out;
    }
    if (std::holds_alternative<A::Assign>(n)) {
        const A::Assign& v = std::get<A::Assign>(n);
        auto& as = out->node.emplace<hir::Expr::Assign>();
        as.op = v.op;
        as.lhs = buildExpr(*v.lhs);
        // Aggregate init on assignment: `p = {1, 2};` — resolve elements
        // against the lhs struct type so implicit conversions are applied.
        if (v.op == "=" && as.lhs && std::holds_alternative<ivy::Expr::InitList>(v.rhs->node)) {
            as.rhs = buildStructInit(std::get<ivy::Expr::InitList>(v.rhs->node),
                                     as.lhs->type, {}, e.loc);
        } else {
            as.rhs = buildExpr(*v.rhs);
        }
        if (as.lhs && as.rhs) {
            const bool lhsIsVar = std::holds_alternative<hir::Expr::IdentRef>(as.lhs->node);
            const bool lhsIsIdx = std::holds_alternative<hir::Expr::Index>(as.lhs->node);
            const bool lhsIsMem = std::holds_alternative<hir::Expr::Member>(as.lhs->node);
            if (!lhsIsVar && !lhsIsIdx && !lhsIsMem) {
                error(e.loc, "left-hand side of assignment is not assignable");
            } else if (as.op == "=") {
                if (as.lhs->type.isConst && as.lhs->type.pointerDepth == 0) {
                    error(e.loc, "cannot assign to const variable");
                } else if (!isAssignable(as.lhs->type, as.rhs->type)) {
                    error(e.loc, "cannot assign value of type '" + typeToString(as.rhs->type) +
                                     "' to '" + typeToString(as.lhs->type) + "'");
                }
            } else if (!(isNumeric(as.lhs->type) && isNumeric(as.rhs->type))) {
                error(e.loc, "compound assignment '" + std::string(as.op) +
                                 "' expects numeric operands");
            }
        }
        out->type = as.lhs ? as.lhs->type : dummyType();
        return out;
    }
    if (std::holds_alternative<A::New>(n)) {
        const A::New& v = std::get<A::New>(n);
        auto& nw = out->node.emplace<hir::Expr::New>();
        nw.type = v.type;
        for (const auto& a : v.args) nw.args.push_back(buildExpr(*a));
        out->type = v.type;
        ++out->type.pointerDepth;  // `new T` yields T*
        return out;
    }
    if (std::holds_alternative<A::Delete>(n)) {
        const A::Delete& v = std::get<A::Delete>(n);
        auto& dl = out->node.emplace<hir::Expr::Delete>();
        dl.isArray = v.isArray;
        dl.operand = buildExpr(*v.operand);
        if (dl.operand && dl.operand->type.pointerDepth == 0) {
            error(e.loc, "'delete' expects a pointer operand");
        }
        out->type = dummyType();  // `delete` yields void
        return out;
    }
    if (std::holds_alternative<A::InitList>(n)) {
        // InitList reached buildExpr without a target struct type — only
        // valid in declaration initializers (handled via buildStructInit).
        error(e.loc, "braced initializer list is only valid in a declaration initializer");
        out->type = dummyType();
        return out;
    }
    if (std::holds_alternative<A::Lambda>(n)) {
        const A::Lambda& v = std::get<A::Lambda>(n);
        return buildLambda(v, e.loc);
    }

    error(e.loc, "internal: unknown expression kind");
    out->type = dummyType();
    return out;
}

std::unique_ptr<hir::Expr> HirBuilder::buildLambda(const Expr::Lambda& lam, SourceLoc loc) {
    // Generate unique names for the closure struct type and call function.
    const int id = lambdaCounter_++;
    const std::string funcName = "__lambda" + std::to_string(id);
    const std::string closureTypeName = "__lambda" + std::to_string(id) + "_closure";
    // Store names in stable storage so string_views remain valid.
    stringStorage_.push_back(funcName);
    stringStorage_.push_back(closureTypeName);
    const std::string_view funcNameSv = stringStorage_[stringStorage_.size() - 2];
    const std::string_view closureTypeSv = stringStorage_.back();

    // Resolve capture types from current variable scopes.
    // Each capture becomes a field in the closure struct.
    StructDecl closureDecl;
    closureDecl.name = closureTypeSv;
    closureDecl.loc = loc;
    // Also build the capture init expressions (values to store in the closure).
    std::vector<std::unique_ptr<hir::Expr>> captureInits;
    // Remember the resolved capture info so we can inject local-variable
    // declarations (initialized from `__closure->field`) before building
    // the lambda body. This makes the body's references to captured names
    // resolve naturally to local variables.
    struct CapInfo { std::string_view name; hir::Type type; bool byRef; };
    std::vector<CapInfo> capInfos;

    for (const auto& cap : lam.captures) {
        // Look up the captured variable's type in the current scopes.
        hir::Type capType;
        bool found = false;
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (it->contains(cap.name)) {
                capType = (*it)[cap.name];
                found = true;
                break;
            }
        }
        if (!found) {
            error(loc, "lambda captures undeclared variable '" + std::string(cap.name) + "'");
            continue;
        }
        capInfos.push_back({cap.name, capType, cap.byRef});
        // For by-reference capture, the closure field is a pointer.
        hir::Type fieldType = capType;
        if (cap.byRef) {
            fieldType.pointerDepth += 1;
            fieldType.isReference = false;
        }
        Field f;
        f.type = fieldType;
        f.name = cap.name;
        f.loc = loc;
        closureDecl.fields.push_back(std::move(f));
        // Build the capture init expression: for by-value, it's the
        // variable itself (IdentRef); for by-reference, it's &var.
        if (cap.byRef) {
            // Build &var as UnaryAddr
            auto ref = std::make_unique<hir::Expr>();
            ref->loc = loc;
            ref->node.emplace<hir::Expr::IdentRef>(hir::Expr::IdentRef{cap.name});
            ref->type = capType;
            auto addr = std::make_unique<hir::Expr>();
            addr->loc = loc;
            addr->node.emplace<hir::Expr::Unary>(hir::Expr::Unary{"&", true, std::move(ref)});
            addr->type = fieldType;
            captureInits.push_back(std::move(addr));
        } else {
            auto ref = std::make_unique<hir::Expr>();
            ref->loc = loc;
            ref->node.emplace<hir::Expr::IdentRef>(hir::Expr::IdentRef{cap.name});
            ref->type = capType;
            captureInits.push_back(std::move(ref));
        }
    }

    // Register the closure struct type (layout computation + HIR TU entry).
    buildStruct(closureDecl);

    // Determine the lambda's return type. If `-> ret` was omitted,
    // try to deduce from the first return statement in the body.
    hir::Type retType = lam.returnType;
    if (retType.base.empty()) {
        // Scan body for first return statement to deduce return type.
        if (lam.body) {
            const auto& compound = std::get<Stmt::Compound>(lam.body->node);
            for (const auto& stmt : compound.stmts) {
                if (std::holds_alternative<Stmt::Return>(stmt->node)) {
                    const auto& ret = std::get<Stmt::Return>(stmt->node);
                    if (ret.value) {
                        auto built = buildExpr(*ret.value);
                        if (built) retType = built->type;
                    }
                    break;
                }
            }
        }
        if (retType.base.empty()) {
            retType.base = "void";
        }
    }

    // Create the call-operator function signature.
    // First param is always `__closure*` (pointer to closure struct).
    auto fn = std::make_unique<hir::Function>();
    fn->name = funcNameSv;
    fn->returnType = retType;
    fn->loc = loc;
    // closure pointer param
    hir::Param closureParam;
    closureParam.type = hir::Type{closureTypeSv, false, false, false, 1};
    closureParam.name = "__closure";
    closureParam.loc = loc;
    fn->params.push_back(std::move(closureParam));
    // user params
    for (const auto& p : lam.params) {
        hir::Param hp;
        hp.type = p.type;
        hp.name = p.name;
        hp.loc = p.loc;
        fn->params.push_back(std::move(hp));
    }

    hir::Function* rawFn = fn.get();
    hir_->functions.push_back(std::move(fn));
    functions_[funcNameSv].push_back(rawFn);

    // Build the function body. The closure's captured variables are
    // accessed via `__closure->fieldName` — we declare them as local
    // variables initialized from the closure struct fields.
    // Save current state.
    hir::Function* savedCurrent = current_;
    bool savedHasReturn = hasReturnInBody_;
    std::string_view savedNs = currentNsPrefix_;

    current_ = rawFn;
    hasReturnInBody_ = false;
    scopes_.push_back({});
    // Declare the closure pointer parameter.
    declare("__closure", hir::Type{closureTypeSv, false, false, false, 1}, loc);
    // Declare user parameters.
    for (const auto& p : lam.params) {
        if (!p.name.empty()) declare(p.name, p.type, p.loc);
    }
    // For each capture, we inject a local-variable declaration at the
    // top of the lambda body, initialized from the corresponding
    // closure struct field (`__closure->fieldName`). This way, the
    // body's references to captured names resolve naturally to local
    // variables — no AST rewriting needed. For by-reference captures,
    // the local is a reference/pointer aliasing the closure field.
    //
    // Build injected declarations for each capture, then build the user
    // body. The capture declarations access `__closure->field` and, for
    // by-reference captures, dereference a pointer — these are
    // compiler-generated operations on a trusted closure pointer, so
    // they are built under an implicit unsafe scope (see below). The
    // user-written body is built with the original unsafe depth so its
    // own pointer operations are still checked normally.
    std::vector<std::unique_ptr<Stmt>> capDeclStmts;
    for (const auto& ci : capInfos) {
        // The closure field type: by-ref => pointer to capType;
        // by-value => capType.
        hir::Type fieldType = ci.type;
        if (ci.byRef) {
            fieldType.pointerDepth += 1;
            fieldType.isReference = false;
        }
        // Build the init expression: `__closure->capName`.
        // Member access on the `__closure` pointer (IdentRef).
        auto base = std::make_unique<Expr>();
        base->loc = loc;
        base->node.emplace<Expr::IdentRef>(Expr::IdentRef{"__closure"});
        auto mem = std::make_unique<Expr>();
        mem->loc = loc;
        mem->node.emplace<Expr::Member>(Expr::Member{std::move(base), ci.name, true});
        // Build the Decl statement: `T capName = __closure->capName;`
        auto decl = std::make_unique<Stmt>();
        decl->loc = loc;
        Stmt::Decl d;
        if (ci.byRef) {
            // By-reference capture: the closure field stores a pointer
            // to the original variable. We want the local `capName` to
            // be a reference (`T&`) that aliases the pointee — so the
            // body can read/write the original variable through `capName`.
            // The init expression is `*(__closure->capName)` — a deref
            // of the pointer stored in the closure field.
            d.type = ci.type;
            d.type.isReference = true;
            // Wrap the member access in a deref: `*( __closure->capName )`
            auto deref = std::make_unique<Expr>();
            deref->loc = loc;
            deref->node.emplace<Expr::Unary>(Expr::Unary{"*", true, std::move(mem)});
            d.init = std::move(deref);
        } else {
            d.type = ci.type;
            d.init = std::move(mem);
        }
        d.name = ci.name;
        decl->node.emplace<Stmt::Decl>(std::move(d));
        capDeclStmts.push_back(std::move(decl));
    }
    // Build the lambda body compound. We push a fresh scope and build
    // the injected capture declarations under an implicit unsafe scope
    // (compiler-generated closure access), then build the user-written
    // body statements with the original unsafe depth.
    {
        auto out = std::make_unique<hir::Stmt>();
        out->loc = loc;
        auto& compound = out->node.emplace<hir::Stmt::Compound>();
        scopes_.push_back({});

        // Build injected capture declarations under an implicit unsafe
        // scope (compiler-generated closure access is always safe).
        ++unsafeDepth_;
        for (const auto& cs : capDeclStmts) {
            if (cs) compound.stmts.push_back(buildStmt(*cs));
        }
        --unsafeDepth_;

        // Build the user-written body statements with the original
        // unsafe depth so user code is checked normally.
        if (lam.body) {
            const auto& userCompound = std::get<Stmt::Compound>(lam.body->node);
            for (const auto& s : userCompound.stmts) {
                compound.stmts.push_back(buildStmt(*s));
            }
        }
        scopes_.pop_back();

        rawFn->body = std::make_unique<hir::Stmt::Compound>(
            std::move(compound));
    }

    scopes_.pop_back();
    // Check for missing return (unless void).
    if (!rawFn->returnType.isConst && rawFn->returnType.pointerDepth == 0 &&
        rawFn->returnType.base != "void" && !hasReturnInBody_) {
        // Lambda may reach end without return — not an error for now
        // (Ivy is lenient with lambdas used for side effects).
    }

    // Restore state.
    current_ = savedCurrent;
    hasReturnInBody_ = savedHasReturn;
    currentNsPrefix_ = savedNs;

    // Build the Lambda HIR expression: a closure struct value (InitList
    // of capture values) plus the function name.
    auto out = std::make_unique<hir::Expr>();
    out->loc = loc;
    auto& lambdaNode = out->node.emplace<hir::Expr::Lambda>();
    lambdaNode.funcName = funcNameSv;
    lambdaNode.closureType = closureTypeSv;
    lambdaNode.captureInits = std::move(captureInits);
    // The type of the lambda expression is the closure struct type.
    out->type = hir::Type{closureTypeSv, false, false, false, 0};
    return out;
}

std::unique_ptr<hir::TranslationUnit> HirBuilder::build() {
    hir_ = std::make_unique<hir::TranslationUnit>();
    // Register enums first (pass 1a) so enum constants are resolvable
    // in function bodies (pass 2) and enum type names are usable as types.
    for (const EnumDecl& ed : ast_.enums) buildEnum(ed);
    // Register structs (pass 1b) so struct types are usable as types and
    // field layout is available for Member resolution in function bodies.
    for (const StructDecl& sd : ast_.structs) buildStruct(sd);
    for (const Function& af : ast_.functions) buildSignature(af);
    for (const Function& af : ast_.functions) {
        if (af.body && af.tplParams.empty()) {
            // Find the matching HIR function in the overload set.
            // Match by signature: same param count + types.
            hir::Function* fn = nullptr;
            auto it = functions_.find(af.name);
            if (it != functions_.end()) {
                for (hir::Function* cand : it->second) {
                    if (cand->params.size() != af.params.size()) continue;
                    bool match = true;
                    for (std::size_t i = 0; i < af.params.size() && match; ++i) {
                        hir::Type a = cand->params[i].type; a.isReference = false;
                        hir::Type b = af.params[i].type;    b.isReference = false;
                        if (!(a == b)) match = false;
                    }
                    if (match) { fn = cand; break; }
                }
            }
            if (fn) {
                currentNsPrefix_ = af.namespacePrefix;
                buildBody(*fn, *af.body);
                currentNsPrefix_ = {};
            }
        }
    }
    // Build method bodies (pass 2b).  Methods are registered in
    // functions_ under "StructName::methodName" with an implicit
    // `this` param prepended, so we match by the AST method's name
    // (which is already "StructName::methodName").
    for (const StructDecl& sd : ast_.structs) {
        for (const Function& mf : sd.methods) {
            if (!mf.body || !mf.tplParams.empty()) continue;
            // Find the HIR function we registered in buildStruct.
            hir::Function* fn = nullptr;
            auto it = functions_.find(mf.name);
            if (it != functions_.end()) {
                for (hir::Function* cand : it->second) {
                    // The HIR method has 1 extra param (implicit `this`).
                    if (cand->params.size() != mf.params.size() + 1) continue;
                    bool match = true;
                    for (std::size_t i = 0; i < mf.params.size() && match; ++i) {
                        hir::Type a = cand->params[i + 1].type; a.isReference = false;
                        hir::Type b = mf.params[i].type;        b.isReference = false;
                        if (!(a == b)) match = false;
                    }
                    if (match) { fn = cand; break; }
                }
            }
            if (fn) {
                currentNsPrefix_ = mf.namespacePrefix;
                buildBody(*fn, *mf.body);
                currentNsPrefix_ = {};
            }
        }
    }
    if (failed_) return nullptr;
    return std::move(hir_);
}

// ---------------------------------------------------------------------------
//                      constexpr evaluation
// ---------------------------------------------------------------------------

// A simple tree-walking constant evaluator for constexpr function bodies.
// It operates on HIR expressions, which have already been type-checked
// and had names resolved.  Only integer and float literals, unary/binary
// arithmetic, conditional (ternary), and calls to other constexpr
// functions are supported — anything else (pointers, structs, strings,
// builtins like printf) causes the evaluation to fail gracefully.

bool HirBuilder::evalConstExpr(const hir::Expr& e, const hir::Function& fn,
                               ConstValue& result) const {
    using E = hir::Expr;
    if (std::holds_alternative<E::IntegerLit>(e.node)) {
        result.isInt = true;
        result.i = std::get<E::IntegerLit>(e.node).value;
        return true;
    }
    if (std::holds_alternative<E::FloatLit>(e.node)) {
        result.isInt = false;
        result.f = std::get<E::FloatLit>(e.node).value;
        return true;
    }
    if (std::holds_alternative<E::BoolLit>(e.node)) {
        result.isInt = true;
        result.i = std::get<E::BoolLit>(e.node).value ? 1 : 0;
        return true;
    }
    if (std::holds_alternative<E::NullptrLit>(e.node)) {
        result.isInt = true;
        result.i = 0;
        return true;
    }
    // Parameter reference: look up the argument value.
    if (std::holds_alternative<E::IdentRef>(e.node)) {
        const auto& ref = std::get<E::IdentRef>(e.node);
        // Check if it's a parameter of the current constexpr function.
        for (const auto& p : fn.params) {
            if (p.name == ref.name) {
                // Parameter values are stored in a separate map —
                // but we don't have one here in the const evaluator.
                // Instead, constexpr call args are pre-folded into
                // the body via tryEvalConstexprCall's paramValues.
                return false;  // handled by tryEvalConstexprCall
            }
        }
        // Check enum constants.
        auto it = enumConstants_.find(ref.name);
        if (it != enumConstants_.end()) {
            result.isInt = true;
            result.i = it->second;
            return true;
        }
        return false;
    }
    if (std::holds_alternative<E::Unary>(e.node)) {
        const auto& u = std::get<E::Unary>(e.node);
        ConstValue operand;
        if (!evalConstExpr(*u.operand, fn, result)) return false;
        if (u.isPrefix && (u.op == "++" || u.op == "--")) return false;  // side-effect
        if (u.op == "-") { result.i = -result.i; result.f = -result.f; return true; }
        if (u.op == "+") { return true; }
        if (u.op == "!") { result.isInt = true; result.i = !result.i; return true; }
        if (u.op == "~") { result.isInt = true; result.i = ~result.i; return true; }
        return false;
    }
    if (std::holds_alternative<E::Binary>(e.node)) {
        const auto& b = std::get<E::Binary>(e.node);
        // Short-circuit && and ||.
        if (b.op == "&&" || b.op == "||") {
            ConstValue lhs;
            if (!evalConstExpr(*b.lhs, fn, lhs)) return false;
            bool lb = lhs.i != 0;
            if (b.op == "&&" && !lb) { result.isInt = true; result.i = 0; return true; }
            if (b.op == "||" && lb)  { result.isInt = true; result.i = 1; return true; }
            if (!evalConstExpr(*b.rhs, fn, result)) return false;
            result.isInt = true;
            result.i = (result.i != 0) ? 1 : 0;
            return true;
        }
        ConstValue lhs, rhs;
        if (!evalConstExpr(*b.lhs, fn, lhs)) return false;
        if (!evalConstExpr(*b.rhs, fn, rhs)) return false;
        if (lhs.isInt && rhs.isInt) {
            result.isInt = true;
            if (b.op == "+") result.i = lhs.i + rhs.i;
            else if (b.op == "-") result.i = lhs.i - rhs.i;
            else if (b.op == "*") result.i = lhs.i * rhs.i;
            else if (b.op == "/") { if (rhs.i == 0) return false; result.i = lhs.i / rhs.i; }
            else if (b.op == "%") { if (rhs.i == 0) return false; result.i = lhs.i % rhs.i; }
            else if (b.op == "==") result.i = lhs.i == rhs.i;
            else if (b.op == "!=") result.i = lhs.i != rhs.i;
            else if (b.op == "<") result.i = lhs.i < rhs.i;
            else if (b.op == "<=") result.i = lhs.i <= rhs.i;
            else if (b.op == ">") result.i = lhs.i > rhs.i;
            else if (b.op == ">=") result.i = lhs.i >= rhs.i;
            else if (b.op == "&") result.i = lhs.i & rhs.i;
            else if (b.op == "|") result.i = lhs.i | rhs.i;
            else if (b.op == "^") result.i = lhs.i ^ rhs.i;
            else if (b.op == "<<") result.i = lhs.i << rhs.i;
            else if (b.op == ">>") result.i = lhs.i >> rhs.i;
            else return false;
            return true;
        }
        // Float arithmetic.
        double lv = lhs.isInt ? static_cast<double>(lhs.i) : lhs.f;
        double rv = rhs.isInt ? static_cast<double>(rhs.i) : rhs.f;
        result.isInt = false;
        if (b.op == "+") result.f = lv + rv;
        else if (b.op == "-") result.f = lv - rv;
        else if (b.op == "*") result.f = lv * rv;
        else if (b.op == "/") { if (rv == 0.0) return false; result.f = lv / rv; }
        else if (b.op == "==") { result.isInt = true; result.i = lv == rv; }
        else if (b.op == "!=") { result.isInt = true; result.i = lv != rv; }
        else if (b.op == "<")  { result.isInt = true; result.i = lv < rv; }
        else if (b.op == "<=") { result.isInt = true; result.i = lv <= rv; }
        else if (b.op == ">")  { result.isInt = true; result.i = lv > rv; }
        else if (b.op == ">=") { result.isInt = true; result.i = lv >= rv; }
        else return false;
        return true;
    }
    if (std::holds_alternative<E::Ternary>(e.node)) {
        const auto& t = std::get<E::Ternary>(e.node);
        if (!evalConstExpr(*t.cond, fn, result)) return false;
        if (result.i != 0) return evalConstExpr(*t.thenBranch, fn, result);
        return evalConstExpr(*t.elseBranch, fn, result);
    }
    // Call to another constexpr function.
    if (std::holds_alternative<E::Call>(e.node)) {
        const auto& c = std::get<E::Call>(e.node);
        if (!c.target || !c.target->isConstexpr) return false;
        // Check all args are constant.
        std::vector<ConstValue> argVals;
        for (const auto& a : c.args) {
            ConstValue v;
            if (!a || !evalConstExpr(*a, fn, v)) return false;
            argVals.push_back(v);
        }
        // Recursive constexpr call — bind params and eval body.
        const hir::Function& inner = *c.target;
        // Build a paramValues map for the inner function.
        // We use a thread_local map for simplicity.
        // Actually, we can just recursively evaluate the body with
        // param values substituted.  For now, we only support
        // functions that don't reference their params in complex ways.
        // This is a limitation — will be improved later.
        return false;  // TODO: recursive constexpr calls
    }
    return false;
}

bool HirBuilder::evalConstStmt(const hir::Stmt& s, const hir::Function& fn,
                               ConstValue& result) const {
    using S = hir::Stmt;
    if (std::holds_alternative<S::Return>(s.node)) {
        const auto& r = std::get<S::Return>(s.node);
        if (!r.value) { result.isInt = true; result.i = 0; return true; }
        return evalConstExpr(*r.value, fn, result);
    }
    if (std::holds_alternative<S::Compound>(s.node)) {
        const auto& c = std::get<S::Compound>(s.node);
        for (const auto& st : c.stmts) {
            if (evalConstStmt(*st, fn, result)) return true;
        }
        result.isInt = true; result.i = 0;
        return true;
    }
    if (std::holds_alternative<S::If>(s.node)) {
        const auto& ifst = std::get<S::If>(s.node);
        ConstValue cond;
        if (!evalConstExpr(*ifst.cond, fn, cond)) return false;
        if (cond.i != 0) {
            if (ifst.thenBranch) return evalConstStmt(*ifst.thenBranch, fn, result);
        } else {
            if (ifst.elseBranch) return evalConstStmt(*ifst.elseBranch, fn, result);
        }
        result.isInt = true; result.i = 0;
        return true;
    }
    return false;
}

bool HirBuilder::tryEvalConstexprCall(hir::Expr& out, const hir::Expr::Call& call,
                                      const hir::Function& fn, SourceLoc loc) {
    // Check: function must have a body.
    if (!fn.body) return false;
    // Collect argument values.
    std::vector<ConstValue> argVals;
    for (const auto& a : call.args) {
        if (!a) return false;
        ConstValue v;
        if (!evalConstExpr(*a, fn, v)) return false;
        argVals.push_back(v);
    }
    // For now, we can only evaluate functions whose body consists of
    // a single return statement (possibly wrapped in a compound) that
    // uses only the function parameters and constants.  We handle
    // parameter substitution by creating a modified copy of the body
    // where IdentRef to a param is replaced by the argument literal.
    //
    // This is a simple approach that works for most constexpr functions.
    // A full implementation would use the MIR interpreter.

    // Create a clone of the body with parameters substituted.
    // We do this by walking the HIR Expr tree and replacing IdentRef
    // nodes that match parameter names with the corresponding literal.

    std::unordered_map<std::string_view, ConstValue> paramValues;
    for (std::size_t i = 0; i < fn.params.size() && i < argVals.size(); ++i) {
        paramValues[fn.params[i].name] = argVals[i];
    }

    // Helper: substitute params in an expression tree.
    // Uses if-else chain (not std::visit) to avoid instantiating
    // copy-constructors for variants containing unique_ptr members
    // (Call, New, InitList, Lambda, etc.) which are non-copyable.
    std::function<std::unique_ptr<hir::Expr>(const hir::Expr&)> substituteParams =
        [&](const hir::Expr& src) -> std::unique_ptr<hir::Expr> {
        using E = hir::Expr;
        auto dst = std::make_unique<hir::Expr>();
        dst->loc = src.loc;
        dst->type = src.type;

        if (std::holds_alternative<E::IntegerLit>(src.node)) {
            dst->node = std::get<E::IntegerLit>(src.node);
        } else if (std::holds_alternative<E::FloatLit>(src.node)) {
            dst->node = std::get<E::FloatLit>(src.node);
        } else if (std::holds_alternative<E::BoolLit>(src.node)) {
            dst->node = std::get<E::BoolLit>(src.node);
        } else if (std::holds_alternative<E::NullptrLit>(src.node)) {
            dst->node = std::get<E::NullptrLit>(src.node);
        } else if (std::holds_alternative<E::IdentRef>(src.node)) {
            const auto& ref = std::get<E::IdentRef>(src.node);
            auto it = paramValues.find(ref.name);
            if (it != paramValues.end()) {
                if (it->second.isInt) {
                    dst->node = E::IntegerLit{it->second.i};
                } else {
                    dst->node = E::FloatLit{it->second.f};
                }
            } else {
                dst->node = ref;
            }
        } else if (std::holds_alternative<E::Unary>(src.node)) {
            const auto& u = std::get<E::Unary>(src.node);
            E::Unary copy{u.op, u.isPrefix,
                          u.operand ? substituteParams(*u.operand) : nullptr};
            dst->node = std::move(copy);
        } else if (std::holds_alternative<E::Binary>(src.node)) {
            const auto& b = std::get<E::Binary>(src.node);
            E::Binary copy{b.op,
                           b.lhs ? substituteParams(*b.lhs) : nullptr,
                           b.rhs ? substituteParams(*b.rhs) : nullptr};
            dst->node = std::move(copy);
        } else if (std::holds_alternative<E::Ternary>(src.node)) {
            const auto& t = std::get<E::Ternary>(src.node);
            E::Ternary copy{
                t.cond ? substituteParams(*t.cond) : nullptr,
                t.thenBranch ? substituteParams(*t.thenBranch) : nullptr,
                t.elseBranch ? substituteParams(*t.elseBranch) : nullptr};
            dst->node = std::move(copy);
        } else {
            // Unsupported node type (Call, New, Delete, InitList,
            // Member, Index, Assign, Lambda, StringLit, CharLit) —
            // return nullptr to signal failure.
            return nullptr;
        }
        return dst;
    };

    // Substitute params in the body and evaluate.
    // We need to find the return statement.
    ConstValue result;
    bool ok = false;

    // Walk the body (a Stmt::Compound) looking for the return.
    // Handle simple: compound → return, or compound → if → return.
    std::function<bool(const hir::Stmt&)> evalStmt = [&](const hir::Stmt& s) -> bool {
        using S = hir::Stmt;
        if (std::holds_alternative<S::Compound>(s.node)) {
            const auto& c = std::get<S::Compound>(s.node);
            for (const auto& st : c.stmts) {
                if (evalStmt(*st)) return true;
            }
            return false;
        }
        if (std::holds_alternative<S::Return>(s.node)) {
            const auto& r = std::get<S::Return>(s.node);
            if (!r.value) { result.isInt = true; result.i = 0; return true; }
            auto substituted = substituteParams(*r.value);
            if (!substituted) return false;
            return evalConstExpr(*substituted, fn, result);
        }
        if (std::holds_alternative<S::If>(s.node)) {
            const auto& ifst = std::get<S::If>(s.node);
            auto condSub = substituteParams(*ifst.cond);
            if (!condSub) return false;
            ConstValue cond;
            if (!evalConstExpr(*condSub, fn, cond)) return false;
            if (cond.i != 0 && ifst.thenBranch) return evalStmt(*ifst.thenBranch);
            if (cond.i == 0 && ifst.elseBranch) return evalStmt(*ifst.elseBranch);
            return false;
        }
        if (std::holds_alternative<S::ExprStmt>(s.node)) {
            // Skip expression statements (no side effects in constexpr).
            return false;
        }
        if (std::holds_alternative<S::Decl>(s.node)) {
            // Local variable declaration in a constexpr function.
            // For now, we don't support local variables — skip.
            return false;
        }
        return false;
    };

    // The body is a unique_ptr<Stmt::Compound>. We can't copy it into
    // a Stmt (unique_ptr members), so evaluate directly.
    ok = false;
    if (fn.body) {
        for (const auto& st : fn.body->stmts) {
            if (evalStmt(*st)) { ok = true; break; }
        }
    }
    if (!ok) return false;

    // Build the folded literal.
    out.loc = loc;
    out.type = fn.returnType;
    if (result.isInt) {
        out.node = hir::Expr::IntegerLit{result.i};
    } else {
        out.node = hir::Expr::FloatLit{result.f};
    }
    return true;
}

// ---------------------------------------------------------------------------
//                      template instantiation
// ---------------------------------------------------------------------------

const Function* HirBuilder::lookupTemplate(std::string_view name) const {
    auto it = templates_.find(name);
    if (it != templates_.end()) return it->second;
    // Try with namespace prefix.
    if (!currentNsPrefix_.empty()) {
        std::string qualified;
        qualified.reserve(currentNsPrefix_.size() + name.size());
        qualified += currentNsPrefix_;
        qualified += name;
        auto it2 = templates_.find(qualified);
        if (it2 != templates_.end()) return it2->second;
    }
    return nullptr;
}

hir::Type HirBuilder::substituteType(const hir::Type& t,
    const std::unordered_map<std::string_view, hir::Type>& mapping) const {
    auto it = mapping.find(t.base);
    if (it != mapping.end()) {
        hir::Type sub = it->second;
        // Preserve pointer/reference modifiers from the original type.
        sub.pointerDepth = t.pointerDepth;
        sub.isReference = t.isReference;
        sub.isConst = t.isConst;
        return sub;
    }
    return t;
}

hir::Function* HirBuilder::instantiateTemplate(const Function& tplFunc,
                                               std::string_view instantiatedName,
                                               const std::vector<hir::Type>& tplArgs,
                                               SourceLoc loc) {
    // Build the mangled specialization name: "add<int>" → store in stringStorage_.
    // We'll use a simple mangling: name + "<" + arg1 + "," + arg2 + ">"
    std::string mangled;
    mangled += std::string(tplFunc.name);
    mangled += "<";
    for (std::size_t i = 0; i < tplArgs.size(); ++i) {
        if (i > 0) mangled += ",";
        // Use the base type name (simplified).
        std::string arg;
        arg += std::string(tplArgs[i].base);
        if (tplArgs[i].isUnsigned) arg += " unsigned";
        if (tplArgs[i].isConst) arg += " const";
        for (std::uint32_t d = 0; d < tplArgs[i].pointerDepth; ++d) arg += "*";
        mangled += arg;
    }
    mangled += ">";

    // Check if already instantiated.
    auto it = instantiated_.find(mangled);
    if (it != instantiated_.end()) return it->second;

    // Build type mapping: template param name → concrete type.
    std::unordered_map<std::string_view, hir::Type> mapping;
    for (std::size_t i = 0; i < tplFunc.tplParams.size() && i < tplArgs.size(); ++i) {
        if (tplFunc.tplParams[i].isTypename) {
            mapping[tplFunc.tplParams[i].name] = tplArgs[i];
        }
    }

    // Create the HIR function with substituted signature.
    auto fn = std::make_unique<hir::Function>();
    fn->isTemplate = true;
    fn->tplArgs = tplArgs;
    fn->isExternC = tplFunc.isExternC;
    fn->isConstexpr = tplFunc.isConstexpr;
    fn->isConsteval = tplFunc.isConsteval;
    fn->loc = loc;
    fn->namespacePrefix = tplFunc.namespacePrefix;

    // Substitute return type.
    fn->returnType = substituteType(tplFunc.returnType, mapping);

    // Store the mangled name in stable storage.
    stringStorage_.push_back(std::move(mangled));
    fn->name = stringStorage_.back();

    // Substitute param types.
    for (const Param& ap : tplFunc.params) {
        hir::Param p;
        p.type = substituteType(ap.type, mapping);
        p.name = ap.name;
        p.loc = ap.loc;
        fn->params.push_back(std::move(p));
    }

    // Register the function before building the body (so recursive
    // calls can resolve it).
    hir::Function* raw = fn.get();
    hir_->functions.push_back(std::move(fn));
    functions_[raw->name].push_back(raw);
    instantiated_[raw->name] = raw;

    // Build the body using the same mechanism as buildBody, but on the
    // cloned AST body (so we own a mutable copy). The HIR builder will
    // resolve names against the already-declared params and the
    // substituted types in `raw->params`.
    if (tplFunc.body) {
        // Clone the AST body: tplFunc.body is a unique_ptr<Stmt::Compound>.
        // We build a new Compound by cloning each statement individually.
        Stmt::Compound clonedCompound;
        for (const auto& st : tplFunc.body->stmts) {
            clonedCompound.stmts.push_back(cloneStmt(*st));
        }
        // Save/restore namespace prefix so bare-name resolution works.
        std::string_view savedNs = currentNsPrefix_;
        currentNsPrefix_ = raw->namespacePrefix;
        buildBody(*raw, clonedCompound);
        currentNsPrefix_ = savedNs;
    }

    return raw;
}

}  // namespace ivy
