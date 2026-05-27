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
    std::string_view source_;
    DiagnosticEngine& diagnostics_;
    std::size_t position_ = 0;
};

} // namespace bezel
