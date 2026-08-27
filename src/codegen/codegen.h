#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/diagnostic.h"
#include "mir/mir.h"

namespace ivy {

// Emits textual LLVM IR from the CFG-based MIR.
//
// Mapping:
//   - mir::Block i  -> basic block "bb<i>"
//   - every variable (params included) lives in an alloca slot
//   - ternary ?: and short-circuit && / || are lowered with phi nodes in
//     freshly created inline blocks
//   - 'new' lowers to @malloc (+ optional store of the first argument),
//     'delete' to @free
//   - string literals become private unnamed_addr constants (@.str.N)
class CodeGen {
public:
    explicit CodeGen(const mir::TranslationUnit& mir);

    // Writes the whole module to `out`. Returns false if any diagnostic was
    // reported; check diagnostics() then.
    bool generate(std::ostream& out);

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // Overrides the ABI platform (default is auto-detected from the
    // host platform). Intended for `--target` flag support so Ivy
    // can cross-emit for a non-host platform.
    enum class Platform { Itanium, MSVC };
    void setPlatform(Platform p) { platform_ = p; }
    Platform platform() const { return platform_; }

private:
    const mir::TranslationUnit& mir_;
    std::vector<Diagnostic> diagnostics_;
    bool failed_ = false;
    std::ostream* out_ = nullptr;

    // module-wide
    std::unordered_map<std::string, std::string> strings_;  // decoded bytes -> global name
    std::vector<std::pair<std::string, std::string>> stringList_;  // global name, bytes
    std::unordered_set<std::string> declaredC_;  // names declared via extern "C"
    bool usesMalloc_ = false;
    bool usesFree_ = false;

    // per-function
    std::unordered_map<std::string_view, std::string> vars_;  // var name -> llvm value
    std::unordered_map<const mir::Block*, std::string> blockNames_;  // block -> "bb<i>"
    std::string curBlock_;
    int temp_ = 0;
    int alloca_ = 0;
    int inlineBlock_ = 0;
    mir::Type curFnReturnType_;  // for implicit cast in Ret
    // true when the current instruction is inside an [[ivy::unsafe]] block;
    // array bounds checks are suppressed in this mode.
    bool inUnsafe_ = false;
    // true when the __ivy_panic function declaration has been emitted.
    bool declaredIvyPanic_ = false;

    // Enum name → underlying type base (e.g. "Color" → "int"). Populated
    // in the pre-pass from mir::TranslationUnit::enums so llvmType() can
    // resolve enum-typed variables to the correct integer LLVM type.
    // Also tracks whether the enum is scoped (`enum class`) and its
    // namespace prefix, so the ABI mangler can emit the correct type
    // encoding (scoped enums are not collapsed to their underlying type).
    struct EnumMeta {
        std::string underlyingBase;
        std::string namespacePrefix;
        bool isScoped = false;
    };
    std::unordered_map<std::string_view, EnumMeta> enumTypes_;

    // Struct name → layout metadata (field list + computed offsets +
    // LLVM type string). Populated in the pre-pass from
    // mir::TranslationUnit::structs so llvmType() can resolve
    // struct-typed variables to named LLVM struct types.
    struct StructMeta {
        std::vector<mir::StructField> fields;  // {name, type}
        std::string llvmType;  // e.g. "%struct.Point"
    };
    std::unordered_map<std::string_view, StructMeta> structTypes_;

    // ABI platform: Itanium (POSIX) or MSVC (Windows). Chosen at
    // construction time via compile-time detection so the emitted
    // symbol names match the host platform's C++ ABI. Can be
    // overridden via setPlatform() for cross-compilation.
    Platform platform_;

    // Reports a diagnostic and marks the pass as failed.
    void error(SourceLoc loc, std::string message);

    // Mangles a function for the target platform ABI. `name` is the
    // fully-qualified C++ name (e.g. "ns::func" or "func"). `fn`
    // provides the parameter types used in the mangled signature;
    // it may be null for an unresolved callee (in which case only the
    // name is mangled, without a type signature — this keeps calls
    // to unknown externs linkable). `extern "C"` functions are NOT
    // mangled — their names must match the C ABI.
    std::string mangleFunction(std::string_view name,
                               const mir::Function* fn) const;

    // Mangles an enum *type* name (e.g. for use in a function
    // signature or as a standalone symbol). Unscoped enums are
    // mangled as their underlying integer type (per both ABIs);
    // scoped enums (`enum class`) are mangled as a nested type.
    std::string mangleEnumType(std::string_view name,
                               std::string_view nsPrefix,
                               bool isScoped) const;

    // Itanium ABI name mangling for a function.
    std::string itaniumMangleFunction(std::string_view name,
                                      const mir::Function* fn) const;
    // MSVC ABI name mangling for a function.
    std::string msvcMangleFunction(std::string_view name,
                                  const mir::Function* fn) const;

    // Itanium ABI mangling of a *type* (used in function signatures).
    std::string itaniumMangleType(const mir::Type& t) const;
    // MSVC ABI mangling of a *type* (used in function signatures).
    std::string msvcMangleType(const mir::Type& t) const;

    // Itanium ABI mangling of a scoped enum type name (nested-name).
    std::string itaniumMangleEnumType(std::string_view name,
                                      std::string_view nsPrefix,
                                      bool isScoped) const;
    // MSVC ABI mangling of a scoped enum type name (W4 prefix).
    std::string msvcMangleEnumType(std::string_view name,
                                   std::string_view nsPrefix,
                                   bool isScoped) const;

    // Helpers for the manglers.
    static std::string itaniumEncodeName(std::string_view name);
    static std::string msvcEncodeScope(std::string_view name);

    // Returns the LLVM type string for a MIR type (e.g. "i32", "ptr",
    // "%struct.Point"). Struct types resolve to the named LLVM struct
    // type; enum types resolve to their underlying integer type.
    std::string llvmType(const mir::Type& t) const;

    // Strip reference qualifier — the value type of T& is T.
    mir::Type valueType(const mir::Type& t) const;

    // Returns the LLVM type of the *value* held by an expression of the
    // given type (e.g. for a reference type, the underlying object type).
    std::string valueLlvmType(const mir::Type& t) const;

    // Returns the LLVM type of the element of a pointer/array type
    // (used for indexing and dereferencing).
    std::string llvmElemType(const mir::Type& t) const;

    // Returns the size in bytes of a type (for `malloc`/`sizeof`).
    std::string sizeofType(const mir::Type& t) const;

    // Emits a cast from `fromTy` to `toTy` if needed (e.g. integer
    // extensions/truncations, float promotions). Returns the resulting
    // value (possibly a new SSA name).
    std::string emitCast(const std::string& value, const std::string& from,
                         const std::string& to, SourceLoc loc);

    // Allocates a fresh SSA temporary name (e.g. "%tmp.0", "%tmp.1", ...).
    std::string newTemp();

    // Allocates a fresh inline block name (e.g. "bb.i0", "bb.i1", ...).
    std::string newInlineBlock();

    // Emits a line to the output stream (with leading indentation).
    void emitLine(const std::string& line);

    // Decodes a string literal body (without quotes) into raw bytes.
    bool decodeBody(std::string_view body, std::string& bytes);
    // Decodes a full string literal (with quotes/raw prefix) into bytes.
    bool decodeString(std::string_view raw, std::string& bytes);
    // Decodes a character literal into its integer value.
    bool decodeChar(std::string_view raw, long long& value);
    // Escapes raw bytes for inclusion in an LLVM string constant.
    std::string llvmEscape(const std::string& bytes) const;

    // Collects all referenced string literals / extern "C" declarations
    // from an expression (for the module-wide pre-pass).
    void collectExpr(const mir::Expr& e);
    // Collects strings/malloc/free usage from all instructions in a function.
    void collectFunction(const mir::Function& fn);

    // Emits the module header (comment only).
    void emitHeader();
    // Emits string constants and malloc/free declarations.
    void emitGlobals();

    // Returns the alloca slot name for a variable (unique per function).
    std::string valueName(std::string_view name);

    // Allocates a fresh anonymous alloca slot (for temporary struct
    // storage during aggregate lowering).
    std::string newAllocaSlot() {
        return "%a." + std::to_string(alloca_++);
    }

    // Returns the LLVM IR reference for a global/function symbol.
    // Names containing characters outside [A-Za-z0-9_.$-] must be
    // quoted in double quotes in LLVM IR (e.g. MSVC-mangled names
    // like "?foo@@YA...@Z"). This helper wraps such names in quotes.
    std::string llvmGlobalName(std::string_view name) const;

    // Escapes characters that are invalid in unquoted LLVM IR
    // identifiers. Template specialization names like `Box<int32_t>`
    // contain `<`, `>`, `,` which are not allowed — replace with `_`.
    std::string escapeLlvmIdent(std::string_view name) const;

    // Lowers an expression to its value (an SSA name or literal).
    // For lvalue-only expressions (e.g. IdentRef of a variable), this
    // emits a `load` and returns the loaded value.
    std::string lowerExpr(const mir::Expr& e);
    // Lowers an expression to its address (an SSA name pointing to the
    // storage). Used for lvalues in assignment / member access context.
    std::string lowerLValue(const mir::Expr& e);
    // Lower a struct aggregate initializer into the given storage slot.
    // Emits a `store ... zeroinitializer` first (so unspecified trailing
    // fields are zero), then GEP + store for each provided element.
    void lowerInitListInto(const mir::Expr::InitList& il,
                           const std::string& slot,
                           const mir::Type& structType, SourceLoc loc);
    // Lowers a single MIR instruction.
    void lowerInst(const mir::Inst& inst);
    // Emits array bounds-check LLVM IR for index `idxVal` into array of
    // size `arrayN`. If the check fails, calls __ivy_panic and aborts.
    // Skipped when inUnsafe_ is true.
    void emitBoundsCheck(const std::string& idxVal, const std::string& idxTy,
                         std::uint32_t arrayN, SourceLoc loc);
    // Lowers all instructions in a basic block.
    void lowerBlock(const mir::Block& b, int index, bool isVoidRet);
    // Lowers a function definition to LLVM IR.
    void lowerFunction(const mir::Function& fn);
};

}  // namespace ivy
