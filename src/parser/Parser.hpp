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

    // `>>` consumed by the lexer as one Punctuator must be split when it
    // closes nested generic argument lists (e.g. `List<List<int>>`). This
    // helper consumes a single closing `>`, splitting `>>` / `>>=` in place
    // so the leftover stays in `current_` for the outer scope. Returns false
    // when no closing angle is present.
    bool consumeClosingAngle();

    // Grammar productions.
    std::unique_ptr<ast::ModuleDecl> parseModuleDecl();
    std::unique_ptr<ast::Import> parseImport();
    std::unique_ptr<ast::TopDecl> parseTopDecl();
    std::unique_ptr<ast::TypeDecl> parseTypeOrInterfaceDecl(
        ast::Visibility visibility, std::vector<std::string_view> modifiers);
    std::unique_ptr<ast::TypeAlias> parseTypeAlias();
    // Slice 25.
    std::unique_ptr<ast::TopDecl> parseExtensionDecl(ast::Visibility visibility);
    std::unique_ptr<ast::TopDecl> parseExternBlock();
    std::unique_ptr<ast::Type> parseType();
    // Like parseQualifiedName, but the first component may be a primitive-
    // type keyword (`int`, `string`, `bool`, …). Used only in type position;
    // module and import names go through parseQualifiedName.
    std::unique_ptr<ast::QualifiedName> parseTypeName();
    // Slice 5: generic parameter list `<` (Variance? Identifier) (`,` ...)* `>`.
    // Empty vector if no `<...>` is present at the current position.
    std::vector<ast::GenericParam> parseGenericParams();
    // Slice 5: inheritance clause `:` Type ("," Type)*. Optional
    // base-class argument list `(...)` after each item is currently skipped
    // (it contains expressions; see the inheritance slice notes in the PR).
    std::vector<std::unique_ptr<ast::Type>> parseInheritance();
    // Slice 24: zero or more `where T: Constraint, ...` clauses.
    std::vector<ast::GenericConstraint> parseGenericConstraints();
    // Slice 6/10: a single class member. FieldDecl (with optional initializer)
    // and MethodDecl (with expression/block/abstract body); other Member forms
    // arrive with their slices.
    std::unique_ptr<ast::Member> parseMember();
    std::unique_ptr<ast::MethodDecl> parseMethodDecl(
        ast::Visibility visibility, std::vector<std::string_view> modifiers);
    std::vector<ast::Param> parseParamList();
    // Slice 11: a single statement. Dispatches on the first token: `var` /
    // `using` for LocalDecl, `return` for ReturnStmt, otherwise an
    // expression that may be the start of an AssignmentStmt or an
    // ExpressionStmt.
    std::unique_ptr<ast::Stmt> parseStatement();
    // Slice 7: primary expressions plus postfix `.member` and `(args)`.
    // Slice 8: precedence-climbing binary expressions wrap primary+postfix.
    // Slice 9: unary prefix sits between binary and postfix.
    std::unique_ptr<ast::Expr> parseExpression();
    std::unique_ptr<ast::Expr> parseBinaryExpression(int minPrecedence);
    std::unique_ptr<ast::Expr> parseUnaryExpression();
    std::unique_ptr<ast::Expr> parsePrimaryExpression();
    std::unique_ptr<ast::Expr> parsePostfix(std::unique_ptr<ast::Expr> head);
    std::unique_ptr<ast::QualifiedName> parseQualifiedName();

    Lexer& lexer_;
    DiagnosticEngine& diagnostics_;
    Token current_{};
};

} // namespace bezel
