#pragma once

// Lexer: turns source text into a token stream. Derives from §1 (Lexical
// Structure), §18 (Literals and Strings), §34 (Comments), and Appendix B
// (Reserved Words). No lexing is implemented yet; see the lexer milestone.

#include <cstddef>
#include <string_view>

#include "lexer/Token.hpp"
#include "support/Diagnostic.hpp"

namespace bezel {

class Lexer {
public:
    Lexer(std::string_view source, DiagnosticEngine& diagnostics);

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
    SourceLocation locationAt(std::size_t offset) const;

    std::string_view source_;
    DiagnosticEngine& diagnostics_;
    std::size_t position_ = 0;
};

} // namespace bezel
