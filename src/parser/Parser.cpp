#include "parser/Parser.hpp"

namespace bezel {

Parser::Parser(Lexer& lexer, DiagnosticEngine& diagnostics)
    : lexer_(lexer), diagnostics_(diagnostics) {}

std::unique_ptr<ast::Node> Parser::parseCompilationUnit() {
    // TODO(parser milestone): implement the grammar (Appendix A).
    return nullptr;
}

} // namespace bezel
