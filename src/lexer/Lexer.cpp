#include "lexer/Lexer.hpp"

#include <cstddef>
#include <string_view>

namespace bezel {

namespace {

bool isAsciiLetter(unsigned char b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z');
}

bool isAsciiDigit(unsigned char b) { return b >= '0' && b <= '9'; }

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

    // Unrecognized character: consume one byte and emit an Unknown token plus
    // a diagnostic. Operators/punctuation are added by their own milestone, at
    // which point most "unexpected" bytes become real tokens.
    std::size_t start = position_;
    ++position_;
    diagnostics_.report(Severity::Error, SourceLocation{},
                        "unexpected character");
    return Token{TokenKind::Unknown, source_.substr(start, 1), SourceLocation{}};
}

} // namespace bezel
