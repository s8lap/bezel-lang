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
    // Operator, punctuation, and literal kinds are added by their own lexer
    // sub-milestones (§17, §18); see tests/lexer.
    Unknown,
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string_view text;
    SourceLocation location;
};

} // namespace bezel
