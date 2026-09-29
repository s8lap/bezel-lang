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

    // §1.3 / §17.13 / A.1 / B.1 (v0.244, proposal 0083; #3049): the bare `_`.
    // A.1 splits the Identifier production (`Letter IdentCont* | "_"
    // IdentCont+`), so the lexer hands the bare token back as its own kind and
    // NO declaration-name slot can ever match it. `atDiscard()` is the test.
    bool atDiscard() const { return current_.kind == TokenKind::Discard; }
    // Report `_` in a declaration-name slot and CONSUME it (so one `_` does
    // not cascade into a run of follow-on syntax errors, exactly as the
    // Appendix-B.1 keyword-in-a-name-slot recovery does). `what` names the
    // slot ("field", "parameter", …); `remedy` replaces the default
    // "choose a name" tail when the position has a specific sanctioned form.
    // §17.13's rejected-position list requires the diagnostic to name the
    // sanctioned forms, which is what `remedy` carries.
    void reportDiscardNotDeclarable(std::string_view what,
                                    std::string_view remedy = {});
    // A.6 `DiscardStmt ::= "_" ":=" Expression ";"` (§17.13). current_ is the
    // `_` token; §17.13 states the form "is recognized lexically by the
    // leading `_` token", which since v0.244 is the ONLY way to reach it —
    // `_` is not an expression, so no leading-expression recovery exists.
    // `semicolonOptional` is set in case/when arm-body position, where §19.7
    // lets `,`/`end` terminate the arm directly. Returns null (having consumed
    // the `_`) when what follows is not `:=`.
    std::unique_ptr<ast::Stmt> parseDiscardStmt(bool semicolonOptional);

    // `>>` consumed by the lexer as one Punctuator must be split when it
    // closes nested generic argument lists (e.g. `List<List<int>>`). This
    // helper consumes a single closing `>`, splitting `>>` / `>>=` in place
    // so the leftover stays in `current_` for the outer scope. Returns false
    // when no closing angle is present.
    bool consumeClosingAngle();

    // §1.6 / v0.170 (#1374): uniform-closer keyword label. Call immediately
    // after `consumeKeyword("end")` for a keyword-labelled construct. If the
    // current token is `expected`, it is consumed (label matches); if it is
    // any other keyword from the known end-label set, a mismatch diagnostic
    // is reported and it is consumed for error recovery; otherwise the token
    // is left untouched (bare `end;` form — always valid).
    void consumeOptionalEndKeyword(std::string_view expected);

    // §1.6 / §21.5 (v0.201, #2051): the closer of a `while`/`for`/`await for`.
    // Call after `consumeKeyword("end")`. `keyword` is the loop's introducing
    // keyword (`"while"`/`"for"`); `loopLabel` is its loop label (empty when
    // unlabeled). A loop with a label is a NAMED construct: its closer is the
    // label (`end scan;`) or bare `end;`, and the keyword closer is rejected; an
    // unlabeled loop takes the keyword closer (`end for;`) or bare `end;`, and an
    // identifier closer is rejected. Mismatches report a fix-naming diagnostic
    // and consume the token for error recovery.
    void consumeOptionalLoopEndLabel(std::string_view keyword,
                                     std::string_view loopLabel);

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
    // Slice 5: inheritance clause `:` Type ("," Type)*. `decl` is the
    // enclosing declaration being parsed (kind + name only are used here).
    // For a `Class` decl reaching this function — guaranteed to have NO
    // primary-constructor parameter list, since a primary-ctor class is
    // parsed by the caller's separate `hasPositionalFields` branch — an
    // argument list `(...)` on any inheritance item is rejected with a
    // diagnostic (§11.1 / §22.10 / §12.8, v0.235; issue #2455): the header
    // argument-list shorthand exists to feed a primary constructor's
    // `base(...)` chain, and a class without one gets an implicit
    // no-argument `base()` (§12.8) that cannot receive these arguments.
    // Interface and record decls keep the original tokenize-away behavior
    // unchanged (out of #2455's scope; see the .cpp for the tracking note).
    std::vector<std::unique_ptr<ast::Type>> parseInheritance(
        const ast::TypeDecl* decl);
    // Slice 24: zero or more `where T: Constraint, ...` clauses.
    std::vector<ast::GenericConstraint> parseGenericConstraints();
    // §22.16 (v0.203, #2053): zero or more contract clauses
    // (A.4 `ContractClause ::= "static"? ("requires"|"ensures") Expression ";"`)
    // in the clause region of a method or constructor. Returns whatever
    // clauses were parsed; the marker-wins rule (App. B.2 — `requires`/
    // `ensures` are contextual keywords whose keyword reading always wins
    // in this region) means a `requires`/`ensures` Identifier always opens a
    // clause here, never an expression. The clause region ends at the first
    // non-clause token (the body's `=>` / `;` / statement-starter, or the
    // enclosing scope's continuation for a clause-terminated body-less
    // member).
    std::vector<ast::ContractClause> parseContractClauses();
    // §22.16 (v0.203, #2053): does the token stream at current_ look like the
    // start of a contract clause (`requires` / `ensures`, optionally preceded
    // by `static`)? Used to gate entry into the clause region without
    // committing (so existing zero-clause methods continue to parse exactly
    // as before). Non-const because the two-token lookahead for `static
    // requires|ensures` uses the lazily-populating peekAt buffer.
    bool currentTokenStartsContractClause();
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
    // fix #2418: `memberStart` (default {} for call sites with none to give,
    // e.g. none today) anchors OperatorDecl::span.begin, mirroring
    // parseMethodDeclBody's #783 convention, so SymbolCollection's
    // MethodSymbol::declLoc (operators reuse MethodSymbol) has a real
    // location to read instead of the default {0,0}.
    std::unique_ptr<ast::Member> parseOperatorDeclBody(
        ast::Visibility visibility, std::vector<std::string_view> modifiers,
        SourceLocation memberStart = {});
    std::vector<ast::Param> parseParamList();
    // Slice 41: A.3 PositionalField list — `Identifier ":" Type
    // SerializedClause?`. Distinct from ParamList: no modifiers, no default
    // value, but admits a SerializedClause per field.
    std::vector<ast::Param> parsePositionalFieldList();
    // §A.3 / §35.1 (v0.200, #2044): `CaseField ::= "owned"? Identifier ":"
    // Type SerializedClause?` — PositionalField plus the optional leading
    // `owned` marker (stored in `Param::modifier`).
    std::vector<ast::Param> parseCasePayloadList();
    // Slice 49: A.4 ArgList — `Argument ("," Argument)*`. Caller is
    // positioned at the first token of the first argument; the closing `)`
    // is left for the caller to consume.
    std::vector<ast::CallArgument> parseArgList();
    // Slice 36: a single LambdaParam (A.7). Differs from method ParamList
    // in that the `: Type` ascription is optional.
    ast::Param parseLambdaParam();
    // §27.4 (v0.183, #1408) / A.7 `LambdaDestructure ::= QualifiedName "("
    // LambdaSlot ("," LambdaSlot)* ")"`. Caller (parseLambdaParam) has
    // confirmed via tokensLookLikeLambdaDestructure() that the parser is
    // positioned at the QualifiedName head. Reuses parsePositionalPattern
    // for slot parsing (same spelling as §19.3 case-pattern destructuring),
    // then validates every slot — recursively, through nested destructuring —
    // is one of the three irrefutable forms A.7 admits (naked binding, `_`,
    // nested LambdaDestructure), reporting a dedicated diagnostic for any
    // other pattern shape (constant/range/relational/type-test/`var`-form/
    // alternative — all refutable, and a parameter list is not a `case`).
    std::unique_ptr<ast::DestructuringPattern> parseLambdaDestructure();
    void validateLambdaDestructureSlots(const ast::DestructuringPattern& dp);
    // Lookahead: does the token stream starting at current_ look like a
    // LambdaDestructure head — `QualifiedName "("`? Used to discriminate the
    // two LambdaParam alternatives before committing to either parse path.
    bool tokensLookLikeLambdaDestructure();
    // Slice 37: peek-style helper — is the parser positioned at the start
    // of a `get`/`set` accessor (with optional Visibility prefix)?
    bool isPropertyAccessorStart();
    // Slice 37: parse one Accessor within a PropertyBody block form.
    ast::PropertyAccessor parsePropertyAccessor();
    // Slice 38: patterns and case expressions (§A.7).
    // parsePattern parses an AltPattern (§A.7, v0.167 #1371): a primary
    // pattern optionally followed by `|`-joined constant alternatives.
    // parsePrimaryPattern parses one non-alternative pattern. inPositional is
    // threaded through so a naked-binding Identifier can be recognized in a
    // destructuring slot.
    std::unique_ptr<ast::Pattern> parsePattern(bool inPositional = false);
    std::unique_ptr<ast::Pattern> parsePrimaryPattern(bool inPositional = false);
    // §19.3 / §A.7 (v0.167, #1371): fold one already-parsed primary pattern
    // into an alternatives ConstantPattern as a constant member, rejecting a
    // structurally non-constant member with the §19.3 membership diagnostic.
    void appendAlternativeMember(ast::ConstantPattern& cp,
                                 std::unique_ptr<ast::Pattern> p);
    // parseCaseArm: when `scrutineeLess` is true the arm "pattern" is a
    // full boolean expression (§19.1 scrutinee-less form); the Pattern
    // grammar is bypassed and parseExpression() is used directly, wrapping
    // the result in a ConstantPattern so the arm's pattern field is always
    // non-null. Issue #657 / spec_0026, spec_0020, spec_0100.
    ast::CaseArm parseCaseArm(bool scrutineeLess = false);
    // #744 / §19: `case` always takes a scrutinee (the scrutinee-less form is
    // now `when`); no source-line heuristic is consulted.
    // §19.1/§19.7 (v0.208, #2079): the expression form is the parenthesized
    // ATOM `(case … of arm, …)` — `parenAtom` true means the caller consumed
    // the opening `(` and the closer is `)`; false means the RETIRED bare
    // `case … end` expression spelling reached expression position — the
    // targeted migration diagnostic is reported and the old form is parsed
    // for recovery.
    std::unique_ptr<ast::Expr> parseCaseExpr(bool parenAtom);
    // §19.1 / A.7 CaseExpr (v0.166, #1370): consume the `of` delimiter that
    // terminates the case scrutinee, reporting the missing-`of` diagnostic
    // otherwise. `of` is a CONTEXTUAL keyword — it is not in Appendix B's
    // reserved-word list, so it is lexed as an ordinary identifier and
    // recognized only in this position; identifiers named `of` elsewhere are
    // unaffected. Returns true when the delimiter was present.
    bool expectOfDelimiter();
    // #744 / §19: `when` is the dedicated scrutinee-less condition-dispatch
    // construct (arms are boolean conditions; the first true wins). It reuses
    // the null-scrutinee CaseExpr representation. `parenAtom` as for
    // parseCaseExpr (v0.208, #2079).
    std::unique_ptr<ast::Expr> parseWhenExpr(bool parenAtom);
    // §20 / A.7 IfExpr (v0.208, #2079): the conditional atom
    // `(if c then a elif c2 then b else d)`. The caller consumed the opening
    // `(` and current_ is the `if` keyword — the reading is committed ("("
    // followed by reserved `if` begins no other expression). Produces the
    // (right-nested, for elif chains) ConditionalExpr representation the
    // retired postfix spelling produced, so sema/ownership/codegen are
    // unchanged (v0.208: semantics untouched).
    std::unique_ptr<ast::Expr> parseIfExprAtom();
    // §17.15 / A.7 BindExpr (v0.216, #2098): the binding atom
    // `(var x := E; var y := E2; body)`. The caller consumed the opening `(`
    // and current_ is the reserved `var` — which begins no other expression,
    // so the reading is committed at the second token like the other paren
    // atoms. Clauses `var x (":" Type)? ":=" Expr ";"` chain while the next
    // token is `var` (at least one is guaranteed by the dispatch); the body
    // is one full Expression; the atom supplies its own `)`. Clause
    // initializers admit the `owned` binding-position transfer marker
    // (K3.4, §28.3); the body position does not (§17.15).
    std::unique_ptr<ast::Expr> parseBindExprAtom();
    // §20 (v0.208, #2079): retired-postfix-conditional diagnostic — the
    // sibling of checkRetiredParenLambda for `x if c else y`. Called at the
    // same generic-error sites; when the unexpected token is `if` and an
    // error-path scan finds a top-level `else` before `then`/`do`/`;`, the
    // targeted migration diagnostic is reported and the `if c else y` tail
    // is swallowed. Returns true when it fired.
    bool checkRetiredPostfixConditional();
    // Umbrella for the v0.208/v0.217 retired-spelling checks at statement/
    // argument error sites.
    bool checkRetiredExprSpelling();
    // The positional sub-pattern of `Name(p1, p2)` destructuring. Mostly
    // delegates to parsePattern, but also admits naked-binding Identifier
    // per A.7.
    ast::PositionalPattern parsePositionalPattern();
    // `qualifiedNameToExpr` rebuilds a QualifiedName as the corresponding
    // Identifier/MemberAccess expression chain (used to fold a NamedPattern
    // into a ConstantPattern alternative member).
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
    // §9.2 / fix #4643: parse `equality skip` (A.4 EqualityClause). The
    // caller checks `isEqualityClauseStart()` first — `equality` is
    // contextual (App. B.2) so we identify by text. Only the `skip` form
    // exists (no name, no `optional`); the parser rejects other forms
    // (§9.2 line 1385 — vocabulary is one word, closed).
    bool isEqualityClauseStart() const;
    std::unique_ptr<ast::EqualityClause> parseEqualityClause();
    // §9.8 v0.141 (P6): class-body `polymorphic("disc": Type, ...);` member
    // clause — populates decl.serializedClass->polyCases.
    void parsePolymorphicMember(ast::TypeDecl& decl);
    // fix #4222 (§26.3, Appendix A.4, v0.298): `parseExtensionDecl`'s member
    // loop's own variant — an extension body has no `TypeDecl` for
    // `parsePolymorphicMember` to mutate (`decl.serializedClass`), so a
    // `polymorphic(...)` clause inside an extension is parsed-and-discarded
    // here instead, emitting exactly ONE §26.3-citing diagnostic rather than
    // falling through to `parseMember()`'s generic member-kind dispatch
    // (which has no arm for a bare `polymorphic` identifier and previously
    // cascaded one "expected member visibility ... or member-kind keyword"
    // diagnostic per stray token — measured: 7 for one clause).
    void parseAndDiscardExtensionPolymorphicMember();
    // §27.3 / A.7 (v0.217, #2101): retired-lambda-spelling diagnostics. The
    // keyword-led form needs NO lookahead (the pre-v0.217
    // `tokensLookLikeLambda` scan-to-`)` — the parser's one unbounded
    // buffered lookahead — is deleted with the old spelling); these helpers
    // exist purely to turn the parse errors an old-spelling `(params) =>
    // body` now produces into one targeted "insert `lambda`" diagnostic.
    //
    // checkRetiredParenLambda: called at generic-error sites (a missing `;`
    // after a statement, a missing `,`/`)` after a call argument) — if the
    // unexpected token is `=>`, report the targeted diagnostic, swallow the
    // arrow and the lambda body, and return true (the caller then skips its
    // generic error). Never called from arm/guard contexts, where an
    // expression legitimately precedes `=>`.
    bool checkRetiredParenLambda();
    // retiredLambdaHeadAhead: ERROR-PATH-ONLY scan — from inside an already-
    // failed parenthesized expression, peek to the matching `)` and test for
    // `=>`. Runs only when the paren-expression parse has already failed at
    // `,`/`:` (a multi-param or type-ascribed retired head), so it is a
    // diagnostic-quality aid, not a dispatch lookahead.
    bool retiredLambdaHeadAhead();
    // skipRetiredLambdaBody: consume and discard the body after a retired
    // `=>` (an expression, or `do … end`), so one old lambda yields one
    // diagnostic instead of a cascade.
    void skipRetiredLambdaBody();
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

    // Bounded guard on expression-recursion depth (fix #4348). A
    // parenthesized sub-expression recurses through `parseExpression()` from
    // `parsePrimaryExpressionImpl()`'s `(` arm — without a depth bound,
    // deeply nested but valid source overflows the native call stack and
    // SIGSEGVs the compiler. Each `parseExpressionImpl()` entry bumps this
    // counter and bails with a located diagnostic once it exceeds the cap
    // (no real Bezel program gets anywhere near it). Reset at the start of
    // `parseCompilationUnit()` so a parse run starts clean.
    static constexpr std::size_t kMaxExpressionRecursion = 1000;
    std::size_t expressionRecursionDepth_ = 0;

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
    // (The pre-v0.217 `suppressTopLevelLambda_` when-guard carve (#1256) is
    // deleted: a keyword-led lambda (§27.3, v0.217) commits at `lambda`, so
    // a parenthesized guard followed by the arm `=>` can never be mis-read
    // as a lambda head.)
};

} // namespace bezel
