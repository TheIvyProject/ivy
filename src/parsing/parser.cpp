#include "parsing/parser.h"

#include <cctype>
#include <string>

namespace ivy {
namespace {

template <typename T, typename... Args>
std::unique_ptr<Expr> makeExpr(SourceLoc loc, Args&&... args) {
    auto e = std::make_unique<Expr>();
    e->loc = loc;
    e->node = T{std::forward<Args>(args)...};
    return e;
}

template <typename T, typename... Args>
std::unique_ptr<Stmt> makeStmt(SourceLoc loc, Args&&... args) {
    auto s = std::make_unique<Stmt>();
    s->loc = loc;
    s->node = T{std::forward<Args>(args)...};
    return s;
}

SourceLoc locOf(const Token& tok) { return SourceLoc{tok.line, tok.col}; }

// {token kind, operator, precedence} — higher binds tighter.
struct BinOpInfo {
    TokenKind kind;
    std::string_view op;
    int prec;
};

constexpr BinOpInfo kBinOps[] = {
    {TokenKind::OrOr,  "||", 1},  {TokenKind::AndAnd, "&&", 2},
    {TokenKind::Pipe,  "|",  3},  {TokenKind::Caret,  "^",  4},
    {TokenKind::Amp,   "&",  5},  {TokenKind::Eq,     "==", 6},
    {TokenKind::Ne,    "!=", 6},  {TokenKind::Lt,     "<",  7},
    {TokenKind::Gt,    ">",  7},  {TokenKind::Le,     "<=", 7},
    {TokenKind::Ge,    ">=", 7},  {TokenKind::Shl,    "<<", 8},
    {TokenKind::Shr,   ">>", 8},  {TokenKind::Plus,   "+",  9},
    {TokenKind::Minus, "-",  9},  {TokenKind::Star,   "*",  10},
    {TokenKind::Slash, "/",  10}, {TokenKind::Percent, "%", 10},
};

const BinOpInfo* binOpInfo(TokenKind kind) {
    for (const BinOpInfo& info : kBinOps) {
        if (info.kind == kind) return &info;
    }
    return nullptr;
}

bool isAssignmentOp(TokenKind kind) {
    switch (kind) {
        case TokenKind::Assign:
        case TokenKind::PlusAssign:
        case TokenKind::MinusAssign:
        case TokenKind::StarAssign:
        case TokenKind::SlashAssign:
        case TokenKind::PercentAssign:
        case TokenKind::AmpAssign:
        case TokenKind::PipeAssign:
        case TokenKind::CaretAssign:
        case TokenKind::ShlAssign:
        case TokenKind::ShrAssign:
            return true;
        default:
            return false;
    }
}

// Keywords that are unsupported in the Ivy subset, with a targeted message.
constexpr struct {
    std::string_view keyword;
    std::string_view message;
} kUnsupported[] = {
    {"class", ""},   // classes ARE supported (P4.4) — handled in parseTopLevel (same as struct)
    {"struct", ""},  // structs ARE supported (P4.4) — handled in parseTopLevel
    {"union", "unions are not supported in the Ivy subset"},
    {"enum", ""},  // enums ARE supported (P4.1) — handled in parseTopLevel
    {"template", "templates are not supported in the Ivy subset"},
    {"namespace", ""},  // namespaces ARE supported (P4.2) — handled in parseTopLevel
    {"using", "'using' declarations are not supported in the Ivy subset"},
    {"typedef", "typedefs are not supported in the Ivy subset"},
    {"static_assert", "static_assert is not supported in the Ivy subset"},
    {"try", "exceptions are not supported in the Ivy subset"},
    {"catch", "exceptions are not supported in the Ivy subset"},
    {"throw", "exceptions are not supported in the Ivy subset"},
    {"switch", "switch statements are not supported in the Ivy subset yet"},
    {"case", "switch statements are not supported in the Ivy subset yet"},
    {"default", "switch statements are not supported in the Ivy subset yet"},
    {"goto", "goto is not supported in the Ivy subset"},
    {"lambda", ""},  // lambdas have no keyword; handled via '[' in parsePrimary
    {"auto", "'auto' type deduction is not supported in the Ivy subset yet"},
    {"decltype", "'decltype' is not supported in the Ivy subset"},
    {"operator", "operator overloading is not supported in the Ivy subset"},
    {"asm", "'asm' is not supported in the Ivy subset"},
    {"const_cast", "casts are not supported; use [[ivy::unsafe]] blocks"},
    {"static_cast", "casts are not supported; use [[ivy::unsafe]] blocks"},
    {"reinterpret_cast", "casts are not supported; use [[ivy::unsafe]] blocks"},
    {"dynamic_cast", "casts are not supported; use [[ivy::unsafe]] blocks"},
    {"virtual", "virtual dispatch is not supported in the Ivy subset"},
    {"friend", "friends are not supported in the Ivy subset"},
    {"extern", "only 'extern \"C\"' is supported"},
};

}  // namespace

Parser::Parser(std::span<const Token> tokens, bool cnumberEnabled)
    : tokens_(tokens), cnumberEnabled_(cnumberEnabled) {}

const Token& Parser::expect(TokenKind kind, std::string_view what) {
    if (at(kind)) return next();
    errorAt(peek(), what);
    return next();  // consume anyway so parsing always progresses
}

void Parser::expectKeyword(std::string_view kw, std::string_view what) {
    if (atKeyword(kw)) {
        next();
        return;
    }
    errorAt(peek(), what);
}

void Parser::errorAt(const Token& tok, std::string_view message) {
    if (failed_ && tok.kind == TokenKind::EndOfFile) return;  // avoid error storms at EOF
    diagnostics_.push_back(Diagnostic{tok.line, tok.col, std::string(message)});
    failed_ = true;
}

// Skip to the next ';' or '}' (recovery after an error). Always makes
// progress so callers cannot loop forever on the same token.
void Parser::synchronize() {
    if (at(TokenKind::EndOfFile)) return;
    bool advanced = false;
    while (!at(TokenKind::Semi) && !at(TokenKind::RBrace) && !at(TokenKind::EndOfFile)) {
        next();
        advanced = true;
    }
    if (at(TokenKind::Semi)) {
        next();
        advanced = true;
    }
    if (!advanced) next();  // consume the blocking token (e.g. '}') and move on
}

// --- attributes ---

std::vector<Attribute> Parser::parseAttributeList() {
    std::vector<Attribute> attrs;
    while (at(TokenKind::LBracket) && peek(1).kind == TokenKind::LBracket) {
        const Token attrStart = peek();
        next();  // [
        next();  // [

        std::string_view ns, name;
        if (at(TokenKind::Identifier)) {
            ns = next().lexeme;
            if (at(TokenKind::ColonColon)) {
                next();
                if (!at(TokenKind::Identifier)) {
                    errorAt(peek(), "expected attribute name after '::'");
                } else {
                    name = next().lexeme;
                }
            } else {
                name = ns;
                ns = {};
            }
        } else {
            errorAt(peek(), "expected attribute name");
        }

        std::vector<std::string_view> args;
        if (at(TokenKind::LParen)) {
            next();
            while (!at(TokenKind::RParen) && !at(TokenKind::EndOfFile)) {
                const Token& a = peek();
                if (a.kind == TokenKind::Identifier || a.kind == TokenKind::Integer ||
                    a.kind == TokenKind::Float || a.kind == TokenKind::String ||
                    a.kind == TokenKind::Keyword) {
                    args.push_back(a.lexeme);
                    next();
                } else {
                    errorAt(a, "expected attribute argument");
                    next();
                }
                if (!at(TokenKind::RParen)) expect(TokenKind::Comma, "expected ',' in attribute argument list");
            }
            expect(TokenKind::RParen, "expected ')' to close attribute argument list");
        }

        expect(TokenKind::RBracket, "expected ']'");
        expect(TokenKind::RBracket, "expected ']'");

        if (!ns.empty() && ns != "ivy") {
            errorAt(attrStart, "unknown attribute namespace '" + std::string(ns) +
                                   "': only ivy:: attributes are supported in the Ivy subset");
            continue;
        }
        attrs.push_back(Attribute{name, std::move(args), locOf(attrStart)});
    }
    return attrs;
}

void Parser::validateAttributes(const std::vector<Attribute>& attrs,
                                std::initializer_list<std::string_view> allowed) {
    for (const Attribute& a : attrs) {
        bool ok = false;
        for (std::string_view name : allowed) {
            if (a.name == name) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            errorAt(Token{TokenKind::Identifier, a.name, a.loc.line, a.loc.col},
                    "unknown Ivy attribute 'ivy::" + std::string(a.name) +
                        "' in this position");
        }
    }
}

// --- declarations ---

// C-style number type keywords that are only accepted when
// `#pragma ivy cnumber` is active. Hex/octal/binary integer literals
// are always allowed (they are just syntax, not a type).
static bool isCStyleTypeKeyword(std::string_view kw) {
    return kw == "unsigned" || kw == "signed" || kw == "char" ||
           kw == "short" || kw == "int" || kw == "long" ||
           kw == "float" || kw == "double";
}

bool Parser::isTypeStart() const {
    // User-defined enum/struct type names arrive as Identifier (not Keyword).
    if (peek().kind == TokenKind::Identifier) {
        const std::string_view id = peek().lexeme;
        for (const auto& en : enumNames_) {
            if (en == id) return true;
        }
        for (const auto& sn : structNames_) {
            if (sn == id) return true;
        }
        // Qualified name: `ns::Struct` — peek ahead for `::`.
        if (peek(1).kind == TokenKind::ColonColon) {
            // Build the qualified name and check against struct/enum names.
            std::string qual;
            qual += peek().lexeme;
            std::size_t lookahead = 1;
            while (lookahead + 1 < tokens_.size() &&
                   tokens_[pos_ + lookahead].kind == TokenKind::ColonColon &&
                   tokens_[pos_ + lookahead + 1].kind == TokenKind::Identifier) {
                qual += "::";
                qual += tokens_[pos_ + lookahead + 1].lexeme;
                lookahead += 2;
                if (pos_ + lookahead >= tokens_.size() ||
                    tokens_[pos_ + lookahead].kind != TokenKind::ColonColon) break;
            }
            for (const auto& sn : structNames_) {
                if (sn == qual) return true;
            }
            for (const auto& en : enumNames_) {
                if (en == qual) return true;
            }
        }
    }
    if (peek().kind != TokenKind::Keyword) return false;
    const std::string_view kw = peek().lexeme;
    // C-style types: only when #pragma ivy cnumber is active.
    if (!cnumberEnabled_ && isCStyleTypeKeyword(kw)) return false;
    if (kw == "const" || kw == "unsigned" || kw == "signed" || kw == "void" ||
        kw == "bool" || kw == "char" || kw == "short" || kw == "int" ||
        kw == "long" || kw == "float" || kw == "double" ||
        // Ivy builtin types
        kw == "int8_t" || kw == "int16_t" || kw == "int32_t" || kw == "int64_t" ||
        kw == "uint8_t" || kw == "uint16_t" || kw == "uint32_t" || kw == "uint64_t" ||
        kw == "float16_t" || kw == "float32_t" || kw == "float64_t" ||
        kw == "float128_t" || kw == "bfloat16_t" ||
        kw == "size_t" || kw == "ptrdiff_t" || kw == "nullptr_t" ||
        kw == "max_align_t") {
        return true;
    }
    return false;
}

Type Parser::parseType() {
    Type t;
    if (atKeyword("const")) {
        t.isConst = true;
        next();
    }
    // User-defined enum/struct type name (Identifier, not Keyword).
    if (at(TokenKind::Identifier)) {
        // Try qualified name: `ns::Struct` or `ns::ns2::Struct`.
        // Peek ahead: Identifier `::` Identifier [`::` ...]
        if (peek(1).kind == TokenKind::ColonColon) {
            std::string qual;
            qual.reserve(peek().lexeme.size());
            qual += peek().lexeme;
            std::size_t lookahead = 1;
            while (lookahead + 1 < tokens_.size() &&
                   tokens_[pos_ + lookahead].kind == TokenKind::ColonColon &&
                   tokens_[pos_ + lookahead + 1].kind == TokenKind::Identifier) {
                qual += "::";
                qual += tokens_[pos_ + lookahead + 1].lexeme;
                lookahead += 2;
                // Stop if next is not `::`
                if (pos_ + lookahead >= tokens_.size() ||
                    tokens_[pos_ + lookahead].kind != TokenKind::ColonColon) break;
            }
            // Check if `qual` matches a struct or enum name.
            bool isUserType = false;
            for (const auto& sn : structNames_) {
                if (sn == qual) { isUserType = true; break; }
            }
            if (!isUserType) {
                for (const auto& en : enumNames_) {
                    if (en == qual) { isUserType = true; break; }
                }
            }
            if (isUserType) {
                // Store the qualified name in stable storage so the
                // string_view in `t.base` remains valid.
                nameStorage_.push_back(qual);
                t.base = nameStorage_.back();
                // Consume the qualified name tokens.
                next();  // first Identifier
                while (at(TokenKind::ColonColon)) {
                    next();  // `::`
                    next();  // Identifier
                }
                // pointer / reference modifiers
                while (at(TokenKind::Star)) {
                    next();
                    ++t.pointerDepth;
                    if (atKeyword("const")) next();
                }
                if (at(TokenKind::Amp)) {
                    next();
                    t.isReference = true;
                    if (atKeyword("const")) { t.isConst = true; next(); }
                }
                return t;
            }
        }
        // Single-identifier user type (no namespace qualifier).
        bool isUserType = false;
        for (const auto& en : enumNames_) {
            if (en == peek().lexeme) { isUserType = true; break; }
        }
        if (!isUserType) {
            for (const auto& sn : structNames_) {
                if (sn == peek().lexeme) { isUserType = true; break; }
            }
        }
        if (isUserType) {
            t.base = next().lexeme;
            // pointer / reference modifiers
            while (at(TokenKind::Star)) {
                next();
                ++t.pointerDepth;
                if (atKeyword("const")) next();
            }
            if (at(TokenKind::Amp)) {
                next();
                t.isReference = true;
                if (atKeyword("const")) { t.isConst = true; next(); }
            }
            return t;
        }
    }
    // `unsigned`/`signed` — C-style, requires #pragma ivy cnumber.
    if (atKeyword("unsigned") || atKeyword("signed")) {
        if (!cnumberEnabled_) {
            errorAt(peek(), "C-style type '" + std::string(peek().lexeme) +
                    "' requires #pragma ivy cnumber; use a fixed-width type like int32_t/uint32_t");
        }
        t.isUnsigned = atKeyword("unsigned");
        next();
    }
    if (atKeyword("int8_t") || atKeyword("int16_t") || atKeyword("int32_t") ||
        atKeyword("int64_t") || atKeyword("uint8_t") || atKeyword("uint16_t") ||
        atKeyword("uint32_t") || atKeyword("uint64_t") || atKeyword("float16_t") ||
        atKeyword("float32_t") || atKeyword("float64_t") || atKeyword("float128_t") ||
        atKeyword("bfloat16_t") || atKeyword("size_t") || atKeyword("ptrdiff_t") ||
        atKeyword("nullptr_t") || atKeyword("max_align_t")) {
        t.base = next().lexeme;
    } else if (atKeyword("char")) {
        if (!cnumberEnabled_) {
            errorAt(peek(), "C-style type 'char' requires #pragma ivy cnumber; use int8_t/uint8_t");
        }
        next();
        t.base = "char";
        // `char int` is not valid C++, but `signed char` / `unsigned char`
        // are — those are handled via isUnsigned above. No trailing int.
    } else if (atKeyword("short")) {
        if (!cnumberEnabled_) {
            errorAt(peek(), "C-style type 'short' requires #pragma ivy cnumber; use int16_t/uint16_t");
        }
        next();
        if (atKeyword("int")) next();  // `short int` == `short`
        t.base = "short";
    } else if (atKeyword("int")) {
        if (!cnumberEnabled_) {
            errorAt(peek(), "C-style type 'int' requires #pragma ivy cnumber; use int32_t/uint32_t");
        }
        next();
        t.base = "int";
    } else if (atKeyword("float")) {
        if (!cnumberEnabled_) {
            errorAt(peek(), "C-style type 'float' requires #pragma ivy cnumber; use float32_t");
        }
        next();
        t.base = "float";
    } else if (atKeyword("double")) {
        if (!cnumberEnabled_) {
            errorAt(peek(), "C-style type 'double' requires #pragma ivy cnumber; use float64_t");
        }
        next();
        t.base = "double";
    } else if (atKeyword("bool") || atKeyword("void")) {
        t.base = next().lexeme;
    } else if (atKeyword("long")) {
        if (!cnumberEnabled_) {
            errorAt(peek(), "C-style type 'long' requires #pragma ivy cnumber; use int64_t");
        }
        next();
        if (atKeyword("long")) {
            next();
            if (atKeyword("int")) next();  // `long long int` == `long long`
            t.base = "long long";
        } else if (atKeyword("double")) {
            next();
            t.base = "long double";
        } else if (atKeyword("int")) {
            next();  // `long int` == `long`
            t.base = "long";
        } else {
            t.base = "long";
        }
    } else {
        if (!t.isUnsigned && !t.isConst) {
            errorAt(peek(), "expected a type (void, bool, int8_t, int16_t, int32_t, int64_t, uint8_t, ..., float32_t, float64_t, ...)");
        } else {
            t.base = "int";  // "unsigned" / "signed" / "const ..." alone → int
            if (t.isUnsigned) t.isUnsigned = true;
        }
    }
    while (at(TokenKind::Star)) {
        next();
        ++t.pointerDepth;
        if (atKeyword("const")) next();  // 'const' after a pointer star
    }
    // T&  or  const T&
    if (at(TokenKind::Amp)) {
        next();
        t.isReference = true;
        if (atKeyword("const")) { t.isConst = true; next(); }
    }
    return t;
}

void Parser::parseTopLevel(TranslationUnit& tu) {
    while (!at(TokenKind::EndOfFile)) {
        if (at(TokenKind::Hash)) {
            // The preprocessor runs before the parser and consumes every
            // #include it recognizes. A `#` token reaching the parser
            // means an unsupported directive (e.g. #define / #ifdef, not
            // yet implemented) or a stray `#`.
            errorAt(peek(),
                    "unsupported preprocessor directive (only #include is handled)");
            synchronize();
            continue;
        }

        // 'extern "C"' is handled before the unsupported-keyword scan, which
        // would otherwise report it (kUnsupported contains "extern").
        if (atKeyword("extern")) {
            const SourceLoc loc = locOf(peek());
            next();
            if (at(TokenKind::String) && peek().lexeme == "\"C\"") {
                next();
                std::vector<Attribute> attrs = parseAttributeList();
                parseExternC(tu, loc, std::move(attrs));
            } else {
                errorAt(peek(), "only 'extern \"C\"' is supported in the Ivy subset");
                synchronize();
            }
            continue;
        }

        // `enum` / `enum class` — handled before the unsupported-keyword
        // scan (which has `enum` with an empty message, so it would be
        // skipped anyway, but we intercept here to route to parseEnum).
        if (atKeyword("enum")) {
            const SourceLoc loc = locOf(peek());
            parseEnum(tu, loc);
            continue;
        }

        // `struct` / `class` — handled before the unsupported-keyword
        // scan (which has them with empty messages, so they would be
        // skipped anyway, but we intercept here to route to parseStruct).
        if (atKeyword("struct") || atKeyword("class")) {
            const SourceLoc loc = locOf(peek());
            const bool isClass = atKeyword("class");
            parseStruct(tu, loc, isClass);
            continue;
        }

        // `namespace` — handled before the unsupported-keyword scan
        // (which has `namespace` with an empty message).  Recursively
        // parses nested declarations within the namespace block.
        if (atKeyword("namespace")) {
            const SourceLoc loc = locOf(peek());
            parseNamespace(tu, loc);
            continue;
        }

        // Report unsupported constructs with targeted messages.
        const Token& t = peek();
        if (t.kind == TokenKind::Keyword) {
            bool reported = false;
            for (const auto& [kw, msg] : kUnsupported) {
                if (t.lexeme == kw && !msg.empty()) {
                    errorAt(t, std::string(msg));
                    synchronize();
                    reported = true;
                    break;
                }
            }
            if (reported) continue;
        }

        const SourceLoc loc = locOf(peek());
        std::vector<Attribute> attrs = parseAttributeList();

        parseFunction(tu, loc, std::move(attrs), /*isExternC=*/false);
    }
}

void Parser::parseExternC(TranslationUnit& tu, SourceLoc loc, std::vector<Attribute> attrs) {
    if (at(TokenKind::LBrace)) {
        next();
        while (!at(TokenKind::RBrace) && !at(TokenKind::EndOfFile)) {
            std::vector<Attribute> fattrs = parseAttributeList();
            const SourceLoc floc = locOf(peek());
            parseFunction(tu, floc, std::move(fattrs), /*isExternC=*/true);
        }
        expect(TokenKind::RBrace, "expected '}' to close extern \"C\" block");
        return;
    }
    parseFunction(tu, loc, std::move(attrs), /*isExternC=*/true);
}

std::string_view Parser::qualifyName(std::string_view name) {
    if (namespaceStack_.empty()) return name;
    std::string qualified;
    for (const auto& ns : namespaceStack_) {
        qualified += ns;
        qualified += "::";
    }
    qualified += name;
    nameStorage_.push_back(std::move(qualified));
    return std::string_view(nameStorage_.back());
}

std::string_view Parser::currentNamespacePrefix() {
    if (namespaceStack_.empty()) return {};
    std::string prefix;
    for (const auto& ns : namespaceStack_) {
        prefix += ns;
        prefix += "::";
    }
    nameStorage_.push_back(std::move(prefix));
    return std::string_view(nameStorage_.back());
}

void Parser::parseNamespace(TranslationUnit& tu, SourceLoc /*loc*/) {
    expectKeyword("namespace", "expected 'namespace'");
    // Namespace name (Ivy requires a name — no anonymous namespaces).
    if (!at(TokenKind::Identifier)) {
        errorAt(peek(), "expected namespace name after 'namespace'");
        synchronize();
        return;
    }
    const std::string_view nsName = next().lexeme;

    // Optional `= expr` (namespace alias) — not supported in Ivy.
    if (at(TokenKind::Assign)) {
        errorAt(peek(), "namespace aliases are not supported in the Ivy subset");
        synchronize();
        return;
    }

    expect(TokenKind::LBrace, "expected '{' to start namespace body");

    namespaceStack_.emplace_back(nsName);

    // Recursively parse top-level declarations inside the namespace.
    // We inline the dispatch loop (rather than calling parseTopLevel)
    // because parseTopLevel consumes until EOF, which we don't want.
    while (!at(TokenKind::RBrace) && !at(TokenKind::EndOfFile)) {
        if (at(TokenKind::Hash)) {
            errorAt(peek(), "preprocessor directives inside namespace body are not supported");
            synchronize();
            continue;
        }
        if (atKeyword("extern")) {
            const SourceLoc eloc = locOf(peek());
            next();
            if (at(TokenKind::String) && peek().lexeme == "\"C\"") {
                next();
                std::vector<Attribute> attrs = parseAttributeList();
                parseExternC(tu, eloc, std::move(attrs));
            } else {
                errorAt(peek(), "only 'extern \"C\"' is supported in the Ivy subset");
                synchronize();
            }
            continue;
        }
        if (atKeyword("enum")) {
            const SourceLoc eloc = locOf(peek());
            parseEnum(tu, eloc);
            continue;
        }
        if (atKeyword("struct") || atKeyword("class")) {
            const SourceLoc sloc = locOf(peek());
            parseStruct(tu, sloc, atKeyword("class"));
            continue;
        }
        if (atKeyword("namespace")) {
            const SourceLoc eloc = locOf(peek());
            parseNamespace(tu, eloc);
            continue;
        }
        // Unsupported constructs
        const Token& t = peek();
        if (t.kind == TokenKind::Keyword) {
            bool reported = false;
            for (const auto& [kw, msg] : kUnsupported) {
                if (t.lexeme == kw && !msg.empty()) {
                    errorAt(t, std::string(msg));
                    synchronize();
                    reported = true;
                    break;
                }
            }
            if (reported) continue;
        }
        const SourceLoc floc = locOf(peek());
        std::vector<Attribute> attrs = parseAttributeList();
        parseFunction(tu, floc, std::move(attrs), /*isExternC=*/false);
    }

    namespaceStack_.pop_back();
    expect(TokenKind::RBrace, "expected '}' to close namespace body");
    // Optional trailing ';' (C++ allows but does not require it).
    if (at(TokenKind::Semi)) next();
}

void Parser::parseEnum(TranslationUnit& tu, SourceLoc loc) {
    // `enum`  /  `enum class`  /  `enum struct`
    expectKeyword("enum", "expected 'enum'");
    bool isScoped = false;
    if (atKeyword("class") || atKeyword("struct")) {
        isScoped = true;
        next();
    }

    // Optional name. C++ allows anonymous enums but Ivy requires a name.
    if (!at(TokenKind::Identifier)) {
        errorAt(peek(), "expected enum name after 'enum'");
        synchronize();
        return;
    }
    const std::string_view name = qualifyName(next().lexeme);

    // Optional explicit underlying type: `enum E : int32_t { ... }`
    Type underlyingType;
    underlyingType.base = "int";  // C++ default for both `enum` and `enum class`
    if (at(TokenKind::Colon)) {
        next();
        underlyingType = parseType();
        // Validate it's an integer type.
        const std::string_view b = underlyingType.base;
        const bool isInt = b == "int" || b == "int8_t" || b == "int16_t" ||
                           b == "int32_t" || b == "int64_t" ||
                           b == "uint8_t" || b == "uint16_t" ||
                           b == "uint32_t" || b == "uint64_t" ||
                           b == "char" || b == "short" || b == "long" ||
                           b == "long long" || b == "bool" ||
                           b == "size_t" || b == "ptrdiff_t";
        if (!isInt) {
            errorAt(peek(), "enum underlying type must be an integer type");
        }
    }

    expect(TokenKind::LBrace, "expected '{' to start enum body");

    EnumDecl ed;
    ed.name = name;
    ed.namespacePrefix = currentNamespacePrefix();
    ed.isScoped = isScoped;
    ed.underlyingType = underlyingType;
    ed.loc = loc;

    while (!at(TokenKind::RBrace) && !at(TokenKind::EndOfFile)) {
        if (!at(TokenKind::Identifier)) {
            errorAt(peek(), "expected enumerator name");
            synchronize();
            break;
        }
        Enumerator en;
        en.loc = locOf(peek());
        en.name = next().lexeme;

        // Optional `= value`
        if (at(TokenKind::Assign)) {
            next();
            en.value = parseExpr();
        }

        ed.enumerators.push_back(std::move(en));

        if (!at(TokenKind::RBrace)) {
            expect(TokenKind::Comma, "expected ',' between enumerators");
            if (at(TokenKind::RBrace)) break;  // trailing comma OK
        }
    }

    expect(TokenKind::RBrace, "expected '}' to close enum body");
    expect(TokenKind::Semi, "expected ';' after enum definition");

    // Register the enum name so isTypeStart()/parseType() accept it.
    enumNames_.push_back(name);

    tu.enums.push_back(std::move(ed));
}

void Parser::parseStruct(TranslationUnit& tu, SourceLoc loc, bool isClass) {
    // `struct Name { ... } ;`  /  `class Name { ... } ;`
    expectKeyword(isClass ? "class" : "struct",
                  isClass ? "expected 'class'" : "expected 'struct'");

    // Ivy requires a name — no anonymous structs.
    if (!at(TokenKind::Identifier)) {
        errorAt(peek(), isClass ? "expected class name after 'class'"
                                : "expected struct name after 'struct'");
        synchronize();
        return;
    }
    const std::string_view name = qualifyName(next().lexeme);

    // Ivy does not support inheritance — reject `final` or `: Base`.
    if (atKeyword("final")) {
        errorAt(peek(), "'final' is not supported in the Ivy subset");
        next();
    }
    if (at(TokenKind::Colon)) {
        errorAt(peek(), "struct/class inheritance is not supported in the Ivy subset");
        synchronize();
        return;
    }

    expect(TokenKind::LBrace, "expected '{' to start struct body");

    StructDecl sd;
    sd.name = name;
    sd.namespacePrefix = currentNamespacePrefix();
    sd.isClass = isClass;
    sd.loc = loc;

    // Parse fields: `Type name;` or `Type name = default;`
    // Access specifiers (`public:`/`private:`) are accepted but
    // ignored — Ivy treats all members as public (C-style aggregate).
    while (!at(TokenKind::RBrace) && !at(TokenKind::EndOfFile)) {
        // Accept and ignore access specifiers.
        if (atKeyword("public") || atKeyword("private") || atKeyword("protected")) {
            next();
            if (at(TokenKind::Colon)) next();
            continue;
        }
        if (!isTypeStart()) {
            errorAt(peek(), "expected field declaration in struct body");
            synchronize();
            break;
        }
        Field f;
        f.type = parseType();
        if (!at(TokenKind::Identifier)) {
            errorAt(peek(), "expected field name");
            synchronize();
            break;
        }
        f.loc = locOf(peek());
        f.name = next().lexeme;

        // Optional default initializer: `Type name = expr;`
        if (at(TokenKind::Assign)) {
            next();
            f.init = parseExpr();
        }

        expect(TokenKind::Semi, "expected ';' after field declaration");
        sd.fields.push_back(std::move(f));
    }

    expect(TokenKind::RBrace, "expected '}' to close struct body");
    expect(TokenKind::Semi, "expected ';' after struct definition");

    // Register the struct name so isTypeStart()/parseType() accept it.
    structNames_.push_back(name);

    tu.structs.push_back(std::move(sd));
}

void Parser::parseFunction(TranslationUnit& tu, SourceLoc loc, std::vector<Attribute> attrs,
                           bool isExternC) {
    Type returnType = parseType();
    if (returnType.base.empty()) {
        // parseType already reported the error; recover at statement-ish boundary.
        synchronize();
        return;
    }

    // Attributes between the return type and the name, e.g. [[ivy::lt_ret(a)]].
    std::vector<Attribute> midAttrs = parseAttributeList();
    attrs.insert(attrs.end(), std::make_move_iterator(midAttrs.begin()),
                 std::make_move_iterator(midAttrs.end()));
    validateAttributes(attrs, {"lt_def", "lt_ret"});

    if (!at(TokenKind::Identifier)) {
        errorAt(peek(), "expected function name");
        synchronize();
        return;
    }
    const std::string_view name = qualifyName(next().lexeme);

    expect(TokenKind::LParen, "expected '(' after function name '" + std::string(name) + "'");
    std::vector<Param> params = parseParams();
    expect(TokenKind::RParen, "expected ')' to close parameter list");

    Function fn;
    fn.attrs = std::move(attrs);
    fn.returnType = std::move(returnType);
    fn.name = name;
    fn.namespacePrefix = currentNamespacePrefix();
    fn.params = std::move(params);
    fn.isExternC = isExternC;
    fn.loc = loc;

    if (at(TokenKind::LBrace)) {
        std::unique_ptr<Stmt> bodyStmt = parseCompound();
        fn.body =
            std::make_unique<Stmt::Compound>(std::move(std::get<Stmt::Compound>(bodyStmt->node)));
    } else {
        expect(TokenKind::Semi, "expected '{' for function body or ';' for a declaration");
    }

    tu.functions.push_back(std::move(fn));
}

std::vector<Param> Parser::parseParams() {
    std::vector<Param> params;
    while (!at(TokenKind::RParen) && !at(TokenKind::EndOfFile)) {
        // Variadic `...` — only valid in extern "C" declarations (e.g. printf).
        if (at(TokenKind::Ellipsis)) {
            next();  // consume `...`
            // The variadic arg doesn't produce a Param — it's metadata
            // for codegen (which currently ignores it and just uses the
            // fixed params).  We skip it here.
            break;
        }
        Param p;
        p.loc = locOf(peek());
        p.type = parseType();
        if (at(TokenKind::Identifier)) {
            p.name = next().lexeme;
        } else if (at(TokenKind::LBracket) && peek(1).kind == TokenKind::LBracket) {
            // [[ivy::lt(a)]] on an unnamed parameter — name comes after.
            // (Not valid C++; handle gracefully.)
            errorAt(peek(), "attribute must follow the parameter name");
        }
        p.attrs = parseAttributeList();
        validateAttributes(p.attrs, {"lt"});
        params.push_back(std::move(p));
        if (!at(TokenKind::RParen)) {
            expect(TokenKind::Comma, "expected ',' between parameters");
            if (at(TokenKind::RParen)) break;  // tolerate trailing comma
        }
    }
    return params;
}

// --- statements ---

std::unique_ptr<Stmt> Parser::parseStatement() {
    const SourceLoc loc = locOf(peek());

    // [[ivy::unsafe]] { ... }
    if (at(TokenKind::LBracket) && peek(1).kind == TokenKind::LBracket) {
        std::vector<Attribute> attrs = parseAttributeList();
        validateAttributes(attrs, {"unsafe"});
        if (attrs.size() != 1 || attrs[0].name != "unsafe") {
            errorAt(peek(), "only [[ivy::unsafe]] is allowed on statements");
        }
        const bool saved = inUnsafe_;
        inUnsafe_ = true;
        std::unique_ptr<Stmt> body = parseStatement();
        inUnsafe_ = saved;
        return makeStmt<Stmt::Unsafe>(loc, std::move(body));
    }

    if (atKeyword("if")) return parseIf();
    if (atKeyword("while")) return parseWhile();
    if (atKeyword("do")) return parseDoWhile();
    if (atKeyword("for")) return parseFor();
    if (atKeyword("return")) return parseReturn();
    if (atKeyword("break")) {
        next();
        expect(TokenKind::Semi, "expected ';' after 'break'");
        return makeStmt<Stmt::Break>(loc);
    }
    if (atKeyword("continue")) {
        next();
        expect(TokenKind::Semi, "expected ';' after 'continue'");
        return makeStmt<Stmt::Continue>(loc);
    }
    if (atKeyword("switch")) {
        errorAt(peek(), "switch statements are not supported in the Ivy subset yet");
        synchronize();
        return makeStmt<Stmt::Null>(loc);
    }
    if (atKeyword("goto")) {
        errorAt(peek(), "goto is not supported in the Ivy subset");
        synchronize();
        return makeStmt<Stmt::Null>(loc);
    }
    if (atKeyword("try") || atKeyword("throw")) {
        errorAt(peek(), "exceptions are not supported in the Ivy subset");
        synchronize();
        return makeStmt<Stmt::Null>(loc);
    }

    if (at(TokenKind::LBrace)) return parseCompound();
    return parseDeclOrExprStmt();
}

std::unique_ptr<Stmt> Parser::parseCompound() {
    const SourceLoc loc = locOf(peek());
    auto compound = makeStmt<Stmt::Compound>(loc);
    auto& stmts = std::get<Stmt::Compound>(compound->node).stmts;
    expect(TokenKind::LBrace, "expected '{'");
    while (!at(TokenKind::RBrace) && !at(TokenKind::EndOfFile)) {
        stmts.push_back(parseStatement());
    }
    expect(TokenKind::RBrace, "expected '}' to close block");
    return compound;
}

std::unique_ptr<Stmt> Parser::parseIf() {
    const SourceLoc loc = locOf(peek());
    next();  // if
    expect(TokenKind::LParen, "expected '(' after 'if'");
    std::unique_ptr<Expr> cond = parseExpr();
    expect(TokenKind::RParen, "expected ')' after if condition");
    std::unique_ptr<Stmt> thenBranch = parseStatement();
    std::unique_ptr<Stmt> elseBranch;
    if (atKeyword("else")) {
        next();
        elseBranch = parseStatement();
    }
    return makeStmt<Stmt::If>(loc, std::move(cond), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<Stmt> Parser::parseWhile() {
    const SourceLoc loc = locOf(peek());
    next();  // while
    expect(TokenKind::LParen, "expected '(' after 'while'");
    std::unique_ptr<Expr> cond = parseExpr();
    expect(TokenKind::RParen, "expected ')' after while condition");
    std::unique_ptr<Stmt> body = parseStatement();
    return makeStmt<Stmt::While>(loc, std::move(cond), std::move(body));
}

std::unique_ptr<Stmt> Parser::parseDoWhile() {
    const SourceLoc loc = locOf(peek());
    next();  // do
    std::unique_ptr<Stmt> body = parseStatement();
    expectKeyword("while", "expected 'while' after do-while body");
    expect(TokenKind::LParen, "expected '(' after 'while'");
    std::unique_ptr<Expr> cond = parseExpr();
    expect(TokenKind::RParen, "expected ')' after do-while condition");
    expect(TokenKind::Semi, "expected ';' after do-while");
    return makeStmt<Stmt::DoWhile>(loc, std::move(body), std::move(cond));
}

std::unique_ptr<Stmt> Parser::parseFor() {
    const SourceLoc loc = locOf(peek());
    next();  // for
    expect(TokenKind::LParen, "expected '(' after 'for'");

    // init
    std::unique_ptr<Stmt> init;
    if (at(TokenKind::Semi)) {
        init = makeStmt<Stmt::Null>(loc);
    } else if (isTypeStart()) {
        init = parseDeclaration(/*expectSemi=*/false);
    } else {
        init = makeStmt<Stmt::ExprStmt>(loc, parseExpr());
    }
    expect(TokenKind::Semi, "expected ';' after for-loop init");

    // condition
    std::unique_ptr<Expr> cond;
    if (!at(TokenKind::Semi)) cond = parseExpr();
    expect(TokenKind::Semi, "expected ';' after for-loop condition");

    // increment
    std::unique_ptr<Expr> incr;
    if (!at(TokenKind::RParen)) incr = parseExpr();
    expect(TokenKind::RParen, "expected ')' after for-loop increment");

    std::unique_ptr<Stmt> body = parseStatement();
    return makeStmt<Stmt::For>(loc, std::move(init), std::move(cond), std::move(incr),
                               std::move(body));
}

std::unique_ptr<Stmt> Parser::parseReturn() {
    const SourceLoc loc = locOf(peek());
    next();  // return
    std::unique_ptr<Expr> value;
    if (!at(TokenKind::Semi)) value = parseExpr();
    expect(TokenKind::Semi, "expected ';' after return statement");
    return makeStmt<Stmt::Return>(loc, std::move(value));
}

std::unique_ptr<Stmt> Parser::parseDeclOrExprStmt() {
    if (isTypeStart()) return parseDeclaration(/*expectSemi=*/true);
    return parseExpressionStatement();
}

std::unique_ptr<Stmt> Parser::parseDeclaration(bool expectSemi) {
    const SourceLoc loc = locOf(peek());
    Type type = parseType();
    if (type.base.empty()) {
        synchronize();
        return makeStmt<Stmt::Null>(loc);
    }

    auto parseDeclarator = [this](Type t) {
        Stmt::Decl d;
        d.type = std::move(t);
        if (!at(TokenKind::Identifier)) {
            errorAt(peek(), "expected variable name");
            return d;
        }
        d.name = next().lexeme;
        if (at(TokenKind::Assign)) {
            next();
            d.init = parseExpr();
        } else if (at(TokenKind::LBrace)) {
            // Constructor-style aggregate init: `Point p{1, 2};`
            d.init = parsePrimary();
        }
        return d;
    };

    std::vector<Stmt::Decl> decls;
    decls.push_back(parseDeclarator(type));
    while (at(TokenKind::Comma)) {  // `int a = 1, b = 2;`
        next();
        Type t2 = type;
        while (at(TokenKind::Star)) {
            next();
            ++t2.pointerDepth;
        }
        decls.push_back(parseDeclarator(std::move(t2)));
    }

    if (expectSemi) expect(TokenKind::Semi, "expected ';' after declaration");

    if (decls.size() == 1) {
        auto s = makeStmt<Stmt::Decl>(loc);
        std::get<Stmt::Decl>(s->node) = std::move(decls[0]);
        return s;
    }
    // Multi-declarators are desugared into a compound of declarations.
    auto s = makeStmt<Stmt::Compound>(loc);
    for (auto& d : decls) {
        auto one = makeStmt<Stmt::Decl>(loc);
        std::get<Stmt::Decl>(one->node) = std::move(d);
        std::get<Stmt::Compound>(s->node).stmts.push_back(std::move(one));
    }
    return s;
}

std::unique_ptr<Stmt> Parser::parseExpressionStatement() {
    const SourceLoc loc = locOf(peek());
    std::unique_ptr<Expr> value = parseExpr();
    expect(TokenKind::Semi, "expected ';' after expression");
    return makeStmt<Stmt::ExprStmt>(loc, std::move(value));
}

// --- expressions ---

std::unique_ptr<Expr> Parser::parseExpr() {
    const SourceLoc loc = locOf(peek());
    std::unique_ptr<Expr> lhs = parseConditional();
    if (at(TokenKind::Assign) || at(TokenKind::PlusAssign) || at(TokenKind::MinusAssign) ||
        at(TokenKind::StarAssign) || at(TokenKind::SlashAssign) || at(TokenKind::PercentAssign) ||
        at(TokenKind::AmpAssign) || at(TokenKind::PipeAssign) || at(TokenKind::CaretAssign) ||
        at(TokenKind::ShlAssign) || at(TokenKind::ShrAssign)) {
        const Token& opTok = next();
        std::unique_ptr<Expr> rhs = parseExpr();  // right-associative
        return makeExpr<Expr::Assign>(loc, opTok.lexeme, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseConditional() {
    const SourceLoc loc = locOf(peek());
    std::unique_ptr<Expr> cond = parseBinary(1);
    if (at(TokenKind::Question)) {
        next();
        std::unique_ptr<Expr> thenBranch = parseExpr();
        expect(TokenKind::Colon, "expected ':' in conditional expression");
        std::unique_ptr<Expr> elseBranch = parseConditional();
        return makeExpr<Expr::Ternary>(loc, std::move(cond), std::move(thenBranch),
                                       std::move(elseBranch));
    }
    return cond;
}

std::unique_ptr<Expr> Parser::parseBinary(int minPrec) {
    std::unique_ptr<Expr> lhs = parseUnary();
    for (;;) {
        const BinOpInfo* info = binOpInfo(peek().kind);
        if (!info || info->prec < minPrec) break;
        const SourceLoc loc = locOf(peek());
        next();
        std::unique_ptr<Expr> rhs = parseBinary(info->prec + 1);
        lhs = makeExpr<Expr::Binary>(loc, info->op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parseUnary() {
    const SourceLoc loc = locOf(peek());
    switch (peek().kind) {
        case TokenKind::Bang:
        case TokenKind::Tilde:
        case TokenKind::Minus:
        case TokenKind::Plus:
        case TokenKind::Star:
        case TokenKind::Amp:
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus: {
            const Token& op = next();
            std::unique_ptr<Expr> operand = parseUnary();
            return makeExpr<Expr::Unary>(loc, op.lexeme, /*isPrefix=*/true, std::move(operand));
        }
        default:
            break;
    }
    if (atKeyword("new")) return parseNew(loc);
    if (atKeyword("delete")) return parseDelete(loc);
    return parsePostfix(parsePrimary());
}

std::unique_ptr<Expr> Parser::parsePostfix(std::unique_ptr<Expr> lhs) {
    for (;;) {
        const SourceLoc loc = locOf(peek());
        // `::` scope resolution — used for scoped enum constants
        // (`EnumName::Value`). Reuses Expr::Member with isArrow=false
        // and the base being an IdentRef; the HIR builder resolves it.
        if (at(TokenKind::ColonColon)) {
            next();
            const Token& nameTok = peek();
            if (!at(TokenKind::Identifier)) {
                errorAt(peek(), "expected name after '::'");
                return lhs;
            }
            next();
            lhs = makeExpr<Expr::Member>(loc, std::move(lhs), nameTok.lexeme, /*isArrow=*/false);
            continue;
        }
        if (at(TokenKind::LParen)) {  // call
            next();
            std::vector<std::unique_ptr<Expr>> args;
            while (!at(TokenKind::RParen) && !at(TokenKind::EndOfFile)) {
                args.push_back(parseExpr());
                if (!at(TokenKind::RParen)) {
                    expect(TokenKind::Comma, "expected ',' between call arguments");
                    if (at(TokenKind::RParen)) break;
                }
            }
            expect(TokenKind::RParen, "expected ')' to close call");
            lhs = makeExpr<Expr::Call>(loc, std::move(lhs), std::move(args));
        } else if (at(TokenKind::LBracket)) {  // index
            next();
            std::unique_ptr<Expr> index = parseExpr();
            expect(TokenKind::RBracket, "expected ']' after index expression");
            lhs = makeExpr<Expr::Index>(loc, std::move(lhs), std::move(index));
        } else if (at(TokenKind::Dot) || at(TokenKind::Arrow)) {  // member access
            const bool isArrow = at(TokenKind::Arrow);
            next();
            const Token& nameTok = peek();
            if (!at(TokenKind::Identifier)) {
                errorAt(peek(), "expected member name after '.' or '->'");
                return lhs;
            }
            next();
            lhs = makeExpr<Expr::Member>(loc, std::move(lhs), nameTok.lexeme, isArrow);
        } else if (at(TokenKind::PlusPlus) || at(TokenKind::MinusMinus)) {  // postfix ++/--
            const Token& op = next();
            lhs = makeExpr<Expr::Unary>(loc, op.lexeme, /*isPrefix=*/false, std::move(lhs));
        } else {
            break;
        }
    }
    return lhs;
}

std::unique_ptr<Expr> Parser::parsePrimary() {
    const SourceLoc loc = locOf(peek());
    const Token& t = peek();

    if (at(TokenKind::Integer)) {
        next();
        return makeExpr<Expr::IntegerLit>(loc, parseIntegerValue(t.lexeme, t));
    }
    if (at(TokenKind::Float)) {
        next();
        return makeExpr<Expr::FloatLit>(loc, parseFloatValue(t.lexeme, t));
    }
    if (at(TokenKind::String)) {
        next();
        return makeExpr<Expr::StringLit>(loc, t.lexeme);
    }
    if (at(TokenKind::Char)) {
        next();
        return makeExpr<Expr::CharLit>(loc, t.lexeme);
    }
    if (atKeyword("true") || atKeyword("false")) {
        next();
        return makeExpr<Expr::BoolLit>(loc, t.lexeme == "true");
    }
    if (atKeyword("nullptr")) {
        next();
        return makeExpr<Expr::NullptrLit>(loc);
    }
    if (at(TokenKind::Identifier)) {
        next();
        return makeExpr<Expr::IdentRef>(loc, t.lexeme);
    }
    if (at(TokenKind::LBrace)) {
        // Braced aggregate initializer: `{ expr, expr, ... }`.
        // Permitted in initializer position (e.g. `Point p = {1, 2};`).
        // An empty `{}` is a value-initializer (zero-init for structs).
        next();
        auto out = makeExpr<Expr::InitList>(loc);
        auto& il = std::get<Expr::InitList>(out->node);
        if (!at(TokenKind::RBrace)) {
            il.elements.push_back(parseExpr());
            while (at(TokenKind::Comma)) {
                next();
                if (at(TokenKind::RBrace)) break;  // trailing comma
                il.elements.push_back(parseExpr());
            }
        }
        expect(TokenKind::RBrace, "expected '}' to close braced initializer");
        return out;
    }
    if (at(TokenKind::LBracket)) {
        // Lambda expression: `[caps](params) -> ret { body }` or `[](params){ body }`.
        // Capture list: `[x, &y, =, &]` — Ivy supports `[x, &y]` and empty `[]`.
        // `[=]` and `[&]` (capture-all) are not supported.
        next();  // consume '['
        auto out = makeExpr<Expr::Lambda>(loc);
        auto& lam = std::get<Expr::Lambda>(out->node);
        // Parse capture list
        if (!at(TokenKind::RBracket)) {
            do {
                bool byRef = false;
                if (at(TokenKind::Amp)) {
                    byRef = true;
                    next();  // consume '&'
                }
                if (!at(TokenKind::Identifier)) {
                    errorAt(peek(), "expected capture name in lambda capture list");
                    break;
                }
                Expr::Capture cap;
                cap.name = next().lexeme;
                cap.byRef = byRef;
                lam.captures.push_back(cap);
            } while (at(TokenKind::Comma) && (next(), true));
        }
        expect(TokenKind::RBracket, "expected ']' to close lambda capture list");
        // Parse parameter list (optional — `()` required in Ivy)
        expect(TokenKind::LParen, "expected '(' for lambda parameter list");
        lam.params = parseParams();
        expect(TokenKind::RParen, "expected ')' to close lambda parameter list");
        // Parse optional trailing return type: `-> ret`
        if (at(TokenKind::Arrow)) {
            next();  // consume '->'
            lam.returnType = parseType();
            if (lam.returnType.base.empty()) {
                errorAt(peek(), "expected return type after '->' in lambda");
            }
        }
        // Parse body — parseCompound() expects and consumes '{' itself.
        if (at(TokenKind::LBrace)) {
            auto bodyStmt = parseCompound();
            if (bodyStmt) {
                lam.body = std::move(bodyStmt);
            }
        } else {
            errorAt(peek(), "expected '{' for lambda body");
        }
        return out;
    }
    if (at(TokenKind::LParen)) {
        next();
        std::unique_ptr<Expr> inner = parseExpr();
        expect(TokenKind::RParen, "expected ')' to close parenthesized expression");
        return inner;
    }

    // Unsupported constructs in expression position.
    if (t.kind == TokenKind::Keyword) {
        for (const auto& [kw, msg] : kUnsupported) {
            if (t.lexeme == kw && !msg.empty()) {
                errorAt(t, std::string(msg));
                next();
                return makeExpr<Expr::NullptrLit>(loc);  // placeholder, recovery
            }
        }
        if (t.lexeme == "this") {
            errorAt(t, "'this' is only available in classes, which are not supported");
            next();
            return makeExpr<Expr::NullptrLit>(loc);
        }
    }

    errorAt(t, "unexpected token in expression");
    next();
    return makeExpr<Expr::NullptrLit>(loc);
}

std::unique_ptr<Expr> Parser::parseNew(SourceLoc loc) {
    if (!inUnsafe_) {
        errorAt(peek(), "'new' requires an [[ivy::unsafe]] block in the Ivy subset");
    }
    next();  // new
    Type type = parseType();
    std::vector<std::unique_ptr<Expr>> args;
    if (at(TokenKind::LParen)) {
        next();
        while (!at(TokenKind::RParen) && !at(TokenKind::EndOfFile)) {
            args.push_back(parseExpr());
            if (!at(TokenKind::RParen)) {
                expect(TokenKind::Comma, "expected ',' between new arguments");
                if (at(TokenKind::RParen)) break;
            }
        }
        expect(TokenKind::RParen, "expected ')' after new arguments");
    }
    return makeExpr<Expr::New>(loc, std::move(type), std::move(args));
}

std::unique_ptr<Expr> Parser::parseDelete(SourceLoc loc) {
    if (!inUnsafe_) {
        errorAt(peek(), "'delete' requires an [[ivy::unsafe]] block in the Ivy subset");
    }
    next();  // delete
    bool isArray = false;
    if (at(TokenKind::LBracket)) {
        next();
        expect(TokenKind::RBracket, "expected ']' after 'delete ['");
        isArray = true;
    }
    std::unique_ptr<Expr> operand = parseUnary();
    return makeExpr<Expr::Delete>(loc, std::move(operand), isArray);
}

long long Parser::parseIntegerValue(std::string_view lexeme, const Token& tok) {
    std::string text(lexeme);
    text.erase(std::remove(text.begin(), text.end(), '\''), text.end());  // digit separators
    while (!text.empty() && (text.back() == 'u' || text.back() == 'U' || text.back() == 'l' ||
                             text.back() == 'L')) {
        text.pop_back();
    }
    if (text.empty()) {
        errorAt(tok, "invalid integer literal");
        return 0;
    }
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text = text.substr(2);
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        text = text.substr(2);
    } else if (text.size() > 1 && text[0] == '0') {
        base = 8;
    }
    try {
        std::size_t consumed = 0;
        const long long value = std::stoll(text, &consumed, base);
        if (consumed != text.size()) {
            errorAt(tok, "invalid integer literal");
            return 0;
        }
        return value;
    } catch (const std::exception&) {
        errorAt(tok, "integer literal is out of range");
        return 0;
    }
}

double Parser::parseFloatValue(std::string_view lexeme, const Token& tok) {
    std::string text(lexeme);
    text.erase(std::remove(text.begin(), text.end(), '\''), text.end());
    while (!text.empty() && (text.back() == 'f' || text.back() == 'F' || text.back() == 'l' ||
                             text.back() == 'L')) {
        text.pop_back();
    }
    try {
        return std::stod(text);
    } catch (const std::exception&) {
        errorAt(tok, "invalid floating-point literal");
        return 0.0;
    }
}

// --- driver ---

std::unique_ptr<TranslationUnit> Parser::parse() {
    auto tu = std::make_unique<TranslationUnit>();
    parseTopLevel(*tu);
    if (failed_) return nullptr;
    return tu;
}

}  // namespace ivy
