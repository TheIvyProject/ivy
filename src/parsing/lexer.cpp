#include "parsing/lexer.h"

#include <cctype>
#include <string>
#include <unordered_set>

namespace ivy {
namespace {

bool isIdentStart(char c) {
    return c == '_' || std::isalpha(static_cast<unsigned char>(c)) != 0;
}

bool isIdentContinue(char c) {
    return isIdentStart(c) || std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isHexDigit(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isBinDigit(char c) { return c == '0' || c == '1'; }

// Full C++ keyword set. The parser decides which are actually accepted in
// the Ivy subset; the lexer knows them all so diagnostics stay precise.
const std::unordered_set<std::string_view>& keywords() {
    static const std::unordered_set<std::string_view> k = {
        "alignas",     "alignof",      "and",          "and_eq",
        "asm",         "auto",         "bitand",       "bitor",
        "bool",        "break",        "case",         "catch",
        "char",        "char8_t",      "char16_t",     "char32_t",
        "class",       "compl",        "concept",      "const",
        "consteval",   "constexpr",    "constinit",    "const_cast",
        "continue",    "co_await",     "co_return",    "co_yield",
        "decltype",    "default",      "delete",       "do",
        "double",      "dynamic_cast", "else",         "enum",
        "explicit",    "export",       "extern",       "false",
        "float",       "for",          "friend",       "goto",
        "if",          "inline",       "int",          "long",
        "mutable",     "namespace",    "new",          "noexcept",
        "not",         "not_eq",       "nullptr",      "operator",
        "or",          "or_eq",        "private",      "protected",
        "public",      "register",     "reinterpret_cast", "requires",
        "return",      "short",        "signed",       "sizeof",
        "static",      "static_assert","static_cast",  "struct",
        "switch",      "template",     "this",         "thread_local",
        "throw",       "true",         "try",          "typedef",
        "typeid",      "typename",     "union",        "unsigned",
        "using",       "virtual",      "void",         "volatile",
        "wchar_t",     "while",        "xor",          "xor_eq",
        // Ivy additions (contextual keywords, always tokenized as Keyword
        // so the parser can match them with atKeyword()).
        "final",        "override",
        // 9.2: Module keywords (contextual in C++20, but we always tokenize
        // as Keyword so atKeyword() works in parseTopLevel dispatch).
        "import",       "module",
        // Ivy builtin types
        "int8_t",      "int16_t",      "int32_t",      "int64_t",
        "uint8_t",     "uint16_t",     "uint32_t",     "uint64_t",
        "float16_t",   "float32_t",    "float64_t",    "float128_t",
        "bfloat16_t",  "size_t",       "ptrdiff_t",    "nullptr_t",
        "max_align_t",
    };
    return k;
}

bool isRawStringPrefix(std::string_view id) {
    return id == "R" || id == "u8R" || id == "uR" || id == "UR" || id == "LR";
}

// 8.8: Check if an identifier is a string/char literal prefix.
// L   = wchar_t (wide string, [N x i16])
// u   = char16_t ([N x i16])
// U   = char32_t ([N x i32])
// u8  = char (UTF-8, [N x i8] — same as plain string)
bool isStringPrefix(std::string_view id) {
    return id == "L" || id == "u" || id == "U" || id == "u8";
}

// 8.8: Check if an identifier is a char literal prefix.
bool isCharPrefix(std::string_view id) {
    return id == "L" || id == "u" || id == "U";
}

bool isIntegerSuffixChar(char c) {
    return c == 'u' || c == 'U' || c == 'l' || c == 'L' || c == 'f' || c == 'F';
}

// Longest-match first.
constexpr std::pair<std::string_view, TokenKind> kOperators[] = {
    {"...",  TokenKind::Ellipsis},
    {"<<=",  TokenKind::ShlAssign},
    {">>=",  TokenKind::ShrAssign},
    {"<<",   TokenKind::Shl},
    {">>",   TokenKind::Shr},
    {"+=",   TokenKind::PlusAssign},
    {"-=",   TokenKind::MinusAssign},
    {"*=",   TokenKind::StarAssign},
    {"/=",   TokenKind::SlashAssign},
    {"%=",   TokenKind::PercentAssign},
    {"&=",   TokenKind::AmpAssign},
    {"|=",   TokenKind::PipeAssign},
    {"^=",   TokenKind::CaretAssign},
    {"++",   TokenKind::PlusPlus},
    {"--",   TokenKind::MinusMinus},
    {"->*",  TokenKind::ArrowStar},
    {"->",   TokenKind::Arrow},
    {"<=",   TokenKind::Le},
    {">=",   TokenKind::Ge},
    {"==",   TokenKind::Eq},
    {"!=",   TokenKind::Ne},
    {"&&",   TokenKind::AndAnd},
    {"||",   TokenKind::OrOr},
    {"::",   TokenKind::ColonColon},
    {".*",   TokenKind::DotStar},
    {"+",    TokenKind::Plus},
    {"-",    TokenKind::Minus},
    {"*",    TokenKind::Star},
    {"/",    TokenKind::Slash},
    {"%",    TokenKind::Percent},
    {"=",    TokenKind::Assign},
    {"<",    TokenKind::Lt},
    {">",    TokenKind::Gt},
    {"!",    TokenKind::Bang},
    {"~",    TokenKind::Tilde},
    {"&",    TokenKind::Amp},
    {"|",    TokenKind::Pipe},
    {"^",    TokenKind::Caret},
    {"?",    TokenKind::Question},
    {":",    TokenKind::Colon},
    {";",    TokenKind::Semi},
    {",",    TokenKind::Comma},
    {"(",    TokenKind::LParen},
    {")",    TokenKind::RParen},
    {"{",    TokenKind::LBrace},
    {"}",    TokenKind::RBrace},
    {"[",    TokenKind::LBracket},
    {"]",    TokenKind::RBracket},
    {".",    TokenKind::Dot},
    {"##",   TokenKind::HashHash},  // 8.7: token-paste operator
    {"#",    TokenKind::Hash},
};

}  // namespace

Lexer::Lexer(std::string_view source) : source_(source) {}

char Lexer::advance() {
    const char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

Token Lexer::makeToken(TokenKind kind, std::size_t start, std::uint32_t line,
                       std::uint32_t col) const {
    return Token{kind, source_.substr(start, pos_ - start), line, col};
}

void Lexer::errorAt(std::uint32_t line, std::uint32_t col, std::string_view message) {
    diagnostics_.push_back(Diagnostic{line, col, std::string(message)});
}

void Lexer::skipTrivia() {
    for (;;) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f' || c == '\n') {
            advance();
            continue;
        }
        if (c == '/' && peek(1) == '/') {  // line comment
            while (peek() != '\n' && peek() != '\0') advance();
            continue;
        }
        if (c == '/' && peek(1) == '*') {  // block comment
            const std::uint32_t startLine = line_, startCol = col_;
            advance();
            advance();
            bool closed = false;
            while (peek() != '\0') {
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    closed = true;
                    break;
                }
                advance();
            }
            if (!closed) errorAt(startLine, startCol, "unterminated block comment");
            continue;
        }
        break;
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    if (source_.size() >= 3 && source_.substr(0, 3) == "\xEF\xBB\xBF") {
        pos_ = 3;  // skip UTF-8 BOM
    }
    while (pos_ < source_.size()) {
        skipTrivia();
        if (pos_ >= source_.size()) break;
        tokens.push_back(lexToken());
    }
    tokens.push_back(Token{TokenKind::EndOfFile, {}, line_, col_});
    return tokens;
}

Token Lexer::lexToken() {
    const std::size_t start = pos_;
    const std::uint32_t line = line_, col = col_;
    const char c = peek();

    if (isIdentStart(c)) {
        while (isIdentContinue(peek())) advance();
        const std::string_view id = source_.substr(start, pos_ - start);
        // Raw string prefixes (R"delim(...)delim", u8R"(...)", ...).
        if (isRawStringPrefix(id) && peek() == '"') return lexRawString(start, line, col);
        // 8.8: String/char literal prefixes (L"...", u"...", U"...", u8"...").
        // Also L'...', u'...', U'...' for prefixed char literals.
        if (isStringPrefix(id)) {
            if (peek() == '"') return lexStringLiteral(start, line, col);
            if (peek() == '\'') return lexCharLiteral(start, line, col);
        }
        const TokenKind kind = keywords().contains(id) ? TokenKind::Keyword : TokenKind::Identifier;
        return makeToken(kind, start, line, col);
    }

    if (isDigit(c) || (c == '.' && isDigit(peek(1)))) return lexNumber(line, col);

    if (c == '"') return lexStringLiteral(start, line, col);
    if (c == '\'') return lexCharLiteral(start, line, col);

    // Longest-match operator scan.
    for (const auto& [text, kind] : kOperators) {
        if (source_.substr(pos_).starts_with(text)) {
            for (std::size_t i = 0; i < text.size(); ++i) advance();
            return makeToken(kind, start, line, col);
        }
    }

    errorAt(line, col, "unexpected character");
    advance();
    return makeToken(TokenKind::EndOfFile, start, line, col);
}

Token Lexer::lexNumber(std::uint32_t line, std::uint32_t col) {
    const std::size_t start = pos_;
    bool isFloat = false;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance();
        advance();
        bool any = false;
        while (isHexDigit(peek()) || peek() == '\'') {
            if (peek() != '\'') any = true;
            advance();
        }
        if (!any) errorAt(line, col, "invalid hexadecimal literal: expected hex digits after '0x'");
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        advance();
        advance();
        bool any = false;
        while (isBinDigit(peek()) || peek() == '\'') {
            if (peek() != '\'') any = true;
            advance();
        }
        if (!any) errorAt(line, col, "invalid binary literal: expected binary digits after '0b'");
    } else {
        while (isDigit(peek()) || peek() == '\'') advance();
        // Fractional part; '.' followed by another '.' is an ellipsis, not part of the number.
        if (peek() == '.' && peek(1) != '.') {
            isFloat = true;
            advance();
            while (isDigit(peek()) || peek() == '\'') advance();
        }
        // Exponent.
        if ((peek() == 'e' || peek() == 'E') &&
            (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))))) {
            isFloat = true;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            while (isDigit(peek()) || peek() == '\'') advance();
        }
    }

    // Integer/float suffixes (u, U, l, L, f, F) — ordering is not validated here.
    while (isIntegerSuffixChar(peek())) {
        if (peek() == 'f' || peek() == 'F') isFloat = true;
        advance();
    }

    return makeToken(isFloat ? TokenKind::Float : TokenKind::Integer, start, line, col);
}

Token Lexer::lexStringLiteral(std::size_t start, std::uint32_t line, std::uint32_t col) {
    // `start` may point at a prefix (L, u, U, u8) or at the opening '"'.
    // In either case, `pos_` is at the opening '"', so advance past it.
    advance();  // opening '"'
    for (;;) {
        const char c = peek();
        if (c == '\0' || c == '\n') {
            errorAt(line, col, "unterminated string literal");
            break;
        }
        advance();
        if (c == '"') break;
        if (c == '\\' && peek() != '\0') advance();  // skip escaped character
    }
    return makeToken(TokenKind::String, start, line, col);
}

Token Lexer::lexCharLiteral(std::size_t start, std::uint32_t line, std::uint32_t col) {
    // `start` may point at a prefix (L, u, U) or at the opening '\''.
    // In either case, `pos_` is at the opening '\''.
    advance();  // opening '\''
    for (;;) {
        const char c = peek();
        if (c == '\0' || c == '\n') {
            errorAt(line, col, "unterminated character literal");
            break;
        }
        advance();
        if (c == '\'') break;
        if (c == '\\' && peek() != '\0') advance();  // skip escaped character
    }
    return makeToken(TokenKind::Char, start, line, col);
}

Token Lexer::lexRawString(std::size_t start, std::uint32_t line, std::uint32_t col) {
    advance();  // opening '"'
    std::string delim;
    while (peek() != '(' && peek() != '\0' && peek() != '\n') delim += advance();
    if (peek() != '(') {
        errorAt(line, col, "unterminated raw string literal");
        return makeToken(TokenKind::String, start, line, col);
    }
    advance();  // '('
    const std::string closer = ")" + delim + "\"";
    bool closed = false;
    while (pos_ < source_.size()) {
        if (source_.compare(pos_, closer.size(), closer) == 0) {
            for (std::size_t i = 0; i < closer.size(); ++i) advance();
            closed = true;
            break;
        }
        advance();
    }
    if (!closed) errorAt(line, col, "unterminated raw string literal");
    return makeToken(TokenKind::String, start, line, col);
}

}  // namespace ivy