#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/diagnostic.h"
#include "parsing/token.h"

namespace ivy {

// C-preprocessor-style expansion for the Ivy subset.
//
// Runs after the lexer and before the parser. Currently supported:
//   - `#include "..."` / `#include <...>` (recursively, with cycle guard)
//   - `#define NAME body...` (object-like macro)
//   - `#define NAME(params) body...` (function-like macro; fixed-arity
//     and variadic — the latter via `...` in the parameter list and
//     `__VA_ARGS__` in the body)
//   - `#ifdef NAME` / `#ifndef NAME` / `#else` / `#endif` (conditional
//     compilation)
//   - `#if EXPR` / `#elif EXPR` where EXPR is a C++ integral constant
//     expression over integer literals, `defined(NAME)`, macro names
//     (expanding to their bodies), and the usual C++ arithmetic, bitwise,
//     logical, comparison, and ternary operators.
//   - `#undef NAME`
//   - Predefined macros: `__LINE__`, `__FILE__`, `__DATE__`, `__TIME__`,
//     `__cplusplus` (always defined; `__LINE__`/`__FILE__` are
//     context-sensitive — they expand to the current token's line / the
//     current file path; `__DATE__`/`__TIME__` are fixed at the start of
//     preprocessing; `__cplusplus` is `202302L` for C++23).
//   - `#pragma ivy cnumber` — opts into C-style number types (`int`,
//     `unsigned`, `long`, `short`, `char`, `float`, `double`,
//     `long long`). Without this pragma, Ivy only accepts its
//     fixed-width types (`int8_t` … `int64_t`, `uint8_t` … `uint64_t`,
//     `float16_t` … `float128_t`, `bfloat16_t`, `size_t`, `ptrdiff_t`,
//     `bool`, `void`). Hex/octal/binary integer literals are always
//     allowed regardless of this pragma.
//
// Output is a flat token stream with directives consumed and macro
// invocations replaced by their expansion (recursively, with cycle guard).
//
// Token line/col are kept from the original lex; there is currently no
// #line marker emission. The `-o file.i` / `-o file.ii` CLI flag dumps the
// reconstructed source text instead of feeding the parser, mirroring
// `g++ -E`.
class Preprocessor {
public:
    // `mainFile` is the path of the file the `tokens` came from (used to
    // resolve relative `#include "..."`). `includePaths` are searched for
    // `#include <...>` (and as a fallback for `"..."`). It may be empty.
    Preprocessor(std::vector<Token> tokens,
                 std::filesystem::path mainFile,
                 std::vector<std::filesystem::path> includePaths);

    // Returns the expanded token stream (with a trailing EOF). Diagnostics
    // are reported via diagnostics(); on error the returned stream is the
    // best-effort prefix.
    std::vector<Token> run();

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // Returns true if `#pragma ivy cnumber` was seen (C-style number
    // types are allowed). Passed to the parser so it can gate C-style
    // types like `int`/`unsigned`/`long`/`short`/`char`/`float`/`double`.
    bool cnumberEnabled() const { return cnumberEnabled_; }

    // 8.6: Returns true if a `#error` directive was encountered.
    // The caller should check this and abort compilation.
    bool hasError() const { return hasError_; }

    // 8.6: Returns the file name override set by `#line N "file"`.
    // Empty when no override is in effect. Used by the caller to
    // report diagnostics with the remapped file name.
    const std::string& lineFile() const { return lineFile_; }

    // Reconstructs source text from the expanded token stream. Used by the
    // `-o file.i` / `-o file.ii` CLI mode. Whitespace between tokens is
    // approximated: a single space between same-line tokens, a newline when
    // the line number advances. String/char literals keep their original
    // lexeme (quotes included).
    std::string reconstructSource() const;

private:
    // A macro definition.
    //   - Object-like: `isFunctionLike == false`, `params` empty.
    //   - Function-like fixed-arity: `isFunctionLike == true`,
    //     `isVariadic == false`, `params` is the list of formal parameter
    //     names.
    //   - Function-like variadic: `isFunctionLike == true`,
    //     `isVariadic == true`, `params` holds the named parameters
    //     (excluding `...`). The extra actual arguments (beyond the named
    //     ones) are joined with `, ` and substituted for `__VA_ARGS__`
    //     in the body. A call with exactly `params.size()` args yields
    //     an empty `__VA_ARGS__`; a call with more args yields the
    //     comma-joined extras. C++ requires at least as many args as
    //     named params.
    struct Macro {
        bool isFunctionLike = false;
        bool isVariadic = false;
        std::vector<std::string> params;
        std::vector<Token> body;
    };

    // A frame on the conditional-compilation stack (#ifdef / #ifndef /
    // #if / #elif / #else / #endif). `taken` is true when the currently-
    // active branch of this frame should emit tokens. `seenElse` tracks
    // whether a #else has already been seen (a second #else is an error).
    // `parentActive` caches whether the enclosing context was emitting —
    // if the parent is skipping, this frame skips regardless of its own
    // condition. `anyTaken` records whether any branch of this frame has
    // been taken (for #elif: once any branch is taken, later #elif are
    // skipped — C++ rule).
    struct CondFrame {
        bool taken = false;
        bool seenElse = false;
        bool parentActive = false;
        bool anyTaken = false;
    };

    std::vector<Token> input_;
    std::filesystem::path mainFile_;
    std::vector<std::filesystem::path> includePaths_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<Token> output_;

    // Macro table. Keyed by the macro name.
    std::unordered_map<std::string, Macro> macros_;

    // Owned source buffers for included files and predefined-macro bodies.
    // Kept alive for the lifetime of the Preprocessor so that
    // Token::lexeme (which is a string_view into these buffers) stays
    // valid through run() and beyond. A deque is used (not vector)
    // because pushing more strings here must not invalidate the
    // string_views already handed out to Token::lexeme — deque nodes
    // are stable.
    std::deque<std::string> buffers_;

    // Canonicalized paths currently on the include stack — cycle guard.
    std::vector<std::filesystem::path> activePaths_;

    // Names currently being expanded — macro self-reference cycle guard.
    std::vector<std::string> expansionStack_;

    // Conditional-compilation stack (#ifdef/#ifndef/#else/#endif). Empty
    // when not inside any conditional block.
    std::vector<CondFrame> condStack_;

    // Set to true when `#pragma ivy cnumber` is encountered. Gates
    // C-style number types in the parser. Persists across the entire
    // translation unit (not scoped — once on, stays on).
    bool cnumberEnabled_ = false;

    // 8.6: Set to true when a `#error` directive is encountered. The
    // caller (main.cpp) checks this to abort compilation.
    bool hasError_ = false;

    // 8.6: #line directive support. `lineOffset_` is added to the
    // physical line number of subsequent tokens to produce the reported
    // line number. `lineFile_` overrides the file name for diagnostics.
    // When `lineFile_` is empty, the original file name is used.
    //
    // `#line N "file"` sets the *next* line's number to N, so the offset
    // is `N - (physicalLineOfNextLine)`. Since #line is on line L, the
    // next line is L+1, so offset = N - (L + 1).
    long long lineOffset_ = 0;
    std::string lineFile_;  // empty = use original file name

    // Parses a `#pragma` directive starting at `pos` (pointing at `#`).
    // Currently only recognizes `#pragma ivy cnumber`. Unknown pragmas
    // are dropped with a warning. `pos` is advanced past the directive
    // line. Returns true if the `#` was recognized as a pragma (so the
    // caller doesn't fall through to the unknown-directive path).
    bool parsePragma(const std::vector<Token>& tokens, std::size_t& pos);

    // 8.6: Parses `#error message...` — collects the rest of the line as
    // the error message text, reports a fatal diagnostic, and sets
    // `hasError_` so the caller knows compilation should abort. `pos`
    // points at `#`; on return `pos` is advanced past the directive line.
    // Returns true if the directive was recognized as `#error`.
    bool parseError(const std::vector<Token>& tokens, std::size_t& pos);

    // 8.6: Parses `#warning message...` — like `#error` but non-fatal.
    // Reports a warning diagnostic (isWarning = true). Returns true if the
    // directive was recognized as `#warning`.
    bool parseWarning(const std::vector<Token>& tokens, std::size_t& pos);

    // 8.6: Parses `#line N` or `#line N "file"` — sets the line number
    // (and optionally the file name) for the *next* line. Subsequent
    // tokens have their line numbers remapped so diagnostics report the
    // user-specified location. Returns true if the directive was recognized
    // as `#line`.
    bool parseLine(const std::vector<Token>& tokens, std::size_t& pos);

    // Predefined macros that are always defined and cannot be #undef'd
    // or re-#define'd by the user. `__LINE__` and `__FILE__` are
    // context-sensitive (they depend on the current token / file), so
    // they are expanded specially in `tryExpandPredefined` rather than
    // being stored in `macros_`. `__DATE__`, `__TIME__`, and
    // `__cplusplus` are fixed and inserted into `macros_` by
    // `initPredefinedMacros()`.
    std::unordered_map<std::string, Macro> predefinedMacros_;
    std::string dateStr_;  // "Mmm dd yyyy"
    std::string timeStr_;  // "HH:MM:SS"

    static constexpr std::size_t kMaxDepth = 200;
    static constexpr std::size_t kMaxExpansionDepth = 100;

    // Returns true when the current context is emitting tokens (i.e. every
    // frame on condStack_ is `taken` and the parent context was active).
    bool active() const;

    // Returns true when `name` is currently #defined (including predefined
    // macros).
    bool isDefined(std::string_view name) const;

    // Returns true if `name` is a predefined macro (`__LINE__`, `__FILE__`,
    // `__DATE__`, `__TIME__`, `__cplusplus`). These cannot be redefined
    // or #undef'd by the user.
    bool isPredefined(std::string_view name) const;

    // If `tok` is a predefined macro identifier, expands it to the
    // appropriate token(s) into `output_` and returns true. `__LINE__`
    // expands to an integer literal of `tok.line`; `__FILE__` expands
    // to a string literal of `mainFile_`; `__DATE__`/`__TIME__` expand
    // to string literals captured at preprocessing start; `__cplusplus`
    // expands to `202302L`. Returns false if `tok` is not a predefined
    // macro (caller falls through to user-macro expansion).
    bool tryExpandPredefined(const Token& tok);

    // Initializes the fixed predefined macros (`__DATE__`, `__TIME__`,
    // `__cplusplus`) into `macros_` and `predefinedMacros_`. Called
    // once at the start of `run()`.
    void initPredefinedMacros();

    // Advances `pos` past every token on the same line as `tokens[pos]`
    // (used to skip the rest of a directive the preprocessor doesn't
    // emit, e.g. the body of `#define` after it has been consumed, or an
    // unrecognized directive).
    void skipLine(const std::vector<Token>& tokens, std::size_t& pos);

    // Parses a `#include "..."` / `#include <...>` directive in `tokens`
    // starting at `pos` (which points at the `#` token). On success,
    // `resolved` receives the file path and `pos` is advanced past the
    // directive; on failure reports a diagnostic into `diagnostics_` and
    // leaves `pos` at the `#` token (caller emits it + skips the line).
    bool parseInclude(const std::vector<Token>& tokens, std::size_t& pos,
                      std::filesystem::path& resolved);

    // Parses a `#define NAME body...` or `#define NAME(params) body...`
    // directive starting at `pos` (which points at the `#` token). Registers
    // the macro in `macros_` and advances `pos` past the whole directive
    // line. Returns true if the directive was recognized as `#define`;
    // false otherwise (pos left at `#`).
    bool parseDefine(const std::vector<Token>& tokens, std::size_t& pos);

    // Handles a conditional directive (`#ifdef`/`#ifndef`/`#if`/`#elif`/
    // `#else`/`#endif`/`#undef`). `pos` points at the `#` token; on
    // return `pos` is advanced past the whole directive line. Returns
    // true if the directive was recognized as a conditional (so the
    // caller doesn't fall through to the generic unknown-directive path);
    // false otherwise (pos left at `#`).
    bool parseConditional(const std::vector<Token>& tokens, std::size_t& pos);

    // ------------------------------------------------------------------
    // #if / #elif constant-expression evaluation
    // ------------------------------------------------------------------
    //
    // C++16 [cpp.cond] rules, simplified:
    //  1. `defined NAME` and `defined(NAME)` are replaced by 1/0 before
    //     any macro expansion. `NAME` is looked up in `macros_`.
    //  2. Remaining identifiers (and keywords) are macro-expanded. Any
    //     identifier that survives expansion (undefined macro, or a macro
    //     that expands to nothing) is replaced by `0` (C++ rule).
    //  3. The resulting token sequence is parsed as a C++ integral
    //     constant expression. The grammar (lowest → highest precedence):
    //       expr      := ternary
    //       ternary   := logicalOr ('?' ternary ':' ternary)?
    //       logicalOr := logicalAnd ('||' logicalAnd)*
    //       logicalAnd:= bitOr ('&&' bitOr)*
    //       bitOr     := bitXor ('|' bitXor)*
    //       bitXor    := bitAnd ('^' bitAnd)*
    //       bitAnd    := eq (('&' eq)*)
    //       eq        := rel (('=='|'!=') rel)*
    //       rel       := shift (('<'|'<='|'>'|'>=') shift)*
    //       shift     := add (('<<'|'>>') add)*
    //       add       := mul (('+'|'-') mul)*
    //       mul       := unary (('*'|'/'|'%') unary)*
    //       unary     := ('!'|'-'|'+'|'~') unary | primary
    //       primary   := int | '(' expr ')'
    //     All arithmetic is done in `intmax_t`. Division by zero is
    //     rejected with a diagnostic.
    //
    // `tokens` is the raw token slice of the directive line (after
    // `#if` / `#elif`). `line`/`col` are used for diagnostics. Returns
    // the evaluated value (0 = false, nonzero = true).
    long long evalConstExpr(const std::vector<Token>& tokens,
                            std::uint32_t line, std::uint32_t col);

    // Resolves `target` per `#include "..."` (relative to current file
    // first, then include paths) or `#include <...>` (include paths only).
    std::filesystem::path resolveQuoted(std::string_view target) const;
    std::filesystem::path resolveAngle(std::string_view target) const;

    // Called from the main loop when `tokens[pos]` is an Identifier that
    // names an object-like macro. Recursively expands the macro body into
    // `output_` (with cycle guard). Does NOT consume any input beyond the
    // single invocation token (object-like macros take no arguments).
    void expandObjectLike(const Token& tok, const Macro& macro);

    // Called from the main loop when `tokens[pos]` is an Identifier that
    // names a function-like macro. Peeks the next token: if it is `(`,
    // consumes the argument list (balanced parens), substitutes formal
    // parameters in the body with the actual-argument token sequences, and
    // emits the substituted body into `output_` (recursively, with cycle
    // guard). If the next token is not `(`, the macro is not invoked —
    // the identifier is emitted verbatim (C++ rule). Returns the number of
    // input tokens consumed (including the invocation identifier) in
    // `consumed`. Reports diagnostics on malformed argument lists.
    void expandFunctionLike(const std::vector<Token>& tokens, std::size_t& pos,
                            const Token& nameTok, const Macro& macro);

    // Runs macro expansion over a token vector (a macro body after
    // parameter substitution, or any token slice) and appends the result
    // to `output_`. Object-like macros are expanded in place; function-
    // like macros consume their argument tokens from `tokens` (so a body
    // like `SQUARE(x)` where SQUARE is function-like expands correctly).
    // `pos` is advanced past every consumed token. The expansion stack
    // is shared with the caller for cycle guarding.
    void expandTokenVector(const std::vector<Token>& tokens);

    // Emits a single token to `output_`, expanding object-like macros if
    // `tok` is an identifier naming a macro. Function-like macro names
    // without a following `(` are also emitted verbatim through this path.
    // Used for macro body tokens during recursive expansion.
    void emitToken(const Token& tok);

    // 8.7: Stringify a sequence of argument tokens into a single string
    // literal token. Used for the `#param` operator in function-like
    // macro bodies. The tokens are joined with single spaces; leading/
    // trailing whitespace is trimmed. String literals and char literals
    // within the argument are escaped per C++ rules (backslashes and
    // quotes are doubled). The result is stored in `buffers_` for stable
    // lifetime and returned as a `String` token.
    Token stringifyTokens(const std::vector<Token>& toks,
                          std::uint32_t line, std::uint32_t col);

    // 8.7: Paste (concatenate) two tokens into one. Used for the `##`
    // operator in macro bodies. The pasted lexeme is `left.lexeme +
    // right.lexeme`; the result is re-lexed to determine its token kind
    // (e.g., `foo` → Identifier, `123` → Integer, `+=` → PlusAssign).
    // If the paste produces an invalid token, a diagnostic is reported
    // and the pasted lexeme is emitted as an Identifier.
    Token pasteTokens(const Token& left, const Token& right);

    // 8.7: Process a macro body for `#` (stringify) and `##` (paste)
    // operators, substituting parameters from `paramMap` where needed.
    // Returns the processed token vector (with all #/## resolved). This
    // is called before `expandTokenVector` so that stringification uses
    // raw (unexpanded) argument tokens and pasting joins raw tokens.
    // For object-like macros, `paramMap` is empty (only ## is relevant).
    //
    // `paramMap` maps parameter names to ArgInfo, which holds both raw
    // (unexpanded) and expanded argument tokens. `#` uses raw tokens;
    // normal substitution uses expanded tokens.
    struct ArgInfo {
        const std::vector<Token>* raw = nullptr;
        const std::vector<Token>* expanded = nullptr;
    };
    std::vector<Token> substituteBody(
        const std::vector<Token>& body,
        const std::unordered_map<std::string, ArgInfo>& paramMap,
        const Token& macroNameTok);

    // Lexes `file` and recursively expands its #includes into `output_`.
    void processFile(const std::filesystem::path& file);
};

}  // namespace ivy
