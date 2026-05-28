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
    // TODO(parser milestone slice 1, §2.1): parse module declaration.
    // Returns an empty SourceFile so tests-first assertions fail meaningfully
    // (e.g. `module == nullptr`) rather than crashing on a null result.
    return std::make_unique<ast::SourceFile>();
}

std::unique_ptr<ast::ModuleDecl> Parser::parseModuleDecl() {
    // TODO(parser milestone slice 1, §2.1).
    return nullptr;
}

std::unique_ptr<ast::QualifiedName> Parser::parseQualifiedName() {
    // TODO(parser milestone slice 1, §2.1).
    return nullptr;
}

} // namespace bezel
