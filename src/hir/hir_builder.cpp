#include "hir/hir_builder.h"

#include <algorithm>
#include <deque>
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

// 8.5: Check if a name is a known runtime builtin function.
// These functions are resolved at runtime by the interpreter or linked
// from libc in codegen mode. They don't need HIR registration.
bool isBuiltinFn(std::string_view name) {
    // C runtime builtins (already supported).
    if (name == "printf" || name == "puts" || name == "putchar" ||
        name == "exit" || name == "abort" || name == "malloc" ||
        name == "free")
        return true;
    // 8.5: Ivy stdlib builtins.
    if (name == "ivy::print" || name == "ivy::println" ||
        name == "ivy::print_int" || name == "ivy::print_float" ||
        name == "ivy::print_str" || name == "ivy::print_char" ||
        name == "ivy::println_int" || name == "ivy::println_float" ||
        name == "ivy::println_str" || name == "ivy::println_char")
        return true;
    return false;
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

// 8.3: Computes the size of a type for sizeof/alignof and struct layout.
// Member function (not static) so it can look up struct sizes from structs_.
// Uses the same rank table as codegen's sizeofType() so layout matches.
std::uint64_t HirBuilder::typeSize(const hir::Type& t) const {
    if (t.pointerDepth > 0 || t.isReference) return 8;  // 64-bit pointers
    const std::string_view b = t.base;
    if (b == "bool" || b == "char" || b == "int8_t" || b == "uint8_t") return 1;
    if (b == "short" || b == "int16_t" || b == "uint16_t" || b == "float16_t" || b == "bfloat16_t") return 2;
    if (b == "int" || b == "unsigned" || b == "int32_t" || b == "uint32_t" ||
        b == "float" || b == "float32_t" || b == "size_t" || b == "ptrdiff_t") return 4;
    if (b == "long" || b == "long long" || b == "int64_t" || b == "uint64_t" ||
        b == "double" || b == "long double" || b == "float64_t" || b == "float128_t") return 8;
    // 8.3: Struct type — look up the pre-computed size from buildStruct.
    auto it = structs_.find(b);
    if (it != structs_.end()) return it->second.size;
    // 8.3: Array type: element size * count.
    if (t.arraySize > 0) {
        hir::Type elem = t;
        elem.arraySize = 0;
        return typeSize(elem) * t.arraySize;
    }
    return 4;  // fallback (e.g. unresolved enum → int)
}

void HirBuilder::buildUsing(const UsingDecl& ud) {
    // Register the alias: name → target type.  The AST Type and HIR
    // Type are the same struct, so direct copy is fine.
    // Store under both the qualified name (ud.name, e.g. "math::Float")
    // and the bare name (last `::` segment, e.g. "Float") so that
    // `resolveTypeAlias` finds it whether the source used the
    // qualified or unqualified form.
    typeAliases_[ud.name] = ud.targetType;
    const std::string_view qual = ud.name;
    const std::size_t pos = qual.rfind("::");
    const std::string_view bare =
        (pos != std::string_view::npos) ? qual.substr(pos + 2) : qual;
    if (bare != qual) typeAliases_[bare] = ud.targetType;
}

hir::Type HirBuilder::resolveTypeAlias(const hir::Type& type) const {
    // If type.base is a registered alias, replace it with the aliased
    // type.  Preserve const/ref/pointer qualifiers from the *usage*
    // (e.g. `const Int&` → `const int32_t&`).  Recurse for alias chains.
    if (type.base.empty()) return type;
    auto it = typeAliases_.find(type.base);
    if (it == typeAliases_.end()) return type;
    hir::Type expanded = it->second;
    // Merge qualifiers: if the alias usage has const/ref/pointer,
    // apply them on top of the expanded base type.
    if (type.isConst) expanded.isConst = true;
    if (type.isReference) expanded.isReference = true;
    expanded.pointerDepth += type.pointerDepth;
    // Recurse in case the target is also an alias.
    return resolveTypeAlias(expanded);
}

// 8.3: Computes the alignment of a type for alignof and struct layout.
std::uint32_t HirBuilder::typeAlign(const hir::Type& t) const {
    if (t.pointerDepth > 0 || t.isReference) return 8;  // 64-bit pointers
    const std::string_view b = t.base;
    // 8.3: Struct type — look up pre-computed alignment from buildStruct.
    auto it = structs_.find(b);
    if (it != structs_.end()) return it->second.align;
    // 8.3: Array type: alignment = element alignment.
    if (t.arraySize > 0) {
        hir::Type elem = t;
        elem.arraySize = 0;
        return typeAlign(elem);
    }
    // Scalar types: alignment = size.
    return static_cast<std::uint32_t>(typeSize(t));
}

void HirBuilder::buildStruct(const StructDecl& sd) {
    // Template struct definition: do not build — register the template
    // and instantiate on demand when a template-id type (`Box<int>`) is
    // encountered.  The template definition lives in `structTemplates_`
    // and the AST is kept alive by `ast::TranslationUnit::structs`.
    if (!sd.tplParams.empty()) {
        structTemplates_[sd.name] = &sd;
        return;
    }

    // Reject duplicate struct name.
    if (structs_.contains(sd.name)) {
        error(sd.loc, "redefinition of struct '" + std::string(sd.name) + "'");
        return;
    }

    // --- 7.7: Inheritance + virtual ---
    // Resolve base classes. They must already be built (declared earlier).
    // Determine if this struct is polymorphic (has virtual methods or
    // inherits from a polymorphic base).
    bool hasVirtualMethod = false;
    for (const Function& mf : sd.methods) {
        if (mf.isVirtual || mf.isPureVirtual) { hasVirtualMethod = true; break; }
    }
    bool basesPolymorphic = false;
    for (const BaseClass& bc : sd.bases) {
        auto it = structs_.find(bc.type.base);
        if (it != structs_.end() && it->second.isPolymorphic) {
            basesPolymorphic = true; break;
        }
    }
    bool isPolymorphic = hasVirtualMethod || basesPolymorphic;

    StructDef def;
    def.nsPrefix = std::string(sd.namespacePrefix);
    def.isPolymorphic = isPolymorphic;

    // Compute field layout: vptr (if polymorphic) at offset 0, then
    // base subobjects, then derived fields. Each is aligned to its
    // natural alignment. The struct's overall alignment is the max
    // of all component alignments. Size is rounded up to alignment.
    std::uint64_t offset = 0;
    std::uint32_t structAlign = 1;

    // vptr at offset 0 (8 bytes, pointer-aligned).
    if (isPolymorphic) {
        def.fieldMap["__vptr"] = {0, 0, hir::Type{"void", 0, true, 1, false}};
        offset = 8;
        structAlign = 8;
    }

    // Base subobjects.
    for (const BaseClass& bc : sd.bases) {
        auto it = structs_.find(bc.type.base);
        if (it == structs_.end()) {
            error(bc.loc, "base class '" + std::string(bc.type.base) + "' is not defined");
            continue;
        }
        const StructDef& baseDef = it->second;
        const std::uint64_t baseSize = baseDef.size;
        const std::uint32_t baseAlign = baseDef.align;
        offset = (offset + baseAlign - 1) & ~(std::uint64_t(baseAlign - 1));
        def.bases.push_back({bc.type.base, offset});
        // Merge base fields into derived field map with adjusted offsets.
        for (const auto& [fname, finfo] : baseDef.fieldMap) {
            if (fname == "__vptr") continue;  // skip base vptr — derived has its own
            def.fieldMap[fname] = {finfo.index, finfo.offset + offset, finfo.type};
        }
        offset += baseSize;
        if (baseAlign > structAlign) structAlign = baseAlign;
    }

    // Derived fields.
    for (std::size_t i = 0; i < sd.fields.size(); ++i) {
        const Field& f = sd.fields[i];
        const hir::Type fieldType = resolveTemplateStructType(resolveTypeAlias(f.type), f.loc);
        const std::uint64_t sz = typeSize(fieldType);
        const std::uint32_t align = typeAlign(fieldType);
        // Pad to alignment.
        offset = (offset + align - 1) & ~(std::uint64_t(align - 1));
        def.fieldMap[f.name] = {def.fields.size(), offset, fieldType};
        offset += sz;
        if (align > structAlign) structAlign = align;
    }
    def.size = (offset + structAlign - 1) & ~(std::uint64_t(structAlign - 1));
    def.align = structAlign;
    // Copy field type/name (skip `init` — not needed past HIR).
    for (const Field& f : sd.fields) {
        def.fields.push_back(Field{resolveTemplateStructType(resolveTypeAlias(f.type), f.loc), f.name, nullptr, f.loc});
        def.defaultInits.push_back(f.init.get());
    }

    // Copy the struct declaration into the HIR TU.
    StructDecl resolved;
    resolved.name = sd.name;
    resolved.namespacePrefix = sd.namespacePrefix;
    resolved.isClass = sd.isClass;
    resolved.loc = sd.loc;
    resolved.bases = sd.bases;  // copy base class list (7.7)
    for (const Field& f : sd.fields) {
        Field cf;
        cf.type = resolveTemplateStructType(resolveTypeAlias(f.type), f.loc);
        cf.name = f.name;
        cf.loc = f.loc;
        resolved.fields.push_back(std::move(cf));
    }
    hir_->structs.push_back(std::move(resolved));

    // Build HIR functions for each method with implicit `this` param.
    std::vector<hir::Function*> methodFns;
    methodFns.reserve(sd.methods.size());
    for (const Function& mf : sd.methods) {
        auto fn = std::make_unique<hir::Function>();
        fn->name = mf.name;
        fn->namespacePrefix = mf.namespacePrefix;
        fn->isExternC = mf.isExternC;
        fn->isConstexpr = mf.isConstexpr;
        fn->isConsteval = mf.isConsteval;
        fn->isCtor = mf.isCtor;
        fn->isDtor = mf.isDtor;
        fn->isVirtual = mf.isVirtual || mf.isPureVirtual || mf.isOverride ||
                        (mf.isDtor && isPolymorphic);  // 7.7: polymorphic dtor
        fn->isPureVirtual = mf.isPureVirtual;
        fn->isOverride = mf.isOverride;
        // Set vtableOf so MIR builder can reconstruct vtable layout.
        if (fn->isVirtual) fn->vtableOf = sd.name;
        fn->returnType = resolveTemplateStructType(resolveTypeAlias(mf.returnType), mf.loc);
        fn->loc = mf.loc;

        // Implicit `this` parameter: `StructName& this`.
        hir::Param thisParam;
        thisParam.type.base = sd.name;
        thisParam.type.isReference = true;
        thisParam.name = "this";
        thisParam.loc = mf.loc;
        fn->params.push_back(std::move(thisParam));

        // User-declared parameters.
        for (const Param& ap : mf.params) {
            hir::Param p;
            p.type = resolveTemplateStructType(resolveTypeAlias(ap.type), ap.loc);
            p.name = ap.name;
            p.loc = ap.loc;
            p.lifetime = lowerParamAttribute(*fn, ap);
            p.defaultValue = ap.defaultValue.get();
            fn->params.push_back(std::move(p));
        }

        functions_[fn->name].push_back(fn.get());
        methodFns.push_back(fn.get());
        hir_->functions.push_back(std::move(fn));
    }

    structs_[sd.name] = std::move(def);

    // Register methods in the struct's method table (under bare names).
    StructDef& defRef = structs_[sd.name];

    // Inherit methods from bases (7.7): copy base method pointers into
    // the derived method table so `derived.baseMethod()` resolves.
    for (const BaseClass& bc : sd.bases) {
        auto it = structs_.find(bc.type.base);
        if (it == structs_.end()) continue;
        for (const auto& [bareName, overloads] : it->second.methods) {
            for (hir::Function* f : overloads) {
                // Don't copy if derived overrides (same bare name already present).
                auto& derivedOverloads = defRef.methods[bareName];
                bool already = false;
                for (hir::Function* d : derivedOverloads) {
                    if (d == f) { already = true; break; }
                }
                if (!already) derivedOverloads.push_back(f);
            }
        }
    }

    for (std::size_t i = 0; i < sd.methods.size(); ++i) {
        const Function& mf = sd.methods[i];
        hir::Function* hirFn = i < methodFns.size() ? methodFns[i] : nullptr;
        if (mf.isCtor) {
            defRef.ctors.push_back(hirFn);
            defRef.astCtors.push_back(&mf);
            continue;
        }
        if (mf.isDtor) {
            defRef.dtor = hirFn;
            defRef.astDtor = &mf;
            continue;
        }
        // Regular method — register under its bare name.
        const std::string_view qualName = mf.name;
        const std::size_t pos = qualName.rfind("::");
        const std::string_view bareName =
            (pos != std::string_view::npos)
                ? qualName.substr(pos + 2)
                : qualName;
        if (hirFn) {
            // If overriding a base virtual method, replace the base entry.
            auto& overloads = defRef.methods[bareName];
            bool replaced = false;
            for (hir::Function*& d : overloads) {
                // If the base method is virtual and this is an override,
                // replace the slot.
                if (d && d->isVirtual && hirFn->isOverride) {
                    d = hirFn;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) overloads.push_back(hirFn);
        }
        defRef.astMethods.push_back(&mf);
    }

    // Build vtable (7.7): collect virtual methods from bases (in order),
    // then derived. Override matching names.
    if (isPolymorphic) {
        // First, collect from bases.
        for (const BaseClass& bc : sd.bases) {
            auto it = structs_.find(bc.type.base);
            if (it == structs_.end()) continue;
            for (const auto& vte : it->second.vtable) {
                defRef.vtable.push_back(vte);
            }
        }
        // Then, add/override from derived.
        for (std::size_t i = 0; i < sd.methods.size(); ++i) {
            const Function& mf = sd.methods[i];
            // 7.7: a method participates in the vtable if it is virtual,
            // pure-virtual, an override (override inherits virtuality
            // from the base — e.g. `~Dog() override` overrides a virtual
            // `~Animal()` even without repeating `virtual`), OR a
            // destructor of a polymorphic class (dtors are virtual when
            // the class is polymorphic).
            const bool isVirtualMethod = mf.isVirtual || mf.isPureVirtual ||
                                         mf.isOverride ||
                                         (mf.isDtor && isPolymorphic);
            if (!isVirtualMethod) continue;
            if (mf.isCtor) continue;  // ctors aren't virtual
            const std::string_view qualName = mf.name;
            const std::size_t pos = qualName.rfind("::");
            const std::string_view rawBare =
                (pos != std::string_view::npos)
                    ? qualName.substr(pos + 2)
                    : qualName;
            // 7.7: destructors share a single vtable slot. Normalize
            // `~ClassName` → `~dtor` so derived `~Dog` overrides base
            // `~Animal` (which was stored as `~dtor`).
            const bool isDtorEntry = mf.isDtor;
            std::string bareStorage;
            std::string_view bareName = rawBare;
            if (isDtorEntry) {
                bareStorage = "~dtor";
                stringStorage_.push_back(std::move(bareStorage));
                bareName = stringStorage_.back();
            }
            hir::Function* hirFn = i < methodFns.size() ? methodFns[i] : nullptr;
            // Check if this overrides an existing vtable entry.
            bool overridden = false;
            for (auto& vte : defRef.vtable) {
                if (vte.name == bareName) {
                    vte.fn = hirFn;
                    vte.isPureVirtual = mf.isPureVirtual;
                    overridden = true;
                    break;
                }
            }
            if (!overridden) {
                defRef.vtable.push_back({bareName, hirFn, mf.isPureVirtual});
            }
        }
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
    fn->returnType = resolveTemplateStructType(resolveTypeAlias(af.returnType), af.loc);
    fn->isExternC = af.isExternC;
    fn->isConstexpr = af.isConstexpr;
    fn->isConsteval = af.isConsteval;
    fn->loc = af.loc;

    lowerLifetimeAttributes(*fn, af);

    for (const Param& ap : af.params) {
        hir::Param p;
        p.type = resolveTemplateStructType(resolveTypeAlias(ap.type), ap.loc);
        p.name = ap.name;
        p.loc = ap.loc;
        p.lifetime = lowerParamAttribute(*fn, ap);
        p.defaultValue = ap.defaultValue.get();
        fn->params.push_back(std::move(p));
    }

    // Safety rule: a function definition that returns a pointer must declare
    // the returned pointer's lifetime. (Declarations — e.g. extern "C" C APIs
    // like malloc — are exempt.)
    if (af.body && fn->returnType.pointerDepth > 0 && fn->returnLifetime.empty()) {
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
    // Save/restore current_ so that instantiating a template body
    // (e.g. triggered by a call inside another function's body) does
    // not clobber the caller's current_ context.
    hir::Function* savedCurrent = current_;
    bool savedHasReturn = hasReturnInBody_;
    current_ = &fn;
    hasReturnInBody_ = false;
    scopes_.push_back({});
    dtorStacks_.push_back({});
    for (const hir::Param& p : fn.params) {
        if (!p.name.empty()) declare(p.name, p.type, p.loc);
    }
    std::unique_ptr<hir::Stmt> c = buildCompound(body, fn.loc);
    scopes_.pop_back();
    dtorStacks_.pop_back();
    if (c) {
        fn.body = std::make_unique<hir::Stmt::Compound>(
            std::move(std::get<hir::Stmt::Compound>(c->node)));
    }
    if (!fn.returnType.isConst && fn.returnType.pointerDepth == 0 &&
        fn.returnType.base != "void" && !hasReturnInBody_) {
        error(fn.loc, "function '" + std::string(fn.name) +
                          "' may reach the end without returning a value");
    }
    current_ = savedCurrent;
    hasReturnInBody_ = savedHasReturn;
}

void HirBuilder::declare(std::string_view name, hir::Type type, SourceLoc loc) {
    auto& scope = scopes_.back();
    if (scope.contains(name)) {
        error(loc, "redefinition of variable '" + std::string(name) + "'");
        return;
    }
    // Expand type aliases (e.g. `Int` → `int32_t`) so downstream
    // type comparisons, recordDtorVar, and codegen see the real type.
    type = resolveTypeAlias(type);
    scope.emplace(name, type);
}

void HirBuilder::recordDtorVar(std::string_view name, const hir::Type& type) {
    // Only struct types with a user-declared destructor need cleanup.
    if (type.pointerDepth > 0) return;  // pointers don't own
    auto it = structs_.find(type.base);
    if (it == structs_.end() || !it->second.dtor) return;
    if (dtorStacks_.empty()) return;
    dtorStacks_.back().push_back(name);
}

void HirBuilder::emitDtorCalls(std::size_t uptoScope, SourceLoc loc,
                                std::vector<std::unique_ptr<hir::Stmt>>& out) {
    // Walk scopes from innermost (top of stack) down to `uptoScope`
    // (exclusive), emitting dtor calls in reverse declaration order
    // within each scope (C++ destroys locals in reverse order).
    for (std::size_t i = dtorStacks_.size(); i-- > uptoScope;) {
        const auto& names = dtorStacks_[i];
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            // Look up the variable's type to find its dtor.
            hir::Type varType;
            for (auto sc = scopes_.rbegin(); sc != scopes_.rend(); ++sc) {
                auto f = sc->find(*it);
                if (f != sc->end()) { varType = f->second; break; }
            }
            auto sit = structs_.find(varType.base);
            if (sit == structs_.end() || !sit->second.dtor) continue;
            // Build `dtor(&var)` as an ExprStmt.
            auto stmt = std::make_unique<hir::Stmt>();
            stmt->loc = loc;
            auto& es = stmt->node.emplace<hir::Stmt::ExprStmt>();
            auto call = std::make_unique<hir::Expr>();
            call->loc = loc;
            auto& ce = call->node.emplace<hir::Expr::Call>();
            ce.callee = sit->second.dtor->name;
            ce.target = sit->second.dtor;
            // First arg: address-of the variable (IdentRef).
            auto thisArg = std::make_unique<hir::Expr>();
            thisArg->loc = loc;
            thisArg->node = hir::Expr::IdentRef{*it};
            thisArg->type = varType;
            thisArg->type.isReference = true;
            ce.args.push_back(std::move(thisArg));
            call->type = sit->second.dtor->returnType;  // void
            es.value = std::move(call);
            out.push_back(std::move(stmt));
        }
    }
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
            // 7.7: derived-to-base reference binding (upcast).
            // `Base& ref = derived;` — allowed if `to` is a base of `from`.
            // Use `find()` (not `operator[]`) because `isAssignable` is const.
            auto fromIt = structs_.find(from.base);
            if (fromIt != structs_.end()) {
                for (const auto& b : fromIt->second.bases)
                    if (b.name == to.base) return true;
            }
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
    dtorStacks_.push_back({});
    for (const auto& s : c.stmts) compound.stmts.push_back(buildStmt(*s));
    // Before popping the scope, emit destructor calls for any local
    // struct variables that have a dtor (RAII).  These run in reverse
    // declaration order (handled by emitDtorCalls).
    emitDtorCalls(dtorStacks_.size() - 1, loc, compound.stmts);
    scopes_.pop_back();
    dtorStacks_.pop_back();
    return out;
}

std::unique_ptr<hir::Stmt> HirBuilder::buildDeclaration(const Stmt::Decl& d, SourceLoc loc,
                                                        bool checkInit) {
    auto out = std::make_unique<hir::Stmt>();
    out->loc = loc;
    auto& decl = out->node.emplace<hir::Stmt::Decl>();
    // Expand type aliases (e.g. `Int` → `int32_t`) so all downstream
    // comparisons (isAssignable, struct lookup, ctor injection, etc.)
    // see the real underlying type.  Also resolve template-id types
    // (e.g. `Box<int32_t>` → instantiate and rewrite base to the
    // mangled specialization name).
    decl.type = resolveTemplateStructType(resolveTypeAlias(d.type), loc);
    decl.name = d.name;

    if (decl.type.pointerDepth == 0 && decl.type.base == "void") {
        error(loc, "variable '" + std::string(d.name) + "' cannot have type void");
    }

    // `auto` type deduction: infer type from the initializer expression.
    // `auto x = expr;`  →  type = expr.type (with pointer/ref from the declared auto)
    // `auto* p = &x;`   →  type = expr.type (already a pointer)
    // `auto& r = x;`    →  type = expr.type with isReference = true
    const bool isAuto = (decl.type.base == "auto");
    if (isAuto) {
        if (!d.init) {
            error(loc, "'auto' variable '" + std::string(d.name) +
                           "' must have an initializer");
            declare(d.name, decl.type, loc);
            return out;
        }
        decl.init = buildExpr(*d.init);
        if (!decl.init) {
            declare(d.name, decl.type, loc);
            return out;
        }
        // Infer the concrete type from the initializer.
        hir::Type inferred = decl.init->type;
        // Propagate pointer/ref qualifiers from the `auto` declaration:
        // e.g. `auto* p = expr` keeps inferred pointer depth but adds declared depth.
        inferred.pointerDepth += decl.type.pointerDepth;
        if (decl.type.isReference) inferred.isReference = true;
        if (decl.type.isConst) inferred.isConst = true;
        // Strip reference from plain `auto x = ref_expr` (copy semantics).
        if (!decl.type.isReference) inferred.isReference = false;
        decl.type = inferred;
        declare(d.name, inferred, loc);
        return out;
    }

    if (d.init) {
        // Aggregate init list for a struct: `Point p = {1, 2};`
        // Resolve each element against the corresponding field type so
        // implicit conversions (e.g. int → int32_t) are applied.
        if (auto* il = std::get_if<ivy::Expr::InitList>(&d.init->node)) {
            decl.init = buildStructInit(*il, decl.type, d.name, loc);
        } else {
            decl.init = buildExpr(*d.init);
            if (decl.init && !isAssignable(decl.type, decl.init->type)) {
                error(loc, "cannot initialize variable '" + std::string(d.name) + "' of type '" +
                               typeToString(decl.type) + "' with a value of type '" +
                               typeToString(decl.init->type) + "'");
            }
        }
    } else if (checkInit) {
        // Array variables (T[N]) are zero-initialized — no explicit
        // initializer required (like C arrays and Ivy struct variables).
        if (decl.type.arraySize > 0) {
            // No error — codegen will emit zeroinitializer for the [N x T] alloca.
        } else if (structs_.contains(decl.type.base)) {
            // Struct variables are zero-initialized (like C) — no explicit
            // initializer required.  All other types must be initialized.
            // Synthesize an aggregate initializer from default member
            // initializers (if any). Fields without a default are
            // zero-initialized by `lowerInitListInto` (which emits a
            // `store ... zeroinitializer` first). This preserves C
            // value-initialization semantics while honoring `= default`.
            const StructDef& def = structs_[decl.type.base];
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
                decl.init = buildStructInit(il, decl.type, d.name, loc);
            }
        } else {
            error(loc, "variable '" + std::string(d.name) +
                           "' must be initialized (uninitialized variables are not allowed)");
        }
    }
    declare(d.name, decl.type, loc);

    // Constructor injection: if the declared type is a struct with a
    // user-declared constructor and the declaration has no aggregate
    // initializer (`T x = {a, b}` uses C value-init, bypassing ctors),
    // synthesize a ctor call `StructName::ctor(&p, args...)` right
    // after the declaration.  The object is zero-initialized first by
    // the codegen alloca, then the ctor runs (like C++).
    if (decl.type.pointerDepth == 0 && !decl.type.isReference) {
        auto sit = structs_.find(decl.type.base);
        if (sit != structs_.end() && !sit->second.ctors.empty()) {
            std::vector<std::unique_ptr<hir::Expr>> ctorArgExprs;
            bool isAggregateInit = false;  // InitList bypasses ctor
            bool isDirectCtorCall = false;  // `T x(args)` / `T x = T(args)`
            if (decl.init) {
                if (std::holds_alternative<hir::Expr::InitList>(decl.init->node)) {
                    isAggregateInit = true;  // `T x = {a, b}` → aggregate, no ctor
                } else if (auto* call = std::get_if<hir::Expr::Call>(&decl.init->node);
                           call && call->target && call->target->isCtor) {
                    // Direct ctor call `Type(args)`: extract the args
                    // and pass them to the ctor injection (the ctor
                    // call itself is not stored as `decl.init`).
                    isDirectCtorCall = true;
                    for (const auto& a : call->args) {
                        ctorArgExprs.push_back(a ? hir::cloneHirExpr(*a) : nullptr);
                    }
                } else {
                    // `T x = other` — pass `other` as the single ctor
                    // arg (copy/move constructor).
                    ctorArgExprs.push_back(hir::cloneHirExpr(*decl.init));
                }
            }
            if (!isAggregateInit) {
                // Build arg types for overload resolution.  Ctor
                // params[0] is the implicit `this` (struct&); skip it
                // when comparing against user-supplied args.
                std::vector<hir::Type> argTypes;
                for (const auto& a : ctorArgExprs) argTypes.push_back(a ? a->type : dummyType());
                hir::Function* ctor = resolveCtorOverload(sit->second.ctors, argTypes, loc);
                if (ctor) {
                    auto ctorStmt = std::make_unique<hir::Stmt>();
                    ctorStmt->loc = loc;
                    auto& es = ctorStmt->node.emplace<hir::Stmt::ExprStmt>();
                    auto call = std::make_unique<hir::Expr>();
                    call->loc = loc;
                    auto& ce = call->node.emplace<hir::Expr::Call>();
                    ce.callee = ctor->name;
                    ce.target = ctor;
                    auto thisArg = std::make_unique<hir::Expr>();
                    thisArg->loc = loc;
                    thisArg->node = hir::Expr::IdentRef{d.name};
                    thisArg->type = decl.type;
                    thisArg->type.isReference = true;
                    ce.args.push_back(std::move(thisArg));
                    for (auto& a : ctorArgExprs) {
                        if (a) ce.args.push_back(std::move(a));
                    }
                    call->type = ctor->returnType;  // void
                    es.value = std::move(call);
                    auto wrap = std::make_unique<hir::Stmt>();
                    wrap->loc = loc;
                    auto& wc = wrap->node.emplace<hir::Stmt::Compound>();
                    // If it was a direct ctor call, the Decl doesn't
                    // need an initializer (the ctor handles it).
                    if (isDirectCtorCall) {
                        // Clear the init — the Decl becomes a plain
                        // alloca with no stored initializer.
                        std::get<hir::Stmt::Decl>(out->node).init.reset();
                    }
                    wc.stmts.push_back(std::move(out));       // the Decl
                    wc.stmts.push_back(std::move(ctorStmt));  // the ctor call
                    out = std::move(wrap);
                } else if (!ctorArgExprs.empty()) {
                    error(loc, "no matching constructor for type '" +
                               std::string(decl.type.base) + "'");
                }
            }
        }
    }

    recordDtorVar(d.name, decl.type);
    return out;
}

std::unique_ptr<hir::Stmt> HirBuilder::buildRangeFor(const Stmt::RangeFor& rf, SourceLoc loc) {
    // Desugar `for (T x : range) { body }` into an equivalent
    // index-based C-style for loop:
    //   { for (size_t __rfiN = 0; __rfiN < N; ++__rfiN) {
    //         T x = range[__rfiN];   // (or T& x = range[__rfiN])
    //         body
    //     } }
    // We build the desugared AST and delegate to `buildStmt` so that
    // all type checking, overload resolution, and RAII handling apply
    // uniformly to the synthesized loop.
    //
    // Safety: the array bound N is the compile-time `arraySize`
    // captured once from the range type — the loop body cannot mutate
    // it, so iterator-invalidation (a la std::vector::push_back) is
    // impossible for fixed-size Ivy arrays.  By-value loop variables
    // (`T x`) are copies; reference loop variables (`T& x`) alias the
    // element but the bound is still fixed.

    // Build the range expression first to discover its type.
    std::unique_ptr<hir::Expr> rangeExpr = buildExpr(*rf.range);
    if (!rangeExpr) {
        return [&] {
            auto err = std::make_unique<hir::Stmt>();
            err->loc = loc;
            err->node = hir::Stmt::Null{};
            return err;
        }();
    }

    const hir::Type& rangeType = rangeExpr->type;
    const bool isArray = (rangeType.arraySize > 0);
    if (!isArray) {
        error(loc, "range-based for currently supports only fixed-size arrays "
                    "(T[N]); got non-array range expression");
        auto err = std::make_unique<hir::Stmt>();
        err->loc = loc;
        err->node = hir::Stmt::Null{};
        return err;
    }

    // Generate unique temp names.
    const int id = rangeForCounter_++;
    const std::string idxName = "__rfi" + std::to_string(id);

    // --- Build the desugared AST: a Compound wrapping one For stmt ---
    // init: size_t __rfiN = 0;
    auto initStmt = std::make_unique<ivy::Stmt>();
    initStmt->loc = loc;
    {
        auto& d = initStmt->node.emplace<ivy::Stmt::Decl>();
        d.type.base = "size_t";
        // We need a stable string for the name (string_view must outlive).
        // Use a deque so that push_back never invalidates references to
        // existing elements (a vector reallocates and would dangle the
        // string_view for SSO-short strings like "__rfi0").
        static thread_local std::deque<std::string> namePool;
        namePool.push_back(idxName);
        d.name = std::string_view(namePool.back());
        auto zero = std::make_unique<ivy::Expr>();
        zero->loc = loc;
        zero->node = ivy::Expr::IntegerLit{0};
        d.init = std::move(zero);
    }

    // cond: __rfiN < N
    auto condExpr = std::make_unique<ivy::Expr>();
    condExpr->loc = loc;
    {
        auto& bin = condExpr->node.emplace<ivy::Expr::Binary>();
        bin.op = "<";
        bin.lhs = std::make_unique<ivy::Expr>();
        bin.lhs->loc = loc;
        bin.lhs->node = ivy::Expr::IdentRef{
            std::get<ivy::Stmt::Decl>(initStmt->node).name};
        auto rhs = std::make_unique<ivy::Expr>();
        rhs->loc = loc;
        rhs->node = ivy::Expr::IntegerLit{
            static_cast<long long>(rangeType.arraySize)};
        bin.rhs = std::move(rhs);
    }

    // incr: ++__rfiN
    auto incrExpr = std::make_unique<ivy::Expr>();
    incrExpr->loc = loc;
    {
        auto& un = incrExpr->node.emplace<ivy::Expr::Unary>();
        un.op = "++";
        un.isPrefix = true;
        un.operand = std::make_unique<ivy::Expr>();
        un.operand->loc = loc;
        un.operand->node = ivy::Expr::IdentRef{
            std::get<ivy::Stmt::Decl>(initStmt->node).name};
    }

    // body: Compound { Decl(x, init=range[__rfiN]), <user body> }
    auto bodyStmt = std::make_unique<ivy::Stmt>();
    bodyStmt->loc = loc;
    {
        auto& comp = bodyStmt->node.emplace<ivy::Stmt::Compound>();
        // Decl: T x = range[__rfiN];  (or T& x = range[__rfiN])
        auto varDecl = std::make_unique<ivy::Stmt>();
        varDecl->loc = loc;
        auto& d = varDecl->node.emplace<ivy::Stmt::Decl>();
        d.type = rf.type;
        d.type.isReference = rf.isRef;
        // name — must be stable; rf.name is already a string_view into
        // the source, so it's safe to copy.
        d.name = rf.name;
        // init: range[__rfiN]
        auto indexExpr = std::make_unique<ivy::Expr>();
        indexExpr->loc = loc;
        auto& idx = indexExpr->node.emplace<ivy::Expr::Index>();
        // Clone the range AST (not the HIR) so buildExpr runs fresh
        // inside the loop body scope where __rfiN is declared.
        idx.base = cloneExpr(*rf.range);
        idx.index = std::make_unique<ivy::Expr>();
        idx.index->loc = loc;
        idx.index->node = ivy::Expr::IdentRef{
            std::get<ivy::Stmt::Decl>(initStmt->node).name};
        d.init = std::move(indexExpr);
        comp.stmts.push_back(std::move(varDecl));
        // User body (already a unique_ptr Stmt — clone it so the
        // synthesized compound takes ownership).
        if (rf.body) comp.stmts.push_back(cloneStmt(*rf.body));
    }

    // Assemble the For stmt.
    auto forStmt = std::make_unique<ivy::Stmt>();
    forStmt->loc = loc;
    auto& fr = forStmt->node.emplace<ivy::Stmt::For>();
    fr.init = std::move(initStmt);
    fr.cond = std::move(condExpr);
    fr.incr = std::move(incrExpr);
    fr.body = std::move(bodyStmt);

    // Delegate to the normal C-style for builder.
    return buildStmt(*forStmt);
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
    // RAII: before returning, run destructors for all live locals in
    // every enclosing scope (innermost first).  This mirrors C++ where
    // `return` destroys locals in reverse construction order before
    // leaving the function.  We wrap the dtor calls + return in a
    // Compound so they all execute before the return.
    //
    // Only wrap when there is at least one live local needing a dtor;
    // otherwise the `return` stays a plain Return (so the switch
    // no-fallthrough check, which inspects `cc.stmts.back()->node`,
    // still sees `Return` rather than a wrapping Compound).
    if (!dtorStacks_.empty()) {
        std::vector<std::unique_ptr<hir::Stmt>> dtorStmts;
        emitDtorCalls(0, loc, dtorStmts);  // probe: all scopes, innermost first
        if (!dtorStmts.empty()) {
            auto wrap = std::make_unique<hir::Stmt>();
            wrap->loc = loc;
            auto& wc = wrap->node.emplace<hir::Stmt::Compound>();
            wc.stmts = std::move(dtorStmts);
            // Clear the innermost scope's dtor list so buildCompound
            // doesn't emit them again for the (unreachable) trailing
            // statements after the return.
            if (!dtorStacks_.empty()) dtorStacks_.back().clear();
            wc.stmts.push_back(std::move(out));
            return wrap;
        }
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
    // 7.8: Structured bindings — `auto [a, b] = expr;`
    // Desugar into a compound: { auto __sb = expr; auto a = __sb.f0; auto b = __sb.f1; }
    if (std::holds_alternative<A::StructuredBinding>(n)) {
        const auto& sb = std::get<A::StructuredBinding>(n);
        if (!sb.init) {
            error(s.loc, "structured binding requires an initializer");
            return out;
        }
        // Build the initializer to learn its type.
        auto initExpr = buildExpr(*sb.init);
        if (!initExpr) { out->node = hir::Stmt::Null{}; return out; }
        hir::Type initType = initExpr->type;
        // Strip reference — we copy the value.
        initType.isReference = false;
        // Look up the struct to get ordered field list.
        auto sIt = structs_.find(initType.base);
        if (sIt == structs_.end()) {
            error(s.loc, "structured binding requires a struct type, got '" +
                         std::string(initType.base) + "'");
            out->node = hir::Stmt::Null{};
            return out;
        }
        const auto& def = sIt->second;
        if (sb.names.size() != def.fields.size()) {
            error(s.loc, "structured binding expects " +
                         std::to_string(def.fields.size()) + " names, got " +
                         std::to_string(sb.names.size()));
            out->node = hir::Stmt::Null{};
            return out;
        }
        // Synthesize a compound block with N+1 declarations.
        auto& comp = out->node.emplace<hir::Stmt::Compound>();
        // 1) Temporary: `auto __sb_N = expr;`
        static int sbCounter = 0;
        std::string tmpName = "__sb_" + std::to_string(sbCounter++);
        stringStorage_.push_back(tmpName);
        std::string_view tmpNameView = stringStorage_.back();
        {
            auto tmpStmt = std::make_unique<hir::Stmt>();
            tmpStmt->loc = s.loc;
            auto& tmpDecl = tmpStmt->node.emplace<hir::Stmt::Decl>();
            tmpDecl.type = initType;
            tmpDecl.name = tmpNameView;
            tmpDecl.init = std::move(initExpr);
            declare(tmpNameView, initType, s.loc);
            comp.stmts.push_back(std::move(tmpStmt));
        }
        // 2) Per-field: `auto a = __sb.field;`
        for (std::size_t i = 0; i < sb.names.size(); ++i) {
            const auto& field = def.fields[i];
            hir::Type fieldType = def.fieldMap.find(field.name)->second.type;
            // Build member access: __sb.field
            auto baseRef = std::make_unique<hir::Expr>();
            baseRef->loc = s.loc;
            baseRef->type = initType;
            baseRef->node = hir::Expr::IdentRef{tmpNameView};
            auto memberExpr = std::make_unique<hir::Expr>();
            memberExpr->loc = s.loc;
            memberExpr->type = fieldType;
            auto& mem = memberExpr->node.emplace<hir::Expr::Member>();
            mem.base = std::move(baseRef);
            mem.name = field.name;
            mem.isArrow = false;
            // Build field declaration
            auto fieldStmt = std::make_unique<hir::Stmt>();
            fieldStmt->loc = s.loc;
            auto& fieldDecl = fieldStmt->node.emplace<hir::Stmt::Decl>();
            fieldDecl.type = fieldType;
            fieldDecl.name = sb.names[i];
            fieldDecl.init = std::move(memberExpr);
            declare(sb.names[i], fieldType, s.loc);
            comp.stmts.push_back(std::move(fieldStmt));
        }
        return out;
    }
    if (std::holds_alternative<A::Null>(n)) {
        out->node = hir::Stmt::Null{};
        return out;
    }
    if (std::holds_alternative<A::Break>(n)) {
        // RAII: run dtors for the innermost scope's locals before
        // breaking out of the loop.  We only need the current scope
        // (the loop body scope) — enclosing scopes are still alive.
        if (!dtorStacks_.empty() && !dtorStacks_.back().empty()) {
            auto wrap = std::make_unique<hir::Stmt>();
            wrap->loc = s.loc;
            auto& wc = wrap->node.emplace<hir::Stmt::Compound>();
            emitDtorCalls(dtorStacks_.size() - 1, s.loc, wc.stmts);
            dtorStacks_.back().clear();
            auto brk = std::make_unique<hir::Stmt>();
            brk->loc = s.loc;
            brk->node = hir::Stmt::Break{};
            wc.stmts.push_back(std::move(brk));
            return wrap;
        }
        out->node = hir::Stmt::Break{};
        return out;
    }
    if (std::holds_alternative<A::Continue>(n)) {
        // RAII: same as Break — destroy innermost scope locals.
        if (!dtorStacks_.empty() && !dtorStacks_.back().empty()) {
            auto wrap = std::make_unique<hir::Stmt>();
            wrap->loc = s.loc;
            auto& wc = wrap->node.emplace<hir::Stmt::Compound>();
            emitDtorCalls(dtorStacks_.size() - 1, s.loc, wc.stmts);
            dtorStacks_.back().clear();
            auto cont = std::make_unique<hir::Stmt>();
            cont->loc = s.loc;
            cont->node = hir::Stmt::Continue{};
            wc.stmts.push_back(std::move(cont));
            return wrap;
        }
        out->node = hir::Stmt::Continue{};
        return out;
    }
    if (std::holds_alternative<A::If>(n)) {
        const A::If& v = std::get<A::If>(n);
        auto& ifs = out->node.emplace<hir::Stmt::If>();
        ifs.isConstexpr = v.isConstexpr;
        ifs.cond = buildExpr(*v.cond);
        if (ifs.cond) checkCondition(*ifs.cond);
        // `if constexpr` — attempt to evaluate the condition at
        // compile time and only build the taken branch.  If the
        // condition folds to a constant, the discarded branch is
        // not built (its names are not resolved, its code is not
        // generated) — this is the key semantics of `if constexpr`.
        // If the condition cannot be folded (e.g. it references a
        // runtime value), we fall back to building both branches
        // like a regular `if`.
        if (v.isConstexpr && ifs.cond && current_) {
            ConstValue cv;
            if (evalConstExpr(*ifs.cond, *current_, cv)) {
                const bool taken = (cv.isInt ? cv.i != 0
                                             : cv.f != 0.0);
                if (taken) {
                    ifs.thenBranch = buildStmt(*v.thenBranch);
                    // No elseBranch — leave null.
                } else if (v.elseBranch) {
                    ifs.elseBranch = buildStmt(*v.elseBranch);
                    // No thenBranch — leave null.
                }
                // For `if constexpr (false)` with no else, both
                // branches stay null — the HIR If is effectively a
                // no-op; codegen should emit nothing.
                return out;
            }
            // Could not fold — fall through to build both branches.
        }
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
        dtorStacks_.push_back({});  // for-init scope (RAII)
        if (v.init) fr.init = buildStmt(*v.init);
        if (v.cond) {
            fr.cond = buildExpr(*v.cond);
            if (fr.cond) checkCondition(*fr.cond);
        }
        if (v.incr) fr.incr = buildExpr(*v.incr);
        fr.body = buildStmt(*v.body);
        scopes_.pop_back();
        dtorStacks_.pop_back();
        return out;
    }
    if (std::holds_alternative<A::RangeFor>(n)) {
        return buildRangeFor(std::get<A::RangeFor>(n), s.loc);
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

hir::Function* HirBuilder::resolveCtorOverload(
        const std::vector<hir::Function*>& candidates,
        const std::vector<hir::Type>& argTypes,
        SourceLoc loc) {
    if (candidates.empty()) return nullptr;
    if (candidates.size() == 1) {
        // Fast path: single candidate — verify the param count is
        // compatible (params[0] is `this`, skipped).
        const hir::Function* fn = candidates[0];
        const std::size_t userParamBase = 1;
        const std::size_t expected = (fn->params.size() >= userParamBase)
            ? fn->params.size() - userParamBase : 0;
        const std::size_t actual = argTypes.size();
        if (expected > actual) {
            for (std::size_t i = actual; i < expected; ++i) {
                if (!fn->params[i + userParamBase].defaultValue) return nullptr;
            }
        } else if (expected != actual) {
            return nullptr;
        }
        return candidates[0];
    }
    // Rank each ctor candidate.  Skip param[0] (the implicit `this`).
    auto rank = [this](const hir::Function* fn,
                   const std::vector<hir::Type>& args) -> int {
        const std::size_t userParamBase = 1;  // skip `this`
        const std::size_t expected = (fn->params.size() >= userParamBase)
            ? fn->params.size() - userParamBase : 0;
        const std::size_t actual = args.size();
        if (expected > actual) {
            for (std::size_t i = actual; i < expected; ++i) {
                if (!fn->params[i + userParamBase].defaultValue) return 0;
            }
        } else if (expected != actual) {
            return 0;
        }
        int score = 0;
        // Default ctor (0 user params, 0 args) is an exact match.
        if (expected == 0 && actual == 0) score += 100;
        for (std::size_t i = 0; i < expected; ++i) {
            if (i >= actual) {
                score += 80;  // default-arg match
                continue;
            }
            const hir::Type& pt = fn->params[i + userParamBase].type;
            const hir::Type& at = args[i];
            hir::Type p = pt; p.isReference = false;
            hir::Type a = at; a.isReference = false;
            if (p == a) {
                score += 100;
            } else if (isNumeric(p) && isNumeric(a)) {
                int pw = typeWidth(p.base);
                int aw = typeWidth(a.base);
                int diff = (pw > aw) ? (pw - aw) : (aw - pw);
                score += (pw > 0 && aw > 0) ? (90 - diff) : 50;
            } else if (isAssignable(pt, at)) {
                score += 1;
            } else {
                return 0;
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
        error(loc, "ambiguous constructor call");
        return nullptr;
    }
    if (!best) {
        error(loc, "no matching constructor for argument types");
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

// --- Operator overloading (7.4) -------------------------------------------
// Attempt to rewrite `base op rhs` (or `op base` for unary) as a call to
// the struct's `operator<op>` member method.  `base` must be (or
// degenerate to) a struct type.  On success, returns a Call expression
// with `this` (base) injected as the first argument.  On failure
// (base is not a struct, or no matching operator method), returns
// nullptr and leaves the caller to emit a built-in error.
std::unique_ptr<hir::Expr> HirBuilder::tryOperatorOverload(
        std::unique_ptr<hir::Expr> base, std::string_view op,
        std::unique_ptr<hir::Expr> rhs, SourceLoc loc) {
    if (!base) return nullptr;
    hir::Type baseType = base->type;
    if (baseType.isReference) baseType.isReference = false;
    auto sIt = structs_.find(baseType.base);
    if (sIt == structs_.end()) return nullptr;
    // Build the method key: "operator" + symbol (e.g. "operator+").
    // The method table is keyed by bare name, and operator methods are
    // registered under "operator+" / "operator==" / "operator[]" etc.
    std::string opKey;
    opKey.reserve(8 + op.size());
    opKey += "operator";
    opKey += op;
    auto mIt = sIt->second.methods.find(opKey);
    if (mIt == sIt->second.methods.end() || mIt->second.empty()) {
        return nullptr;
    }
    // Build arg type vector: [this-type, rhs-type?].
    std::vector<hir::Type> argTypes;
    argTypes.push_back(baseType);  // `this` is `Struct&`
    if (rhs) argTypes.push_back(rhs->type);
    // Resolve the overload among the candidate operator methods.
    hir::Function* target = resolveOverload(mIt->second, argTypes, loc);
    if (!target) return nullptr;
    // Construct the Call node, injecting `this` (base) as the first arg.
    auto out = std::make_unique<hir::Expr>();
    out->loc = loc;
    auto& call = out->node.emplace<hir::Expr::Call>();
    call.target = target;
    call.callee = target->name;
    if (rhs) call.args.push_back(std::move(rhs));
    call.args.insert(call.args.begin(), std::move(base));
    out->type = target->returnType;
    checkCall(call, loc);
    return out;
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

        // Operator overloading (7.4): if the operand is a struct type,
        // try to rewrite `op a` / `a op` as `a.operator op()`.
        if (structs_.contains(ot.base)) {
            auto call = tryOperatorOverload(std::move(un.operand), v.op,
                                            nullptr, e.loc);
            if (call) return call;
            // No matching operator method — report and bail.
            out->type = dummyType();
            error(e.loc, "struct '" + std::string(ot.base) +
                  "' has no matching operator '" + std::string(v.op) + "'");
            return out;
        }

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

        // Operator overloading (7.4): if the LHS is a struct type,
        // try to rewrite `a op b` as `a.operator op(b)`.  Member
        // operators take priority over built-in handling — if a
        // matching operator method exists, we use it; otherwise we
        // fall through to the built-in type checks below.
        if (structs_.contains(lt.base)) {
            auto call = tryOperatorOverload(std::move(bin.lhs), v.op,
                                            std::move(bin.rhs), e.loc);
            if (call) return call;
            // No matching operator method — report and bail.  Note:
            // bin.lhs/bin.rhs may have been moved-out, so don't touch them.
            out->type = dummyType();
            error(e.loc, "struct '" + std::string(lt.base) +
                  "' has no matching operator '" + std::string(v.op) + "'");
            return out;
        }

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
        // For pack-expansion arguments (`args...`), splice the expansion
        // into N concrete IdentRef arguments (one per pack element).
        for (const auto& a : v.args) {
            auto built = buildExpr(*a);
            if (built && std::holds_alternative<hir::Expr::PackExpansion>(built->node)) {
                const auto& pe = std::get<hir::Expr::PackExpansion>(built->node);
                auto it = currentPackMapping_.find(pe.packName);
                if (it != currentPackMapping_.end()) {
                    // Use the first element's type for all refs (homogeneous packs).
                    const hir::Type& elemType = it->second.types.empty()
                        ? dummyType() : it->second.types[0];
                    for (const auto& name : it->second.paramNames) {
                        auto ref = std::make_unique<hir::Expr>();
                        ref->loc = built->loc;
                        stringStorage_.push_back(name);
                        ref->node.emplace<hir::Expr::IdentRef>(stringStorage_.back());
                        ref->type = elemType;
                        call.args.push_back(std::move(ref));
                    }
                }
            } else {
                call.args.push_back(std::move(built));
            }
        }
        // Operator overloading (7.4): `obj(args)` — if the callee is a
        // struct-typed expression with an `operator()` method, rewrite
        // as `obj.operator()(args)`.  We build the callee expression and
        // check its type; if it's a struct, try the call operator.
        // Skip this for plain function names (resolved below) and
        // lambdas (handled in a dedicated branch).
        if (std::holds_alternative<A::IdentRef>(callee->node)) {
            const std::string_view bareName = std::get<A::IdentRef>(callee->node).name;
            // Only attempt if `bareName` is NOT a known function/template
            // (otherwise `a()` for a function `a` would be misread).
            bool isFunc = !resolveOverloads(bareName).empty() ||
                          lookupTemplate(bareName) != nullptr;
            if (!isFunc) {
                // Build the callee as an expression to discover its type.
                auto calleeExpr = buildExpr(*callee);
                if (calleeExpr && structs_.contains(calleeExpr->type.base)) {
                    // Collect args (already built above) into a fresh
                    // vector — tryOperatorOverload takes ownership.
                    std::vector<std::unique_ptr<hir::Expr>> opArgs;
                    for (auto& a : call.args) opArgs.push_back(std::move(a));
                    call.args.clear();
                    // tryOperatorOverload expects a single rhs for binary
                    // ops; for `operator()` with N args we need a variant.
                    // Since our helper supports at most one rhs, we
                    // handle the common case (0 or 1 arg) here and fall
                    // back to error otherwise.
                    if (opArgs.size() <= 1) {
                        auto rhs = opArgs.empty() ? nullptr : std::move(opArgs[0]);
                        auto rewritten = tryOperatorOverload(
                            std::move(calleeExpr), "()", std::move(rhs), e.loc);
                        if (rewritten) return rewritten;
                    } else {
                        // Multi-arg operator(): build a Call directly.
                        hir::Type baseType = calleeExpr->type;
                        if (baseType.isReference) baseType.isReference = false;
                        auto sIt = structs_.find(baseType.base);
                        if (sIt != structs_.end()) {
                            auto mIt = sIt->second.methods.find("operator()");
                            if (mIt != sIt->second.methods.end() && !mIt->second.empty()) {
                                std::vector<hir::Type> argTypes;
                                argTypes.push_back(baseType);
                                for (const auto& a : opArgs) {
                                    argTypes.push_back(a ? a->type : dummyType());
                                }
                                hir::Function* target = resolveOverload(mIt->second, argTypes, e.loc);
                                if (target) {
                                    auto out2 = std::make_unique<hir::Expr>();
                                    out2->loc = e.loc;
                                    auto& call2 = out2->node.emplace<hir::Expr::Call>();
                                    call2.target = target;
                                    call2.callee = target->name;
                                    for (auto& a : opArgs) call2.args.push_back(std::move(a));
                                    call2.args.insert(call2.args.begin(), std::move(calleeExpr));
                                    out2->type = target->returnType;
                                    checkCall(call2, e.loc);
                                    return out2;
                                }
                            }
                        }
                    }
                    // If we reach here, no operator() matched.
                    out->type = dummyType();
                    error(e.loc, "struct '" + std::string(calleeExpr->type.base) +
                          "' has no matching operator '()'");
                    return out;
                }
                // Not a struct variable — fall through to normal call
                // resolution (which will likely report "undeclared").
            }
        }
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
                // Constructor call: `Type(args)` where Type is a struct
                // with user-declared constructors.  Resolve the ctor
                // overload and build the call.
                auto sit = structs_.find(bareName);
                if (sit != structs_.end() && !sit->second.ctors.empty()) {
                    std::vector<hir::Type> argTypes;
                    argTypes.reserve(call.args.size());
                    for (const auto& a : call.args) {
                        argTypes.push_back(a ? a->type : dummyType());
                    }
                    call.target = resolveCtorOverload(sit->second.ctors, argTypes, e.loc);
                    if (call.target) {
                        call.callee = call.target->name;
                        // A ctor call produces a temporary of the struct
                        // type (used in `T x = T(args)` initialization).
                        out->type.base = bareName;
                        return out;
                    }
                    call.callee = bareName;
                    // resolveOverload already reported the error.
                } else {
                    auto overloads = resolveOverloads(bareName);
                    if (overloads.empty()) {
                        // Implicit template instantiation: `func(3, 4)`
                        // where `func` is a template — deduce `T` from
                        // the argument types and instantiate.
                        const Function* tplFunc = lookupTemplate(bareName);
                        if (tplFunc) {
                            std::vector<hir::Type> argTypes;
                            argTypes.reserve(call.args.size());
                            for (const auto& a : call.args) {
                                argTypes.push_back(a ? a->type : dummyType());
                            }
                            std::vector<hir::Type> deduced;
                            if (deduceTemplateArgs(*tplFunc, argTypes, deduced, e.loc)) {
                                call.tplArgs = deduced;
                                call.target = instantiateTemplate(*tplFunc, bareName,
                                                                  deduced, e.loc);
                                if (call.target) {
                                    call.callee = call.target->name;
                                } else {
                                    call.callee = bareName;
                                    error(e.loc, "failed to deduce template arguments for '" + std::string(bareName) + "'");
                                }
                            } else {
                                call.callee = bareName;
                                error(e.loc, "cannot deduce template arguments for '" + std::string(bareName) + "'");
                            }
                        } else {
                            call.callee = bareName;
                            // 8.5: Allow calls to known builtins
                            // (printf, ivy::print, etc.) without HIR
                            // registration — resolved at runtime.
                            if (!isBuiltinFn(bareName)) {
                                error(callee->loc, "call to undeclared function '" + std::string(bareName) + "'");
                            }
                        }
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
                    // 8.5: Check if this is a known interpreter/codegen
                    // builtin (ivy::print, ivy::println, printf, etc.).
                    // These are resolved at runtime, not at HIR level.
                    if (!isBuiltinFn(call.callee)) {
                        error(callee->loc, "call to undeclared function '" +
                              std::string(call.callee) + "'");
                    }
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
                // 7.7: remember whether the object was accessed through
                // a reference/pointer — virtual dispatch is needed even
                // when static type == method's class, because the dynamic
                // type may differ (e.g. `Animal& a = dog; a.speak();`).
                const bool wasRefOrPtr =
                    baseType.isReference || baseType.pointerDepth > 0;
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
                    // 7.7: Virtual dispatch. A virtual method is
                    // dispatched statically ONLY when the object is a
                    // value (not a reference/pointer) — then dynamic
                    // type == static type. Through a reference or
                    // pointer, always virtual-dispatch (the dynamic type
                    // may be a derived class).
                    bool isVirtualDispatch = call.target->isVirtual;
                    if (isVirtualDispatch && !wasRefOrPtr) {
                        // Value object — static type == dynamic type.
                        isVirtualDispatch = false;
                    }

                    // Inject `this` as the first argument.
                    auto thisExpr = std::make_unique<hir::Expr>();
                    if (mem.isArrow) {
                        thisExpr->node = hir::Expr::Member{};
                        auto& tm = std::get<hir::Expr::Member>(thisExpr->node);
                        tm.base = std::move(baseExpr);
                        tm.name = mem.name;
                        tm.isArrow = true;
                        thisExpr->type = baseType;
                    } else {
                        thisExpr = std::move(baseExpr);
                    }
                    thisExpr->loc = e.loc;

                    if (isVirtualDispatch) {
                        // Find vtable slot for this method.
                        std::size_t slot = 0;
                        bool foundSlot = false;
                        for (std::size_t s = 0; s < sIt->second.vtable.size(); ++s) {
                            if (sIt->second.vtable[s].name == mem.name) {
                                slot = s;
                                foundSlot = true;
                                break;
                            }
                        }
                        if (foundSlot) {
                            // Emit a virtual-dispatch Call (7.7).
                            // NOTE: `call` is a reference to the Call already
                            // emplaced in `out->node` (line ~2147).  We must
                            // NOT `emplace` again — that would destroy-then-
                            // move-from-self, leaving `args` empty (UB).
                            call.isVirtual = true;
                            call.vtableSlot = slot;
                            call.methodName = mem.name;
                            call.args.insert(call.args.begin(), std::move(thisExpr));
                            out->type = call.target->returnType;
                            out->loc = e.loc;
                            return out;
                        }
                        // Fallback: static dispatch if slot not found.
                    }

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
        } else if (isBuiltinFn(call.callee)) {
            // 8.5: Builtin functions (ivy::print, printf, etc.) return
            // void (except printf which returns int, but that's rarely
            // used). Set void type for print/println, int for printf.
            if (call.callee == "printf" || call.callee == "puts" ||
                call.callee == "putchar")
                out->type = hir::Type{"int", false, false, 0};
            else
                out->type = hir::Type{"void", false, false, 0};
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
            // Operator overloading (7.4): if the base is a struct type
            // (not array, not pointer), try `operator[]`.
            if (bt.arraySize == 0 && bt.pointerDepth == 0 &&
                structs_.contains(bt.base)) {
                auto call = tryOperatorOverload(std::move(idx.base), "[]",
                                                std::move(idx.index), e.loc);
                if (call) return call;
                out->type = dummyType();
                error(e.loc, "struct '" + std::string(bt.base) +
                      "' has no matching operator '[]'");
                return out;
            }
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
    // --- variadic template pack expansion (7.6) ---
    if (std::holds_alternative<A::SizeofPack>(n)) {
        // sizeof...(pack) — fold to an integer literal at build time.
        const A::SizeofPack& v = std::get<A::SizeofPack>(n);
        std::size_t count = 0;
        auto it = currentPackMapping_.find(v.packName);
        if (it != currentPackMapping_.end()) count = it->second.types.size();
        auto out = std::make_unique<hir::Expr>();
        out->loc = e.loc;
        out->node.emplace<hir::Expr::IntegerLit>(static_cast<long long>(count));
        out->type.base = "int64_t";
        return out;
    }
    // 8.3: sizeof(type) — fold to an integer literal at build time.
    if (std::holds_alternative<A::SizeOf>(n)) {
        const A::SizeOf& v = std::get<A::SizeOf>(n);
        hir::Type resolved = resolveTypeAlias(v.operandType);
        std::uint64_t sz = typeSize(resolved);
        auto out = std::make_unique<hir::Expr>();
        out->loc = e.loc;
        out->node.emplace<hir::Expr::IntegerLit>(static_cast<long long>(sz));
        out->type.base = "int64_t";
        return out;
    }
    // 8.3: alignof(type) — fold to an integer literal at build time.
    if (std::holds_alternative<A::AlignOf>(n)) {
        const A::AlignOf& v = std::get<A::AlignOf>(n);
        hir::Type resolved = resolveTypeAlias(v.operandType);
        std::uint32_t al = typeAlign(resolved);
        auto out = std::make_unique<hir::Expr>();
        out->loc = e.loc;
        out->node.emplace<hir::Expr::IntegerLit>(static_cast<long long>(al));
        out->type.base = "int64_t";
        return out;
    }
    // 8.4: static_cast / reinterpret_cast / const_cast / C-style cast.
    if (std::holds_alternative<A::Cast>(n)) {
        const A::Cast& v = std::get<A::Cast>(n);
        hir::Type targetType = resolveTypeAlias(v.targetType);
        // Strip reference from target — casts produce values, not references.
        targetType.isReference = false;
        auto operand = buildExpr(*v.operand);
        if (!operand) return operand;
        hir::Type fromType = operand->type;
        fromType.isReference = false;

        if (v.kind == A::CastKind::Static) {
            // static_cast: numeric↔numeric, pointer↔pointer (same type),
            // pointer↔void*, nullptr→pointer, enum→int, int→enum.
            bool ok = false;
            if (isNumeric(targetType) && isNumeric(fromType)) {
                ok = true;
            } else if (targetType.pointerDepth > 0 && fromType.pointerDepth > 0) {
                // Pointer-to-pointer: same base type or void*.
                ok = targetType.base == fromType.base ||
                     targetType.base == "void" || fromType.base == "void";
            } else if (targetType.pointerDepth > 0 &&
                       (fromType.base == "nullptr" || fromType.base == "nullptr_t")) {
                ok = true;  // nullptr → pointer
            } else if (targetType.pointerDepth > 0 && isIntegerBase(fromType.base)) {
                ok = true;  // int → pointer (static_cast allows this)
            } else if (isIntegerBase(targetType.base) && fromType.pointerDepth > 0) {
                ok = true;  // pointer → int (static_cast allows this)
            } else if (isIntegerBase(targetType.base) && enums_.contains(fromType.base)) {
                ok = true;  // enum → int
            } else if (enums_.contains(targetType.base) && isIntegerBase(fromType.base)) {
                ok = true;  // int → enum
            }
            if (!ok) {
                error(e.loc, "static_cast: cannot convert from '" +
                      std::string(fromType.base) + "' to '" +
                      std::string(targetType.base) + "'");
            }
        } else if (v.kind == A::CastKind::Reinterpret ||
                   v.kind == A::CastKind::CStyle) {
            // reinterpret_cast / C-style cast: requires [[ivy::unsafe]].
            requireUnsafe(e.loc, (v.kind == A::CastKind::Reinterpret
                                  ? "reinterpret_cast"
                                  : "C-style cast"));
            // Must be pointer↔pointer or pointer↔integer.
            bool ok = false;
            if (targetType.pointerDepth > 0 && fromType.pointerDepth > 0) {
                ok = true;  // pointer → pointer (any type)
            } else if (targetType.pointerDepth > 0 && isIntegerBase(fromType.base)) {
                ok = true;  // int → pointer
            } else if (isIntegerBase(targetType.base) && fromType.pointerDepth > 0) {
                ok = true;  // pointer → int
            } else if (isNumeric(targetType) && isNumeric(fromType)) {
                ok = true;  // numeric ↔ numeric (C-style allows this)
            }
            if (!ok) {
                error(e.loc, "reinterpret_cast: cannot convert from '" +
                      std::string(fromType.base) + "' to '" +
                      std::string(targetType.base) + "'");
            }
        } else if (v.kind == A::CastKind::Const) {
            // const_cast: add/remove const on pointer/reference types.
            // Only valid between same-base types differing only in const.
            requireUnsafe(e.loc, "const_cast");
            if (targetType.base != fromType.base ||
                targetType.pointerDepth != fromType.pointerDepth) {
                error(e.loc, "const_cast: target type must match source "
                      "type except for const qualifier");
            }
        }

        auto out = std::make_unique<hir::Expr>();
        out->loc = e.loc;
        out->type = targetType;
        out->node.emplace<hir::Expr::Cast>(v.kind, std::move(operand));
        return out;
    }
    if (std::holds_alternative<A::PackExpansion>(n)) {
        // `expr...` — record the pattern + pack name + count. Callers
        // (e.g. Call argument splicing) inspect this node to expand.
        const A::PackExpansion& v = std::get<A::PackExpansion>(n);
        auto out = std::make_unique<hir::Expr>();
        out->loc = e.loc;
        auto& pe = out->node.emplace<hir::Expr::PackExpansion>();
        if (v.pattern) pe.pattern = buildExpr(*v.pattern);
        if (v.pattern && std::holds_alternative<A::IdentRef>(v.pattern->node)) {
            pe.packName = std::get<A::IdentRef>(v.pattern->node).name;
            auto it = currentPackMapping_.find(pe.packName);
            if (it != currentPackMapping_.end()) pe.count = it->second.types.size();
        }
        out->type = dummyType();
        return out;
    }
    if (std::holds_alternative<A::FoldExpr>(n)) {
        // Fold expression — expand into a Binary chain using the
        // concrete pack element parameter names.
        const A::FoldExpr& v = std::get<A::FoldExpr>(n);
        // Helper: extract pack element names + type from an expression
        // that is either a bare IdentRef to the pack or a PackExpansion
        // of such a ref, or any expression containing a PackExpansion
        // (e.g. a Call with a pack-expansion argument).
        std::vector<std::string> elemNames;
        hir::Type elemType = dummyType();
        // Recursively search for a PackExpansion in an expression tree.
        // Returns the pack name if found, or empty string_view if not.
        std::function<std::string_view(const Expr&)> findPackExpansion =
            [&](const Expr& ex) -> std::string_view {
            if (std::holds_alternative<A::PackExpansion>(ex.node)) {
                const auto& pex = std::get<A::PackExpansion>(ex.node);
                if (pex.pattern && std::holds_alternative<A::IdentRef>(pex.pattern->node))
                    return std::get<A::IdentRef>(pex.pattern->node).name;
            }
            if (std::holds_alternative<A::IdentRef>(ex.node)) {
                return std::get<A::IdentRef>(ex.node).name;
            }
            // Recurse into Call args.
            if (std::holds_alternative<A::Call>(ex.node)) {
                const auto& c = std::get<A::Call>(ex.node);
                for (const auto& a : c.args) {
                    if (a) {
                        auto pn = findPackExpansion(*a);
                        if (!pn.empty()) return pn;
                    }
                }
            }
            return {};
        };
        auto extractPack = [&](const std::unique_ptr<Expr>& pe) -> bool {
            if (!pe) return false;
            std::string_view packName = findPackExpansion(*pe);
            if (packName.empty()) return false;
            auto it = currentPackMapping_.find(packName);
            if (it == currentPackMapping_.end()) return false;
            elemNames = it->second.paramNames;
            if (!it->second.types.empty()) elemType = it->second.types[0];
            return true;
        };
        // Determine which side is the pack and build the chain.
        // Unary left fold `(... op pack)`: lhs=null, pack on rhs.
        // Unary right fold `(pack op ...)`: rhs=null, pack on lhs.
        // Binary fold `(lhs op ... op rhs)`: pack side determined by
        //   which side actually references a pack.
        bool ok = false;
        std::unique_ptr<hir::Expr> init;  // fixed operand (binary fold)
        // Track which AST expr is the pack pattern (for cloning).
        const Expr* packPattern = nullptr;
        if (!v.lhs && v.rhs) {
            ok = extractPack(v.rhs);
            if (ok) packPattern = v.rhs.get();
        } else if (!v.rhs && v.lhs) {
            ok = extractPack(v.lhs);
            if (ok) packPattern = v.lhs.get();
        } else if (v.lhs && v.rhs) {
            ok = extractPack(v.lhs);
            if (ok) {
                packPattern = v.lhs.get();
                init = buildExpr(*v.rhs);
            } else {
                ok = extractPack(v.rhs);
                if (ok) {
                    packPattern = v.rhs.get();
                    init = buildExpr(*v.lhs);
                }
            }
        }
        if (!ok) {
            auto out = std::make_unique<hir::Expr>();
            out->loc = e.loc;
            out->type = dummyType();
            error(e.loc, "fold expression references unknown parameter pack");
            return out;
        }
        // Empty pack: return the identity element for the operator.
        // `&&`→true, `||`→false, `,`→void, else 0.
        if (elemNames.empty()) {
            auto out = std::make_unique<hir::Expr>();
            out->loc = e.loc;
            if (v.op == "&&") {
                out->node.emplace<hir::Expr::BoolLit>(true);
                out->type.base = "bool";
            } else if (v.op == "||") {
                out->node.emplace<hir::Expr::BoolLit>(false);
                out->type.base = "bool";
            } else if (v.op == ",") {
                // Empty comma fold: no side effects, return a dummy value.
                out->node.emplace<hir::Expr::IntegerLit>(0LL);
                out->type.base = "int64_t";
            } else {
                out->node.emplace<hir::Expr::IntegerLit>(0LL);
                out->type.base = "int64_t";
            }
            return out;
        }
        // Clone the pack pattern N times, replacing the PackExpansion
        // with an IdentRef to the i-th element parameter.
        // For bare `args` (no PackExpansion wrapper), substitute the
        // IdentRef directly.
        std::string_view packName = findPackExpansion(*packPattern);
        std::vector<std::unique_ptr<hir::Expr>> builtElems;
        for (const auto& name : elemNames) {
            auto cloned = cloneExpr(*packPattern);
            // Recursively substitute any PackExpansion{IdentRef(packName)}
            // or bare IdentRef(packName) with IdentRef(name).
            // Use stringStorage_ for pointer-stable string_view (same
            // pattern as Call argument splicing above).
            std::function<void(Expr&)> substitute = [&](Expr& ex) {
                if (std::holds_alternative<A::PackExpansion>(ex.node)) {
                    auto& pex = std::get<A::PackExpansion>(ex.node);
                    if (pex.pattern) {
                        if (std::holds_alternative<A::IdentRef>(pex.pattern->node) &&
                            std::get<A::IdentRef>(pex.pattern->node).name == packName) {
                            stringStorage_.push_back(name);
                            ex.node.emplace<A::IdentRef>(stringStorage_.back());
                            return;
                        }
                        substitute(*pex.pattern);
                    }
                    return;
                }
                if (std::holds_alternative<A::IdentRef>(ex.node)) {
                    if (std::get<A::IdentRef>(ex.node).name == packName) {
                        stringStorage_.push_back(name);
                        ex.node.emplace<A::IdentRef>(stringStorage_.back());
                    }
                    return;
                }
                if (std::holds_alternative<A::Call>(ex.node)) {
                    auto& c = std::get<A::Call>(ex.node);
                    if (c.callee) substitute(*c.callee);
                    for (auto& a : c.args) if (a) substitute(*a);
                    return;
                }
                if (std::holds_alternative<A::Unary>(ex.node)) {
                    auto& u = std::get<A::Unary>(ex.node);
                    if (u.operand) substitute(*u.operand);
                    return;
                }
                if (std::holds_alternative<A::Binary>(ex.node)) {
                    auto& b = std::get<A::Binary>(ex.node);
                    if (b.lhs) substitute(*b.lhs);
                    if (b.rhs) substitute(*b.rhs);
                    return;
                }
                if (std::holds_alternative<A::Member>(ex.node)) {
                    auto& m = std::get<A::Member>(ex.node);
                    if (m.base) substitute(*m.base);
                    return;
                }
                if (std::holds_alternative<A::Index>(ex.node)) {
                    auto& idx = std::get<A::Index>(ex.node);
                    if (idx.base) substitute(*idx.base);
                    if (idx.index) substitute(*idx.index);
                    return;
                }
            };
            substitute(*cloned);
            builtElems.push_back(buildExpr(*cloned));
        }

        std::unique_ptr<hir::Expr> result;
        if (init) {
            // Binary fold: init op a0 op a1 ...
            result = std::move(init);
            for (auto& elem : builtElems) {
                auto b = std::make_unique<hir::Expr>();
                b->loc = e.loc;
                b->node.emplace<hir::Expr::Binary>(v.op, std::move(result), std::move(elem));
                b->type = elemType;
                result = std::move(b);
            }
        } else {
            // Unary fold: chain elements left-to-right.
            result = std::move(builtElems[0]);
            for (std::size_t i = 1; i < builtElems.size(); ++i) {
                auto b = std::make_unique<hir::Expr>();
                b->loc = e.loc;
                b->node.emplace<hir::Expr::Binary>(v.op, std::move(result),
                                                   std::move(builtElems[i]));
                b->type = elemType;
                result = std::move(b);
            }
        }
        return result ? std::move(result) : [&] {
            // Empty pack — identity element.
            auto out = std::make_unique<hir::Expr>();
            out->loc = e.loc;
            if (v.op == "&&") {
                out->node.emplace<hir::Expr::BoolLit>(true);
                out->type.base = "bool";
            } else if (v.op == "||") {
                out->node.emplace<hir::Expr::BoolLit>(false);
                out->type.base = "bool";
            } else {
                out->node.emplace<hir::Expr::IntegerLit>(0LL);
                out->type.base = "int64_t";
            }
            return out;
        }();
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
    dtorStacks_.push_back({});  // parameter scope (RAII)
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
        dtorStacks_.push_back({});  // lambda body scope (RAII)

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
        dtorStacks_.pop_back();

        rawFn->body = std::make_unique<hir::Stmt::Compound>(
            std::move(compound));
    }

    scopes_.pop_back();
    dtorStacks_.pop_back();
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
    // Register type aliases (pass 0) so alias names are usable as types
    // in function/struct member declarations (pass 1).
    for (const UsingDecl& ud : ast_.usingDecls) buildUsing(ud);
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
                        // Expand `using` aliases on the AST side so the
                        // comparison sees the real underlying type (the
                        // HIR side was already resolved in buildSignature).
                        hir::Type b = resolveTypeAlias(af.params[i].type);
                        b.isReference = false;
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
                        // Expand `using` aliases on the AST side so the
                        // comparison sees the real underlying type (the
                        // HIR side was already resolved in buildStruct).
                        hir::Type b = resolveTypeAlias(mf.params[i].type);
                        b.isReference = false;
                        if (!(a == b)) match = false;
                    }
                    if (match) { fn = cand; break; }
                }
            }
            if (fn) {
                currentNsPrefix_ = mf.namespacePrefix;
                // Constructor member initializer list: `: x(42), y(3)`.
                // Synthesize `this->field = arg` assignments and prepend
                // them to the body before building it.  Each assignment
                // is `*this . field = arg` (member access on `this`).
                if (mf.isCtor && !mf.memberInits.empty()) {
                    Stmt::Compound synthBody;
                    synthBody.stmts.reserve(mf.memberInits.size() + mf.body->stmts.size());
                    for (const auto& mi : mf.memberInits) {
                        auto stmt = std::make_unique<Stmt>();
                        stmt->loc = mf.loc;
                        auto& es = stmt->node.emplace<Stmt::ExprStmt>();
                        auto assign = std::make_unique<Expr>();
                        assign->loc = mf.loc;
                        // lhs: this.field (Member, dot — `this` is a
                        // reference, not a pointer)
                        auto lhs = std::make_unique<Expr>();
                        lhs->loc = mf.loc;
                        auto base = std::make_unique<Expr>();
                        base->loc = mf.loc;
                        base->node = Expr::This{};
                        lhs->node = Expr::Member{std::move(base), mi.name, /*isArrow*/false, /*isScope*/false};
                        // rhs: clone the init expression
                        auto rhs = mi.arg ? ivy::cloneExpr(*mi.arg) : nullptr;
                        assign->node = Expr::Assign{"=", std::move(lhs), std::move(rhs)};
                        es.value = std::move(assign);
                        synthBody.stmts.push_back(std::move(stmt));
                    }
                    // Append the original body statements (clone so the
                    // AST TranslationUnit still owns the originals).
                    for (const auto& s : mf.body->stmts) {
                        synthBody.stmts.push_back(ivy::cloneStmt(*s));
                    }
                    buildBody(*fn, synthBody);
                } else {
                    buildBody(*fn, *mf.body);
                }
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
    // For variadic params (`typename... Args`), the pack absorbs all
    // remaining type arguments.
    std::unordered_map<std::string_view, hir::Type> mapping;
    // Track whether this template has a variadic pack, and if so, its
    // index in tplParams. We also build a PackInfo to populate
    // currentPackMapping_ before building the body.
    bool hasVariadic = false;
    std::size_t variadicIdx = 0;
    for (std::size_t i = 0; i < tplFunc.tplParams.size(); ++i) {
        if (tplFunc.tplParams[i].isVariadic) {
            hasVariadic = true;
            variadicIdx = i;
            break;
        }
    }
    // Non-variadic params consume tplArgs one-to-one.
    // The variadic pack (if any) consumes all remaining args.
    std::size_t nonVariadicCount = tplFunc.tplParams.size();
    if (hasVariadic) {
        // The variadic param itself doesn't consume a slot in the
        // one-to-one mapping; it takes all the remaining args.
        nonVariadicCount = variadicIdx;
    }
    for (std::size_t i = 0; i < nonVariadicCount && i < tplArgs.size(); ++i) {
        if (tplFunc.tplParams[i].isTypename) {
            mapping[tplFunc.tplParams[i].name] = tplArgs[i];
        }
    }
    // Build the pack info for the variadic param.
    PackInfo packInfo;
    std::vector<hir::Type> packTypes;
    // The function parameter name that corresponds to the pack (e.g.
    // `args` in `Args... args`).  Fold expressions and sizeof...(pack)
    // in the body reference the function parameter name, while the
    // template parameter name (`Args`) is what we store as the key.
    // We record both so that lookups by either name succeed.
    std::string_view packFuncParamName;
    if (hasVariadic) {
        const std::string_view vName = tplFunc.tplParams[variadicIdx].name;
        // Find the function parameter whose type or name matches the
        // variadic template param name.
        for (const Param& ap : tplFunc.params) {
            if (ap.type.base == vName || ap.name == vName) {
                if (!ap.name.empty()) packFuncParamName = ap.name;
                break;
            }
        }
        // All args from variadicIdx onward belong to the pack.
        for (std::size_t i = variadicIdx; i < tplArgs.size(); ++i) {
            packTypes.push_back(tplArgs[i]);
        }
        // Synthesize per-element parameter names: __args0, __args1, ...
        // Use the function param name if available (e.g. `args`),
        // otherwise fall back to the template param name.
        const std::string baseName = packFuncParamName.empty()
            ? std::string(vName) : std::string(packFuncParamName);
        for (std::size_t i = 0; i < packTypes.size(); ++i) {
            packInfo.paramNames.push_back(
                "__" + baseName + std::to_string(i));
        }
        packInfo.types = packTypes;
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

    // Substitute param types. For the variadic parameter, expand into
    // N separate HIR params (one per pack element).
    for (const Param& ap : tplFunc.params) {
        // Check if this param is the variadic pack parameter. We match
        // by name against the variadic template param.
        bool isPackParam = false;
        if (hasVariadic) {
            const std::string_view vName = tplFunc.tplParams[variadicIdx].name;
            // The param type base should be the pack type name, and the
            // param name should be the pack parameter name (e.g. `Args... args`).
            if (ap.type.base == vName || ap.name == vName) {
                isPackParam = true;
            }
        }
        if (isPackParam) {
            // Expand the pack into N params, one per concrete type.
            for (std::size_t i = 0; i < packInfo.types.size(); ++i) {
                hir::Param p;
                p.type = packInfo.types[i];
                // Use the synthesized name. Store in stringStorage_ for stability.
                stringStorage_.push_back(packInfo.paramNames[i]);
                p.name = stringStorage_.back();
                p.loc = ap.loc;
                fn->params.push_back(std::move(p));
            }
        } else {
            hir::Param p;
            p.type = substituteType(ap.type, mapping);
            p.name = ap.name;
            p.loc = ap.loc;
            fn->params.push_back(std::move(p));
        }
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
        // Set up the pack mapping so pack-expansion nodes (`args...`,
        // `sizeof...(args)`, fold expressions) can resolve during body build.
        // We register the pack under both the template parameter name
        // (e.g. `Args`) and the function parameter name (e.g. `args`),
        // because AST nodes may reference either.
        decltype(currentPackMapping_) savedPack;
        if (hasVariadic) {
            savedPack = std::move(currentPackMapping_);
            currentPackMapping_.clear();
            const std::string_view tplName = tplFunc.tplParams[variadicIdx].name;
            // Store under the template param name.
            currentPackMapping_[tplName] = packInfo;  // copy
            // Also store under the function param name if different.
            if (!packFuncParamName.empty() && packFuncParamName != tplName) {
                currentPackMapping_[packFuncParamName] = std::move(packInfo);
            }
        }
        buildBody(*raw, clonedCompound);
        if (hasVariadic) {
            currentPackMapping_ = std::move(savedPack);
        }
        currentNsPrefix_ = savedNs;
    }

    return raw;
}

bool HirBuilder::deduceTemplateArgs(const Function& tplFunc,
                                    const std::vector<hir::Type>& argTypes,
                                    std::vector<hir::Type>& deduced,
                                    SourceLoc loc) const {
    // Collect the template type parameter names (only `typename` params;
    // non-type template params are not supported for deduction yet).
    std::vector<std::string_view> typeParams;
    for (const auto& tp : tplFunc.tplParams) {
        if (tp.isTypename) typeParams.push_back(tp.name);
    }
    if (typeParams.empty()) return false;  // nothing to deduce

    // Detect a variadic parameter pack (7.6). If present, the pack
    // absorbs all "extra" call arguments beyond the non-variadic params.
    bool hasVariadic = false;
    std::size_t variadicIdx = 0;
    std::string_view variadicName;
    for (std::size_t i = 0; i < tplFunc.tplParams.size(); ++i) {
        if (tplFunc.tplParams[i].isVariadic && tplFunc.tplParams[i].isTypename) {
            hasVariadic = true;
            variadicIdx = i;
            variadicName = tplFunc.tplParams[i].name;
            break;
        }
    }

    // Build a mapping: type-param-name → deduced type.
    // For the variadic pack, we accumulate multiple deduced types.
    std::unordered_map<std::string_view, hir::Type> mapping;
    std::vector<hir::Type> variadicPack;  // deduced types for the pack

    // Match each call argument against the corresponding template param type.
    // If the param type references a type parameter `T`, we deduce `T` from
    // the argument's type.
    const std::size_t numParams = tplFunc.params.size();
    const std::size_t numArgs = argTypes.size();

    // Determine the number of non-variadic (fixed) function params. The
    // variadic function param (if any) maps to the variadic template
    // param and expands to absorb all extra args.
    // The variadic function param is the one whose type or name matches
    // the variadic template param name.
    std::size_t fixedParamCount = numParams;
    std::size_t variadicParamIdx = numParams;  // index of variadic fn param
    if (hasVariadic) {
        for (std::size_t i = 0; i < numParams; ++i) {
            if (tplFunc.params[i].type.base == variadicName ||
                tplFunc.params[i].name == variadicName) {
                variadicParamIdx = i;
                fixedParamCount = i;  // params before the variadic are fixed
                break;
            }
        }
    }

    if (!hasVariadic) {
        if (numArgs < numParams) {
            // Allow trailing params with default values.
            for (std::size_t i = numArgs; i < numParams; ++i) {
                if (!tplFunc.params[i].defaultValue) return false;
            }
        }
    } else {
        // With a variadic pack, we need at least `fixedParamCount` args.
        if (numArgs < fixedParamCount) return false;
    }

    // Match fixed (non-variadic) params against the first N args.
    const std::size_t fixedMatchCount =
        (numArgs < fixedParamCount) ? numArgs : fixedParamCount;
    for (std::size_t i = 0; i < fixedMatchCount; ++i) {
        // Resolve the param type through type aliases (so `using`-aliases
        // in the param type are expanded before deduction).
        hir::Type paramType = resolveTypeAlias(tplFunc.params[i].type);
        hir::Type argType = argTypes[i];

        // Strip reference-ness for deduction (a `const T&` param can
        // bind to an lvalue of type `T`).
        paramType.isReference = false;
        argType.isReference = false;

        // Case 1: param type is exactly `T` (a bare type parameter).
        // Check if `paramType.base` is one of the type params.
        bool isTypeParam = false;
        for (std::string_view tp : typeParams) {
            if (paramType.base == tp) { isTypeParam = true; break; }
        }
        if (isTypeParam) {
            // Deduce: T = argType (stripping const from the arg if the
            // param is `const T&` — the const is on the param side).
            hir::Type d = argType;
            // If the param is `const T`, the deduced type should not
            // inherit the arg's const (e.g. `const int&` → `T=int`).
            if (paramType.isConst) d.isConst = false;
            // If the param is `T*` (pointer to T), the arg should be a
            // pointer and we deduce the pointee type.
            if (paramType.pointerDepth > 0) {
                if (argType.pointerDepth != paramType.pointerDepth) {
                    // Can't deduce — pointer depth mismatch.
                    return false;
                }
                // Strip pointer depth for the deduced type.
                d.pointerDepth = 0;
            }
            auto it = mapping.find(paramType.base);
            if (it != mapping.end()) {
                // Already deduced — check consistency.
                hir::Type existing = it->second;
                existing.isConst = false;
                d.isConst = false;
                if (!(existing == d)) return false;  // conflicting deduction
            } else {
                mapping[paramType.base] = d;
            }
            continue;
        }

        // Case 2: param is `T[N]` (array of T) — deduce T from arg
        // (which should be `T*` after array-to-pointer decay).
        if (paramType.arraySize > 0 && paramType.pointerDepth == 0) {
            // The param type base should be a type parameter.
            bool baseIsTp = false;
            for (std::string_view tp : typeParams) {
                if (paramType.base == tp) { baseIsTp = true; break; }
            }
            if (baseIsTp && argType.pointerDepth == 1 &&
                argType.base == paramType.base) {
                // Deduce T = argType.base (already matches).
                hir::Type d;
                d.base = argType.base;
                d.isUnsigned = argType.isUnsigned;
                auto it = mapping.find(paramType.base);
                if (it != mapping.end()) {
                    if (!(it->second == d)) return false;
                } else {
                    mapping[paramType.base] = d;
                }
                continue;
            }
            // Can't deduce from array param — skip (not a type param).
            continue;
        }

        // Case 3: param doesn't reference a type parameter at all
        // (e.g. `int` param in a mixed template).  No deduction needed
        // for this argument — skip it.
    }

    // Deduce the variadic pack: each extra arg (from fixedParamCount
    // onward) contributes its type as a pack element.
    if (hasVariadic) {
        for (std::size_t i = fixedParamCount; i < numArgs; ++i) {
            hir::Type argType = argTypes[i];
            argType.isReference = false;
            // Each pack element type is deduced from the arg type.
            // If the variadic param type is `T` (bare pack), each arg
            // deduces T for that slot.
            variadicPack.push_back(argType);
        }
    }

    // Build the deduced args vector in the order of type params.
    // For the variadic pack, its elements are appended at the pack's
    // position in the type-params list.
    deduced.clear();
    for (std::size_t i = 0; i < tplFunc.tplParams.size(); ++i) {
        const auto& tp = tplFunc.tplParams[i];
        if (!tp.isTypename) continue;
        if (tp.isVariadic) {
            // Append all pack element types.
            for (const auto& pt : variadicPack) {
                deduced.push_back(pt);
            }
        } else {
            auto it = mapping.find(tp.name);
            if (it == mapping.end()) {
                // This type parameter was not deduced — fail.
                (void)loc;  // could report error here
                return false;
            }
            deduced.push_back(it->second);
        }
    }
    return true;
}

// --- template struct instantiation ---

const StructDecl* HirBuilder::lookupStructTemplate(std::string_view name) const {
    auto it = structTemplates_.find(name);
    if (it != structTemplates_.end()) return it->second;
    // Try with namespace prefix.
    if (!currentNsPrefix_.empty()) {
        std::string qualified;
        qualified.reserve(currentNsPrefix_.size() + name.size());
        qualified += currentNsPrefix_;
        qualified += name;
        auto it2 = structTemplates_.find(qualified);
        if (it2 != structTemplates_.end()) return it2->second;
    }
    return nullptr;
}

std::string HirBuilder::mangleStructSpec(std::string_view tplName,
                                          const std::vector<hir::Type>& tplArgs) const {
    std::string mangled;
    mangled += std::string(tplName);
    mangled += "<";
    for (std::size_t i = 0; i < tplArgs.size(); ++i) {
        if (i > 0) mangled += ",";
        std::string arg;
        arg += std::string(tplArgs[i].base);
        if (tplArgs[i].isUnsigned) arg += " unsigned";
        if (tplArgs[i].isConst) arg += " const";
        for (std::uint32_t d = 0; d < tplArgs[i].pointerDepth; ++d) arg += "*";
        mangled += arg;
    }
    mangled += ">";
    return mangled;
}

std::string_view HirBuilder::instantiateStructTemplate(const StructDecl& tplStruct,
                                                        std::string_view tplName,
                                                        const std::vector<hir::Type>& tplArgs,
                                                        SourceLoc loc) {
    std::string mangled = mangleStructSpec(tplName, tplArgs);

    // Check cache.
    auto it = instantiatedStructs_.find(mangled);
    if (it != instantiatedStructs_.end()) return it->second;

    // Build type mapping: template param name → concrete type.
    std::unordered_map<std::string_view, hir::Type> mapping;
    for (std::size_t i = 0; i < tplStruct.tplParams.size() && i < tplArgs.size(); ++i) {
        if (tplStruct.tplParams[i].isTypename) {
            mapping[tplStruct.tplParams[i].name] = tplArgs[i];
        }
    }

    // Clone the AST StructDecl with substituted types.
    StructDecl cloned;
    cloned.namespacePrefix = tplStruct.namespacePrefix;
    cloned.isClass = tplStruct.isClass;
    cloned.loc = loc;
    // Store the mangled name stably.
    stringStorage_.push_back(std::move(mangled));
    cloned.name = stringStorage_.back();

    // Substitute field types.
    for (const Field& f : tplStruct.fields) {
        Field cf;
        cf.type = substituteType(resolveTypeAlias(f.type), mapping);
        cf.name = f.name;
        cf.loc = f.loc;
        if (f.init) cf.init = cloneExpr(*f.init);
        cloned.fields.push_back(std::move(cf));
    }

    // Substitute method types (return type, params, member init args
    // are AST-owned and re-built during `buildStruct`/`buildBody`).
    for (const Function& mf : tplStruct.methods) {
        Function cm = cloneFunctionAst(mf);
        // Rewrite the qualified method name to use the specialization
        // name instead of the template name.  The method name looks
        // like "TplName::method" — replace the "TplName" prefix with
        // the mangled specialization name.
        const std::string_view bareMethod = [&]() {
            const auto pos = mf.name.rfind("::");
            return (pos != std::string_view::npos) ? mf.name.substr(pos + 2) : mf.name;
        }();
        std::string qualName;
        qualName.reserve(cloned.name.size() + 2 + bareMethod.size());
        qualName += std::string(cloned.name);
        qualName += "::";
        qualName += std::string(bareMethod);
        stringStorage_.push_back(std::move(qualName));
        cm.name = stringStorage_.back();
        // Substitute return type.
        cm.returnType = substituteType(resolveTypeAlias(mf.returnType), mapping);
        // Substitute param types.
        for (auto& p : cm.params) {
            p.type = substituteType(resolveTypeAlias(p.type), mapping);
        }
        cloned.methods.push_back(std::move(cm));
    }

    // Register the specialization name in structNames_ so that the
    // cloned methods' `this` parameter (which uses `cloned.name`)
    // resolves correctly during `buildStruct`.
    // (buildStruct itself also checks structs_ for duplicates, which
    // is what we want.)

    // Build the specialized struct.
    // Save/restore namespace prefix so bare-name resolution works.
    // Also save/restore `current_` and `hasReturnInBody_` because
    // `buildBody` for cloned methods will overwrite them — we may be
    // running in the middle of building the caller's body.
    std::string_view savedNs = currentNsPrefix_;
    hir::Function* savedCurrent = current_;
    bool savedHasReturn = hasReturnInBody_;
    currentNsPrefix_ = cloned.namespacePrefix;
    buildStruct(cloned);

    // Build method bodies for the cloned methods.  `buildStruct`
    // registered them in `functions_` under "SpecName::method" but
    // did not build their bodies (bodies are built in pass 2b, which
    // only walks `ast_.structs` — cloned structs are not in the AST).
    // We resolve each cloned method's HIR function by name and build
    // its body now, while `currentNsPrefix_` is set correctly.
    for (const Function& mf : cloned.methods) {
        if (!mf.body) continue;
        hir::Function* fn = nullptr;
        auto it = functions_.find(mf.name);
        if (it != functions_.end()) {
            for (hir::Function* cand : it->second) {
                if (cand->params.size() != mf.params.size() + 1) continue;
                bool match = true;
                for (std::size_t i = 0; i < mf.params.size() && match; ++i) {
                    hir::Type a = cand->params[i + 1].type; a.isReference = false;
                    hir::Type b = substituteType(resolveTypeAlias(mf.params[i].type), mapping);
                    b.isReference = false;
                    if (!(a == b)) match = false;
                }
                if (match) { fn = cand; break; }
            }
        }
        if (fn) {
            // Constructor member initializer list synthesis.
            if (mf.isCtor && !mf.memberInits.empty()) {
                Stmt::Compound synthBody;
                synthBody.stmts.reserve(mf.memberInits.size() + mf.body->stmts.size());
                for (const auto& mi : mf.memberInits) {
                    auto stmt = std::make_unique<Stmt>();
                    stmt->loc = mf.loc;
                    auto& es = stmt->node.emplace<Stmt::ExprStmt>();
                    auto assign = std::make_unique<Expr>();
                    assign->loc = mf.loc;
                    auto lhs = std::make_unique<Expr>();
                    lhs->loc = mf.loc;
                    auto base = std::make_unique<Expr>();
                    base->loc = mf.loc;
                    base->node = Expr::This{};
                    lhs->node = Expr::Member{std::move(base), mi.name, false, false};
                    auto rhs = mi.arg ? ivy::cloneExpr(*mi.arg) : nullptr;
                    assign->node = Expr::Assign{"=", std::move(lhs), std::move(rhs)};
                    es.value = std::move(assign);
                    synthBody.stmts.push_back(std::move(stmt));
                }
                for (const auto& s : mf.body->stmts) {
                    synthBody.stmts.push_back(ivy::cloneStmt(*s));
                }
                buildBody(*fn, synthBody);
            } else {
                buildBody(*fn, *mf.body);
            }
        }
    }
    currentNsPrefix_ = savedNs;
    current_ = savedCurrent;
    hasReturnInBody_ = savedHasReturn;

    // Record in cache.
    instantiatedStructs_[cloned.name] = cloned.name;
    return cloned.name;
}

hir::Type HirBuilder::resolveTemplateStructType(const hir::Type& type, SourceLoc loc) {
    // If no template args, nothing to do.
    if (type.tplArgs.empty()) return type;

    // Look up the struct template.
    const StructDecl* tpl = lookupStructTemplate(type.base);
    if (!tpl) {
        // Not a struct template — maybe a type alias or a regular
        // struct with stray template args.  Leave as-is.
        return type;
    }

    // Instantiate (or fetch from cache).
    std::string_view specName = instantiateStructTemplate(*tpl, type.base,
                                                           type.tplArgs, loc);
    // Build the resolved type: base = specialization name, no tplArgs.
    hir::Type resolved = type;
    resolved.base = specName;
    resolved.tplArgs.clear();
    return resolved;
}

}  // namespace ivy
