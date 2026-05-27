#include "lexer/Lexer.hpp"

namespace bezel {

Lexer::Lexer(std::string_view source, DiagnosticEngine& diagnostics)
    : source_(source), diagnostics_(diagnostics) {}

Token Lexer::next() {
    // TODO(lexer milestone): scan a real token per §1. The skeleton lexer
    // consumes the whole source and immediately reports end-of-file.
    position_ = source_.size();
    return Token{TokenKind::EndOfFile, {}, SourceLocation{}};
}

} // namespace bezel
