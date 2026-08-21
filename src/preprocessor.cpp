#include "preprocessor.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include "lexer.h"

namespace ivy {

namespace {

// Reads a file into a std::string. Returns empty + sets *ok=false on failure.
std::string readFile(const std::filesystem::path& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *ok = false;
        return {};
    }
    *ok = true;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Strips the surrounding quotes/angle brackets from a string-like token.
// `lexeme` is the raw lexer slice, e.g. `"header.h"` or `<header.h>`.
// Returns the inner content and sets *isAngle true if the delimiters were <>.
std::string_view stripIncludeQuotes(std::string_view lexeme, bool* isAngle) {
    if (lexeme.size() < 2) return {};
    const char first = lexeme.front();
    const char last = lexeme.back();
    if (first == '<' && last == '>') {
        *isAngle = true;
        return lexeme.substr(1, lexeme.size() - 2);
    }
    if (first == '"' && last == '"') {
        *isAngle = false;
        return lexeme.substr(1, lexeme.size() - 2);
    }
    return {};
}

bool atEnd(const std::vector<Token>& tokens, std::size_t pos) {
    return pos >= tokens.size() || tokens[pos].kind == TokenKind::EndOfFile;
}

// Parses a preprocessor integer literal (decimal, hex 0x..., octal 0...,
// binary 0b...) with optional suffix (u/U, l/L, ul/UL, ll/LL, etc. —
// suffix is ignored, value is always stored as `long long`).
// Returns true on success; `out` receives the value.
bool parseIntLiteral(std::string_view lex, long long* out) {
    if (lex.empty()) return false;
    std::size_t i = 0;
    int base = 10;
    if (lex.size() >= 2 && lex[0] == '0' && (lex[1] == 'x' || lex[1] == 'X')) {
        base = 16; i = 2;
    } else if (lex.size() >= 2 && lex[0] == '0' && (lex[1] == 'b' || lex[1] == 'B')) {
        base = 2; i = 2;
    } else if (lex.size() >= 2 && lex[0] == '0' && lex[1] != '.') {
        base = 8; i = 1;
    }
    // Find the end of the digit run (strip any suffix u/U/l/L).
    std::size_t end = i;
    while (end < lex.size()) {
        const char c = lex[end];
        const bool digit =
            (base == 10 && c >= '0' && c <= '9') ||
            (base == 16 && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) ||
            (base == 8  && c >= '0' && c <= '7') ||
            (base == 2  && (c == '0' || c == '1'));
        if (!digit) break;
        ++end;
    }
    if (end == i) {
        // No digits parsed — maybe a plain `0` with suffix like `0L`.
        if (lex[0] == '0' && lex.size() > 1) { *out = 0; return true; }
        return false;
    }
    const std::string digits(lex.substr(i, end - i));
    errno = 0;
    char* endp = nullptr;
    const long long v = std::strtoll(digits.c_str(), &endp, base);
    if (endp == digits.c_str() || *endp != '\0') return false;
    *out = v;
    return true;
}

}  // namespace

bool Preprocessor::active() const {
    // The current context emits tokens iff every frame on the conditional
    // stack is `taken`. (parentActive is folded into `taken` at push time.)
    for (const CondFrame& f : condStack_) {
        if (!f.taken) return false;
    }
    return true;
}

bool Preprocessor::isDefined(std::string_view name) const {
    if (isPredefined(name)) return true;
    return macros_.find(std::string(name)) != macros_.end();
}

bool Preprocessor::isPredefined(std::string_view name) const {
    static const std::unordered_map<std::string, bool> kPredefined = {
        {"__LINE__", true}, {"__FILE__", true}, {"__DATE__", true},
        {"__TIME__", true}, {"__cplusplus", true},
    };
    return kPredefined.find(std::string(name)) != kPredefined.end();
}

void Preprocessor::initPredefinedMacros() {
    predefinedMacros_.clear();
    // Capture the date and time at the start of preprocessing (C++ rule:
    // they are fixed for the entire run).
    std::time_t now = std::time(nullptr);
    std::tm* tm = std::localtime(&now);
    if (tm) {
        char dateBuf[16] = {};
        char timeBuf[16] = {};
        std::strftime(dateBuf, sizeof(dateBuf), "%b %d %Y", tm);
        std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", tm);
        dateStr_ = dateBuf;
        timeStr_ = timeBuf;
    } else {
        dateStr_ = "??? ?? ????";
        timeStr_ = "??:??:??";
    }

    // Register `__cplusplus` as an object-like macro with body `202302L`
    // (C++23 value). The body token is backed by a stable buffer.
    {
        Macro m;
        m.isFunctionLike = false;
        // Use a buffer in `buffers_` for the lexeme.
        buffers_.push_back("202302L");
        m.body.push_back(Token{TokenKind::Integer, buffers_.back(), 0, 0});
        predefinedMacros_["__cplusplus"] = m;
        // Also insert into `macros_` so `defined(__cplusplus)` and #ifdef
        // work without special-casing.
        macros_["__cplusplus"] = m;
    }
    // Register `__DATE__` as an object-like macro with body `"Mmm dd yyyy"`.
    {
        Macro m;
        m.isFunctionLike = false;
        buffers_.push_back("\"" + dateStr_ + "\"");
        m.body.push_back(Token{TokenKind::String, buffers_.back(), 0, 0});
        predefinedMacros_["__DATE__"] = m;
        macros_["__DATE__"] = m;
    }
    // Register `__TIME__` as an object-like macro with body `"HH:MM:SS"`.
    {
        Macro m;
        m.isFunctionLike = false;
        buffers_.push_back("\"" + timeStr_ + "\"");
        m.body.push_back(Token{TokenKind::String, buffers_.back(), 0, 0});
        predefinedMacros_["__TIME__"] = m;
        macros_["__TIME__"] = m;
    }
    // `__LINE__` and `__FILE__` are context-sensitive — not stored in
    // `macros_`; they are expanded specially in `tryExpandPredefined`.
    // But we register them in `predefinedMacros_` (with empty body) so
    // `isPredefined()` and `defined(__LINE__)` work.
    {
        Macro m;
        m.isFunctionLike = false;
        predefinedMacros_["__LINE__"] = m;
        predefinedMacros_["__FILE__"] = m;
    }
}

bool Preprocessor::tryExpandPredefined(const Token& tok) {
    if (tok.kind != TokenKind::Identifier) return false;
    const std::string_view name = tok.lexeme;
    if (name == "__LINE__") {
        // Expand to an integer literal of the current token's line.
        buffers_.push_back(std::to_string(tok.line));
        output_.push_back(Token{TokenKind::Integer, buffers_.back(), tok.line, tok.col});
        return true;
    }
    if (name == "__FILE__") {
        // Expand to a string literal of the current file path.
        buffers_.push_back("\"" + mainFile_.string() + "\"");
        output_.push_back(Token{TokenKind::String, buffers_.back(), tok.line, tok.col});
        return true;
    }
    // `__DATE__`, `__TIME__`, `__cplusplus` are in `macros_` (inserted by
    // `initPredefinedMacros`), so they expand through the normal object-like
    // path. Return false here to let the caller handle them.
    return false;
}

void Preprocessor::skipLine(const std::vector<Token>& tokens, std::size_t& pos) {
    if (atEnd(tokens, pos)) return;
    const std::uint32_t line = tokens[pos].line;
    while (!atEnd(tokens, pos) && tokens[pos].line == line) ++pos;
}

Preprocessor::Preprocessor(std::vector<Token> tokens,
                           std::filesystem::path mainFile,
                           std::vector<std::filesystem::path> includePaths)
    : input_(std::move(tokens)),
      mainFile_(std::move(mainFile)),
      includePaths_(std::move(includePaths)) {}

std::filesystem::path Preprocessor::resolveQuoted(std::string_view target) const {
    // #include "..." : directory of the including file first, then -I paths.
    if (!mainFile_.empty()) {
        const std::filesystem::path base = mainFile_.parent_path();
        std::error_code ec;
        std::filesystem::path candidate = base / target;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    for (const std::filesystem::path& dir : includePaths_) {
        std::error_code ec;
        std::filesystem::path candidate = dir / target;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}

std::filesystem::path Preprocessor::resolveAngle(std::string_view target) const {
    // #include <...> : -I paths only.
    for (const std::filesystem::path& dir : includePaths_) {
        std::error_code ec;
        std::filesystem::path candidate = dir / target;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}

bool Preprocessor::parseInclude(const std::vector<Token>& tokens, std::size_t& pos,
                               std::filesystem::path& resolved) {
    // pos points at the `#` token.
    const Token& hashTok = tokens[pos];
    if (pos + 1 >= tokens.size() ||
        tokens[pos + 1].kind != TokenKind::Identifier ||
        tokens[pos + 1].lexeme != "include") {
        return false;  // not a `#include` directive; leave pos at `#`.
    }
    std::size_t p = pos + 2;  // past `# include`

    if (atEnd(tokens, p)) {
        diagnostics_.push_back({hashTok.line, hashTok.col,
                                "#include expects \"file\" or <file>"});
        pos = p;
        return false;
    }
    const Token& hdr = tokens[p];
    if (hdr.kind == TokenKind::String) {
        bool isAngle = false;
        const std::string_view inner = stripIncludeQuotes(hdr.lexeme, &isAngle);
        if (inner.empty()) {
            diagnostics_.push_back({hdr.line, hdr.col, "malformed #include"});
            pos = p + 1;
            return false;
        }
        pos = p + 1;
        resolved = resolveQuoted(inner);
        if (resolved.empty()) {
            diagnostics_.push_back({hdr.line, hdr.col,
                                    "include file not found: " + std::string(inner)});
            return false;
        }
        return true;
    }
    if (hdr.kind == TokenKind::Lt) {
        // Reassemble the angle-bracketed header name: <a/b/c.h>. The lexer
        // split it into tokens; collect until matching `>`.
        ++p;
        std::string acc;
        while (!atEnd(tokens, p) && tokens[p].kind != TokenKind::Gt) {
            acc.append(tokens[p].lexeme);
            ++p;
        }
        if (atEnd(tokens, p)) {
            diagnostics_.push_back({hdr.line, hdr.col, "unterminated #include <...>"});
            pos = p;
            return false;
        }
        ++p;  // consume `>`
        // Skip any trailing tokens on the same line (valid C++ has none).
        while (!atEnd(tokens, p) && tokens[p].line == hashTok.line) ++p;
        pos = p;
        resolved = resolveAngle(acc);
        if (resolved.empty()) {
            diagnostics_.push_back({hdr.line, hdr.col,
                                    "include file not found: " + acc});
            return false;
        }
        return true;
    }
    diagnostics_.push_back({hdr.line, hdr.col, "#include expects \"file\" or <file>"});
    pos = p + 1;
    return false;
}

bool Preprocessor::parseDefine(const std::vector<Token>& tokens, std::size_t& pos) {
    // pos points at the `#` token.
    const Token& hashTok = tokens[pos];
    if (pos + 1 >= tokens.size() ||
        tokens[pos + 1].kind != TokenKind::Identifier ||
        tokens[pos + 1].lexeme != "define") {
        return false;  // not a `#define` directive; leave pos at `#`.
    }
    std::size_t p = pos + 2;  // past `# define`

    // Macro name.
    if (atEnd(tokens, p) || tokens[p].kind != TokenKind::Identifier) {
        diagnostics_.push_back({hashTok.line, hashTok.col,
                                "#define expects a macro name"});
        while (!atEnd(tokens, p) && tokens[p].line == hashTok.line) ++p;
        pos = p;
        return true;
    }
    const Token& nameTok = tokens[p];

    // Predefined macros cannot be redefined or #undef'd by the user.
    if (isPredefined(nameTok.lexeme)) {
        diagnostics_.push_back({nameTok.line, nameTok.col,
                                "cannot redefine predefined macro: " + std::string(nameTok.lexeme)});
        while (!atEnd(tokens, p) && tokens[p].line == hashTok.line) ++p;
        pos = p;
        return true;
    }

    // Function-like macro detection: `#define NAME(args) body`. In C++ the
    // `(` must immediately follow `NAME` with NO whitespace. Since the
    // lexer already dropped trivia, we check column adjacency: the `(` is
    // at col == nameTok.col + nameTok.lexeme.size().
    const bool isFunctionLike =
        p + 1 < tokens.size() && tokens[p + 1].kind == TokenKind::LParen &&
        tokens[p + 1].line == nameTok.line &&
        tokens[p + 1].col == nameTok.col + nameTok.lexeme.size();

    Macro macro;
    macro.isFunctionLike = isFunctionLike;

    if (isFunctionLike) {
        // Parse parameter list: `(` [ident (, ident)*] [,...] `)`.
        // `...` as the last "parameter" marks the macro as variadic
        // (extra args are captured into `__VA_ARGS__`).
        std::size_t q = p + 1;  // at `(`
        ++q;  // consume `(`
        while (!atEnd(tokens, q) && tokens[q].kind != TokenKind::RParen) {
            if (tokens[q].kind == TokenKind::Ellipsis) {
                macro.isVariadic = true;
                ++q;
                // `...` must be the last parameter; skip to `)`.
                break;
            }
            if (tokens[q].kind == TokenKind::Identifier) {
                macro.params.push_back(std::string(tokens[q].lexeme));
                ++q;
                // Check for named-variadic `name...` (GNU extension) —
                // we only support `...` standalone.
                if (!atEnd(tokens, q) && tokens[q].kind == TokenKind::Ellipsis) {
                    macro.isVariadic = true;
                    ++q;
                    break;
                }
                if (!atEnd(tokens, q) && tokens[q].kind == TokenKind::Comma) {
                    ++q;
                    continue;
                }
                // Allow trailing comma? C++ does not. Just continue; the
                // next iteration will hit `)` or error.
                continue;
            }
            if (tokens[q].kind == TokenKind::Comma) {
                // Empty parameter (e.g. `F(,)` ) — C++ disallows; skip.
                ++q;
                continue;
            }
            // Unexpected token in parameter list.
            if (tokens[q].line == hashTok.line) {
                diagnostics_.push_back({tokens[q].line, tokens[q].col,
                                        "invalid token in macro parameter list"});
            }
            ++q;
        }
        if (atEnd(tokens, q) || tokens[q].kind != TokenKind::RParen) {
            diagnostics_.push_back({nameTok.line, nameTok.col,
                                    "unterminated macro parameter list"});
            while (!atEnd(tokens, p) && tokens[p].line == hashTok.line) ++p;
            pos = p;
            return true;
        }
        ++q;  // consume `)`
        p = q;  // p now points at the first body token (next line or same line)
    } else {
        ++p;  // consume the name (object-like)
    }

    // Collect body tokens until end of the directive line.
    while (!atEnd(tokens, p) && tokens[p].line == hashTok.line) {
        macro.body.push_back(tokens[p]);
        ++p;
    }
    pos = p;

    macros_[std::string(nameTok.lexeme)] = std::move(macro);
    return true;
}

bool Preprocessor::parseConditional(const std::vector<Token>& tokens, std::size_t& pos) {
    // pos points at the `#` token.
    const Token& hashTok = tokens[pos];
    // Directive names may be lexed as either Identifier (`ifdef`, `ifndef`,
    // `undef`) or Keyword (`if`, `else`, `elif`) — accept either.
    if (pos + 1 >= tokens.size()) return false;
    const Token& dirTok = tokens[pos + 1];
    if (dirTok.kind != TokenKind::Identifier && dirTok.kind != TokenKind::Keyword) {
        return false;
    }
    const std::string_view directive = dirTok.lexeme;
    const bool isIfdef  = directive == "ifdef";
    const bool isIfndef = directive == "ifndef";
    const bool isIf     = directive == "if";
    const bool isElif   = directive == "elif";
    const bool isElse   = directive == "else";
    const bool isEndif  = directive == "endif";
    const bool isUndef  = directive == "undef";
    if (!isIfdef && !isIfndef && !isIf && !isElif && !isElse && !isEndif && !isUndef) {
        return false;
    }

    // `#undef NAME` — remove a macro. Only effective when active.
    if (isUndef) {
        std::size_t p = pos + 2;
        if (active() && !atEnd(tokens, p) && tokens[p].kind == TokenKind::Identifier) {
            if (isPredefined(tokens[p].lexeme)) {
                diagnostics_.push_back({tokens[p].line, tokens[p].col,
                                        "cannot #undef predefined macro: " + std::string(tokens[p].lexeme)});
            } else {
                macros_.erase(std::string(tokens[p].lexeme));
            }
        }
        skipLine(tokens, pos);
        return true;
    }

    // `#if EXPR` / `#elif EXPR` — evaluate the constant expression.
    // C++ rule: `#elif` is only considered when no prior branch of the
    // current frame was taken (anyTaken). When parent is skipping, we
    // still need to push/update a frame for nesting correctness but the
    // branch is never taken.
    if (isIf || isElif) {
        // Collect the expression tokens (rest of the directive line).
        std::size_t p = pos + 2;
        std::vector<Token> exprTokens;
        while (!atEnd(tokens, p) && tokens[p].line == hashTok.line) {
            exprTokens.push_back(tokens[p]);
            ++p;
        }
        const bool parentActive = active();

        if (isIf) {
            bool cond = false;
            if (parentActive) {
                const long long v = evalConstExpr(exprTokens, hashTok.line, hashTok.col);
                cond = (v != 0);
            }
            condStack_.push_back(CondFrame{parentActive && cond, false, parentActive, parentActive && cond});
        } else {
            // #elif: update the existing top frame.
            if (condStack_.empty()) {
                diagnostics_.push_back({hashTok.line, hashTok.col, "#elif without #if"});
            } else {
                CondFrame& f = condStack_.back();
                if (f.seenElse) {
                    diagnostics_.push_back({hashTok.line, hashTok.col, "#elif after #else"});
                } else if (!f.parentActive) {
                    // Parent is skipping — this frame stays inactive.
                    f.taken = false;
                } else if (f.anyTaken) {
                    // A prior branch was taken — this #elif is skipped.
                    f.taken = false;
                } else {
                    // Evaluate the expression.
                    const long long v = evalConstExpr(exprTokens, hashTok.line, hashTok.col);
                    f.taken = (v != 0);
                    if (f.taken) f.anyTaken = true;
                }
            }
        }
        pos = p;
        return true;
    }

    if (isIfdef || isIfndef) {
        // Parse the macro name.
        std::size_t p = pos + 2;
        bool cond = false;
        const bool parentActive = active();
        if (!atEnd(tokens, p) && tokens[p].kind == TokenKind::Identifier) {
            const bool defined = isDefined(tokens[p].lexeme);
            cond = isIfdef ? defined : !defined;
        } else {
            diagnostics_.push_back({hashTok.line, hashTok.col,
                                    std::string("#") + std::string(directive) +
                                    " expects a macro name"});
        }
        skipLine(tokens, pos);
        // Push frame. If the parent is skipping, this frame skips too
        // (cond is moot). We record parentActive for #else flipping.
        const bool taken = parentActive && cond;
        condStack_.push_back(CondFrame{taken, false, parentActive, taken});
        return true;
    }

    if (isElse) {
        skipLine(tokens, pos);
        if (condStack_.empty()) {
            diagnostics_.push_back({hashTok.line, hashTok.col, "#else without #if"});
            return true;
        }
        CondFrame& f = condStack_.back();
        if (f.seenElse) {
            diagnostics_.push_back({hashTok.line, hashTok.col, "duplicate #else"});
        }
        f.seenElse = true;
        // Flip the branch — but only if the parent was active and no
        // prior branch was taken. If the parent was skipping, this frame
        // stays skipped.
        if (f.parentActive && !f.anyTaken) {
            f.taken = true;
            f.anyTaken = true;
        } else {
            f.taken = false;
        }
        return true;
    }

    if (isEndif) {
        skipLine(tokens, pos);
        if (condStack_.empty()) {
            diagnostics_.push_back({hashTok.line, hashTok.col, "#endif without #if"});
            return true;
        }
        condStack_.pop_back();
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// #pragma directive
// ---------------------------------------------------------------------------
bool Preprocessor::parsePragma(const std::vector<Token>& tokens,
                                std::size_t& pos) {
    const Token& hashTok = tokens[pos];
    if (pos + 1 >= tokens.size()) return false;
    const Token& dirTok = tokens[pos + 1];
    if (dirTok.kind != TokenKind::Identifier && dirTok.kind != TokenKind::Keyword) {
        return false;
    }
    if (dirTok.lexeme != "pragma") return false;

    // Collect the rest of the pragma line as text for inspection.
    std::size_t p = pos + 2;
    std::vector<std::string> args;
    while (!atEnd(tokens, p) && tokens[p].line == hashTok.line) {
        args.emplace_back(tokens[p].lexeme);
        ++p;
    }

    // Only recognized pragma: `#pragma ivy cnumber`
    if (args.size() >= 2 && args[0] == "ivy" && args[1] == "cnumber") {
        // Pragmas in inactive (skipped) conditional blocks have no effect.
        if (active()) {
            cnumberEnabled_ = true;
        }
    } else {
        // Unknown pragma — warn and drop (don't pass to parser).
        const std::string first = args.empty() ? "" : args[0];
        diagnostics_.push_back({hashTok.line, hashTok.col,
                                "unknown #pragma: " + first});
    }

    pos = p;
    return true;
}

// ---------------------------------------------------------------------------
// #if / #elif constant-expression evaluation
// ---------------------------------------------------------------------------
namespace {

// A small recursive-descent parser over a token slice, evaluating a C++
// integral constant expression. All arithmetic is done in `long long`.
// `PP` provides `isDefined()` and `macros_` for macro expansion; we pass
// a pointer to the Preprocessor to reach those.
struct ExprParser {
    const std::vector<Token>& toks;
    std::size_t pos = 0;
    Preprocessor* pp;
    std::vector<Diagnostic>* diags;
    std::uint32_t line = 0;
    std::uint32_t col = 0;

    bool atEnd() const {
        return pos >= toks.size() || toks[pos].kind == TokenKind::EndOfFile;
    }
    const Token& peek() const { return toks[pos]; }
    bool check(TokenKind k) const { return !atEnd() && toks[pos].kind == k; }
    bool consume(TokenKind k) {
        if (!check(k)) return false;
        ++pos; return true;
    }
    void error(const std::string& msg) {
        diags->push_back({line, col, "#if expression: " + msg});
    }

    long long parseExpr() {
        return parseTernary();
    }

    long long parseTernary() {
        long long cond = parseLogicalOr();
        if (consume(TokenKind::Question)) {
            long long thenVal = parseTernary();
            if (!consume(TokenKind::Colon)) {
                error("expected ':' in ternary");
                return 0;
            }
            long long elseVal = parseTernary();
            return cond ? thenVal : elseVal;
        }
        return cond;
    }

    long long parseLogicalOr() {
        long long v = parseLogicalAnd();
        while (check(TokenKind::OrOr)) {
            ++pos;
            long long rhs = parseLogicalAnd();
            v = (v || rhs) ? 1 : 0;
        }
        return v;
    }

    long long parseLogicalAnd() {
        long long v = parseBitOr();
        while (check(TokenKind::AndAnd)) {
            ++pos;
            long long rhs = parseBitOr();
            v = (v && rhs) ? 1 : 0;
        }
        return v;
    }

    long long parseBitOr() {
        long long v = parseBitXor();
        while (check(TokenKind::Pipe)) {
            ++pos;
            long long rhs = parseBitXor();
            v |= rhs;
        }
        return v;
    }

    long long parseBitXor() {
        long long v = parseBitAnd();
        while (check(TokenKind::Caret)) {
            ++pos;
            long long rhs = parseBitAnd();
            v ^= rhs;
        }
        return v;
    }

    long long parseBitAnd() {
        long long v = parseEquality();
        while (check(TokenKind::Amp)) {
            ++pos;
            long long rhs = parseEquality();
            v &= rhs;
        }
        return v;
    }

    long long parseEquality() {
        long long v = parseRelational();
        while (check(TokenKind::Eq) || check(TokenKind::Ne)) {
            const TokenKind op = toks[pos].kind;
            ++pos;
            long long rhs = parseRelational();
            if (op == TokenKind::Eq) v = (v == rhs) ? 1 : 0;
            else v = (v != rhs) ? 1 : 0;
        }
        return v;
    }

    long long parseRelational() {
        long long v = parseShift();
        while (check(TokenKind::Lt) || check(TokenKind::Le) ||
               check(TokenKind::Gt) || check(TokenKind::Ge)) {
            const TokenKind op = toks[pos].kind;
            ++pos;
            long long rhs = parseShift();
            switch (op) {
                case TokenKind::Lt: v = (v <  rhs) ? 1 : 0; break;
                case TokenKind::Le: v = (v <= rhs) ? 1 : 0; break;
                case TokenKind::Gt: v = (v >  rhs) ? 1 : 0; break;
                case TokenKind::Ge: v = (v >= rhs) ? 1 : 0; break;
                default: break;
            }
        }
        return v;
    }

    long long parseShift() {
        long long v = parseAdditive();
        while (check(TokenKind::Shl) || check(TokenKind::Shr)) {
            const TokenKind op = toks[pos].kind;
            ++pos;
            long long rhs = parseAdditive();
            if (op == TokenKind::Shl) v <<= rhs;
            else v >>= rhs;
        }
        return v;
    }

    long long parseAdditive() {
        long long v = parseMultiplicative();
        while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
            const TokenKind op = toks[pos].kind;
            ++pos;
            long long rhs = parseMultiplicative();
            if (op == TokenKind::Plus) v += rhs;
            else v -= rhs;
        }
        return v;
    }

    long long parseMultiplicative() {
        long long v = parseUnary();
        while (check(TokenKind::Star) || check(TokenKind::Slash) || check(TokenKind::Percent)) {
            const TokenKind op = toks[pos].kind;
            ++pos;
            long long rhs = parseUnary();
            if (op == TokenKind::Star) {
                v *= rhs;
            } else if (op == TokenKind::Slash) {
                if (rhs == 0) { error("division by zero in #if"); v = 0; }
                else v /= rhs;
            } else {
                if (rhs == 0) { error("modulo by zero in #if"); v = 0; }
                else v %= rhs;
            }
        }
        return v;
    }

    long long parseUnary() {
        if (check(TokenKind::Bang))  { ++pos; return !parseUnary(); }
        if (check(TokenKind::Minus)) { ++pos; return -parseUnary(); }
        if (check(TokenKind::Plus))  { ++pos; return  parseUnary(); }
        if (check(TokenKind::Tilde)) { ++pos; return ~parseUnary(); }
        return parsePrimary();
    }

    long long parsePrimary() {
        if (consume(TokenKind::LParen)) {
            long long v = parseExpr();
            if (!consume(TokenKind::RParen)) {
                error("expected ')' in #if expression");
                return 0;
            }
            return v;
        }
        if (atEnd()) { error("unexpected end of #if expression"); return 0; }
        const Token& t = toks[pos];
        if (t.kind == TokenKind::Integer) {
            long long v = 0;
            if (!parseIntLiteral(t.lexeme, &v)) {
                error("invalid integer literal: " + std::string(t.lexeme));
            }
            ++pos;
            return v;
        }
        // A single '1' is sometimes substituted by `defined(X)` expansion.
        // Here, we just report unknown token.
        error("unexpected token in #if expression: '" + std::string(t.lexeme) + "'");
        ++pos;
        return 0;
    }
};

}  // namespace

long long Preprocessor::evalConstExpr(const std::vector<Token>& tokens,
                                      std::uint32_t line, std::uint32_t col) {
    // Step 1: Replace `defined NAME` / `defined(NAME)` with 1/0.
    // This happens BEFORE macro expansion so that the NAME inside
    // `defined()` is not expanded (C++ rule).
    std::vector<Token> afterDefined;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        const bool isDefinedTok =
            (t.kind == TokenKind::Identifier || t.kind == TokenKind::Keyword) &&
            t.lexeme == "defined";
        if (!isDefinedTok) {
            afterDefined.push_back(t);
            continue;
        }
        // `defined NAME` or `defined ( NAME )`.
        std::size_t j = i + 1;
        bool haveParen = false;
        if (j < tokens.size() && tokens[j].kind == TokenKind::LParen) {
            haveParen = true;
            ++j;
        }
        if (j >= tokens.size() ||
            (tokens[j].kind != TokenKind::Identifier && tokens[j].kind != TokenKind::Keyword)) {
            diagnostics_.push_back({line, col, "#if: 'defined' expects a macro name"});
            continue;
        }
        const bool defined = isDefined(tokens[j].lexeme);
        ++j;  // advance past NAME (in both paren and no-paren cases)
        if (haveParen) {
            if (j >= tokens.size() || tokens[j].kind != TokenKind::RParen) {
                diagnostics_.push_back({line, col, "#if: 'defined(NAME' missing ')'"});
            } else {
                ++j;  // consume ')'
            }
        }
        // Emit a literal 1/0 token.
        afterDefined.push_back(Token{TokenKind::Integer, defined ? "1" : "0", t.line, t.col});
        i = j - 1;  // loop's ++i advances past consumed tokens
        continue;
    }

    // Step 2: Macro-expand the remaining tokens. Any identifier that is a
    // macro is replaced by its body (object-like only; function-like needs
    // arg consumption which we approximate by expansion of object-like).
    // Undefined identifiers become `0` (C++ rule). We reuse `expandTokenVector`
    // — but that writes into `output_`. Instead we do a simplified in-place
    // expansion: object-like only, single level (chained).
    std::vector<Token> expanded;
    for (std::size_t i = 0; i < afterDefined.size(); ++i) {
        const Token& t = afterDefined[i];
        if (t.kind == TokenKind::Identifier) {
            const auto it = macros_.find(std::string(t.lexeme));
            if (it != macros_.end() && !it->second.isFunctionLike) {
                // Substitute body tokens. Recursively expand chains.
                std::vector<Token> body = it->second.body;
                // Simple chain expansion: walk body, replace identifiers.
                for (std::size_t k = 0; k < body.size() && k < 64; ++k) {
                    if (body[k].kind == TokenKind::Identifier) {
                        const auto it2 = macros_.find(std::string(body[k].lexeme));
                        if (it2 != macros_.end() && !it2->second.isFunctionLike &&
                            std::find(expansionStack_.begin(), expansionStack_.end(),
                                      std::string(body[k].lexeme)) == expansionStack_.end()) {
                            // Replace in place.
                            std::vector<Token> sub = it2->second.body;
                            body.erase(body.begin() + k);
                            body.insert(body.begin() + k, sub.begin(), sub.end());
                            k += sub.size();
                        }
                    }
                }
                expanded.insert(expanded.end(), body.begin(), body.end());
                continue;
            }
            // Undefined identifier — C++ rule: replace with 0.
            expanded.push_back(Token{TokenKind::Integer, "0", t.line, t.col});
            continue;
        }
        // Keywords (true/false are C++ keywords — but in #if, `true`→1,
        // `false`→0 per C++). Other keywords are unexpected here.
        if (t.kind == TokenKind::Keyword) {
            if (t.lexeme == "true") {
                expanded.push_back(Token{TokenKind::Integer, "1", t.line, t.col});
                continue;
            }
            if (t.lexeme == "false") {
                expanded.push_back(Token{TokenKind::Integer, "0", t.line, t.col});
                continue;
            }
            // Unknown keyword — treat as 0 (undefined identifier).
            expanded.push_back(Token{TokenKind::Integer, "0", t.line, t.col});
            continue;
        }
        expanded.push_back(t);
    }

    // Step 3: Parse and evaluate.
    ExprParser p{expanded, 0, this, &diagnostics_, line, col};
    const long long result = p.parseExpr();
    if (!p.atEnd()) {
        diagnostics_.push_back({line, col, "#if: trailing tokens in expression"});
    }
    return result;
}

void Preprocessor::expandObjectLike(const Token& tok, const Macro& macro) {
    // Cycle guard: if this macro is already being expanded, emit the token
    // verbatim (so `#define A A + 1` doesn't loop). C++ paints the macro
    // blue for the rest of its expansion; this is the conservative match.
    if (std::find(expansionStack_.begin(), expansionStack_.end(),
                  std::string(tok.lexeme)) != expansionStack_.end()) {
        output_.push_back(tok);
        return;
    }
    if (expansionStack_.size() >= kMaxExpansionDepth) {
        diagnostics_.push_back({tok.line, tok.col,
                                "macro expansion too deep: " + std::string(tok.lexeme)});
        output_.push_back(tok);
        return;
    }
    expansionStack_.push_back(std::string(tok.lexeme));
    // Expand the body as a token vector so that function-like macros inside
    // the body can consume their arguments from the body tokens.
    expandTokenVector(macro.body);
    expansionStack_.pop_back();
}

void Preprocessor::expandFunctionLike(const std::vector<Token>& tokens,
                                       std::size_t& pos,
                                       const Token& nameTok, const Macro& macro) {
    // A function-like macro is only invoked if the next token is `(`.
    // Otherwise the identifier is emitted verbatim (C++ rule).
    const std::size_t next = pos + 1;
    if (next >= tokens.size() || tokens[next].kind != TokenKind::LParen) {
        output_.push_back(nameTok);
        ++pos;
        return;
    }

    // Parse the actual arguments: balanced parens, comma-separated.
    std::vector<std::vector<Token>> args;
    std::size_t p = next + 1;  // past `(`
    std::vector<Token> current;
    int depth = 1;
    while (!atEnd(tokens, p) && depth > 0) {
        const Token& t = tokens[p];
        if (t.kind == TokenKind::LParen) {
            ++depth;
            current.push_back(t);
            ++p;
            continue;
        }
        if (t.kind == TokenKind::RParen) {
            --depth;
            if (depth == 0) {
                // End of argument list. Flush the last argument unless it's
                // empty AND there were no other args (the `()` case).
                if (!current.empty() || !args.empty()) {
                    args.push_back(std::move(current));
                    current.clear();
                }
                ++p;  // consume `)`
                break;
            }
            current.push_back(t);
            ++p;
            continue;
        }
        if (t.kind == TokenKind::Comma && depth == 1) {
            args.push_back(std::move(current));
            current.clear();
            ++p;
            continue;
        }
        current.push_back(t);
        ++p;
    }
    if (depth != 0) {
        diagnostics_.push_back({nameTok.line, nameTok.col,
                                "unterminated argument list invoking " + std::string(nameTok.lexeme)});
        pos = p;
        return;
    }

    // Arity check:
    //   - Fixed-arity: require `args.size() == params.size()`.
    //   - Variadic: require `args.size() >= params.size()` (extra args
    //     go into `__VA_ARGS__`).
    //   - Special case: `()` calls a zero-named-param macro → 0 args.
    //     A variadic macro called with no named params and no extra
    //     args → `__VA_ARGS__` is empty (allowed by C++20).
    if (macro.isVariadic) {
        if (args.size() < macro.params.size()) {
            diagnostics_.push_back({nameTok.line, nameTok.col,
                                    "variadic macro " + std::string(nameTok.lexeme) +
                                    " expects at least " + std::to_string(macro.params.size()) +
                                    " argument(s), got " + std::to_string(args.size())});
            pos = p;
            return;
        }
    } else if (args.size() != macro.params.size()) {
        diagnostics_.push_back({nameTok.line, nameTok.col,
                                "macro " + std::string(nameTok.lexeme) +
                                " expects " + std::to_string(macro.params.size()) +
                                " argument(s), got " + std::to_string(args.size())});
        pos = p;
        return;
    }

    // Build a name→tokens map for substitution.
    std::unordered_map<std::string, const std::vector<Token>*> paramMap;
    for (std::size_t i = 0; i < macro.params.size(); ++i) {
        paramMap[macro.params[i]] = &args[i];
    }
    // For variadic macros, build `__VA_ARGS__` from the extra arguments
    // (those past the named ones), joined with comma tokens to match
    // the original call-site separators.
    std::vector<Token> vaArgs;
    if (macro.isVariadic) {
        for (std::size_t i = macro.params.size(); i < args.size(); ++i) {
            if (i > macro.params.size()) {
                // Insert a comma separator between variadic args.
                vaArgs.push_back(Token{TokenKind::Comma, ",", nameTok.line, nameTok.col});
            }
            vaArgs.insert(vaArgs.end(), args[i].begin(), args[i].end());
        }
        paramMap["__VA_ARGS__"] = &vaArgs;
    }

    // Cycle + depth guard.
    if (std::find(expansionStack_.begin(), expansionStack_.end(),
                  std::string(nameTok.lexeme)) != expansionStack_.end()) {
        output_.push_back(nameTok);
        pos = p;
        return;
    }
    if (expansionStack_.size() >= kMaxExpansionDepth) {
        diagnostics_.push_back({nameTok.line, nameTok.col,
                                "macro expansion too deep: " + std::string(nameTok.lexeme)});
        output_.push_back(nameTok);
        pos = p;
        return;
    }

    expansionStack_.push_back(std::string(nameTok.lexeme));

    // Build the substituted body: replace formal parameters with the
    // actual-argument token sequences. Argument tokens come from the call
    // site (not the body), so they are NOT painted blue and may themselves
    // expand further macros — that happens when expandTokenVector runs
    // emitToken on each argument token.
    std::vector<Token> substituted;
    for (const Token& bt : macro.body) {
        if (bt.kind == TokenKind::Identifier) {
            const auto it = paramMap.find(std::string(bt.lexeme));
            if (it != paramMap.end()) {
                substituted.insert(substituted.end(), it->second->begin(),
                                   it->second->end());
                continue;
            }
        }
        substituted.push_back(bt);
    }
    expandTokenVector(substituted);
    expansionStack_.pop_back();
    pos = p;
}

void Preprocessor::expandTokenVector(const std::vector<Token>& tokens) {
    for (std::size_t i = 0; i < tokens.size() && tokens[i].kind != TokenKind::EndOfFile; ) {
        if (tokens[i].kind == TokenKind::Identifier) {
            // Predefined context-sensitive macros (__LINE__, __FILE__).
            if (tryExpandPredefined(tokens[i])) {
                ++i;
                continue;
            }
            const auto it = macros_.find(std::string(tokens[i].lexeme));
            if (it != macros_.end()) {
                const Macro& macro = it->second;
                if (macro.isFunctionLike) {
                    expandFunctionLike(tokens, i, tokens[i], macro);
                    continue;
                }
                expandObjectLike(tokens[i], macro);
                ++i;
                continue;
            }
        }
        output_.push_back(tokens[i]);
        ++i;
    }
}

void Preprocessor::emitToken(const Token& tok) {
    // Only identifiers can be macro invocations. Keywords (TokenKind::Keyword)
    // are never replaced, even if a macro has the same name — matching C++.
    if (tok.kind != TokenKind::Identifier) {
        output_.push_back(tok);
        return;
    }
    // Predefined context-sensitive macros (__LINE__, __FILE__).
    if (tryExpandPredefined(tok)) return;
    const auto it = macros_.find(std::string(tok.lexeme));
    if (it == macros_.end()) {
        output_.push_back(tok);
        return;
    }
    const Macro& macro = it->second;
    if (!macro.isFunctionLike) {
        expandObjectLike(tok, macro);
        return;
    }
    // Function-like macro name encountered in a single-token context (no
    // ability to consume following args). Emit verbatim; the caller that
    // has access to the surrounding token vector should use
    // expandTokenVector instead for proper function-like handling.
    output_.push_back(tok);
}

void Preprocessor::processFile(const std::filesystem::path& file) {
    // Cycle + depth guard.
    std::error_code ec;
    const std::filesystem::path canon = std::filesystem::weakly_canonical(file, ec);
    if (std::find(activePaths_.begin(), activePaths_.end(), canon) != activePaths_.end()) {
        diagnostics_.push_back({0, 0, "circular #include: " + canon.string()});
        return;
    }
    if (activePaths_.size() >= kMaxDepth) {
        diagnostics_.push_back({0, 0, "#include nesting too deep"});
        return;
    }

    bool ok = false;
    std::string content = readFile(file, &ok);
    if (!ok) {
        diagnostics_.push_back({0, 0, "cannot open include file: " + file.string()});
        return;
    }
    buffers_.push_back(std::move(content));
    const std::string& buf = buffers_.back();

    Lexer lexer(buf);
    std::vector<Token> local = lexer.tokenize();
    for (const Diagnostic& d : lexer.diagnostics()) {
        diagnostics_.push_back(d);
    }

    activePaths_.push_back(canon);
    const std::filesystem::path savedMain = mainFile_;
    mainFile_ = file;  // so quoted includes resolve relative to this file
    const std::size_t condDepthBefore = condStack_.size();

    for (std::size_t i = 0; i < local.size() && local[i].kind != TokenKind::EndOfFile; ) {
        if (local[i].kind == TokenKind::Hash) {
            const std::size_t before = i;
            std::filesystem::path resolved;
            if (active() && parseInclude(local, i, resolved)) {
                processFile(resolved);
                continue;
            }
            std::size_t after = before;
            if (active() && parseDefine(local, after)) {
                i = after;
                continue;
            }
            // Conditionals are processed even when inactive (so nesting
            // is tracked and the right branch is picked on #else/#endif).
            if (parseConditional(local, after)) {
                i = after;
                continue;
            }
            // Pragmas are processed even when inactive (the flag must
            // persist regardless of conditional state — a pragma in a
            // skipped block has no effect, which is correct C++ behavior).
            if (parsePragma(local, after)) {
                i = after;
                continue;
            }
            // Unrecognized directive. If active, emit `#` + skip line so
            // the parser rejects it; if inactive, just skip.
            i = before;
            if (active()) {
                emitToken(local[i]);
                ++i;
                const std::uint32_t dirLine = local[before].line;
                while (i < local.size() && local[i].kind != TokenKind::EndOfFile &&
                       local[i].line == dirLine) {
                    emitToken(local[i]);
                    ++i;
                }
            } else {
                skipLine(local, i);
            }
            continue;
        }
        // Skip tokens when the conditional stack says we're inactive.
        if (!active()) {
            ++i;
            continue;
        }
        // Identifier: check for macro expansion (object-like + function-like).
        if (local[i].kind == TokenKind::Identifier) {
            // Predefined context-sensitive macros (__LINE__, __FILE__).
            if (tryExpandPredefined(local[i])) {
                ++i;
                continue;
            }
            const auto it = macros_.find(std::string(local[i].lexeme));
            if (it != macros_.end()) {
                const Macro& macro = it->second;
                if (macro.isFunctionLike) {
                    expandFunctionLike(local, i, local[i], macro);
                    continue;
                }
                expandObjectLike(local[i], macro);
                ++i;
                continue;
            }
        }
        emitToken(local[i]);
        ++i;
    }

    activePaths_.pop_back();
    mainFile_ = savedMain;

    // A well-formed file balances its #if/#ifdef blocks. If the include
    // changed the conditional stack depth, warn and restore.
    if (condStack_.size() != condDepthBefore) {
        diagnostics_.push_back({0, 0, "unterminated #ifdef/#if block in " + file.string()});
        condStack_.resize(condDepthBefore);
    }
}

std::vector<Token> Preprocessor::run() {
    output_.clear();
    activePaths_.clear();
    buffers_.clear();
    macros_.clear();
    expansionStack_.clear();
    condStack_.clear();

    // Initialize predefined macros (__cplusplus, __DATE__, __TIME__ are
    // fixed; __LINE__ and __FILE__ are expanded context-sensitively).
    initPredefinedMacros();

    std::error_code ec;
    const std::filesystem::path canon = std::filesystem::weakly_canonical(mainFile_, ec);
    activePaths_.push_back(canon);

    for (std::size_t i = 0; i < input_.size() && input_[i].kind != TokenKind::EndOfFile; ) {
        if (input_[i].kind == TokenKind::Hash) {
            const std::size_t before = i;
            std::filesystem::path resolved;
            if (active() && parseInclude(input_, i, resolved)) {
                processFile(resolved);
                continue;
            }
            std::size_t after = before;
            if (active() && parseDefine(input_, after)) {
                i = after;
                continue;
            }
            if (parseConditional(input_, after)) {
                i = after;
                continue;
            }
            if (parsePragma(input_, after)) {
                i = after;
                continue;
            }
            i = before;
            if (active()) {
                emitToken(input_[i]);
                ++i;
                const std::uint32_t dirLine = input_[before].line;
                while (i < input_.size() && input_[i].kind != TokenKind::EndOfFile &&
                       input_[i].line == dirLine) {
                    emitToken(input_[i]);
                    ++i;
                }
            } else {
                skipLine(input_, i);
            }
            continue;
        }
        if (!active()) {
            ++i;
            continue;
        }
        if (input_[i].kind == TokenKind::Identifier) {
            // Predefined context-sensitive macros (__LINE__, __FILE__).
            if (tryExpandPredefined(input_[i])) {
                ++i;
                continue;
            }
            const auto it = macros_.find(std::string(input_[i].lexeme));
            if (it != macros_.end()) {
                const Macro& macro = it->second;
                if (macro.isFunctionLike) {
                    expandFunctionLike(input_, i, input_[i], macro);
                    continue;
                }
                expandObjectLike(input_[i], macro);
                ++i;
                continue;
            }
        }
        emitToken(input_[i]);
        ++i;
    }

    // Warn about unterminated #if/#ifdef blocks.
    if (!condStack_.empty()) {
        diagnostics_.push_back({0, 0, "unterminated #ifdef/#if block"});
        condStack_.clear();
    }

    output_.push_back(Token{TokenKind::EndOfFile, {}, 0, 0});
    return output_;
}

std::string Preprocessor::reconstructSource() const {
    std::ostringstream os;
    std::uint32_t line = 1;
    bool atLineStart = true;
    for (const Token& t : output_) {
        if (t.kind == TokenKind::EndOfFile) break;
        while (line < t.line) {
            os << '\n';
            ++line;
            atLineStart = true;
        }
        if (!atLineStart) os << ' ';
        os << t.lexeme;
        atLineStart = false;
    }
    os << '\n';
    return os.str();
}

}  // namespace ivy
