#pragma once

// Lexer: turns source text into a token stream. Derives from §1 (Lexical
// Structure), §18 (Literals and Strings), §34 (Comments), and Appendix B
// (Reserved Words). No lexing is implemented yet; see the lexer milestone.

#include <cstddef>
#include <string_view>

#include "lexer/Token.hpp"
#include "support/Diagnostic.hpp"

namespace bezel {

// §18.3 (v0.334): a char literal is `'` CharContent `'`, and `CharContent`
// is one Unicode scalar value or one escape sequence — so a COMPLETE char
// literal is exactly an opening quote, one CharContent, and a closing
// quote. Returns the offset one past that closing quote, or `npos` when
// the text at `open` does not begin a complete char literal.
//
// §18.5's hole-delimitation rule (v0.334) requires the lexer to "skip
// nested string/char literals (recognized by their own opening quote and
// parsed as complete literals; they do not terminate the outer string)".
// "Parsed as complete literals" is what makes this the right test: a lone
// apostrophe that never closes before the hole's `}` is not a nested
// literal opener, it is content (a format spec's literal separator, as in
// the `HH'` of `{d:HH'}`). Shared with the parser, which re-scans the same
// hole text when splitting the token into fragments (see
// `Parser::splitInterpolation`).
std::size_t charLiteralEnd(std::string_view src, std::size_t open);

class Lexer {
public:
    Lexer(std::string_view source, DiagnosticEngine& diagnostics);

    // §18.5 (v0.306, #4517): same as above, but every SourceLocation this
    // lexer produces is rebased onto `base` instead of starting at {1, 1, 0}.
    // Used to sub-lex a string-interpolation hole's expression text (a
    // standalone copy, disconnected from the enclosing file's buffer) so its
    // tokens — and any diagnostics reported against them — carry the hole's
    // real position in the file instead of "1:1" of the hole's own text.
    // `base` is the location of the sub-source's first character; locations
    // at further offsets are computed by walking the sub-source from there,
    // same as `locationAt` always has, just with a non-origin starting point.
    Lexer(std::string_view source, DiagnosticEngine& diagnostics,
          SourceLocation base);

    // Returns the next token, or an EndOfFile token once the source is
    // exhausted.
    Token next();

private:
    Token scanCharLiteral();
    Token scanStringLiteral();
    Token scanVerbatimString();
    // Slice 51: `$"..."` and `$@"..."` interpolated-string literals (§18.5/
    // §18.7). The scan tracks `{` / `}` balanced runs inside the string so
    // `{ ... }` interpolation parts are taken as opaque (the parser will
    // re-tokenize them when fragment splitting lands). `{{` and `}}` are
    // literal-brace escapes.
    Token scanInterpolatedString();
    Token scanInterpolatedVerbatimString();
    void consumeEscape();

    // Convert a byte offset within `source_` to a 1-based line/column
    // SourceLocation. Lazy O(N) walk: we don't track line/column as the lexer
    // advances; simplicity beats speed per CLAUDE.md until profiling says
    // otherwise. Convention (not in the spec): line and column are 1-based;
    // CR, LF, and CRLF each advance the line by one (CRLF is one line break,
    // not two); tabs advance the column by one (no expansion).
    // §18.5 (v0.306, #4517): the walk starts from `base_` rather than always
    // {1, 1, 0}, so a sub-lexer constructed with a non-default base rebases
    // every location it produces onto the enclosing source.
    SourceLocation locationAt(std::size_t offset) const;

    std::string_view source_;
    DiagnosticEngine& diagnostics_;
    std::size_t position_ = 0;
    SourceLocation base_{1, 1, 0};
};

} // namespace bezel
