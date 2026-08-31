#include "codegen/codegen.h"

#include <cctype>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// 8.1: LLVM headers for native object file emission.
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/IR/LegacyPassManager.h>

namespace ivy {
namespace {

bool isFloatType(const mir::Type& t) {
    return t.pointerDepth == 0 && (t.base == "float" || t.base == "double" ||
                                      t.base == "float32_t" || t.base == "float64_t" ||
                                      t.base == "float16_t" || t.base == "float128_t");
}

// Splits a fully-qualified C++ name ("ns1::ns2::func") into its
// scope path components: ["ns1", "ns2", "func"]. `nsPrefix` is
// optional — when provided, it is prepended to the path if `name`
// is bare (e.g. name="func", nsPrefix="ns1::" → ["ns1","func"]).
// When `name` already contains "::", the `nsPrefix` is ignored
// (the name is already fully qualified).
std::vector<std::string> splitQualifiedName(std::string_view name,
                                            std::string_view nsPrefix) {
    std::vector<std::string> parts;
    std::string qualified;
    if (name.find("::") != std::string_view::npos) {
        qualified = std::string(name);
    } else if (!nsPrefix.empty()) {
        qualified.reserve(nsPrefix.size() + name.size());
        qualified += nsPrefix;
        qualified += name;
    } else {
        qualified = std::string(name);
    }
    // Split on "::".
    std::size_t pos = 0;
    while (pos < qualified.size()) {
        const std::size_t next = qualified.find("::", pos);
        if (next == std::string::npos) {
            parts.push_back(qualified.substr(pos));
            break;
        }
        parts.push_back(qualified.substr(pos, next - pos));
        pos = next + 2;
    }
    return parts;
}

// Reads a hex/octal escape sequence starting at body[i]; advances i.
bool readHexEscape(std::string_view body, std::size_t& i, long long& out) {
    long long v = 0;
    int n = 0;
    while (i + 1 < body.size() && n < 2 &&
           std::isxdigit(static_cast<unsigned char>(body[i + 1]))) {
        const char c = body[++i];
        v = v * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        ++n;
    }
    if (n == 0) return false;
    out = v;
    return true;
}

bool readOctalEscape(std::string_view body, std::size_t& i, long long& out) {
    long long v = 0;
    int n = 0;
    while (i + 1 < body.size() && n < 3 && body[i + 1] >= '0' && body[i + 1] <= '7') {
        v = v * 8 + (body[++i] - '0');
        ++n;
    }
    out = v;
    return true;
}

}  // namespace

CodeGen::CodeGen(const mir::TranslationUnit& mir) : mir_(mir) {
#if defined(_WIN32) || defined(__CYGWIN__)
    platform_ = Platform::MSVC;
#else
    platform_ = Platform::Itanium;
#endif
}

void CodeGen::error(SourceLoc loc, std::string message) {
    diagnostics_.push_back(Diagnostic{loc.line, loc.col, std::move(message)});
    failed_ = true;
}

std::string CodeGen::newTemp() { return "%tmp." + std::to_string(temp_++); }

std::string CodeGen::newInlineBlock() { return "bb.i" + std::to_string(inlineBlock_++); }

// Escape characters that are invalid in unquoted LLVM IR identifiers.
// Template specialization names like `Box<int32_t>` contain `<` and `>`
// which are not allowed in LLVM identifiers — replace with `_`.
std::string CodeGen::escapeLlvmIdent(std::string_view name) const {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '<' || c == '>' || c == ',' || c == ' ' || c == ':' ||
            c == '&' || c == '*' || c == '(' || c == ')') {
            out += '_';
        } else {
            out += c;
        }
    }
    return out;
}

std::string CodeGen::llvmGlobalName(std::string_view name) const {
    // LLVM IR identifiers may contain [A-Za-z0-9_.$-] unquoted.
    // Mangled names (MSVC: "?foo@@...@Z", Itanium: "_Z3foov")
    // contain '@' and '?' which require quoting.
    // First escape template specialization characters (`<`, `>`, etc.)
    // so names like `Box<int32_t>::set` become valid identifiers.
    const std::string s = escapeLlvmIdent(name);
    bool needsQuote = false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '$' || c == '-')) {
            needsQuote = true;
            break;
        }
    }
    if (needsQuote) return "\"" + s + "\"";
    return s;
}

std::string CodeGen::llvmType(const mir::Type& t) const {
    // Array type T[N] → [N x T_element]
    if (t.arraySize > 0) {
        mir::Type elem = t;
        elem.arraySize = 0;
        return "[" + std::to_string(t.arraySize) + " x " + llvmType(elem) + "]";
    }
    if (t.pointerDepth > 0 || t.isReference) return "ptr";
    if (t.base == "void") return "void";
    if (t.base == "bool") return "i1";
    if (t.base == "char") return "i8";
    if (t.base == "short") return "i16";
    if (t.base == "int") return "i32";
    if (t.base == "long" || t.base == "long long") return "i64";
    if (t.base == "float") return "float";
    if (t.base == "double" || t.base == "long double") return "double";
    // Ivy builtins
    if (t.base == "int8_t") return "i8";
    if (t.base == "int16_t") return "i16";
    if (t.base == "int32_t") return "i32";
    if (t.base == "int64_t") return "i64";
    if (t.base == "uint8_t") return "i8";
    if (t.base == "uint16_t") return "i16";
    if (t.base == "uint32_t") return "i32";
    if (t.base == "uint64_t") return "i64";
    if (t.base == "float16_t") return "half";
    if (t.base == "float32_t") return "float";
    if (t.base == "float64_t") return "double";
    if (t.base == "float128_t") return "fp128";
    if (t.base == "bfloat16_t") return "bfloat";
    if (t.base == "size_t") return "i64";  // x64
    if (t.base == "ptrdiff_t") return "i64";
    if (t.base == "nullptr_t") return "ptr";
    if (t.base == "max_align_t") return "i8";  // alignment only
    // User-defined enum type — resolve to its underlying integer type.
    if (auto eIt = enumTypes_.find(t.base); eIt != enumTypes_.end()) {
        mir::Type underlying = t;
        underlying.base = eIt->second.underlyingBase;
        return llvmType(underlying);
    }
    // User-defined struct type — resolve to named LLVM struct type.
    if (auto sIt = structTypes_.find(t.base); sIt != structTypes_.end()) {
        return sIt->second.llvmType;
    }
    return "i8";  // unknown base: unreachable (HIR rejected it)
}

// Strip reference qualifier — the value type of T& is T.
mir::Type CodeGen::valueType(const mir::Type& t) const {
    mir::Type r = t;
    r.isReference = false;
    return r;
}

// LLVM type of the *value* carried by an expression — references decay to
// their pointee type (a T& expression holds a T value).
std::string CodeGen::valueLlvmType(const mir::Type& t) const {
    return llvmType(valueType(t));
}

std::string CodeGen::llvmElemType(const mir::Type& t) const {
    mir::Type elem = t;
    if (elem.pointerDepth > 0) --elem.pointerDepth;
    if (elem.pointerDepth == 0 && elem.base == "void") elem.base = "char";  // void* -> i8
    return llvmType(elem);
}

std::string CodeGen::sizeofType(const mir::Type& t) const {
    const std::string_view b = t.base;
    if (b == "bool" || b == "char") return "1";
    if (b == "short") return "2";
    if (b == "int" || b == "float") return "4";
    // Ivy builtins
    if (b == "int8_t" || b == "uint8_t") return "1";
    if (b == "int16_t" || b == "uint16_t") return "2";
    if (b == "int32_t" || b == "uint32_t" || b == "float32_t") return "4";
    if (b == "int64_t" || b == "uint64_t") return "8";
    if (b == "float16_t" || b == "bfloat16_t") return "2";
    if (b == "float64_t") return "8";
    if (b == "float128_t") return "16";
    if (b == "size_t" || b == "ptrdiff_t") return "8";  // x64
    // User-defined enum — size of its underlying type.
    if (auto eIt = enumTypes_.find(b); eIt != enumTypes_.end()) {
        mir::Type underlying = t;
        underlying.base = eIt->second.underlyingBase;
        return sizeofType(underlying);
    }
    // User-defined struct — sum of field sizes (no padding in Ivy's
    // simple layout; the HIR builder computes the real size, but we
    // only need a conservative upper bound here for alloca sizing).
    if (auto sIt = structTypes_.find(b); sIt != structTypes_.end()) {
        std::uint64_t total = 0;
        for (const auto& [fName, fType] : sIt->second.fields) {
            total += std::stoull(sizeofType(fType));
        }
        // Round up to 8 (max alignment) — conservative.
        total = (total + 7) & ~std::uint64_t(7);
        return std::to_string(total);
    }
    return "8";  // long, long long, double
}

void CodeGen::emitLine(const std::string& line) { *out_ << line << "\n"; }

// Returns the bit-width of an integer LLVM type ("i8"->8, "i1"->1, "ptr"->64).
static int intBits(const std::string& s) {
    if (s == "ptr") return 64;
    if (s.size() > 1 && s[0] == 'i') {
        int n = 0;
        for (size_t i = 1; i < s.size(); ++i)
            if (s[i] >= '0' && s[i] <= '9') n = n * 10 + (s[i] - '0');
            else return -1;
        return n;
    }
    return -1;
}

static bool isFpType(const std::string& s) {
    return s == "half" || s == "bfloat" || s == "float" || s == "double" ||
           s == "fp128" || s == "x86_fp80";
}

std::string CodeGen::emitCast(const std::string& value, const std::string& from,
                               const std::string& to, SourceLoc loc) {
    if (from == to) return value;
    // `null` literal is a ptr — if target is ptr, just use it directly.
    if (value == "null" && to == "ptr") return "null";
    const std::string t = newTemp();

    // int -> int (zext / trunc / bitcast)
    const int fb = intBits(from), tb = intBits(to);
    if (fb > 0 && tb > 0) {
        if (fb < tb) {
            emitLine(t + " = zext " + from + " " + value + " to " + to);
        } else if (fb > tb) {
            emitLine(t + " = trunc " + from + " " + value + " to " + to);
        } else {
            return value;  // same width
        }
        return t;
    }
    // float -> float (fpext / fptrunc)
    if (isFpType(from) && isFpType(to)) {
        if (from == "float" && to == "double") {
            emitLine(t + " = fpext float " + value + " to double");
        } else if (from == "double" && to == "float") {
            emitLine(t + " = fptrunc double " + value + " to float");
        } else if (from == "half" && (to == "float" || to == "double")) {
            emitLine(t + " = fpext half " + value + " to " + to);
        } else if (to == "half" && (from == "float" || from == "double")) {
            emitLine(t + " = fptrunc " + from + " " + value + " to half");
        } else {
            return value;  // give up — same kind, assume compatible
        }
        return t;
    }
    // int -> float
    if (fb > 0 && isFpType(to)) {
        emitLine(t + " = sitofp " + from + " " + value + " to " + to);
        return t;
    }
    // float -> int
    if (isFpType(from) && tb > 0) {
        emitLine(t + " = fptosi " + from + " " + value + " to " + to);
        return t;
    }
    // ptr -> int / int -> ptr
    if (from == "ptr" && tb > 0) {
        emitLine(t + " = ptrtoint ptr " + value + " to " + to);
        return t;
    }
    if (fb > 0 && to == "ptr") {
        emitLine(t + " = inttoptr " + from + " " + value + " to ptr");
        return t;
    }
    error(loc, "implicit cast from " + from + " to " + to + " is not supported");
    return value;
}

// --- string helpers ---

bool CodeGen::decodeBody(std::string_view body, std::string& bytes) {
    bytes.clear();
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c != '\\') {
            bytes.push_back(c);
            continue;
        }
        if (++i >= body.size()) {
            error({0, 0}, "literal ends inside an escape sequence");
            return false;
        }
        switch (body[i]) {
            case 'n': bytes.push_back('\n'); break;
            case 't': bytes.push_back('\t'); break;
            case 'r': bytes.push_back('\r'); break;
            case 'a': bytes.push_back('\a'); break;
            case 'b': bytes.push_back('\b'); break;
            case 'f': bytes.push_back('\f'); break;
            case 'v': bytes.push_back('\v'); break;
            case '\\': bytes.push_back('\\'); break;
            case '\'': bytes.push_back('\''); break;
            case '"': bytes.push_back('"'); break;
            case '?': bytes.push_back('?'); break;
            case 'x': {
                long long v = 0;
                if (!readHexEscape(body, i, v)) {
                    error({0, 0}, "invalid \\x escape in literal");
                    return false;
                }
                bytes.push_back(static_cast<char>(v));
                break;
            }
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                long long v = 0;
                readOctalEscape(body, i, v);
                bytes.push_back(static_cast<char>(v));
                break;
            }
            default:
                error({0, 0}, "unknown escape sequence in literal");
                return false;
        }
    }
    return true;
}

bool CodeGen::decodeString(std::string_view raw, std::string& bytes) {
    if (raw.size() >= 3 && raw[0] == 'R' && raw[1] == '"') {
        // Raw string R"delim(...)delim"
        const std::size_t open = raw.find('(');
        const std::size_t close = raw.rfind(')');
        if (open == std::string_view::npos || close == std::string_view::npos ||
            close + 1 >= raw.size() || raw.back() != '"') {
            error({0, 0}, "malformed raw string literal");
            return false;
        }
        bytes = std::string(raw.substr(open + 1, close - open - 1));
        return true;
    }
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
        error({0, 0}, "malformed string literal");
        return false;
    }
    return decodeBody(raw.substr(1, raw.size() - 2), bytes);
}

bool CodeGen::decodeChar(std::string_view raw, long long& value) {
    if (raw.size() < 2 || raw.front() != '\'' || raw.back() != '\'') {
        error({0, 0}, "malformed character literal");
        return false;
    }
    std::string bytes;
    if (!decodeBody(raw.substr(1, raw.size() - 2), bytes)) return false;
    if (bytes.empty()) {
        error({0, 0}, "empty character literal");
        return false;
    }
    value = static_cast<unsigned char>(bytes[0]);
    return true;
}

std::string CodeGen::llvmEscape(const std::string& bytes) const {
    std::string out;
    for (const unsigned char c : bytes) {
        if (c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') {
            out.push_back(static_cast<char>(c));
        } else {
            out += "\\";
            const char* hex = "0123456789ABCDEF";
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// --- pre-pass: register string globals and malloc/free usage ---

void CodeGen::collectExpr(const mir::Expr& e) {
    std::visit(
        [&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, mir::Expr::StringLit>) {
                std::string bytes;
                if (decodeString(n.raw, bytes)) {
                    const auto it = strings_.find(bytes);
                    if (it == strings_.end()) {
                        const std::string name = "@.str." + std::to_string(stringList_.size());
                        strings_.emplace(bytes, name);
                        stringList_.emplace_back(name, bytes);
                    }
                }
            } else if constexpr (std::is_same_v<T, mir::Expr::New>) {
                usesMalloc_ = true;
            } else if constexpr (std::is_same_v<T, mir::Expr::Delete>) {
                usesFree_ = true;
            }
        },
        e.node);

    // recurse into children
    const auto& n = e.node;
    using M = mir::Expr;
    if (std::holds_alternative<M::Unary>(n)) {
        collectExpr(*std::get<M::Unary>(n).operand);
    } else if (std::holds_alternative<M::Binary>(n)) {
        const M::Binary& v = std::get<M::Binary>(n);
        collectExpr(*v.lhs);
        collectExpr(*v.rhs);
    } else if (std::holds_alternative<M::Ternary>(n)) {
        const M::Ternary& v = std::get<M::Ternary>(n);
        collectExpr(*v.cond);
        collectExpr(*v.thenBranch);
        collectExpr(*v.elseBranch);
    } else if (std::holds_alternative<M::Call>(n)) {
        for (const auto& a : std::get<M::Call>(n).args) collectExpr(*a);
    } else if (std::holds_alternative<M::Index>(n)) {
        const M::Index& v = std::get<M::Index>(n);
        collectExpr(*v.base);
        collectExpr(*v.index);
    } else if (std::holds_alternative<M::Member>(n)) {
        const M::Member& v = std::get<M::Member>(n);
        collectExpr(*v.base);
    } else if (std::holds_alternative<M::Assign>(n)) {
        const M::Assign& v = std::get<M::Assign>(n);
        collectExpr(*v.lhs);
        collectExpr(*v.rhs);
    } else if (std::holds_alternative<M::InitList>(n)) {
        const M::InitList& v = std::get<M::InitList>(n);
        for (const auto& el : v.elements)
            if (el) collectExpr(*el);
    } else if (std::holds_alternative<M::Lambda>(n)) {
        const M::Lambda& v = std::get<M::Lambda>(n);
        for (const auto& ci : v.captureInits)
            if (ci) collectExpr(*ci);
    }
}

void CodeGen::collectFunction(const mir::Function& fn) {
    for (const auto& b : fn.blocks) {
        for (const auto& inst : b->insts) {
            const auto& n = inst->node;
            using I = mir::Inst;
            if (std::holds_alternative<I::Alloca>(n)) {
                const I::Alloca& a = std::get<I::Alloca>(n);
                if (a.init) collectExpr(*a.init);
            } else if (std::holds_alternative<I::Store>(n)) {
                const I::Store& s = std::get<I::Store>(n);
                if (s.target) collectExpr(*s.target);
                if (s.value) collectExpr(*s.value);
            } else if (std::holds_alternative<I::Eval>(n)) {
                const I::Eval& v = std::get<I::Eval>(n);
                if (v.value) collectExpr(*v.value);
            } else if (std::holds_alternative<I::Ret>(n)) {
                const I::Ret& r = std::get<I::Ret>(n);
                if (r.value) collectExpr(*r.value);
            } else if (std::holds_alternative<I::CondBranch>(n)) {
                const I::CondBranch& cb = std::get<I::CondBranch>(n);
                if (cb.cond) collectExpr(*cb.cond);
            }
        }
    }
}

// --- module scaffolding ---

void CodeGen::emitHeader() { emitLine("; LLVM IR emitted by ivyc"); }

void CodeGen::emitGlobals() {
    for (const auto& [name, bytes] : stringList_) {
        const std::size_t len = bytes.size() + 1;  // + NUL terminator
        emitLine(name + " = private unnamed_addr constant [" + std::to_string(len) +
                 " x i8] c\"" + llvmEscape(bytes) + "\\00\"");
    }
    if (usesMalloc_ && !declaredC_.contains("malloc")) emitLine("declare ptr @malloc(i64)");
    if (usesFree_ && !declaredC_.contains("free")) emitLine("declare void @free(ptr)");
}

// --- expressions ---

std::string CodeGen::valueName(std::string_view name) {
    const std::string base = name.empty() ? "arg" : std::string(name);
    return "%" + base + "." + std::to_string(alloca_++);
}

std::string CodeGen::lowerLValue(const mir::Expr& e) {
    const auto& n = e.node;
    using M = mir::Expr;
    if (std::holds_alternative<M::This>(n)) {
        // `this` — same semantics as IdentRef{name="this"}.
        const auto it = vars_.find("this");
        if (it == vars_.end()) return "ptr null";
        if (e.type.isReference) {
            std::string t = newTemp();
            emitLine(t + " = load ptr, ptr " + it->second);
            return t;
        }
        return it->second;
    }
    if (std::holds_alternative<M::IdentRef>(n)) {
        const std::string_view name = std::get<M::IdentRef>(n).name;
        const auto it = vars_.find(name);
        if (it == vars_.end()) return "ptr null";  // unreachable: HIR rejected it
        if (e.type.isReference) {
            // Reference: slot holds an address — load it.
            std::string t = newTemp();
            emitLine(t + " = load ptr, ptr " + it->second);
            return t;
        }
        return it->second;
    }
    if (std::holds_alternative<M::Index>(n)) {
        const M::Index& v = std::get<M::Index>(n);
        const std::string idx = lowerExpr(*v.index);
        const std::string idxTy = llvmType(v.index->type);
        const mir::Type& baseTy = v.base->type;
        std::string gep = newTemp();
        if (baseTy.arraySize > 0) {
            const std::string base = lowerExpr(*v.base);  // slot (decayed ptr)
            emitBoundsCheck(idx, idxTy, baseTy.arraySize, e.loc);
            const std::string arrTy = llvmType(baseTy);
            emitLine(gep + " = getelementptr " + arrTy + ", ptr " + base +
                     ", i32 0, " + idxTy + " " + idx);
        } else {
            const std::string base = lowerExpr(*v.base);
            emitLine(gep + " = getelementptr " + llvmElemType(e.type) + ", ptr " + base +
                     ", " + llvmType(v.index->type) + " " + idx);
        }
        return gep;
    }
    if (std::holds_alternative<M::Unary>(n)) {
        const M::Unary& u = std::get<M::Unary>(n);
        if (u.op == "*") return lowerExpr(*u.operand);  // deref lvalue: the pointer itself
    }
    if (std::holds_alternative<M::Member>(n)) {
        // Member access as lvalue: return the GEP address (no load).
        const M::Member& v = std::get<M::Member>(n);
        // For `p->f`, the base is a pointer-to-struct; we need to
        // load the pointer first, then GEP into the struct.
        std::string baseAddr;
        if (v.isArrow) {
            // base is a pointer (e.g. stored in a variable or field).
            // If it's an IdentRef, the slot holds the pointer value.
            baseAddr = lowerExpr(*v.base);
        } else {
            // `p.f` — base is a struct lvalue; get its address.
            baseAddr = lowerLValue(*v.base);
        }
        // Find field index.
        const auto sIt = structTypes_.find(e.type.base);
        // The struct type is the base type (for `. `) or the pointee
        // (for `->`). We look it up via the member's type? No — we
        // need the *struct* type, not the field type. Reconstruct:
        mir::Type structType = v.base->type;
        if (v.isArrow) --structType.pointerDepth;
        const auto sIt2 = structTypes_.find(structType.base);
        if (sIt2 == structTypes_.end()) {
            error(e.loc, "internal: struct type not found for member lvalue");
            return "null";
        }
        std::size_t fieldIdx = 0;
        for (std::size_t i = 0; i < sIt2->second.fields.size(); ++i) {
            if (sIt2->second.fields[i].name == v.name) { fieldIdx = i; break; }
        }
        std::string gep = newTemp();
        emitLine(gep + " = getelementptr " + sIt2->second.llvmType + ", ptr " + baseAddr +
                 ", i32 0, i32 " + std::to_string(fieldIdx));
        return gep;
    }
    error(e.loc, "expression is not assignable");
    return "null";
}

std::string CodeGen::lowerExpr(const mir::Expr& e) {
    const auto& n = e.node;
    using M = mir::Expr;

    if (std::holds_alternative<M::IntegerLit>(n)) {
        return std::to_string(std::get<M::IntegerLit>(n).value);
    }
    if (std::holds_alternative<M::FloatLit>(n)) {
        return std::to_string(std::get<M::FloatLit>(n).value);
    }
    if (std::holds_alternative<M::StringLit>(n)) {
        std::string bytes;
        if (!decodeString(std::get<M::StringLit>(n).raw, bytes)) return "null";
        const auto it = strings_.find(bytes);
        if (it == strings_.end()) return "null";  // unreachable: pre-pass collected it
        std::string t = newTemp();
        emitLine(t + " = getelementptr i8, ptr " + it->second + ", i64 0");
        return t;
    }
    if (std::holds_alternative<M::CharLit>(n)) {
        long long v = 0;
        decodeChar(std::get<M::CharLit>(n).raw, v);
        return std::to_string(v);
    }
    if (std::holds_alternative<M::BoolLit>(n)) {
        return std::get<M::BoolLit>(n).value ? "true" : "false";
    }
    if (std::holds_alternative<M::NullptrLit>(n)) {
        return "null";  // emitCast treats "null" as ptr
    }
    if (std::holds_alternative<M::This>(n)) {
        // `this` as an lvalue — return the address of the object.
        // For a reference `this` param, the slot holds the address.
        const auto it = vars_.find("this");
        if (it == vars_.end()) return "undef";
        if (e.type.isReference) {
            std::string addr = newTemp();
            emitLine(addr + " = load ptr, ptr " + it->second);
            return addr;
        }
        return it->second;
    }
    if (std::holds_alternative<M::IdentRef>(n)) {
        const auto it = vars_.find(std::get<M::IdentRef>(n).name);
        if (it == vars_.end()) return "undef";  // unreachable: HIR rejected it
        if (e.type.isReference) {
            // Reference: slot holds address of the referenced value.
            // Load the address, then load the value through it.
            std::string addr = newTemp();
            emitLine(addr + " = load ptr, ptr " + it->second);
            // Strip the reference for the actual value type.
            mir::Type valTy = e.type;
            valTy.isReference = false;
            std::string t = newTemp();
            emitLine(t + " = load " + llvmType(valTy) + ", ptr " + addr);
            return t;
        }
        if (e.type.arraySize > 0) {
            // Array decay: an array variable used as an expression decays
            // to a pointer to its first element — return the slot address
            // directly (like C's array-to-pointer decay).
            return it->second;
        }
        std::string t = newTemp();
        emitLine(t + " = load " + llvmType(e.type) + ", ptr " + it->second);
        return t;
    }
    if (std::holds_alternative<M::Unary>(n)) {
        const M::Unary& v = std::get<M::Unary>(n);
        if (v.op == "&") {
            // Address-of. For an IdentRef, the variable's alloca slot
            // is its address. For a Lambda, lowerExpr returns the
            // closure struct alloca slot (already an address). For
            // other lvalues, use lowerLValue.
            if (std::holds_alternative<M::IdentRef>(v.operand->node)) {
                const auto& ir = std::get<M::IdentRef>(v.operand->node);
                return vars_.at(ir.name);
            }
            if (std::holds_alternative<M::Lambda>(v.operand->node)) {
                // lowerExpr on a Lambda returns the closure alloca slot.
                return lowerExpr(*v.operand);
            }
            return lowerLValue(*v.operand);
        }
        if (v.op == "*") {
            const std::string p = lowerExpr(*v.operand);
            std::string t = newTemp();
            emitLine(t + " = load " + llvmType(e.type) + ", ptr " + p);
            return t;
        }
        if (v.op == "!") {
            std::string t = newTemp();
            emitLine(t + " = xor i1 " + lowerExpr(*v.operand) + ", true");
            return t;
        }
        if (v.op == "~") {
            std::string t = newTemp();
            emitLine(t + " = xor " + llvmType(e.type) + " " + lowerExpr(*v.operand) +
                     ", -1");
            return t;
        }
        if (v.op == "-") {
            std::string t = newTemp();
            const std::string ty = llvmType(e.type);
            const std::string op = isFloatType(e.type) ? "fsub" : "sub";
            emitLine(t + " = " + op + " " + ty + " 0, " + lowerExpr(*v.operand));
            return t;
        }
        if (v.op == "+") {
            return lowerExpr(*v.operand);
        }
        if (v.op == "++" || v.op == "--") {
            const std::string p = lowerLValue(*v.operand);
            const std::string oldV = newTemp();
            const std::string ty = valueLlvmType(v.operand->type);
            emitLine(oldV + " = load " + ty + ", ptr " + p);
            std::string newV = newTemp();
            if (v.operand->type.pointerDepth > 0) {
                emitLine(newV + " = getelementptr " + llvmElemType(v.operand->type) +
                         ", ptr " + oldV + ", i64 " + (v.op == "++" ? "1" : "-1"));
            } else if (isFloatType(v.operand->type)) {
                emitLine(newV + " = fadd " + ty + " " + oldV + ", 1.0");
            } else {
                emitLine(newV + " = " + (v.op == "++" ? "add" : "sub") + " " + ty + " " +
                         oldV + ", 1");
            }
            emitLine("store " + ty + " " + newV + ", ptr " + p);
            return v.isPrefix ? newV : oldV;
        }
        error(e.loc, "unsupported unary operator '" + std::string(v.op) + "'");
        return "undef";
    }
    if (std::holds_alternative<M::Binary>(n)) {
        const M::Binary& v = std::get<M::Binary>(n);
        const bool f = isFloatType(v.lhs->type);
        const bool isUnsigned = v.lhs->type.isUnsigned;
        const bool ptr = v.lhs->type.pointerDepth > 0 || v.rhs->type.pointerDepth > 0;
        const std::string ty = llvmType(e.type);

        if (v.op == "&&" || v.op == "||") {
            // short-circuit: branch + phi
            const std::string lhsV = lowerExpr(*v.lhs);
            const std::string prev = curBlock_;
            const std::string evalB = newInlineBlock();
            const std::string mergeB = newInlineBlock();
            emitLine("br i1 " + lhsV + ", label %" +
                     (v.op == "&&" ? evalB : mergeB) + ", label %" +
                     (v.op == "&&" ? mergeB : evalB));
            curBlock_ = evalB;
            emitLine(evalB + ":");
            const std::string rhsV = lowerExpr(*v.rhs);
            emitLine("br label %" + mergeB);
            curBlock_ = mergeB;
            emitLine(mergeB + ":");
            std::string p = newTemp();
            emitLine(p + " = phi i1 [ " + (v.op == "&&" ? "false" : "true") +
                     ", %" + prev + " ], [ " + rhsV + ", %" + evalB + " ]");
            return p;
        }

        // Comma operator: evaluate lhs for side effects, return rhs.
        if (v.op == ",") {
            lowerExpr(*v.lhs);
            return lowerExpr(*v.rhs);
        }

        const std::string lhsV = lowerExpr(*v.lhs);
        const std::string rhsV = lowerExpr(*v.rhs);
        // For comparison ops the result type is i1, but operands compared at
        // their common (promoted) type.  For other ops, the result type is the
        // promoted type.  Compute the operand type accordingly.
        const bool isCmp = v.op == "==" || v.op == "!=" || v.op == "<" ||
                           v.op == ">" || v.op == "<=" || v.op == ">=";
        const std::string opTy = isCmp
            ? (ptr ? std::string("ptr") : valueLlvmType(v.lhs->type))
            : ty;
        const std::string lhsTy = valueLlvmType(v.lhs->type);
        const std::string rhsTy = valueLlvmType(v.rhs->type);
        const std::string lhsC = isCmp
            ? emitCast(lhsV, lhsTy, opTy, e.loc)
            : emitCast(lhsV, lhsTy, ty, e.loc);
        const std::string rhsC = isCmp
            ? emitCast(rhsV, rhsTy, opTy, e.loc)
            : emitCast(rhsV, rhsTy, ty, e.loc);

        if (isCmp) {
            std::string op;
            if (f) {
                const char* names[6] = {"oeq", "one", "olt", "ogt", "ole", "oge"};
                const char* map[6] = {"==", "!=", "<", ">", "<=", ">="};
                for (int i = 0; i < 6; ++i)
                    if (v.op == map[i]) op = names[i];
                op = "fcmp " + op;
            } else {
                const char* names[6] = {"eq", "ne", "slt", "sgt", "sle", "sge"};
                const char* unames[6] = {"eq", "ne", "ult", "ugt", "ule", "uge"};
                const char* map[6] = {"==", "!=", "<", ">", "<=", ">="};
                const char* chosen = (isUnsigned || ptr) ? unames[0] : names[0];
                for (int i = 0; i < 6; ++i)
                    if (v.op == map[i]) chosen = (isUnsigned || ptr) ? unames[i] : names[i];
                op = "icmp " + std::string(chosen);
            }
            std::string t = newTemp();
            emitLine(t + " = " + op + " " + opTy + " " + lhsC + ", " + rhsC);
            return t;
        }

        if (v.op == "+" || v.op == "-") {
            if (ptr) {
                const mir::Expr& pe = v.lhs->type.pointerDepth > 0 ? *v.lhs : *v.rhs;
                const mir::Expr& ie = v.lhs->type.pointerDepth > 0 ? *v.rhs : *v.lhs;
                std::string idx = lowerExpr(ie);
                if (v.op == "-" && v.lhs->type.pointerDepth > 0) {
                    // p - n  ->  p + (-n)
                    const std::string neg = newTemp();
                    const std::string ity = llvmType(ie.type);
                    emitLine(neg + " = sub " + ity + " 0, " + idx);
                    idx = neg;
                }
                std::string t = newTemp();
                emitLine(t + " = getelementptr " + llvmElemType(pe.type) + ", ptr " +
                         lowerExpr(pe) + ", " + llvmType(ie.type) + " " + idx);
                return t;
            }
            std::string t = newTemp();
            emitLine(t + " = " + (f ? (v.op == "+" ? "fadd" : "fsub")
                                    : (v.op == "+" ? "add" : "sub")) +
                     " " + ty + " " + lhsC + ", " + rhsC);
            return t;
        }

        if (v.op == "*" || v.op == "/" || v.op == "%") {
            std::string t = newTemp();
            std::string op;
            if (f) {
                op = v.op == "*" ? "fmul" : (v.op == "/" ? "fdiv" : "frem");
            } else {
                op = v.op == "*" ? "mul" : (v.op == "/" ? (isUnsigned ? "udiv" : "sdiv")
                                                        : (isUnsigned ? "urem" : "srem"));
            }
            emitLine(t + " = " + op + " " + ty + " " + lhsC + ", " + rhsC);
            return t;
        }

        if (v.op == "<<" || v.op == ">>") {
            std::string t = newTemp();
            emitLine(t + " = " + (v.op == "<<" ? "shl" : (isUnsigned ? "lshr" : "ashr")) +
                     " " + ty + " " + lhsC + ", " + rhsC);
            return t;
        }

        if (v.op == "&" || v.op == "|" || v.op == "^") {
            std::string t = newTemp();
            emitLine(t + " = " + (v.op == "&" ? "and" : (v.op == "|" ? "or" : "xor")) +
                     " " + ty + " " + lhsC + ", " + rhsC);
            return t;
        }

        error(e.loc, "unsupported binary operator '" + std::string(v.op) + "'");
        return "undef";
    }
    if (std::holds_alternative<M::Ternary>(n)) {
        const M::Ternary& v = std::get<M::Ternary>(n);
        const std::string condV = lowerExpr(*v.cond);
        const std::string thenB = newInlineBlock();
        const std::string elseB = newInlineBlock();
        const std::string mergeB = newInlineBlock();
        emitLine("br i1 " + condV + ", label %" + thenB + ", label %" + elseB);
        curBlock_ = thenB;
        emitLine(thenB + ":");
        const std::string thenV = lowerExpr(*v.thenBranch);
        emitLine("br label %" + mergeB);
        curBlock_ = elseB;
        emitLine(elseB + ":");
        const std::string elseV = lowerExpr(*v.elseBranch);
        emitLine("br label %" + mergeB);
        curBlock_ = mergeB;
        emitLine(mergeB + ":");
        std::string p = newTemp();
        emitLine(p + " = phi " + llvmType(e.type) + " [ " + thenV + ", %" + thenB +
                 " ], [ " + elseV + ", %" + elseB + " ]");
        return p;
    }
    if (std::holds_alternative<M::Call>(n)) {
        const M::Call& v = std::get<M::Call>(n);
        // 7.7: Virtual dispatch — load vptr, then fn ptr from vtable[slot].
        if (v.isVirtual) {
            if (v.args.empty() || !v.args[0]) {
                error(e.loc, "internal: virtual call with no `this`");
                return "null";
            }
            std::string objAddr = lowerLValue(*v.args[0]);
            std::string vptr = newTemp();
            emitLine(vptr + " = load ptr, ptr " + objAddr);
            std::string fnPtrAddr = newTemp();
            emitLine(fnPtrAddr + " = getelementptr ptr, ptr " + vptr +
                     ", i32 " + std::to_string(v.vtableSlot));
            std::string fnPtr = newTemp();
            emitLine(fnPtr + " = load ptr, ptr " + fnPtrAddr);
            // Build args using the static callee's parameter types.
            const mir::Function* sig = nullptr;
            for (const auto& f : mir_.functions)
                if (f->name == v.callee) { sig = f.get(); break; }
            std::string args;
            for (std::size_t i = 0; i < v.args.size(); ++i) {
                const auto& a = v.args[i];
                if (!a) continue;
                std::string val, ty;
                if (sig && i < sig->params.size() &&
                    sig->params[i].type.isReference) {
                    val = lowerLValue(*a);
                    ty = "ptr";
                } else {
                    val = lowerExpr(*a);
                    ty = valueLlvmType(a->type);
                }
                args += (args.empty() ? "" : ", ") + ty + " " + val;
            }
            const std::string rt = llvmType(e.type);
            if (rt == "void") {
                emitLine("call void " + fnPtr + "(" + args + ")");
                return "";
            }
            std::string t = newTemp();
            emitLine(t + " = call " + rt + " " + fnPtr + "(" + args + ")");
            return t;
        }
        // Look up the callee's parameter types (to handle reference params
        // and overload resolution — match by name + param signature).
        const mir::Function* callee = nullptr;
        for (const auto& f : mir_.functions) {
            if (f->name != v.callee) continue;
            const std::size_t np = f->params.size();
            const std::size_t na = v.args.size();
            if (np > na) continue;
            if (!f->isExternC && np != na) continue;
            bool sigMatch = true;
            for (std::size_t i = 0; i < np && sigMatch; ++i) {
                mir::Type p = f->params[i].type; p.isReference = false;
                mir::Type a = v.args[i] ? v.args[i]->type : mir::Type{};
                a.isReference = false;
                if (!(p == a)) sigMatch = false;
            }
            if (sigMatch) { callee = f.get(); break; }
        }
        // Fallback: name-only match (e.g. extern "C" variadic).
        if (!callee) {
            for (const auto& f : mir_.functions) {
                if (f->name == v.callee) { callee = f.get(); break; }
            }
        }
        std::string args;
        for (std::size_t i = 0; i < v.args.size(); ++i) {
            const auto& a = v.args[i];
            std::string val;
            std::string ty;
            if (callee && i < callee->params.size() &&
                callee->params[i].type.isReference) {
                // Pass by reference: emit the address of the lvalue.
                val = lowerLValue(*a);
                ty = "ptr";
            } else {
                val = lowerExpr(*a);
                ty = valueLlvmType(a->type);
            }
            args += (args.empty() ? "" : ", ") + ty + " " + val;
        }
        const std::string rt = llvmType(e.type);
        const bool isExternC = callee && callee->isExternC;
        const std::string sym = isExternC ? llvmGlobalName(v.callee)
                                          : llvmGlobalName(mangleFunction(v.callee, callee));
        if (rt == "void") {
            emitLine("call void @" + sym + "(" + args + ")");
            return "";
        }
        std::string t = newTemp();
        emitLine(t + " = call " + rt + " @" + sym + "(" + args + ")");
        return t;
    }
    if (std::holds_alternative<M::Index>(n)) {
        const M::Index& v = std::get<M::Index>(n);
        const std::string idx = lowerExpr(*v.index);
        const std::string idxTy = llvmType(v.index->type);
        const mir::Type& baseTy = v.base->type;
        std::string elemTy = llvmType(e.type);  // element type (result type)
        std::string gep = newTemp();
        if (baseTy.arraySize > 0) {
            // Array indexing: base decays to pointer to [N x T].
            // Emit bounds check unless inUnsafe_.
            const std::string base = lowerExpr(*v.base);  // slot (ptr to array)
            emitBoundsCheck(idx, idxTy, baseTy.arraySize, e.loc);
            const std::string arrTy = llvmType(baseTy);
            emitLine(gep + " = getelementptr " + arrTy + ", ptr " + base +
                     ", i32 0, " + idxTy + " " + idx);
        } else {
            // Pointer indexing (already gated unsafe in HIR).
            const std::string base = lowerExpr(*v.base);
            emitLine(gep + " = getelementptr " + elemTy + ", ptr " + base + ", " +
                     idxTy + " " + idx);
        }
        std::string t = newTemp();
        emitLine(t + " = load " + elemTy + ", ptr " + gep);
        return t;
    }
    if (std::holds_alternative<M::Member>(n)) {
        // Member access as rvalue: GEP + load.
        const M::Member& v = std::get<M::Member>(n);
        // Get the struct address (for `.`) or pointer value (for `->`).
        std::string baseAddr;
        mir::Type structType = v.base->type;
        if (v.isArrow) {
            // `p->f` — base is a pointer-to-struct; load the pointer.
            baseAddr = lowerExpr(*v.base);
            --structType.pointerDepth;
        } else {
            // `p.f` — base is a struct lvalue; get its address.
            baseAddr = lowerLValue(*v.base);
        }
        // Find the struct type metadata.
        const auto sIt = structTypes_.find(structType.base);
        if (sIt == structTypes_.end()) {
            error(e.loc, "internal: struct type not found for member access");
            return "null";
        }
        // Find field index by name.
        std::size_t fieldIdx = 0;
        bool found = false;
        for (std::size_t i = 0; i < sIt->second.fields.size(); ++i) {
            if (sIt->second.fields[i].name == v.name) {
                fieldIdx = i;
                found = true;
                break;
            }
        }
        if (!found) {
            error(e.loc, "internal: field not found in struct");
            return "null";
        }
        std::string gep = newTemp();
        emitLine(gep + " = getelementptr " + sIt->second.llvmType + ", ptr " + baseAddr +
                 ", i32 0, i32 " + std::to_string(fieldIdx));
        // Load the field value.
        std::string t = newTemp();
        emitLine(t + " = load " + llvmType(e.type) + ", ptr " + gep);
        return t;
    }
    if (std::holds_alternative<M::Assign>(n)) {
        const M::Assign& v = std::get<M::Assign>(n);
        const std::string p = lowerLValue(*v.lhs);
        // Aggregate init on assignment: lower directly into the lhs slot
        // (avoid a redundant alloca + load + store of the whole struct).
        if (std::holds_alternative<mir::Expr::InitList>(v.rhs->node)) {
            lowerInitListInto(std::get<mir::Expr::InitList>(v.rhs->node),
                              p, v.lhs->type, e.loc);
            return "";  // aggregate assign yields no value
        }
        const std::string val = lowerExpr(*v.rhs);
        emitLine("store " + llvmType(v.lhs->type) + " " + val + ", ptr " + p);
        return val;
    }
    if (std::holds_alternative<M::New>(n)) {
        const M::New& v = std::get<M::New>(n);
        usesMalloc_ = true;
        std::string t = newTemp();
        emitLine(t + " = call ptr @malloc(i64 " + sizeofType(v.type) + ")");
        if (!v.args.empty()) {
            const std::string val = lowerExpr(*v.args[0]);
            emitLine("store " + llvmType(v.type) + " " + val + ", ptr " + t);
        }
        return t;
    }
    if (std::holds_alternative<M::Delete>(n)) {
        const M::Delete& v = std::get<M::Delete>(n);
        usesFree_ = true;
        const std::string p = lowerExpr(*v.operand);
        emitLine("call void @free(ptr " + p + ")");
        return "";
    }
    if (std::holds_alternative<M::InitList>(n)) {
        // Aggregate initializer used as an rvalue (e.g. assigned to a struct
        // variable, or passed as an argument). Lower into a fresh alloca,
        // then load the struct value out of it.
        const M::InitList& v = std::get<M::InitList>(n);
        const std::string slot = newAllocaSlot();
        const std::string ty = llvmType(e.type);
        emitLine(slot + " = alloca " + ty);
        lowerInitListInto(v, slot, e.type, e.loc);
        std::string t = newTemp();
        emitLine(t + " = load " + ty + ", ptr " + slot);
        return t;
    }
    if (std::holds_alternative<M::Lambda>(n)) {
        // A lambda expression produces a closure struct value. We
        // allocate a closure struct on the stack, initialize each
        // capture field, and return the alloca slot name (the struct
        // lvalue). This is consumed by the enclosing `&` (address-of)
        // when used as a lambda call callee, or stored into a variable
        // when assigned to a local.
        const M::Lambda& v = std::get<M::Lambda>(n);
        const std::string slot = newAllocaSlot();
        const std::string ty = llvmType(e.type);
        emitLine(slot + " = alloca " + ty);
        // Zero-initialize the closure struct first.
        emitLine("store " + ty + " zeroinitializer, ptr " + slot);
        // Store each capture value into its field via GEP.
        const auto sIt = structTypes_.find(e.type.base);
        if (sIt != structTypes_.end()) {
            for (std::size_t i = 0; i < v.captureInits.size() &&
                                 i < sIt->second.fields.size(); ++i) {
                const auto& ci = v.captureInits[i];
                if (!ci) continue;
                const std::string val = lowerExpr(*ci);
                const std::string fieldTy = llvmType(sIt->second.fields[i].type);
                const std::string c = emitCast(val, valueLlvmType(ci->type),
                                               fieldTy, e.loc);
                std::string gep = newTemp();
                emitLine(gep + " = getelementptr " + sIt->second.llvmType +
                         ", ptr " + slot + ", i32 0, i32 " + std::to_string(i));
                emitLine("store " + fieldTy + " " + c + ", ptr " + gep);
            }
        }
        // Return the alloca slot — the closure struct lvalue. When this
        // is used as `&lambda`, lowerLValue will return the slot directly
        // (since the Lambda node is not an IdentRef). We handle the
        // `&Lambda` case in lowerExpr's Unary handler below.
        return slot;
    }
    error(e.loc, "unsupported expression");
    return "undef";
}

// Lower a struct aggregate initializer into the given storage slot.
// Emits a `store ... zeroinitializer` first (so unspecified trailing
// fields are zero), then GEP + store for each provided element.
void CodeGen::lowerInitListInto(const mir::Expr::InitList& il,
                                const std::string& slot,
                                const mir::Type& structType, SourceLoc loc) {
    const auto sIt = structTypes_.find(structType.base);
    if (sIt == structTypes_.end()) {
        error(loc, "internal: InitList for non-struct type '" +
                       std::string(structType.base) + "'");
        return;
    }
    const StructMeta& meta = sIt->second;
    const std::string ty = meta.llvmType;
    // Zero-initialize the whole struct first so unspecified trailing
    // fields are zero (C value-initialization semantics).
    emitLine("store " + ty + " zeroinitializer, ptr " + slot);
    // Store each provided element into its field via GEP.
    for (std::size_t i = 0; i < il.elements.size() && i < meta.fields.size(); ++i) {
        const auto& elem = il.elements[i];
        if (!elem) continue;
        const std::string val = lowerExpr(*elem);
        const std::string fieldTy = llvmType(meta.fields[i].type);
        const std::string c = emitCast(val, valueLlvmType(elem->type), fieldTy, loc);
        std::string gep = newTemp();
        emitLine(gep + " = getelementptr " + ty + ", ptr " + slot +
                 ", i32 0, i32 " + std::to_string(i));
        emitLine("store " + fieldTy + " " + c + ", ptr " + gep);
    }
}

void CodeGen::initVptr(const std::string& slot, std::string_view structName,
                       SourceLoc loc) {
    (void)loc;
    // Find the struct info to check if it's polymorphic.
    for (const auto& si : mir_.structs) {
        if (si.name != structName) continue;
        if (!si.isPolymorphic) return;
        // Emit: store ptr @_ZTV<name>, ptr <slot>
        // (vptr is at offset 0 — the first field).
        std::string vtableGlobal = "@_ZTV" + escapeLlvmIdent(si.name);
        std::string gep = newTemp();
        emitLine(gep + " = getelementptr " + structTypes_[si.name].llvmType +
                 ", ptr " + slot + ", i32 0, i32 0");
        emitLine("store ptr " + vtableGlobal + ", ptr " + gep);
        return;
    }
}

// --- instructions ---

// Emits bounds-check IR for array index. Pattern (Rust-style panic):
//
//   %oob = icmp uge i64 %idx, N      ; unsigned >= catches both negative and >= N
//   br i1 %oob, label %panic, label %ok
// panic:
//   call void @__ivy_panic(ptr @.panic_msg, i32 line)
//   unreachable
// ok:
//
// When inUnsafe_ is true, no IR is emitted (caller already checked).
void CodeGen::emitBoundsCheck(const std::string& idxVal, const std::string& idxTy,
                               std::uint32_t arrayN, SourceLoc loc) {
    if (inUnsafe_) return;

    // Ensure __ivy_panic is declared once per module.
    if (!declaredIvyPanic_) {
        emitLine("declare void @__ivy_panic(ptr, i32)");
        declaredIvyPanic_ = true;
    }

    // Cast idx to i64 for comparison (handles signed negative → becomes huge uint).
    const std::string idx64 = newTemp();
    if (idxTy == "i64") {
        // Already i64 — use directly without cast.
        // (We'll just alias by using idxVal below.)
    }
    // Use unsigned comparison: idx >= N  (catches negative indices too).
    const std::string oob = newTemp();
    // Extend/truncate idx to i64 for comparison.
    std::string idxCasted = idxVal;
    if (idxTy != "i64") {
        emitLine(idx64 + " = sext " + idxTy + " " + idxVal + " to i64");
        idxCasted = idx64;
    }
    emitLine(oob + " = icmp uge i64 " + idxCasted + ", " + std::to_string(arrayN));

    const std::string panicB = newInlineBlock();
    const std::string okB = newInlineBlock();
    emitLine("br i1 " + oob + ", label %" + panicB + ", label %" + okB);

    // panic block
    curBlock_ = panicB;
    emitLine(panicB + ":");
    // Build a panic message string constant (module-level).
    const std::string msgBytes = "index out of bounds (line " +
                                  std::to_string(loc.line) + ")";
    auto strIt = strings_.find(msgBytes);
    if (strIt == strings_.end()) {
        const std::string gname = "@.str." + std::to_string(stringList_.size());
        strings_.emplace(msgBytes, gname);
        stringList_.emplace_back(gname, msgBytes);
        strIt = strings_.find(msgBytes);
    }
    const std::string msgPtr = newTemp();
    emitLine(msgPtr + " = getelementptr i8, ptr " + strIt->second + ", i64 0");
    emitLine("call void @__ivy_panic(ptr " + msgPtr + ", i32 " +
             std::to_string(loc.line) + ")");
    emitLine("unreachable");

    // ok block
    curBlock_ = okB;
    emitLine(okB + ":");
}

void CodeGen::lowerInst(const mir::Inst& inst) {
    const auto& n = inst.node;
    using I = mir::Inst;
    inUnsafe_ = inst.inUnsafe;  // propagate unsafe flag to expression lowering

    if (std::holds_alternative<I::Alloca>(n)) {
        const I::Alloca& a = std::get<I::Alloca>(n);
        const std::string slot = valueName(a.var);
        const std::string slotTy = llvmType(a.type);
        if (a.type.isReference) {
            // Reference: no new alloca — alias the referenced variable's slot.
            // init is guaranteed by HIR to be an lvalue (IdentRef or Deref).
            vars_[a.var] = slot;  // placeholder
            emitLine(slot + " = alloca ptr");  // storage for the address
            if (a.init) {
                // Get the address of the referenced object (lowerLValue).
                const std::string addr = lowerLValue(*a.init);
                emitLine("store ptr " + addr + ", ptr " + slot);
            }
            return;
        }
        emitLine(slot + " = alloca " + slotTy);
        vars_[a.var] = slot;
        if (a.init) {
            if (std::holds_alternative<mir::Expr::InitList>(a.init->node)) {
                // Aggregate init: GEP + store each element into the slot.
                lowerInitListInto(std::get<mir::Expr::InitList>(a.init->node),
                                  slot, a.type, inst.loc);
            } else {
                const std::string v = lowerExpr(*a.init);
                const std::string fromTy = valueLlvmType(a.init->type);
                const std::string c = emitCast(v, fromTy, slotTy, inst.loc);
                emitLine("store " + slotTy + " " + c + ", ptr " + slot);
            }
        } else if (structTypes_.contains(a.type.base)) {
            // Uninitialized struct variable — zero-initialize (C semantics).
            emitLine("store " + slotTy + " zeroinitializer, ptr " + slot);
            // 7.7: If polymorphic, init vptr to point to the vtable.
            initVptr(slot, a.type.base, inst.loc);
        } else if (a.type.arraySize > 0) {
            // Array variable — zero-initialize all elements (C semantics).
            emitLine("store " + slotTy + " zeroinitializer, ptr " + slot);
        }
        return;
    }
    if (std::holds_alternative<I::Store>(n)) {
        const I::Store& s = std::get<I::Store>(n);
        const std::string p = lowerLValue(*s.target);
        const std::string v = lowerExpr(*s.value);
        const std::string toTy = valueLlvmType(s.target->type);
        const std::string fromTy = valueLlvmType(s.value->type);
        const std::string c = emitCast(v, fromTy, toTy, inst.loc);
        emitLine("store " + toTy + " " + c + ", ptr " + p);
        return;
    }
    if (std::holds_alternative<I::Eval>(n)) {
        const I::Eval& v = std::get<I::Eval>(n);
        if (v.value) lowerExpr(*v.value);
        return;
    }
    if (std::holds_alternative<I::Ret>(n)) {
        const I::Ret& r = std::get<I::Ret>(n);
        if (r.value) {
            const std::string v = lowerExpr(*r.value);
            const std::string fromTy = valueLlvmType(r.value->type);
            const std::string toTy = valueLlvmType(curFnReturnType_);
            const std::string c = emitCast(v, fromTy, toTy, inst.loc);
            emitLine("ret " + toTy + " " + c);
        } else {
            emitLine("ret void");
        }
        return;
    }
    if (std::holds_alternative<I::CondBranch>(n)) {
        const I::CondBranch& cb = std::get<I::CondBranch>(n);
        const std::string c = lowerExpr(*cb.cond);
        // block indices are encoded in the block pointers' position; look them up
        // via the function's block list in lowerFunction by precomputing names.
        emitLine("br i1 " + c + ", label %" + blockNames_[cb.thenBlock] +
                 ", label %" + blockNames_[cb.elseBlock]);
        return;
    }
    if (std::holds_alternative<I::Jump>(n)) {
        const I::Jump& j = std::get<I::Jump>(n);
        emitLine("br label %" + blockNames_[j.target]);
        return;
    }
    if (std::holds_alternative<I::Switch>(n)) {
        const I::Switch& sw = std::get<I::Switch>(n);
        if (!sw.cond) return;
        const std::string condV = lowerExpr(*sw.cond);
        const std::string condTy = llvmType(sw.cond->type);
        std::string line = "switch " + condTy + " " + condV +
                           ", label %" + blockNames_[sw.defaultBlock] + " [\n";
        for (const auto& arm : sw.arms) {
            line += "    " + condTy + " " + std::to_string(arm.value) +
                    ", label %" + blockNames_[arm.block] + "\n";
        }
        line += "  ]";
        emitLine(line);
        return;
    }
}

void CodeGen::lowerBlock(const mir::Block& b, int index, bool isVoidRet) {
    emitLine("bb" + std::to_string(index) + ":");
    curBlock_ = "bb" + std::to_string(index);
    for (const auto& inst : b.insts) lowerInst(*inst);
    // guarantee a terminator
    if (b.insts.empty() ||
        (b.insts.back()->kind != mir::Inst::Kind::Ret &&
         b.insts.back()->kind != mir::Inst::Kind::Jump &&
         b.insts.back()->kind != mir::Inst::Kind::CondBranch &&
         b.insts.back()->kind != mir::Inst::Kind::Switch)) {
        emitLine(isVoidRet ? "ret void" : "unreachable");
    }
}

std::string CodeGen::mangleFunction(std::string_view name,
                                     const mir::Function* fn) const {
    // `main` is never mangled — it's the C entry point.
    if (name == "main") return std::string(name);
    if (fn && fn->isExternC) return std::string(name);
    return platform_ == Platform::Itanium
        ? itaniumMangleFunction(name, fn)
        : msvcMangleFunction(name, fn);
}

std::string CodeGen::mangleEnumType(std::string_view name,
                                     std::string_view nsPrefix,
                                     bool isScoped) const {
    return platform_ == Platform::Itanium
        ? itaniumMangleEnumType(name, nsPrefix, isScoped)
        : msvcMangleEnumType(name, nsPrefix, isScoped);
}

// ---------------------------------------------------------------------------
// Itanium C++ ABI mangling
//   <mangled-name> ::= _Z <encoding>
//   <encoding>      ::= <function-name> <bare-function-type>
//   <function-name> ::= <name>
//   <name>          ::= <nested-name> | <unscoped-name>
//   <nested-name>   ::= N <prefix>+ E
//   <unscoped-name> ::= <unqualified-name>
//   <unqualified-name> ::= <source-name>    # length-prefixed identifier
//   <source-name>   ::= <positive length number> <identifier>
//   <bare-function-type> ::= <type>+   # return type is NOT encoded for
//                                     # functions (except templates); only
//                                     # parameter types.  "v" for void/no
//                                     # params.
// ---------------------------------------------------------------------------

std::string CodeGen::itaniumMangleType(const mir::Type& t) const {
    // References and pointers: Itanium uses `R` / `P` followed by the
    // pointee type, but we emit everything as `ptr` in LLVM so the
    // canonical mangling collapses to `Pv` (pointer to void) for all
    // pointer/reference parameters — this keeps signatures distinct
    // from scalars while avoiding per-type pointer mangling.
    if (t.pointerDepth > 0 || t.isReference) return "Pv";
    // Built-in type codes (Itanium ABI §5.1.6).
    if (t.base == "void") return "v";
    if (t.base == "bool") return "b";
    if (t.base == "char") return "c";
    if (t.base == "short") return t.isUnsigned ? "Us" : "s";
    if (t.base == "int") return t.isUnsigned ? "Ui" : "i";
    if (t.base == "long") return t.isUnsigned ? "Ul" : "l";
    if (t.base == "long long") return t.isUnsigned ? "Uy" : "x";
    if (t.base == "float") return "f";
    if (t.base == "double" || t.base == "long double") return "d";
    // Ivy fixed-width types — map to their canonical Itanium type.
    if (t.base == "int8_t" || t.base == "uint8_t") return t.base == "uint8_t" ? "uh" : "h";
    if (t.base == "int16_t" || t.base == "uint16_t") return t.base == "uint16_t" ? "Us" : "s";
    if (t.base == "int32_t" || t.base == "uint32_t") return t.base == "uint32_t" ? "Ui" : "i";
    if (t.base == "int64_t" || t.base == "uint64_t") return t.base == "uint64_t" ? "Uy" : "x";
    if (t.base == "size_t" || t.base == "ptrdiff_t") return t.base == "size_t" ? "m" : "l";
    if (t.base == "float16_t") return "Dh";
    if (t.base == "float32_t") return "f";
    if (t.base == "float64_t") return "d";
    if (t.base == "float128_t") return "g";
    if (t.base == "bfloat16_t") return "u6bfloat";  // no standard code
    if (t.base == "max_align_t") return "St";  // std::max_align_t approximation
    // User-defined enum type — unscoped enums are mangled as their
    // underlying integer type (per Itanium); scoped enums use a
    // nested-name encoding handled by itaniumMangleEnumType.
    if (auto eIt = enumTypes_.find(t.base); eIt != enumTypes_.end()) {
        if (eIt->second.isScoped) {
            return itaniumMangleEnumType(t.base, eIt->second.namespacePrefix, true);
        }
        mir::Type underlying = t;
        underlying.base = eIt->second.underlyingBase;
        return itaniumMangleType(underlying);
    }
    return "i";  // fallback: int (should be unreachable — HIR rejected it)
}

std::string CodeGen::itaniumMangleEnumType(std::string_view name,
                                            std::string_view nsPrefix,
                                            bool isScoped) const {
    // Scoped enums (`enum class`) are mangled as a nested type name
    // so they are distinguishable from plain integers.
    if (isScoped) {
        const auto parts = splitQualifiedName(name, nsPrefix);
        std::string out = "N";
        for (const auto& p : parts) {
            out += std::to_string(p.size());
            out += p;
        }
        out += "E";
        return out;
    }
    // Unscoped enum — mangling collapses to its underlying type.
    if (auto eIt = enumTypes_.find(name); eIt != enumTypes_.end()) {
        mir::Type t;
        t.base = eIt->second.underlyingBase;
        return itaniumMangleType(t);
    }
    return "i";  // default to int if unknown
}

std::string CodeGen::itaniumMangleFunction(std::string_view name,
                                            const mir::Function* fn) const {
    // Constructors and destructors use special Itanium mangling:
    //   ctor: _ZN <prefix> <C1|C2> E <bare-function-type>
    //   dtor: _ZN <prefix> <D1|D2> E <bare-function-type>
    // where <prefix> is the struct's qualified name (length-prefixed
    // parts).  C1/D1 = complete-object ctor/dtor (the form Ivy uses).
    if (fn && (fn->isCtor || fn->isDtor)) {
        std::string out = "_ZN";
        const std::string_view ns = fn->namespacePrefix;
        const auto parts = splitQualifiedName(name, ns);
        // parts: [..., "StructName", "StructName" (ctor)] or
        // [..., "StructName", "~StructName" (dtor)]
        // The struct name is the second-to-last part; we emit all
        // parts up to and including the struct name, then C1/D1.
        // (Drop the last part which is the ctor/dtor name itself.)
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            out += std::to_string(parts[i].size());
            out += parts[i];
        }
        out += fn->isCtor ? "C1" : "D1";
        out += "E";
        // Bare function type: parameter type codes (incl. `this`).
        if (fn && !fn->params.empty()) {
            for (const auto& p : fn->params) {
                out += itaniumMangleType(p.type);
            }
        } else {
            out += "v";  // void parameter list
        }
        return out;
    }
    std::string out = "_Z";
    const std::string_view ns = fn ? fn->namespacePrefix : std::string_view{};
    const auto parts = splitQualifiedName(name, ns);
    // Name encoding.
    if (parts.size() == 1) {
        // Unscoped (global) function: <source-name> only.
        out += std::to_string(parts[0].size());
        out += parts[0];
    } else {
        // Nested name: N <prefix>+ E
        out += "N";
        for (const auto& p : parts) {
            out += std::to_string(p.size());
            out += p;
        }
        out += "E";
    }
    // Bare function type: parameter type codes (or "v" if no params).
    if (fn && !fn->params.empty()) {
        for (const auto& p : fn->params) {
            out += itaniumMangleType(p.type);
        }
    } else {
        out += "v";  // void parameter list
    }
    return out;
}

// ---------------------------------------------------------------------------
// MSVC C++ ABI mangling
//   <mangled-name> ::= ? <name> <class-scope> <type-encoding> @ @
//   <name>         ::= <simple-name>@   # identifier followed by @
//   <class-scope>  ::= <scope>@*        # zero or more scope names, each @
//   <type-encoding> ::= <return-type> <arg-types> @ Z
//   Note: This is a simplified subset of the full MSVC scheme,
//   sufficient for Ivy's function/enum/namespace mangling needs.
// ---------------------------------------------------------------------------

std::string CodeGen::msvcMangleType(const mir::Type& t) const {
    // Pointers/references: MSVC uses PA (ptr to A) / AAB (ref to A) etc.
    // For simplicity, all pointers mangle as "PEAV" (ptr to class void)
    // — but Ivy has no classes, so use "PAV" (pointer-to-void-class).
    if (t.pointerDepth > 0 || t.isReference) return t.isReference ? "AAV" : "PAV";
    // Built-in type codes (MSVC ABI).
    if (t.base == "void") return "?A";  // void type (X in some refs)
    if (t.base == "bool") return "_N";
    if (t.base == "char") return "D";
    if (t.base == "short") return t.isUnsigned ? "G" : "F";
    if (t.base == "int") return t.isUnsigned ? "I" : "H";
    if (t.base == "long") return t.isUnsigned ? "K" : "J";
    if (t.base == "long long") return t.isUnsigned ? "_K" : "_J";
    if (t.base == "float") return "M";
    if (t.base == "double" || t.base == "long double") return "N";
    // Ivy fixed-width types — map to their canonical MSVC type.
    if (t.base == "int8_t") return "C";
    if (t.base == "uint8_t") return "E";
    if (t.base == "int16_t") return "F";
    if (t.base == "uint16_t") return "G";
    if (t.base == "int32_t") return "H";
    if (t.base == "uint32_t") return "I";
    if (t.base == "int64_t") return "_J";
    if (t.base == "uint64_t") return "_K";
    if (t.base == "size_t") return "_K";
    if (t.base == "ptrdiff_t") return "J";
    if (t.base == "float16_t") return "M";
    if (t.base == "float32_t") return "M";
    if (t.base == "float64_t") return "N";
    if (t.base == "float128_t") return "N";
    if (t.base == "bfloat16_t") return "M";
    if (t.base == "max_align_t") return "PEAV";
    // User-defined enum type — unscoped enums are mangled as their
    // underlying integer type; scoped enums use a nested-name encoding.
    if (auto eIt = enumTypes_.find(t.base); eIt != enumTypes_.end()) {
        if (eIt->second.isScoped) {
            return msvcMangleEnumType(t.base, eIt->second.namespacePrefix, true);
        }
        mir::Type underlying = t;
        underlying.base = eIt->second.underlyingBase;
        return msvcMangleType(underlying);
    }
    return "H";  // fallback: int
}

std::string CodeGen::msvcMangleEnumType(std::string_view name,
                                         std::string_view nsPrefix,
                                         bool isScoped) const {
    // Scoped enums are mangled as a nested type (W4 prefix for enum class).
    if (isScoped) {
        const auto parts = splitQualifiedName(name, nsPrefix);
        std::string out = "W4";
        // Last part is the enum name; preceding parts are scope.
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            out += parts[i] + "@";
        }
        out += parts.back() + "@@";
        return out;
    }
    // Unscoped enum — mangling collapses to its underlying type.
    if (auto eIt = enumTypes_.find(name); eIt != enumTypes_.end()) {
        mir::Type t;
        t.base = eIt->second.underlyingBase;
        return msvcMangleType(t);
    }
    return "H";  // default to int if unknown
}

std::string CodeGen::msvcMangleFunction(std::string_view name,
                                         const mir::Function* fn) const {
    // Constructors and destructors use special MSVC mangling:
    //   ctor: ??0<StructName>@<scope>@*@ <type-encoding> @Z
    //   dtor: ??1<StructName>@<scope>@*@ <type-encoding> @Z
    // The struct name replaces the simple-name, and the return type
    // is X (void) for both.  Args include the implicit `this`.
    if (fn && (fn->isCtor || fn->isDtor)) {
        std::string out = "?";
        out += fn->isCtor ? "0" : "1";
        const std::string_view ns = fn->namespacePrefix;
        const auto parts = splitQualifiedName(name, ns);
        // parts: [..., "StructName", "StructName" (ctor)] or
        // [..., "StructName", "~StructName" (dtor)]
        // The struct name is the second-to-last part.
        std::string_view structName = parts.back();
        if (parts.size() >= 2) structName = parts[parts.size() - 2];
        out += std::string(structName) + "@";
        // Scopes: innermost to outermost, each terminated by @.
        for (auto it = parts.rbegin() + 2; it != parts.rend(); ++it) {
            out += *it + "@";
        }
        out += "@";
        // Calling convention: AE for __cdecl (instance method).
        out += "AE";
        // Return type: X for void (ctors/dtors return void).
        out += "X";
        // Argument types: the `this` param + user params.
        if (fn && !fn->params.empty()) {
            for (const auto& p : fn->params) {
                out += msvcMangleType(p.type);
            }
        } else {
            out += "X";  // void parameter list
        }
        out += "@Z";
        return out;
    }
    std::string out = "?";
    const std::string_view ns = fn ? fn->namespacePrefix : std::string_view{};
    const auto parts = splitQualifiedName(name, ns);
    // Name: <simple-name>@ (last part = function name).
    out += parts.back() + "@";
    // Scopes: <scope>@* in reverse order (outermost last).
    // MSVC encodes scopes from innermost to outermost, each terminated by @.
    for (auto it = parts.rbegin() + 1; it != parts.rend(); ++it) {
        out += *it + "@";
    }
    // Scope terminator: another @ marks end of scope list (or marks
    // global scope when there are no scopes).
    out += "@";
    // Calling convention: Y for __cdecl (the Ivy default).
    out += "Y";
    // Return type (A = void if no function info, else the type code).
    if (fn && fn->returnType.base != "void") {
        out += msvcMangleType(fn->returnType);
    } else if (fn) {
        out += "X";  // void return
    } else {
        out += "H";  // int return as fallback
    }
    // Argument types: X for void/no params, else the type codes.
    if (fn && !fn->params.empty()) {
        for (const auto& p : fn->params) {
            out += msvcMangleType(p.type);
        }
    } else {
        out += "X";  // void parameter list
    }
    out += "@Z";
    return out;
}

void CodeGen::lowerFunction(const mir::Function& fn) {
    temp_ = 0;
    alloca_ = 0;
    inlineBlock_ = 0;
    vars_.clear();
    blockNames_.clear();
    curFnReturnType_ = fn.returnType;

    // name the blocks: bb<index>
    for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
        blockNames_[fn.blocks[i].get()] = "bb" + std::to_string(i);
    }

    std::string sig;
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        sig += (sig.empty() ? "" : ", ") + llvmType(fn.params[i].type) + " %arg" +
               std::to_string(i);
    }
    emitLine("define " + llvmType(fn.returnType) + " @" +
             (fn.isExternC ? llvmGlobalName(fn.name) : llvmGlobalName(mangleFunction(fn.name, &fn))) + "(" +
             sig + ") {");

    // prologue: materialize parameters in alloca slots
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        const mir::Param& p = fn.params[i];
        const std::string slot = valueName(p.name);
        emitLine(slot + " = alloca " + llvmType(p.type));
        emitLine("store " + llvmType(p.type) + " %arg" + std::to_string(i) + ", ptr " +
                 slot);
        vars_[p.name] = slot;
    }

    const bool isVoidRet = llvmType(fn.returnType) == "void";
    if (!fn.blocks.empty()) {
        // the entry block (alloca prologue) must end with a terminator before
        // the first MIR block's label
        emitLine("br label %bb0");
    } else {
        emitLine(isVoidRet ? "ret void" : "unreachable");
    }
    for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
        lowerBlock(*fn.blocks[i], static_cast<int>(i), isVoidRet);
    }
    emitLine("}");
    emitLine("");
}

bool CodeGen::generate(std::ostream& out) {
    out_ = &out;
    // Populate enum type map for llvmType() resolution.
    for (const auto& ei : mir_.enums) {
        enumTypes_[ei.name] = {ei.underlyingBase, std::string(ei.namespacePrefix),
                              ei.isScoped};
    }
    // Populate struct type map for llvmType() resolution.
    // Each struct gets a named LLVM type: `%struct.Name` (mangled to
    // avoid collisions with namespace-qualified names). The LLVM type
    // body is the aggregate of field types.
    for (const auto& si : mir_.structs) {
        StructMeta meta;
        // Mangle the struct name into a valid LLVM identifier.
        // Replace `::` with `.` and escape `<`/`>`/`,` etc. from
        // template specialization names (e.g. `Box<int32_t>` → `Box_int32_t_`).
        std::string llvmName = "%struct.";
        llvmName += escapeLlvmIdent(si.name);
        meta.llvmType = llvmName;
        for (const auto& f : si.fields) {
            meta.fields.push_back({f.name, f.type});
        }
        // 7.7: polymorphic structs have a vptr at field index 0.
        if (si.isPolymorphic) {
            mir::StructField vptrField;
            vptrField.name = "__vptr";
            vptrField.type = mir::Type{"void", 0, true, 1, false};  // ptr
            meta.fields.insert(meta.fields.begin(), std::move(vptrField));
        }
        structTypes_[si.name] = std::move(meta);
    }
    // pre-pass over all functions
    for (const auto& fn : mir_.functions) collectFunction(*fn);

    emitHeader();
    // Emit struct type definitions (before any function that uses them).
    // 7.7: polymorphic structs have a `ptr` (vptr) as the first field.
    for (const auto& si : mir_.structs) {
        const auto& meta = structTypes_[si.name];
        std::string body = "{ ";
        for (std::size_t i = 0; i < meta.fields.size(); ++i) {
            if (i > 0) body += ", ";
            body += llvmType(meta.fields[i].type);
        }
        body += " }";
        emitLine(meta.llvmType + " = type " + body);
    }
    if (!mir_.structs.empty()) emitLine("");

    // 7.7: Emit vtable globals for polymorphic structs.
    // Each vtable is a constant array of function pointers.
    for (const auto& si : mir_.structs) {
        if (!si.isPolymorphic) continue;
        std::string vtableGlobal = "@_ZTV" + escapeLlvmIdent(si.name);
        // Emit type: [N x ptr] where N = vtable size.
        std::string vtableType = "[ " + std::to_string(si.vtable.size()) + " x ptr ]";
        emitLine(vtableGlobal + " = internal constant " + vtableType + " [");
        for (std::size_t i = 0; i < si.vtable.size(); ++i) {
            if (i > 0) emitLine(", ");
            if (si.vtable[i].funcName.empty()) {
                // Pure virtual — null pointer.
                emitLine("  ptr null");
            } else {
                // Find the function to get its mangled name.
                const mir::Function* vfn = nullptr;
                for (const auto& fn : mir_.functions) {
                    if (fn->name == si.vtable[i].funcName) { vfn = fn.get(); break; }
                }
                std::string sym;
                if (vfn) {
                    sym = "@" + (vfn->isExternC ? llvmGlobalName(vfn->name)
                                                : llvmGlobalName(mangleFunction(vfn->name, vfn)));
                } else {
                    sym = "@null";
                }
                emitLine("  ptr " + sym);
            }
        }
        emitLine("]");
    }
    if (!mir_.structs.empty()) {
        bool any = false;
        for (const auto& si : mir_.structs) {
            if (si.isPolymorphic) { any = true; break; }
        }
        if (any) emitLine("");
    }
    // collect names of extern "C" prototypes so @malloc/@free fallbacks are
    // not declared twice
    for (const auto& fn : mir_.functions) {
        if (fn->isExternC && !fn->hasBody) declaredC_.insert(std::string(fn->name));
    }
    emitGlobals();
    // declare extern "C" functions that have no body
    for (const auto& fn : mir_.functions) {
        if (fn->isExternC && !fn->hasBody) {
            std::string sig;
            for (const mir::Param& p : fn->params) {
                sig += (sig.empty() ? "" : ", ") + llvmType(p.type);
            }
            emitLine("declare " + llvmType(fn->returnType) + " @" + llvmGlobalName(fn->name) +
                     "(" + sig + ")");
        }
    }
    for (const auto& fn : mir_.functions) {
        // Skip consteval functions — they must be evaluated at compile
        // time and have no runtime representation.  Every call to a
        // consteval function is folded by the HIR builder; if folding
        // failed the call would have been a compile error.
        // constexpr functions are kept: they may have runtime calls
        // (when arguments are not compile-time constants), so they
        // need a definition in the emitted IR.  The LLVM optimizer will
        // dead-code-eliminate them if unused.
        if (fn->isConsteval) continue;
        if (fn->hasBody) lowerFunction(*fn);
    }
    return !failed_;
}

// 8.1: Emit a native object file (.o / .obj) by parsing the textual
// LLVM IR (produced by generate()) into an LLVM Module, then using
// LLVM's TargetMachine to emit the object code.
bool CodeGen::emitObject(const std::string& outPath) {
    // 1) Generate the textual LLVM IR into a string.
    std::ostringstream oss;
    if (!generate(oss)) return false;
    std::string ir = oss.str();

    // 2) Initialize LLVM native target (once per process). We only
    // emit object files for the host triple (x86 on Windows), so the
    // native subset suffices and avoids linking every LLVM target.
    static bool initialized = false;
    if (!initialized) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        initialized = true;
    }

    // 3) Parse the IR text into an LLVM Module.
    llvm::LLVMContext ctx;
    llvm::SMDiagnostic diag;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIR(llvm::MemoryBufferRef(ir, "ivy-module"), diag, ctx);
    if (!module) {
        std::string msg;
        llvm::raw_string_ostream rso(msg);
        diag.print("ivyc", rso);
        rso.flush();
        error({}, "LLVM IR parse error: " + msg);
        return false;
    }

    // 4) Determine the target triple (host native).
    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    module->setTargetTriple(triple);
    module->setDataLayout("");

    // 5) Look up the target and create a TargetMachine.
    std::string err;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple.str(), err);
    if (!target) {
        error({}, "LLVM target lookup failed: " + err);
        return false;
    }

    // 6) Create a relocation model + codegen-opt-level config.
    const llvm::TargetOptions targetOptions;
    const std::optional<llvm::Reloc::Model> relocModel;  // default (PIC/static)
    const llvm::CodeGenOptLevel optLevel = llvm::CodeGenOptLevel::Default;
    std::unique_ptr<llvm::TargetMachine> tm(
        target->createTargetMachine(triple, "", "", targetOptions,
                                    relocModel, std::nullopt, optLevel));
    if (!tm) {
        error({}, "LLVM TargetMachine creation failed");
        return false;
    }
    module->setDataLayout(tm->createDataLayout());

    // 7) Emit the object file to disk.
    std::error_code ec;
    llvm::sys::fs::file_status fstatus;
    auto out = std::make_unique<llvm::raw_fd_ostream>(outPath, ec);
    if (ec) {
        error({}, "cannot open output file '" + outPath + "': " + ec.message());
        return false;
    }
    llvm::legacy::PassManager pm;
    const llvm::CodeGenFileType fileType = llvm::CodeGenFileType::ObjectFile;
    if (tm->addPassesToEmitFile(pm, *out, nullptr, fileType)) {
        error({}, "LLVM TargetMachine cannot emit object file for this target");
        return false;
    }
    pm.run(*module);
    out->flush();
    return true;
}

}  // namespace ivy
