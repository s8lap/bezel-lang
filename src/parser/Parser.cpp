#include "parser/Parser.hpp"

namespace bezel {

Parser::Parser(Lexer& lexer, DiagnosticEngine& diagnostics)
    : lexer_(lexer), diagnostics_(diagnostics) {
    advance(); // prime current_ with the first token
}

void Parser::advance() {
    current_ = lexer_.next();
}

bool Parser::isKeyword(std::string_view text) const {
    return current_.kind == TokenKind::Keyword && current_.text == text;
}

bool Parser::isPunctuator(std::string_view text) const {
    return current_.kind == TokenKind::Punctuator && current_.text == text;
}

bool Parser::consumeKeyword(std::string_view text) {
    if (!isKeyword(text)) return false;
    advance();
    return true;
}

bool Parser::consumePunctuator(std::string_view text) {
    if (!isPunctuator(text)) return false;
    advance();
    return true;
}

std::unique_ptr<ast::SourceFile> Parser::parseCompilationUnit() {
    auto sourceFile = std::make_unique<ast::SourceFile>();

    // §2.1: "Every source file declares its module on its first non-comment,
    // non-import line." The lexer has already skipped comments, so the very
    // first token must be the `module` keyword.
    if (isKeyword("module")) {
        sourceFile->module = parseModuleDecl();
    } else {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'module' declaration");
    }

    // §2.2: zero or more import declarations follow the module declaration.
    while (isKeyword("import")) {
        sourceFile->imports.push_back(parseImport());
    }

    // Top-level declarations (§2.4) come in a later parser slice.
    return sourceFile;
}

std::unique_ptr<ast::Import> Parser::parseImport() {
    auto decl = std::make_unique<ast::Import>();
    // Caller has verified `current_` is the `import` keyword.
    advance(); // past `import`
    decl->name = parseQualifiedName();
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after import declaration");
    }
    return decl;
}

std::unique_ptr<ast::ModuleDecl> Parser::parseModuleDecl() {
    auto decl = std::make_unique<ast::ModuleDecl>();
    // Caller has verified `current_` is the `module` keyword.
    advance(); // past `module`
    decl->name = parseQualifiedName();
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after module declaration");
    }
    return decl;
}

std::unique_ptr<ast::QualifiedName> Parser::parseQualifiedName() {
    // A.2: `QualifiedName ::= Identifier ("." Identifier)*`
    auto name = std::make_unique<ast::QualifiedName>();
    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected identifier");
        return name;
    }
    name->parts.push_back(current_.text);
    advance();
    while (isPunctuator(".")) {
        advance(); // past '.'
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected identifier after '.'");
            break;
        }
        name->parts.push_back(current_.text);
        advance();
    }
    return name;
}

} // namespace bezel
