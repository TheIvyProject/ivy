#pragma once

#include <deque>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/diagnostic.h"
#include "parsing/ast.h"
#include "parsing/token.h"

namespace ivy {

// Recursive-descent parser for the Ivy subset of C++.
//
// The subset (v1):
//   - translation unit: functions + `extern "C"` declarations
//   - types: Ivy fixed-width types (int8_t…int64_t, uint8_t…uint64_t,
//     float16_t…float128_t, bfloat16_t, size_t, ptrdiff_t, bool, void)
//     + pointers. C-style number types (int, unsigned, long, short,
//     char, float, double, long long) are only accepted when
//     `#pragma ivy cnumber` is active.
//   - statements: compound, if/else, while, do-while, for, return, break, continue,
//     declarations, expression statements, [[ivy::unsafe]] blocks
//   - expressions: full C++ precedence chain (no casts, no lambdas, no new/delete
//     outside [[ivy::unsafe]])
//
// Rejected with a targeted diagnostic: class/struct/union/enum/template/namespace,
// exceptions, switch/goto, preprocessor directives other than #include (which
// is expanded by the Preprocessor before parsing), attributes outside `ivy::`.
class Parser {
public:
    // Constructs a parser over `tokens`. `cnumberEnabled` should be true
    // when `#pragma ivy cnumber` was active during preprocessing, which
    // opts into C-style number types (int, unsigned, long, short, char,
    // float, double, long long). When false, only Ivy fixed-width types
    // are accepted.
    Parser(std::span<const Token> tokens, bool cnumberEnabled = false);

    // Returns nullptr if parsing failed (diagnostics are reported via diagnostics()).
    std::unique_ptr<TranslationUnit> parse();

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

private:
    std::span<const Token> tokens_;
    std::size_t pos_ = 0;
    std::vector<Diagnostic> diagnostics_;
    bool failed_ = false;
    bool inUnsafe_ = false;
    bool cnumberEnabled_ = false;  // #pragma ivy cnumber

    // Namespace stack — pushed by `parseNamespace`, popped on `}`.
    // When non-empty, function and enum names are qualified with the
    // full namespace path (e.g. `ns1::ns2::func`). The qualified name
    // is materialized in `nameStorage_` (a deque for pointer stability)
    // and the `string_view` returned to the AST points into it.
    std::vector<std::string> namespaceStack_;
    std::deque<std::string> nameStorage_;

    // Names of enums defined so far (populated by parseEnum). Used by
    // isTypeStart()/parseType() to accept user enum type names which
    // arrive as TokenKind::Identifier (not Keyword). Stored as
    // qualified names (e.g. `ns::Color`).
    std::vector<std::string_view> enumNames_;

    // Names of structs/classes defined so far (populated by parseStruct).
    // Used by isTypeStart()/parseType() to accept user struct type names
    // which arrive as TokenKind::Identifier (not Keyword). Stored as
    // qualified names (e.g. `ns::Point`).
    std::vector<std::string_view> structNames_;

    // Names of template type parameters in the current template declaration
    // (e.g. `T`, `U`). Used by isTypeStart()/parseType() to accept them as
    // types. Cleared after each template function is parsed.
    std::vector<std::string_view> templateParamNames_;

    // --- token helpers ---
    const Token& peek(std::size_t n = 0) const {
        static const Token eof{TokenKind::EndOfFile, "", 0, 0};
        if (pos_ + n >= tokens_.size()) return eof;
        return tokens_[pos_ + n];
    }
    const Token& next() {
        static const Token eof{TokenKind::EndOfFile, "", 0, 0};
        if (pos_ >= tokens_.size()) return eof;
        return tokens_[pos_++];
    }
    bool at(TokenKind kind) const { return peek().kind == kind; }
    bool atKeyword(std::string_view kw) const {
        return pos_ < tokens_.size() &&
               peek().kind == TokenKind::Keyword && peek().lexeme == kw;
    }
    // Consumes the token; on mismatch reports an error and consumes it anyway
    // so callers always make progress.
    const Token& expect(TokenKind kind, std::string_view what);
    void expectKeyword(std::string_view kw, std::string_view what);
    void errorAt(const Token& tok, std::string_view message);
    void synchronize();

    // --- attributes ---
    std::vector<Attribute> parseAttributeList();
    void validateAttributes(const std::vector<Attribute>& attrs,
                            std::initializer_list<std::string_view> allowed);

    // --- declarations ---
    bool isTypeStart() const;
    Type parseType();

    // Parses optional `constexpr`/`consteval` specifiers before a type.
    // Returns flags; consumes the keyword if present.
    struct ConstexprSpec { bool isConstexpr = false; bool isConsteval = false; };
    ConstexprSpec parseConstexprSpec();

    void parseTopLevel(TranslationUnit& tu);
    void parseNamespace(TranslationUnit& tu, SourceLoc loc);
    void parseEnum(TranslationUnit& tu, SourceLoc loc);
    void parseStruct(TranslationUnit& tu, SourceLoc loc, bool isClass);
    void parseExternC(TranslationUnit& tu, SourceLoc loc, std::vector<Attribute> attrs);
    void parseTemplate(TranslationUnit& tu, SourceLoc loc);
    void parseFunction(TranslationUnit& tu, SourceLoc loc, std::vector<Attribute> attrs,
                       bool isExternC, bool isConstexpr = false, bool isConsteval = false,
                       std::vector<TemplateParam> tplParams = {});
    std::vector<Param> parseParams();
    // Parse template parameter list: `<typename T, int N, ...>` (after `<`).
    std::vector<TemplateParam> parseTemplateParams();
    // Parse explicit template arguments: `<int, double, ...>` (after `<`).
    // Returns parsed types; empty if none.
    std::vector<Type> parseTemplateArgs();

    // Builds a qualified name from the current namespace stack + `name`.
    // If the stack is empty, returns `name` as-is (a view into the token
    // buffer). Otherwise materializes `ns1::ns2::name` in `nameStorage_`
    // and returns a view into the stable deque node.
    std::string_view qualifyName(std::string_view name);

    // Returns the current namespace prefix (e.g. "ns1::ns2::" or "" for
    // global scope).  Materialized in `nameStorage_` for pointer stability.
    std::string_view currentNamespacePrefix();

    // --- statements ---
    std::unique_ptr<Stmt> parseStatement();
    std::unique_ptr<Stmt> parseCompound();
    std::unique_ptr<Stmt> parseIf();
    std::unique_ptr<Stmt> parseWhile();
    std::unique_ptr<Stmt> parseDoWhile();
    std::unique_ptr<Stmt> parseFor();
    std::unique_ptr<Stmt> parseReturn();
    std::unique_ptr<Stmt> parseDeclOrExprStmt();
    std::unique_ptr<Stmt> parseDeclaration(bool expectSemi);
    std::unique_ptr<Stmt> parseExpressionStatement();

    // --- expressions ---
    std::unique_ptr<Expr> parseExpr();        // assignment
    std::unique_ptr<Expr> parseConditional();
    std::unique_ptr<Expr> parseBinary(int minPrec);
    std::unique_ptr<Expr> parseUnary();
    std::unique_ptr<Expr> parsePostfix(std::unique_ptr<Expr> lhs);
    std::unique_ptr<Expr> parsePrimary();

    // new/delete — only inside [[ivy::unsafe]]
    std::unique_ptr<Expr> parseNew(SourceLoc loc);
    std::unique_ptr<Expr> parseDelete(SourceLoc loc);

    long long parseIntegerValue(std::string_view lexeme, const Token& tok);
    double parseFloatValue(std::string_view lexeme, const Token& tok);
};

}  // namespace ivy
