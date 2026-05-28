#pragma once

// Parser: turns a token stream into an AST. Derives from the grammar in
// Appendix A and the construct-level rules across §9–§27.
//
// Recursive descent with one token of lookahead. The parser pulls tokens from
// the Lexer on demand via `advance()`; the next-to-be-consumed token lives in
// `current_`. Errors are reported to the diagnostic engine and the parser
// attempts to recover; for slice 1 (module declarations) recovery is minimal.

#include <memory>
#include <string_view>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "lexer/Token.hpp"
#include "support/Diagnostic.hpp"

namespace bezel {

class Parser {
public:
    Parser(Lexer& lexer, DiagnosticEngine& diagnostics);

    // §2.4: parses a complete compilation unit. Always returns a non-null
    // SourceFile (even on errors) so later phases can inspect the
    // partial result.
    std::unique_ptr<ast::SourceFile> parseCompilationUnit();

private:
    // Recursive-descent primitives.
    void advance();
    const Token& peek() const { return current_; }
    bool isKeyword(std::string_view text) const;
    bool isPunctuator(std::string_view text) const;
    bool consumeKeyword(std::string_view text);
    bool consumePunctuator(std::string_view text);

    // Grammar productions.
    std::unique_ptr<ast::ModuleDecl> parseModuleDecl();
    std::unique_ptr<ast::Import> parseImport();
    std::unique_ptr<ast::TypeDecl> parseTopDecl();
    std::unique_ptr<ast::QualifiedName> parseQualifiedName();

    Lexer& lexer_;
    DiagnosticEngine& diagnostics_;
    Token current_{};
};

} // namespace bezel
