#include "lexer/Lexer.hpp"

#include <cstddef>
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
        "catch",     "char",     "class",     "const",      "constructor",
        "continue",  "decimal",  "default",   "destructor", "do",
        "double",    "elif",     "else",      "end",        "enum",
        "event",     "extension","extern",    "false",      "finally",
        "float",     "for",      "if",        "import",     "in",
        "int",       "interface","internal",  "is",         "long",
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

Lexer::Lexer(std::string_view source, DiagnosticEngine& diagnostics)
    : source_(source), diagnostics_(diagnostics) {}

Token Lexer::next() {
    // Skip whitespace and comments — both are trivia per §1.2 ("Whitespace
    // and comments separate tokens but are not themselves tokens"). The two
    // can interleave, so loop until we land on a real token.
    for (;;) {
        if (position_ >= source_.size()) {
            return Token{TokenKind::EndOfFile, {}, SourceLocation{}};
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
                    diagnostics_.report(Severity::Error, SourceLocation{},
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
        TokenKind kind =
            isReservedKeyword(text) ? TokenKind::Keyword : TokenKind::Identifier;
        return Token{kind, text, SourceLocation{}};
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
                consumeIntSuffix(source_, position_);
                return Token{TokenKind::IntLiteral,
                             source_.substr(numStart, position_ - numStart),
                             SourceLocation{}};
            }
            if ((p == 'b' || p == 'B') && isBinDigit(source_[position_ + 2])) {
                position_ += 2;
                while (position_ < source_.size() &&
                       (isBinDigit(source_[position_]) ||
                        source_[position_] == '_')) {
                    ++position_;
                }
                consumeIntSuffix(source_, position_);
                return Token{TokenKind::IntLiteral,
                             source_.substr(numStart, position_ - numStart),
                             SourceLocation{}};
            }
        }

        // Decimal digit run.
        while (position_ < source_.size() &&
               (isAsciiDigit(static_cast<unsigned char>(source_[position_])) ||
                source_[position_] == '_')) {
            ++position_;
        }

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
                     SourceLocation{}};
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
    case ',': case ';': case '!': case '~':
        puLen = 1;
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
                     source_.substr(puStart, puLen), SourceLocation{}};
    }

    // Unrecognized character. Once char/string literals (§18) arrive, `'` and
    // `"` will be handled before this fallback; for now they still land here.
    std::size_t start = position_;
    ++position_;
    diagnostics_.report(Severity::Error, SourceLocation{},
                        "unexpected character");
    return Token{TokenKind::Unknown, source_.substr(start, 1), SourceLocation{}};
}

} // namespace bezel
