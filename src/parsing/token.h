#pragma once

#include <cstdint>
#include <string_view>

namespace ivy {

enum class TokenKind : std::uint8_t {
    // Lexical atoms
    Identifier,
    Keyword,
    Integer,
    Float,
    String,
    Char,
    EndOfFile,

    // Operators & punctuation
    Plus,         // +
    Minus,        // -
    Star,         // *
    Slash,        // /
    Percent,      // %
    Assign,       // =
    PlusAssign,   // +=
    MinusAssign,  // -=
    StarAssign,   // *=
    SlashAssign,  // /=
    PercentAssign,// %=
    Amp,          // &
    Pipe,         // |
    Caret,        // ^
    Tilde,        // ~
    AmpAssign,    // &=
    PipeAssign,   // |=
    CaretAssign,  // ^=
    Shl,          // <<
    Shr,          // >>
    ShlAssign,    // <<=
    ShrAssign,    // >>=
    Bang,         // !
    Eq,           // ==
    Ne,           // !=
    Lt,           // <
    Gt,           // >
    Le,           // <=
    Ge,           // >=
    AndAnd,       // &&
    OrOr,         // ||
    PlusPlus,     // ++
    MinusMinus,   // --
    Arrow,        // ->
    Dot,          // .
    ArrowStar,    // ->*
    DotStar,      // .*
    Question,     // ?
    Colon,        // :
    ColonColon,   // ::
    Semi,         // ;
    Comma,        // ,
    LParen,       // (
    RParen,       // )
    LBrace,       // {
    RBrace,       // }
    LBracket,     // [
    RBracket,     // ]
    Ellipsis,     // ...
    Hash,         // #
};

constexpr std::string_view tokenKindName(TokenKind kind);

struct Token {
    TokenKind kind;
    std::string_view lexeme;  // slice into the source buffer
    std::uint32_t line = 0;   // 1-based
    std::uint32_t col = 0;    // 1-based, in bytes
};

constexpr std::string_view tokenKindName(TokenKind kind) {
    using enum TokenKind;
    switch (kind) {
        case Identifier:   return "identifier";
        case Keyword:      return "keyword";
        case Integer:      return "integer";
        case Float:        return "float";
        case String:       return "string";
        case Char:         return "char";
        case EndOfFile:    return "eof";
        case Plus:         return "+";
        case Minus:        return "-";
        case Star:         return "*";
        case Slash:        return "/";
        case Percent:      return "%";
        case Assign:       return "=";
        case PlusAssign:   return "+=";
        case MinusAssign:  return "-=";
        case StarAssign:   return "*=";
        case SlashAssign:  return "/=";
        case PercentAssign:return "%=";
        case Amp:          return "&";
        case Pipe:         return "|";
        case Caret:        return "^";
        case Tilde:        return "~";
        case AmpAssign:    return "&=";
        case PipeAssign:   return "|=";
        case CaretAssign:  return "^=";
        case Shl:          return "<<";
        case Shr:          return ">>";
        case ShlAssign:    return "<<=";
        case ShrAssign:    return ">>=";
        case Bang:         return "!";
        case Eq:           return "==";
        case Ne:           return "!=";
        case Lt:           return "<";
        case Gt:           return ">";
        case Le:           return "<=";
        case Ge:           return ">=";
        case AndAnd:       return "&&";
        case OrOr:         return "||";
        case PlusPlus:     return "++";
        case MinusMinus:   return "--";
        case Arrow:        return "->";
        case Dot:          return ".";
        case ArrowStar:    return "->*";
        case DotStar:      return ".*";
        case Question:     return "?";
        case Colon:        return ":";
        case ColonColon:   return "::";
        case Semi:         return ";";
        case Comma:        return ",";
        case LParen:       return "(";
        case RParen:       return ")";
        case LBrace:       return "{";
        case RBrace:       return "}";
        case LBracket:     return "[";
        case RBracket:     return "]";
        case Ellipsis:     return "...";
        case Hash:         return "#";
    }
    return "?";
}

}  // namespace ivy