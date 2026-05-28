#pragma once

// Tokens produced by the lexer (§1 Lexical Structure). The full token taxonomy
// — keywords (Appendix B), operators and punctuation (§17), and literal forms
// (§18) — is defined by the lexer milestone. This stub carries only the minimum
// the skeleton needs to type-check; the kinds below are NOT the complete set.

#include <string_view>

#include "support/SourceLocation.hpp"

namespace bezel {

enum class TokenKind {
    EndOfFile,
    Identifier,
    Keyword,
    // Operators and punctuation share a single Punctuator kind; the specific
    // symbol is carried in the token lexeme (`text`). Matches the keyword
    // model — per-symbol enum is deferred until the parser needs faster
    // dispatch. Literal kinds arrive with the literal slices (§18).
    Punctuator,
    Unknown,
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string_view text;
    SourceLocation location;
};

} // namespace bezel
