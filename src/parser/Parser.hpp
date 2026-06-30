#pragma once

// Parser: turns a token stream into an AST. Derives from the grammar in
// Appendix A and the construct-level rules across §9–§27.
//
// Recursive descent with one token of lookahead. The parser pulls tokens from
// the Lexer on demand via `advance()`; the next-to-be-consumed token lives in
// `current_`. Errors are reported to the diagnostic engine and the parser
// attempts to recover; for slice 1 (module declarations) recovery is minimal.

#include <cstddef>
#include <deque>
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
    // Multi-token lookahead. `peekAt(0)` is `current_`; `peekAt(k)` returns
    // the k-th token beyond it. The buffer is lazily filled and drained by
    // `advance()`. Use sparingly: the parser is one-token-lookahead almost
    // everywhere; buffered lookahead exists for constructs that can't be
    // disambiguated locally (e.g. `(...) => …` lambda vs. parenthesised
    // expression, see parsePrimaryExpression).
    const Token& peekAt(std::size_t k);
    const Token& peekNext() { return peekAt(1); }
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
    // Slice 6/10/19/...: a single class member.
    std::unique_ptr<ast::Member> parseMember();
    // Slice 34: a single interface member (A.4 InterfaceMember). Smaller and
    // different grammar than class Member — no visibility, signature only.
    std::unique_ptr<ast::Member> parseInterfaceMember();
    // Caller has already consumed the `method` keyword.
    // #783: `memberStart` is the location of the member's first token (so the
    // builder can slice the full method-declaration text for cross-module
    // generic-method body export); the span is stamped here because only this
    // function knows where the body terminates.
    std::unique_ptr<ast::MethodDecl> parseMethodDeclBody(
        ast::Visibility visibility, std::vector<std::string_view> modifiers,
        SourceLocation memberStart = {});
    // Caller has already consumed `method operator`.
    std::unique_ptr<ast::Member> parseOperatorDeclBody(
        ast::Visibility visibility, std::vector<std::string_view> modifiers);
    std::vector<ast::Param> parseParamList();
    // Slice 41: A.3 PositionalField list — `Identifier ":" Type
    // SerializedClause?`. Distinct from ParamList: no modifiers, no default
    // value, but admits a SerializedClause per field.
    std::vector<ast::Param> parsePositionalFieldList();
    // Slice 49: A.4 ArgList — `Argument ("," Argument)*`. Caller is
    // positioned at the first token of the first argument; the closing `)`
    // is left for the caller to consume.
    std::vector<ast::CallArgument> parseArgList();
    // Slice 36: a single LambdaParam (A.7). Differs from method ParamList
    // in that the `: Type` ascription is optional.
    ast::Param parseLambdaParam();
    // Slice 37: peek-style helper — is the parser positioned at the start
    // of a `get`/`set` accessor (with optional Visibility prefix)?
    bool isPropertyAccessorStart();
    // Slice 37: parse one Accessor within a PropertyBody block form.
    ast::PropertyAccessor parsePropertyAccessor();
    // Slice 38: patterns and case expressions (§A.7).
    // inPositional=true suppresses the ConstantPattern `,` alternatives loop
    // to avoid consuming the sub-pattern separator in DestructuringPatterns.
    std::unique_ptr<ast::Pattern> parsePattern(bool inPositional = false);
    // parseCaseArm: when `scrutineeLess` is true the arm "pattern" is a
    // full boolean expression (§19.1 scrutinee-less form); the Pattern
    // grammar is bypassed and parseExpression() is used directly, wrapping
    // the result in a ConstantPattern so the arm's pattern field is always
    // non-null. Issue #657 / spec_0026, spec_0020, spec_0100.
    ast::CaseArm parseCaseArm(bool scrutineeLess = false);
    // #744 / §19: `case` always takes a scrutinee (the scrutinee-less form is
    // now `when`); no source-line heuristic is consulted.
    std::unique_ptr<ast::Expr> parseCaseExpr();
    // #744 / §19: `when` is the dedicated scrutinee-less condition-dispatch
    // construct (arms are boolean conditions; the first true wins). It reuses
    // the null-scrutinee CaseExpr representation.
    std::unique_ptr<ast::Expr> parseWhenExpr();
    // The positional sub-pattern of `Name(p1, p2)` destructuring. Mostly
    // delegates to parsePattern, but also admits naked-binding Identifier
    // per A.7.
    ast::PositionalPattern parsePositionalPattern();
    // Slice 44: ConstantPattern alternatives (`1, 2, 3` and
    // `Color.Red, Color.Blue`). `isConstantStart` returns true for a token
    // that could begin an A.8 Constant (literal or non-`_` Identifier).
    // `parseConstantExpr` parses one Constant; `qualifiedNameToExpr`
    // rebuilds a QualifiedName as the corresponding Identifier/MemberAccess
    // expression chain.
    bool isConstantStart(const Token& t) const;
    std::unique_ptr<ast::Expr> parseConstantExpr();
    std::unique_ptr<ast::Expr> qualifiedNameToExpr(
        std::unique_ptr<ast::QualifiedName> qn);
    // Slice 39: statement-position `case` (A.6). Sibling of parseCaseExpr;
    // arms have Expression-form, Statement-form, or `do … end` bodies.
    std::unique_ptr<ast::Stmt> parseCaseStmt();
    // #744 / §19: statement-position `when` (scrutinee-less condition dispatch).
    std::unique_ptr<ast::Stmt> parseWhenStmt();
    ast::CaseArm parseCaseStmtArm(bool scrutineeLess = false);
    // Slice 41: parse `serialized ( "skip" | StringLiteral? "optional"? )`
    // (A.4 SerializedClause). The caller checks `isSerializedClauseStart()`
    // first — `serialized` is contextual so we identify by text.
    bool isSerializedClauseStart() const;
    std::unique_ptr<ast::SerializedClause> parseSerializedClause();
    // §9.8 v0.141 (P6): class-body `polymorphic("disc": Type, ...);` member
    // clause — populates decl.serializedClass->polyCases.
    void parsePolymorphicMember(ast::TypeDecl& decl);
    // Slice 36: peeks the matching `)` for the `(` at peekAt(startK) and
    // returns true iff a `=>` follows. Used to distinguish
    // `( LambdaParams? ) => …` from a parenthesised expression `( EXPR )`.
    bool tokensLookLikeLambda(std::size_t startK);
    // per §14.1: disambiguates `Method<T>(…)` (method-level type args) from
    // `expr < T > (expr)` (relational comparison chain). peekAt(0) is the `<`
    // token. Returns true iff the `<` starts a generic-arg list that closes
    // with `>` followed immediately by `(` — the only context in which the
    // programmer writes explicit method type args. Uses bounded lookahead;
    // bails out (returns false) on EOF or on tokens that cannot appear inside
    // a type-argument list.
    bool tokensLookLikeMethodTypeArgs();
    // per §14.1 / §22.12 (issue #520): disambiguates `GenericClass<T>.Member`
    // (receiver-position explicit type args) from `expr < T > (expr)`.
    // Like tokensLookLikeMethodTypeArgs() but requires `>` to be followed by
    // `.` (dot) rather than `(` — the dot being the static-member access.
    bool tokensLookLikeReceiverTypeArgs();
    // Slice 11: a single statement. Dispatches on the first token: `var` /
    // `using` for LocalDecl, `return` for ReturnStmt, otherwise an
    // expression that may be the start of an AssignmentStmt or an
    // ExpressionStmt.
    std::unique_ptr<ast::Stmt> parseStatement();
    std::unique_ptr<ast::Stmt> parseStatementImpl();
    // Slice 7: primary expressions plus postfix `.member` and `(args)`.
    // Slice 8: precedence-climbing binary expressions wrap primary+postfix.
    // Slice 9: unary prefix sits between binary and postfix.
    std::unique_ptr<ast::Expr> parseExpression();
    std::unique_ptr<ast::Expr> parseExpressionImpl();
    std::unique_ptr<ast::Expr> parseBinaryExpression(int minPrecedence);
    std::unique_ptr<ast::Expr> parseUnaryExpression();
    std::unique_ptr<ast::Expr> parsePrimaryExpression();
    std::unique_ptr<ast::Expr> parsePrimaryExpressionImpl();
    std::unique_ptr<ast::Expr> parsePostfix(std::unique_ptr<ast::Expr> head);
    std::unique_ptr<ast::QualifiedName> parseQualifiedName();
    // §18.5: split an interpolated string token into alternating literal
    // and expression parts. Fills `e.parts` in place.
    void splitInterpolation(ast::InterpolatedStringExpr& e);

    Lexer& lexer_;
    DiagnosticEngine& diagnostics_;
    Token current_{};
    std::deque<Token> peeked_; // see peekAt()
    // Slice 50: when true, the postfix `with` suffix (record copy-with /
    // object initializer block) is *not* consumed by parsePostfix. Used to
    // parse the iterable expression of an `await for` so its trailing
    // `with EXPR` cancellation-token clause (§30.7) stays available to the
    // for-statement parser.
    bool disableWithSuffix_ = false;
    // §19.4/§17 (#1256): when true, the very next call to
    // parsePrimaryExpressionImpl suppresses top-level `(…)=>` lambda
    // detection.  The flag is consumed (cleared) immediately on entry to
    // parsePrimaryExpressionImpl so that nested lambdas in call-argument
    // position (e.g. `xs.Any((y) => y > 0)` inside a when-guard) are
    // unaffected.  Set by parseCaseArm while parsing a when-guard expression.
    bool suppressTopLevelLambda_ = false;
};

} // namespace bezel
