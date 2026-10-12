#include "lexer/Lexer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bezel {

namespace {

bool isAsciiLetter(unsigned char b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
}

bool isAsciiDigit(unsigned char b) { return b >= '0' && b <= '9'; }

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool isBinDigit(char c) { return c == '0' || c == '1'; }

std::uint32_t hexValue(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint32_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint32_t>(c - 'A' + 10);
    return 0;
}

// Advance past one Unicode scalar value: one ASCII byte, or the full UTF-8
// sequence introduced by a non-ASCII lead byte. Inside char/string literals
// the delimiter and escape introducer characters (`'`, `"`, `\\`) are all
// ASCII, so UTF-8 continuation bytes never collide with them — but we still
// need to advance past the whole sequence as one logical character.
void consumeOneScalar(std::string_view source, std::size_t& pos) {
    if (pos >= source.size()) return;
    unsigned char b = static_cast<unsigned char>(source[pos]);
    ++pos;
    if (b < 0x80) return;
    int extras = 0;
    if      ((b & 0xE0) == 0xC0) extras = 1;
    else if ((b & 0xF0) == 0xE0) extras = 2;
    else if ((b & 0xF8) == 0xF0) extras = 3;
    while (extras > 0 && pos < source.size() &&
           (static_cast<unsigned char>(source[pos]) & 0xC0) == 0x80) {
        ++pos;
        --extras;
    }
}

// (consumeEscape moved to Lexer::consumeEscape so it can resolve diagnostic
//  locations via locationAt; see below.)

// Per v0.127 A.1, every numeric digit-run is `Digit ("_"* Digit)*`: the run
// must end with a digit. Our scanners consume the digit-and-underscore set
// greedily and then call this to back off any trailing `_`s. The first
// character of each scanned run is always a digit, so the backup cannot pass
// the start of the literal.
void trimTrailingUnderscores(std::string_view source, std::size_t& pos) {
    while (pos > 0 && source[pos - 1] == '_') {
        --pos;
    }
}

// IntSuffix per A.1: "L" | "U" | "UL" | "LU", case-insensitive. Advances
// `pos` past whichever suffix is found (if any).
void consumeIntSuffix(std::string_view source, std::size_t& pos) {
    if (pos >= source.size()) return;
    char c = source[pos];
    if (c == 'L' || c == 'l') {
        ++pos;
        if (pos < source.size() && (source[pos] == 'U' || source[pos] == 'u')) {
            ++pos; // LU
        }
    } else if (c == 'U' || c == 'u') {
        ++pos;
        if (pos < source.size() && (source[pos] == 'L' || source[pos] == 'l')) {
            ++pos; // UL
        }
    }
}

// §1.3 / A.1: an identifier begins with a letter or underscore. We accept any
// byte with the high bit set as the start of a UTF-8-encoded Unicode letter
// — an approximation, since full Unicode letter classification is deferred to
// a later milestone (it would also accept non-letter scripts like digits in
// other scripts and emoji as identifier starts). The spec calls for "any
// Unicode letter"; the approximation passes the spec-derived tests in this
// slice and is documented in the PR.
bool isIdentStart(unsigned char b) {
    return isAsciiLetter(b) || b == '_' || b >= 0x80;
}

// §1.3 / A.1: identifier continues with letters, digits (ASCII per A.1), or
// underscores. Same UTF-8 approximation for non-ASCII bytes.
bool isIdentCont(unsigned char b) {
    return isIdentStart(b) || isAsciiDigit(b);
}

// Appendix B.1 — unconditionally reserved keywords. Independently encoded
// here; the spec-derived test in tests/lexer/keywords_test.cpp independently
// transcribes the same list, so any divergence between this list and the spec
// surfaces as a test failure.
bool isReservedKeyword(std::string_view text) {
    static constexpr std::string_view kReserved[] = {
        "abstract",  "and",      "as",        "async",      "await",
        "base",      "bool",     "break",     "byte",       "case",
        "catch",     "char",     "choice",    "class",      "closed",
        "const",
        "constructor", "continue", "decimal",  "default",    "destructor",
        "do",        "double",   "elif",      "else",       "end",
        "enum",
        "event",     "extension","extern",    "false",      "finally",
        "float",     "for",      "if",        "import",     "in",
        "int",       "interface","internal",  "is",         "lambda",
        "long",
        "method",    "mod",      "module",    "move",       "nameof",
        "new",       "not",      "null",      "or",         "out",
        "owned",     "override", "private",   "property",   "protected",
        "public",    "readonly", "record",    "ref",        "return",
        "sbyte",     "sealed",   "short",     "static",     "string",
        "then",      "this",     "throw",     "true",       "try",
        "type",      "typeof",   "uint",      "ulong",      "ushort",
        "using",     "var",      "virtual",   "when",       "where",
        "while",     "with",     "yield",
    };
    for (std::string_view keyword : kReserved) {
        if (keyword == text) {
            return true;
        }
    }
    return false;
}

} // namespace

// See Lexer.hpp for the contract. The two `CharContent` shapes of §18.3
// (v0.334) — one escape sequence, else one Unicode scalar value — are
// measured off `src` without touching a diagnostic engine, because this runs
// as a *speculative* lookahead: a `'` that does not close into a complete
// literal must leave the caller's position untouched and be treated as
// ordinary content.
std::size_t charLiteralEnd(std::string_view src, std::size_t open) {
    if (open >= src.size() || src[open] != '\'') return std::string_view::npos;
    std::size_t p = open + 1;
    if (p >= src.size()) return std::string_view::npos;
    if (src[p] == '\\') {
        // One escape sequence. §18.5/§18.7 keep a char literal inside a
        // `$@"…"` hole an ordinary char literal — `@` suppresses *string*
        // escapes, not CharContent escapes — so this arm serves both scans.
        ++p;
        if (p >= src.size()) return std::string_view::npos;
        const char k = src[p++];
        if (k == 'x' || k == 'X') {
            std::size_t digits = 0;
            while (digits < 2 && p < src.size() && isHexDigit(src[p])) {
                ++p;
                ++digits;
            }
            if (digits == 0) return std::string_view::npos;
        } else if (k == 'u' || k == 'U') {
            const std::size_t want = (k == 'u') ? 4 : 8;
            for (std::size_t n = 0; n < want; ++n) {
                if (p >= src.size() || !isHexDigit(src[p]))
                    return std::string_view::npos;
                ++p;
            }
        } else if (!(k == 'n' || k == 'r' || k == 't' || k == '\\' ||
                     k == '\'' || k == '"' || k == '0' || k == 'a' ||
                     k == 'b' || k == 'f' || k == 'v')) {
            return std::string_view::npos; // §18.8's table is closed
        }
    } else {
        std::size_t probe = p; // one Unicode scalar value
        consumeOneScalar(src, probe);
        p = probe;
    }
    if (p < src.size() && src[p] == '\'') return p + 1;
    return std::string_view::npos;
}

Lexer::Lexer(std::string_view source, DiagnosticEngine& diagnostics)
    : source_(source), diagnostics_(diagnostics) {}

// §18.5 (v0.306, #4517): rebase every produced location onto `base` instead
// of the default {1, 1, 0} origin — see Lexer.hpp for why.
Lexer::Lexer(std::string_view source, DiagnosticEngine& diagnostics,
             SourceLocation base)
    : source_(source), diagnostics_(diagnostics), base_(base) {}

SourceLocation Lexer::locationAt(std::size_t offset) const {
    std::uint32_t line = base_.line;
    std::uint32_t column = base_.column;
    const std::size_t limit = offset < source_.size() ? offset : source_.size();
    for (std::size_t i = 0; i < limit; ++i) {
        const char c = source_[i];
        if (c == '\r') {
            ++line;
            column = 1;
            if (i + 1 < limit && source_[i + 1] == '\n') {
                ++i; // CRLF advances the line once, not twice
            }
        } else if (c == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return SourceLocation{line, column,
                          static_cast<std::uint32_t>(base_.offset + offset)};
}

Token Lexer::next() {
    // Skip whitespace and comments — both are trivia per §1.2 ("Whitespace
    // and comments separate tokens but are not themselves tokens"). The two
    // can interleave, so loop until we land on a real token.
    for (;;) {
        if (position_ >= source_.size()) {
            return Token{TokenKind::EndOfFile, {}, locationAt(position_)};
        }
        unsigned char b = static_cast<unsigned char>(source_[position_]);

        // §1.1 / §1.2: whitespace and line endings. Consuming '\r' and '\n'
        // individually handles all three line-ending forms (LF, CR, CRLF).
        if (b == ' ' || b == '\t' || b == '\n' || b == '\r' ||
            b == '\f' || b == '\v') {
            ++position_;
            continue;
        }

        // §1.5: comments. "//" line, "/* … */" block (non-nesting), "///" doc
        // (treated as a line comment for trivia in this slice; doc-comment
        // preservation/attachment per §34 is deferred to the parser).
        if (b == '/' && position_ + 1 < source_.size()) {
            unsigned char b2 = static_cast<unsigned char>(source_[position_ + 1]);
            if (b2 == '/') {
                position_ += 2;
                while (position_ < source_.size() &&
                       source_[position_] != '\n' &&
                       source_[position_] != '\r') {
                    ++position_;
                }
                continue;
            }
            if (b2 == '*') {
                position_ += 2;
                bool closed = false;
                while (position_ < source_.size()) {
                    if (source_[position_] == '*' &&
                        position_ + 1 < source_.size() &&
                        source_[position_ + 1] == '/') {
                        position_ += 2;
                        closed = true;
                        break;
                    }
                    ++position_;
                }
                if (!closed) {
                    diagnostics_.report(Severity::Error, locationAt(position_),
                                        "unterminated block comment");
                }
                continue;
            }
        }
        break;
    }

    // §1.3 identifier or §1.4 / Appendix B keyword.
    unsigned char b = static_cast<unsigned char>(source_[position_]);
    if (isIdentStart(b)) {
        std::size_t start = position_;
        while (position_ < source_.size() &&
               isIdentCont(static_cast<unsigned char>(source_[position_]))) {
            ++position_;
        }
        std::string_view text = source_.substr(start, position_ - start);
        // per A.1 / §1.3 / B.1 (v0.244, proposal 0083): `Identifier ::= Letter
        // IdentCont* | "_" IdentCont+`. The bare single-character `_` matches
        // NEITHER alternative — the second requires at least one IdentCont
        // after the underscore — so it is lexed as its own reserved token, the
        // discard (§17.13). `_x`, `_1`, `__` and `snake_case` all satisfy a
        // production and stay ordinary identifiers.
        if (text.size() == 1 && text[0] == '_')
            return Token{TokenKind::Discard, text, locationAt(start)};
        // A.6 `AssignOp` (v0.242, #3052), verbatim: "`mod=` is one token,
        // lexed by longest match: the keyword `mod` immediately followed by
        // `=` with no intervening space. `x mod = y` remains a syntax error."
        //
        // The rule is stated on the KEYWORD, so it belongs here rather than in
        // the punctuator switch below: an identifier that merely ENDS in "mod"
        // (`amod=5`) is consumed whole by the loop above and never reaches
        // this test. `mod=` is emitted as a Punctuator so it joins the other
        // compound-assignment tokens (`+=`, `<<=`, `??=`) at every parser site
        // that tests `TokenKind::Punctuator` against the AssignOp set.
        if (text == "mod" && position_ < source_.size() &&
            source_[position_] == '=') {
            ++position_;
            return Token{TokenKind::Punctuator,
                         source_.substr(start, position_ - start),
                         locationAt(start)};
        }
        TokenKind kind =
            isReservedKeyword(text) ? TokenKind::Keyword : TokenKind::Identifier;
        return Token{kind, text, locationAt(start)};
    }

    // §18.1 numeric literals (grammar A.1: IntLiteral, FloatLiteral). Value
    // parsing and range-checking (e.g. §18.1's "3_000_000_000 without L is a
    // compile error") are sema-level concerns; the lexer recognizes the token
    // and spans its full lexeme.
    if (isAsciiDigit(b)) {
        const std::size_t numStart = position_;

        // Hex / binary prefix. The character after `0x` / `0b` must itself be
        // a valid digit of that base — otherwise we don't enter the prefix
        // path, and `0` falls through to plain decimal (so e.g. `0xz` lexes
        // as IntLiteral "0" + Identifier "xz", not a malformed hex literal).
        if (b == '0' && position_ + 2 < source_.size()) {
            const char p = source_[position_ + 1];
            if ((p == 'x' || p == 'X') && isHexDigit(source_[position_ + 2])) {
                position_ += 2;
                while (position_ < source_.size() &&
                       (isHexDigit(source_[position_]) ||
                        source_[position_] == '_')) {
                    ++position_;
                }
                trimTrailingUnderscores(source_, position_);
                consumeIntSuffix(source_, position_);
                return Token{TokenKind::IntLiteral,
                             source_.substr(numStart, position_ - numStart),
                             locationAt(numStart)};
            }
            if ((p == 'b' || p == 'B') && isBinDigit(source_[position_ + 2])) {
                position_ += 2;
                while (position_ < source_.size() &&
                       (isBinDigit(source_[position_]) ||
                        source_[position_] == '_')) {
                    ++position_;
                }
                trimTrailingUnderscores(source_, position_);
                consumeIntSuffix(source_, position_);
                return Token{TokenKind::IntLiteral,
                             source_.substr(numStart, position_ - numStart),
                             locationAt(numStart)};
            }
        }

        // Decimal digit run.
        while (position_ < source_.size() &&
               (isAsciiDigit(static_cast<unsigned char>(source_[position_])) ||
                source_[position_] == '_')) {
            ++position_;
        }
        trimTrailingUnderscores(source_, position_);

        bool isFloat = false;

        // Fractional part (A.1 form 1): `.` followed by a digit. The
        // digit-required lookahead is what distinguishes `1.0` (float) from
        // `1..10` (int, range, int) and `obj.member` (int, dot, identifier).
        if (position_ + 1 < source_.size() && source_[position_] == '.' &&
            isAsciiDigit(static_cast<unsigned char>(source_[position_ + 1]))) {
            isFloat = true;
            ++position_; // consume '.'
            while (position_ < source_.size() &&
                   (isAsciiDigit(static_cast<unsigned char>(source_[position_])) ||
                    source_[position_] == '_')) {
                ++position_;
            }
            trimTrailingUnderscores(source_, position_);
        }

        // Exponent (A.1 forms 1 and 2): e/E with optional sign and required
        // digit run. We peek past the sign before committing so a stray `e`
        // (e.g. in `42e`) is left for the next iteration to lex as an
        // identifier rather than producing a malformed literal.
        if (position_ < source_.size() &&
            (source_[position_] == 'e' || source_[position_] == 'E')) {
            std::size_t peek = position_ + 1;
            if (peek < source_.size() &&
                (source_[peek] == '+' || source_[peek] == '-')) {
                ++peek;
            }
            if (peek < source_.size() &&
                isAsciiDigit(static_cast<unsigned char>(source_[peek]))) {
                isFloat = true;
                position_ = peek;
                while (position_ < source_.size() &&
                       (isAsciiDigit(static_cast<unsigned char>(source_[position_])) ||
                        source_[position_] == '_')) {
                    ++position_;
                }
                trimTrailingUnderscores(source_, position_);
            }
        }

        // Suffix: float suffix (f/F/m/M, A.1 forms 1–3) takes precedence over
        // integer suffix; the grammar makes them mutually exclusive.
        if (position_ < source_.size()) {
            const char c = source_[position_];
            if (c == 'f' || c == 'F' || c == 'm' || c == 'M') {
                isFloat = true;
                ++position_;
            } else if (!isFloat) {
                consumeIntSuffix(source_, position_);
            }
        }

        return Token{isFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral,
                     source_.substr(numStart, position_ - numStart),
                     locationAt(numStart)};
    }

    // §18.3 char, §18.4 string, §18.6 verbatim string, §18.5/§18.7 interp.
    if (b == '\'') {
        return scanCharLiteral();
    }
    if (b == '"') {
        return scanStringLiteral();
    }
    if (b == '@' && position_ + 1 < source_.size() &&
        source_[position_ + 1] == '"') {
        return scanVerbatimString();
    }
    if (b == '$' && position_ + 1 < source_.size() &&
        source_[position_ + 1] == '"') {
        return scanInterpolatedString();
    }
    if (b == '$' && position_ + 1 < source_.size() &&
        source_[position_ + 1] == '@' &&
        position_ + 2 < source_.size() &&
        source_[position_ + 2] == '"') {
        return scanInterpolatedVerbatimString();
    }

    // §17 / Appendix A operators and punctuation. Maximal munch via a
    // first-character switch with one- and two-char lookahead. `//` and `/*`
    // never reach here — they are consumed by the trivia loop above — so the
    // `/` case below only ever sees a real division-or-`/=` punctuator.
    const std::size_t puStart = position_;
    const char c1 =
        (position_ + 1 < source_.size()) ? source_[position_ + 1] : '\0';
    const char c2 =
        (position_ + 2 < source_.size()) ? source_[position_ + 2] : '\0';
    std::size_t puLen = 0;

    switch (static_cast<char>(b)) {
    case '(': case ')': case '[': case ']': case '{': case '}':
    case ',': case ';': case '~':
        puLen = 1;
        break;
    case '!':
        // §17.8: `!` alone is the postfix null-forgiving operator (e.g. `x!`,
        // `x!.member`). `!=` is NOT in the Bezel grammar (Appendix A); the
        // inequality operator is `<>` (§17.3). We lex `!=` as a single
        // two-character Punctuator token so the parser can report a clear
        // "use '<>'" diagnostic rather than silently miscompiling it as
        // PostfixBang followed by the equality operator `=`. per §17.3 / #468.
        puLen = (c1 == '=') ? 2 : 1;             // != (error) or ! (valid)
        break;
    case ':':
        puLen = (c1 == '=') ? 2 : 1;             // := or :
        break;
    case '.':
        if (c1 == '.' && c2 == '<') puLen = 3;   // ..<
        else if (c1 == '.')          puLen = 2;  // ..
        else                         puLen = 1;  // .
        break;
    case '+':
        if (c1 == '+' || c1 == '=')  puLen = 2;  // ++  +=
        else                         puLen = 1;  // +
        break;
    case '-':
        if (c1 == '-' || c1 == '=' || c1 == '>') puLen = 2;  // --  -=  ->
        else                                     puLen = 1;  // -
        break;
    case '*':
    case '/':
    case '&':
    case '|':
    case '^':
        puLen = (c1 == '=') ? 2 : 1;             // *= /= &= |= ^= or single
        break;
    case '<':
        if (c1 == '<' && c2 == '=') puLen = 3;                       // <<=
        else if (c1 == '<' || c1 == '=' || c1 == '>') puLen = 2;     // <<  <=  <>
        else                                          puLen = 1;     // <
        break;
    case '>':
        if (c1 == '>' && c2 == '=') puLen = 3;                       // >>=
        else if (c1 == '>' || c1 == '=') puLen = 2;                  // >>  >=
        else                             puLen = 1;                  // >
        break;
    case '=':
        puLen = (c1 == '>') ? 2 : 1;             // => or =
        break;
    case '?':
        if (c1 == '?' && c2 == '=') puLen = 3;                       // ??=
        else if (c1 == '?' || c1 == '.' || c1 == '(') puLen = 2;     // ??  ?.  ?(
        else                                          puLen = 1;     // ?
        break;
    default:
        break;
    }

    if (puLen > 0) {
        position_ += puLen;
        return Token{TokenKind::Punctuator,
                     source_.substr(puStart, puLen), locationAt(puStart)};
    }

    // Unrecognized character. Char/string/verbatim literal openers (`'`, `"`,
    // `@"`) are dispatched above; what reaches here is something the lexer
    // has no rule for at all.
    std::size_t start = position_;
    const SourceLocation unknownLoc = locationAt(start);
    ++position_;
    diagnostics_.report(Severity::Error, unknownLoc, "unexpected character");
    return Token{TokenKind::Unknown, source_.substr(start, 1), unknownLoc};
}

// §18.3: `CharLiteral ::= "'" CharContent "'"`. The lexer scans for the
// closing quote (consuming one escape or one scalar value of content) and
// reports diagnostics for malformed forms; value validation beyond surrogate
// detection is sema's responsibility.
Token Lexer::scanCharLiteral() {
    const std::size_t start = position_;
    const SourceLocation startLoc = locationAt(start);
    ++position_; // past opening '\''
    if (position_ >= source_.size()) {
        diagnostics_.report(Severity::Error, startLoc,
                            "unterminated character literal");
        return Token{TokenKind::CharLiteral,
                     source_.substr(start, position_ - start),
                     startLoc};
    }
    if (source_[position_] == '\'') {
        // Empty content — A.1 CharContent requires one scalar or escape.
        diagnostics_.report(Severity::Error, startLoc,
                            "empty character literal");
    } else if (source_[position_] == '\\') {
        consumeEscape();
    } else {
        consumeOneScalar(source_, position_);
    }
    if (position_ < source_.size() && source_[position_] == '\'') {
        ++position_;
    } else {
        diagnostics_.report(Severity::Error, startLoc,
                            "unterminated character literal");
    }
    return Token{TokenKind::CharLiteral,
                 source_.substr(start, position_ - start),
                 startLoc};
}

// §18.4: `StringLiteral ::= '"' StringContent* '"'`. Content is any code
// point or escape other than an unescaped `"`. UTF-8 multi-byte sequences
// flow through `consumeOneScalar`; named/unicode/hex escapes flow through
// `consumeEscape`, which also detects surrogate escapes per §18.8.
Token Lexer::scanStringLiteral() {
    const std::size_t start = position_;
    const SourceLocation startLoc = locationAt(start);
    ++position_; // past opening '"'
    bool terminated = false;
    while (position_ < source_.size()) {
        const char c = source_[position_];
        if (c == '"') {
            ++position_;
            terminated = true;
            break;
        }
        if (c == '\\') {
            consumeEscape();
            continue;
        }
        consumeOneScalar(source_, position_);
    }
    if (!terminated) {
        diagnostics_.report(Severity::Error, startLoc,
                            "unterminated string literal");
    }
    return Token{TokenKind::StringLiteral,
                 source_.substr(start, position_ - start),
                 startLoc};
}

// §18.6: `'@"' VerbatimContent* '"'`. No `\` escape processing — backslashes
// are literal. The only "escape" is `""`, representing one `"` inside the
// literal; the lexer must look ahead to distinguish that pair from a single
// closing quote. Line breaks become part of the lexeme (multi-line).
Token Lexer::scanVerbatimString() {
    const std::size_t start = position_;
    const SourceLocation startLoc = locationAt(start);
    position_ += 2; // past '@' and '"'
    bool terminated = false;
    while (position_ < source_.size()) {
        const char c = source_[position_];
        if (c == '"') {
            if (position_ + 1 < source_.size() && source_[position_ + 1] == '"') {
                position_ += 2;
                continue;
            }
            ++position_;
            terminated = true;
            break;
        }
        consumeOneScalar(source_, position_);
    }
    if (!terminated) {
        diagnostics_.report(Severity::Error, startLoc,
                            "unterminated verbatim string literal");
    }
    return Token{TokenKind::VerbatimStringLiteral,
                 source_.substr(start, position_ - start),
                 startLoc};
}

// §18.5 InterpString: `'$"' (InterpPart | StringContent)* '"'`. Slice 51
// scans the whole `$"..."` as a single InterpolatedString token; the inner
// `{ ... }` parts are taken opaquely (tracking only balanced brace nesting
// so `{ "..." }` style interpolations don't terminate the string early).
// `\{` and the literal-brace escapes `{{` / `}}` keep their literal meaning
// inside non-verbatim interp strings; standard `\` escape sequences also
// apply to the surrounding content. Detailed escape validation lives in
// consumeEscape; this scan just bypasses escapes far enough to find the
// closing `"`.
Token Lexer::scanInterpolatedString() {
    const std::size_t start = position_;
    const SourceLocation startLoc = locationAt(start);
    position_ += 2; // past '$' and '"'
    bool terminated = false;
    while (position_ < source_.size()) {
        const char c = source_[position_];
        if (c == '"') {
            ++position_;
            terminated = true;
            break;
        }
        if (c == '\\') {
            consumeEscape();
            continue;
        }
        if (c == '{') {
            if (position_ + 1 < source_.size() &&
                source_[position_ + 1] == '{') {
                // literal-brace escape
                position_ += 2;
                continue;
            }
            // Interpolation part — consume balanced braces.
            ++position_; // past `{`
            int depth = 1;
            // per §18.5 (v0.334): the format spec runs from the FIRST
            // top-level `:` to the hole's closing `}`, so only the text
            // before that `:` is hole-expression source. Below it a
            // complete char literal is one opaque unit (its own `}` is
            // content, not the hole's closer); above it an apostrophe is an
            // ordinary format character and the hole's `}` always closes.
            bool inFormatSpec = false;
            while (position_ < source_.size() && depth > 0) {
                const char ic = source_[position_];
                if (ic == '"') {
                    // Nested string inside the interpolation — skip it as
                    // a unit so its braces/quotes don't confuse the depth
                    // counter. This is the slice-51 approximation; a true
                    // re-entrant scan lands with the splitting slice.
                    ++position_;
                    while (position_ < source_.size() &&
                           source_[position_] != '"') {
                        if (source_[position_] == '\\') consumeEscape();
                        else consumeOneScalar(source_, position_);
                    }
                    if (position_ < source_.size()) ++position_; // closing "
                    continue;
                }
                // per §18.5 (v0.334): skip a nested char literal as a unit.
                // `charLiteralEnd` reports a COMPLETE literal, so an
                // apostrophe that never closes (`HH'` in `{d:HH'}`) is left
                // as content rather than swallowing the hole's `}`.
                if (!inFormatSpec && depth == 1 && ic == '\'') {
                    const std::size_t litEnd = charLiteralEnd(source_, position_);
                    if (litEnd != std::string_view::npos) {
                        position_ = litEnd;
                        continue;
                    }
                }
                if (ic == ':' && depth == 1) inFormatSpec = true;
                if (ic == '{') ++depth;
                else if (ic == '}') --depth;
                ++position_;
            }
            continue;
        }
        if (c == '}') {
            if (position_ + 1 < source_.size() &&
                source_[position_ + 1] == '}') {
                position_ += 2;
                continue;
            }
            // Stray `}` — report and consume to avoid an infinite loop.
            diagnostics_.report(Severity::Error, locationAt(position_),
                                "unmatched '}' in interpolated string");
            ++position_;
            continue;
        }
        consumeOneScalar(source_, position_);
    }
    if (!terminated) {
        diagnostics_.report(Severity::Error, startLoc,
                            "unterminated interpolated string literal");
    }
    return Token{TokenKind::InterpolatedString,
                 source_.substr(start, position_ - start),
                 startLoc};
}

// §18.7 verbatim InterpString: `'$@"' (InterpPart | VerbatimContent)* '"'`.
// Like scanInterpolatedString but with the verbatim-string content rules
// (no `\` escapes; `""` is a literal `"`; line breaks are part of the
// lexeme).
Token Lexer::scanInterpolatedVerbatimString() {
    const std::size_t start = position_;
    const SourceLocation startLoc = locationAt(start);
    position_ += 3; // past '$', '@', '"'
    bool terminated = false;
    while (position_ < source_.size()) {
        const char c = source_[position_];
        if (c == '"') {
            if (position_ + 1 < source_.size() &&
                source_[position_ + 1] == '"') {
                position_ += 2;
                continue;
            }
            ++position_;
            terminated = true;
            break;
        }
        if (c == '{') {
            if (position_ + 1 < source_.size() &&
                source_[position_ + 1] == '{') {
                position_ += 2;
                continue;
            }
            ++position_;
            int depth = 1;
            // per §18.5 (v0.334) — the same rule as scanInterpolatedString:
            // only text before the first top-level `:` is hole-expression
            // source, and a complete char literal there is one opaque unit.
            bool inFormatSpec = false;
            while (position_ < source_.size() && depth > 0) {
                const char ic = source_[position_];
                if (ic == '"') {
                    // Nested string inside the interpolation — skip it as a
                    // unit so its braces/quotes don't confuse the depth
                    // counter.
                    ++position_;
                    while (position_ < source_.size() &&
                           source_[position_] != '"') {
                        consumeOneScalar(source_, position_);
                    }
                    if (position_ < source_.size()) ++position_;
                    continue;
                }
                // per §18.5 (v0.334): skip a nested char literal as a unit.
                // A char literal keeps §18.3's CharContent grammar inside a
                // `$@"…"` hole — `@` suppresses *string* escapes, not
                // CharContent ones — so `charLiteralEnd` serves both.
                if (!inFormatSpec && depth == 1 && ic == '\'') {
                    const std::size_t litEnd = charLiteralEnd(source_, position_);
                    if (litEnd != std::string_view::npos) {
                        position_ = litEnd;
                        continue;
                    }
                }
                if (ic == ':' && depth == 1) inFormatSpec = true;
                if (ic == '{') ++depth;
                else if (ic == '}') --depth;
                ++position_;
            }
            continue;
        }
        if (c == '}') {
            if (position_ + 1 < source_.size() &&
                source_[position_ + 1] == '}') {
                position_ += 2;
                continue;
            }
            diagnostics_.report(
                Severity::Error, locationAt(position_),
                "unmatched '}' in interpolated verbatim string");
            ++position_;
            continue;
        }
        consumeOneScalar(source_, position_);
    }
    if (!terminated) {
        diagnostics_.report(
            Severity::Error, startLoc,
            "unterminated interpolated verbatim string literal");
    }
    return Token{TokenKind::InterpolatedVerbatimString,
                 source_.substr(start, position_ - start),
                 startLoc};
}

// Consume one escape sequence starting at source_[position_] == '\\' (caller
// has verified). Reports diagnostics for malformed escapes and for any escape
// resolving to a surrogate code point (§18.3, §18.8). Advances position_ past
// the escape regardless of validity so the surrounding scan can keep going.
// All diagnostics raised from inside one escape point at the `\` for clarity.
void Lexer::consumeEscape() {
    const SourceLocation escapeLoc = locationAt(position_);
    if (position_ + 1 >= source_.size()) {
        diagnostics_.report(Severity::Error, escapeLoc,
                            "unterminated escape sequence");
        ++position_;
        return;
    }
    ++position_; // past the backslash
    const char c = source_[position_];
    switch (c) {
    case 'n': case 'r': case 't': case '\\': case '"': case '\'':
    case '0': case 'a': case 'b': case 'f': case 'v':
        // §18.8 named escapes; single character after `\`.
        ++position_;
        return;
    case 'u':
    case 'U': {
        // §18.8 \u: 4 hex digits, BMP-only; \U: 8 hex digits, full range.
        const int required = (c == 'u') ? 4 : 8;
        ++position_;
        std::uint32_t value = 0;
        int count = 0;
        while (count < required && position_ < source_.size() &&
               isHexDigit(source_[position_])) {
            value = (value << 4) | hexValue(source_[position_]);
            ++position_;
            ++count;
        }
        if (count < required) {
            diagnostics_.report(
                Severity::Error, escapeLoc,
                std::string("\\") + c + " escape requires exactly " +
                    (required == 4 ? "4" : "8") + " hex digits");
        }
        // §18.3 / §18.8: surrogate code points are compile errors.
        if (value >= 0xD800 && value <= 0xDFFF) {
            diagnostics_.report(Severity::Error, escapeLoc,
                                "escape sequence resolves to a surrogate code point");
        }
        return;
    }
    case 'x': {
        // §18.8 \x: 1–4 hex digits, greedy. Greediness matters for surrogate
        // detection (e.g. `\xD800` decodes to U+D800).
        ++position_;
        std::uint32_t value = 0;
        int count = 0;
        while (count < 4 && position_ < source_.size() &&
               isHexDigit(source_[position_])) {
            value = (value << 4) | hexValue(source_[position_]);
            ++position_;
            ++count;
        }
        if (count == 0) {
            diagnostics_.report(Severity::Error, escapeLoc,
                                "\\x escape requires at least one hex digit");
        }
        if (value >= 0xD800 && value <= 0xDFFF) {
            diagnostics_.report(Severity::Error, escapeLoc,
                                "escape sequence resolves to a surrogate code point");
        }
        return;
    }
    default:
        diagnostics_.report(Severity::Error, escapeLoc,
                            "unrecognized escape sequence");
        ++position_;
        return;
    }
}

} // namespace bezel
