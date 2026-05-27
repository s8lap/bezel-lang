#pragma once

// Parser: turns a token stream into an AST. Derives from the grammar in
// Appendix A and the construct-level rules across §9–§27. Nothing is parsed
// yet; see the parser milestone.

#include <memory>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "support/Diagnostic.hpp"

namespace bezel {

class Parser {
public:
    Parser(Lexer& lexer, DiagnosticEngine& diagnostics);

    // Parses a complete compilation unit (§2). Returns null until the parser
    // milestone implements the grammar.
    std::unique_ptr<ast::Node> parseCompilationUnit();

private:
    Lexer& lexer_;
    DiagnosticEngine& diagnostics_;
};

} // namespace bezel
