#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "common/diagnostic.h"
#include "parsing/token.h"

namespace ivy {

// Lexer for the Ivy subset of C++.
//
// Restrictions are enforced at the parse stage, not here: the lexer
// recognizes the full token vocabulary of C++, so the parser can produce
// proper diagnostics for constructs outside the subset (class, template,
// preprocessor, ...).
class Lexer {
public:
    explicit Lexer(std::string_view source);

    // Tokenizes the whole source. Lexing errors are recovered from (the
    // stream continues) and reported through diagnostics().
    std::vector<Token> tokenize();

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

private:
    std::string_view source_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;  // 1-based
    std::uint32_t col_ = 1;   // 1-based, in bytes
    std::vector<Diagnostic> diagnostics_;

    char peek(std::size_t n = 0) const { return pos_ + n < source_.size() ? source_[pos_ + n] : '\0'; }
    char advance();

    void skipTrivia();
    Token lexToken();
    Token lexNumber(std::uint32_t line, std::uint32_t col);
    Token lexStringLiteral(std::uint32_t line, std::uint32_t col);
    Token lexCharLiteral(std::uint32_t line, std::uint32_t col);
    Token lexRawString(std::size_t start, std::uint32_t line, std::uint32_t col);
    Token makeToken(TokenKind kind, std::size_t start, std::uint32_t line, std::uint32_t col) const;
    void errorAt(std::uint32_t line, std::uint32_t col, std::string_view message);
};

}  // namespace ivy
