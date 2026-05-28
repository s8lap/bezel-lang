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

    // §2.4: top-level declarations. Slice 3 covers empty class/interface/
    // record/enum heads only; extension declarations (§26), type aliases,
    // and extern blocks land in later slices.
    while (current_.kind != TokenKind::EndOfFile) {
        auto decl = parseTopDecl();
        if (decl) {
            sourceFile->topDecls.push_back(std::move(decl));
        } else if (current_.kind != TokenKind::EndOfFile) {
            // parseTopDecl already reported; consume one token to make
            // progress and try again. Better recovery comes when we have a
            // notion of "synchronization tokens" (semicolons / `end`).
            advance();
        }
    }

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

std::unique_ptr<ast::TypeDecl> Parser::parseTopDecl() {
    // Grammar A.3 (ClassDecl/InterfaceDecl/RecordDecl/EnumDecl):
    //   Visibility? Modifier* keyword Identifier ... ClassBody
    // Slice 3 limits the body to the empty form `end Identifier? ;`.

    // §13 visibility keyword (optional, must precede modifiers).
    ast::Visibility visibility = ast::Visibility::None;
    if (isKeyword("public")) {
        visibility = ast::Visibility::Public;
        advance();
    } else if (isKeyword("private")) {
        visibility = ast::Visibility::Private;
        advance();
    } else if (isKeyword("internal")) {
        visibility = ast::Visibility::Internal;
        advance();
    } else if (isKeyword("protected")) {
        visibility = ast::Visibility::Protected;
        advance();
    }

    // Grammar `Modifier* = ("abstract" | "sealed" | "static")*`. The set of
    // semantically valid combinations is constrained by §11.7 (sema's
    // problem); the parser admits any combination per the grammar.
    std::vector<std::string_view> modifiers;
    while (isKeyword("abstract") || isKeyword("sealed") || isKeyword("static")) {
        modifiers.push_back(current_.text);
        advance();
    }

    // The keyword that selects which kind of top-level declaration this is.
    ast::TypeDeclKind kind;
    if (isKeyword("class"))          kind = ast::TypeDeclKind::Class;
    else if (isKeyword("interface")) kind = ast::TypeDeclKind::Interface;
    else if (isKeyword("record"))    kind = ast::TypeDeclKind::Record;
    else if (isKeyword("enum"))      kind = ast::TypeDeclKind::Enum;
    else {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected top-level declaration (class, interface, record, enum)");
        return nullptr;
    }
    advance(); // past the kind keyword

    auto decl = std::make_unique<ast::TypeDecl>();
    decl->kind = kind;
    decl->visibility = visibility;
    decl->modifiers = std::move(modifiers);

    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected type name");
    } else {
        decl->name = current_.text;
        advance();
    }

    // §1.6 named-construct termination: `end Identifier? ;`. The optional
    // identifier must match the declared name exactly when supplied.
    if (!consumeKeyword("end")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'end'");
        return decl;
    }
    if (current_.kind == TokenKind::Identifier) {
        decl->endLabel = current_.text;
        if (!decl->name.empty() && decl->endLabel != decl->name) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "end label does not match the declared type name");
        }
        advance();
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end'");
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
