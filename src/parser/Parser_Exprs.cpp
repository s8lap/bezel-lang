#include "parser/Parser.hpp"
#include "parser/Parser_impl.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace bezel {

namespace {

// Higher precedence = tighter binding. The numeric scale is internal — we
// just need an ordering consistent with §17.1's precedence table. Returns 0
// for tokens that aren't binary operators recognised yet (deferred ops:
// `in`/`is`/`as` keyword relationals, `is not` two-token relational,
// conditional `expr if cond else expr`, assignments — assignments live in
// AssignmentStmt at statement level).
int binaryPrecedence(const Token& tok) {
    if (tok.kind == TokenKind::Punctuator) {
        const std::string_view t = tok.text;
        if (t == "*" || t == "/")  return 14; // §17.1 L3 multiplicative
        if (t == "+" || t == "-")  return 13; // §17.1 L4 additive
        if (t == "<<" || t == ">>") return 12; // §17.1 L5 shift
        if (t == ".." || t == "..<") return 11; // §17.1 L6 range
        if (t == "&")              return 10; // §17.1 L7 bitwise AND
        if (t == "^")              return 9;  // §17.1 L8 bitwise XOR
        if (t == "|")              return 8;  // §17.1 L9 bitwise OR
        if (t == "<" || t == ">" || t == "<=" || t == ">=") return 7;
        if (t == "=" || t == "<>") return 6;  // §17.1 L11 equality
        if (t == "??")             return 5;  // §17.1 L12 null-coalescing
    }
    if (tok.kind == TokenKind::Keyword) {
        const std::string_view t = tok.text;
        if (t == "mod") return 14; // multiplicative (keyword-spelled)
        // §17.1 L10 relational, keyword-spelled. `is not` is also relational
        // and handled below as a two-token sequence starting at `is`.
        if (t == "in" || t == "is" || t == "as") return 7;
        if (t == "and") return 4;  // §17.1 L13 logical AND
        if (t == "or")  return 3;  // §17.1 L14 logical OR
    }
    return 0;
}

// `??` (and only `??` at slice-14 scope) is right-associative per §17.1's
// note on level 12.
bool isRightAssociative(std::string_view op) {
    return op == "??";
}

// §17.1 mandatory parenthesization (#1086): the operator categories that are
// "non-associative across categories" and require explicit parentheses when
// mixed. Only the categories the spec lists as incompatible appear here; an
// operator outside them (`*`/`/`/`mod`, `??`, `in`/`is`/`as`) is `None` and
// never triggers the rule.
enum class OpCategory { None, Additive, Shift, Range, Bitwise, Relational,
                        Equality, Logical };

OpCategory opCategory(std::string_view op) {
    if (op == "+" || op == "-") return OpCategory::Additive;
    if (op == "<<" || op == ">>") return OpCategory::Shift;
    if (op == ".." || op == "..<") return OpCategory::Range;
    if (op == "&" || op == "|" || op == "^") return OpCategory::Bitwise;
    if (op == "<" || op == "<=" || op == ">" || op == ">=")
        return OpCategory::Relational;
    if (op == "=" || op == "<>") return OpCategory::Equality;
    if (op == "and" || op == "or") return OpCategory::Logical;
    return OpCategory::None;
}

// The §17.1 incompatible-category pairs (symmetric): bitwise with
// equality/relational/logical, shift with additive, range with bitwise.
bool incompatibleCategories(OpCategory a, OpCategory b) {
    auto is = [&](OpCategory x, OpCategory y) {
        return (a == x && b == y) || (a == y && b == x);
    };
    using C = OpCategory;
    return is(C::Bitwise, C::Equality) || is(C::Bitwise, C::Relational) ||
           is(C::Bitwise, C::Logical) || is(C::Shift, C::Additive) ||
           is(C::Range, C::Bitwise);
}

// The category of an operand IF it is an UNPARENTHESIZED binary or range node
// (the only operands that can carry a competing operator category); otherwise
// None. A `(...)`-grouped operand carries `parenthesized` and is exempt.
OpCategory operandCategory(const ast::Expr* e) {
    if (!e || e->parenthesized) return OpCategory::None;
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e))
        return opCategory(b->op);
    if (dynamic_cast<const ast::RangeExpr*>(e)) return OpCategory::Range;
    return OpCategory::None;
}

} // namespace

// Issue #135: span-stamping wrappers. Each captures the location of the
// first token of the construct, delegates to the *Impl that builds the
// node, then stamps `span = [start, current token)` if the node didn't
// already carry a (tighter) span from a child parse. Spans flow to every
// later phase's diagnostics, which previously all reported 0:0.
namespace {
template <typename NodePtr>
NodePtr stampSpan(NodePtr node, const SourceLocation& start,
                  const SourceLocation& end) {
    if (node && node->span.begin.line == 0) {
        node->span.begin = start;
        node->span.end = end;
    }
    return node;
}
} // namespace

std::unique_ptr<ast::Expr> Parser::parseExpression() {
    SourceLocation start = current_.location;
    return stampSpan(parseExpressionImpl(), start, current_.location);
}

std::unique_ptr<ast::Expr> Parser::parseExpressionImpl() {
    // fix #4348: bounded recursion-depth guard on expression parsing. A
    // parenthesized sub-expression recurses through here from
    // `parsePrimaryExpressionImpl()`'s `(` arm, and unbounded recursion
    // overflows the native call stack on deeply nested (but valid) source
    // — measured: SIGSEGV at ~7000 paren levels on the default 8 MiB
    // stack. Bail with a located diagnostic once we cross the cap; a real
    // program never gets near it. The counter is reset at
    // `parseCompilationUnit()` entry so a parse run starts clean.
    if (expressionRecursionDepth_ >= kMaxExpressionRecursion) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expression nesting too deep (limit " +
                std::to_string(kMaxExpressionRecursion) +
                "); refactor to reduce parenthesization or operator "
                "chaining depth (#4348)");
        // Skip past the rest of the innermost parenthesized group so the
        // outer `parsePrimaryExpressionImpl()` `)` arm finds its closing
        // paren and does not cascade "expected ')' to close parenthesized
        // expression" diagnostics back up the entire paren stack.
        std::size_t parenDepth = 1;
        while (current_.kind != TokenKind::EndOfFile && parenDepth > 0) {
            if (current_.kind == TokenKind::Punctuator) {
                if (current_.text == "(") {
                    ++parenDepth;
                } else if (current_.text == ")") {
                    --parenDepth;
                    if (parenDepth == 0) { advance(); break; }
                }
            }
            advance();
        }
        // Synthesize a placeholder so the outer AST is well-formed; the
        // error diagnostic above is the actual signal to the user, and
        // subsequent passes will see a (typed) error node.
        auto placeholder = std::make_unique<ast::IntLiteralExpr>();
        placeholder->text = "0";
        return std::unique_ptr<ast::Expr>(placeholder.release());
    }
    ++expressionRecursionDepth_;
    struct DepthRestore {
        std::size_t& depth;
        ~DepthRestore() { --depth; }
    } restore{expressionRecursionDepth_};

    // A.7 `Expression ::= LogicalOr | ThrowExpr` (v0.208): the conditional is
    // the parenthesized `(if …)` PrimaryAtom (§20) — the postfix
    // `x if c else y` spelling is retired and no conditional level exists in
    // the operator table (§17.1). The retired spelling's stray `if` surfaces
    // at the caller's terminator check; checkRetiredPostfixConditional names
    // the migration there.
    if (isKeyword("throw")) {
        advance();
        auto te = std::make_unique<ast::ThrowExpr>();
        const bool atTerminator =
            (current_.kind == TokenKind::EndOfFile) ||
            (current_.kind == TokenKind::Punctuator &&
             (current_.text == ";" || current_.text == ")" ||
              current_.text == "," || current_.text == "]")) ||
            (current_.kind == TokenKind::Keyword && current_.text == "end");
        if (!atTerminator) {
            te->operand = parseExpression();
        }
        return std::unique_ptr<ast::Expr>(te.release());
    }
    return parseBinaryExpression(1);
}

std::unique_ptr<ast::Expr> Parser::parseBinaryExpression(int minPrecedence) {
    // Precedence climbing. Right-associative operators (`??`) recurse with the
    // same minPrec; left-associative ones recurse with prec + 1 so same-prec
    // ops associate to the left. The keyword relationals `is`, `is not`, and
    // `as` have a Type on the right-hand side and produce TypeTestExpr; `in`
    // takes a regular expression on the right.
    // §17.1 (#1086): reject an unparenthesized mix of incompatible operator
    // categories (`a & 1 = 0`, `a << 2 + 1`, `mask & 0..n`, …). Run as each
    // binary/range node is built — the parser still knows whether an operand was
    // parenthesized (no ParenExpr node survives into the AST).
    auto checkMandatoryParens = [&](std::string_view op,
                                    const SourceLocation& loc,
                                    const ast::Expr* l, const ast::Expr* r) {
        OpCategory oc = opCategory(op);
        if (oc == OpCategory::None) return;
        auto childOp = [](const ast::Expr* e) -> std::string_view {
            if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) return b->op;
            if (dynamic_cast<const ast::RangeExpr*>(e)) return "..";
            return {};
        };
        for (const ast::Expr* operand : {l, r}) {
            OpCategory cc = operandCategory(operand);
            if (cc != OpCategory::None && incompatibleCategories(oc, cc)) {
                diagnostics_.report(
                    Severity::Error, loc,
                    std::string("operators `") + std::string(op) + "` and `" +
                    std::string(childOp(operand)) + "` are of incompatible "
                    "precedence categories and may not be mixed without "
                    "parentheses (§17.1); add explicit parentheses to "
                    "disambiguate");
                return;  // one diagnostic per node
            }
        }
    };
    auto left = parseUnaryExpression();
    for (;;) {
        // §17.3 / #468: `!=` is NOT a Bezel operator (Appendix A). The lexer
        // emits it as a single two-character Punctuator so we can catch it here
        // and emit a helpful diagnostic rather than letting it drift through as
        // a miscompiled PostfixBang followed by the `=` equality operator.
        if (current_.kind == TokenKind::Punctuator && current_.text == "!=") {
            diagnostics_.report(Severity::Error, current_.location,
                                "'!=' is not a Bezel operator; "
                                "use '<>' for inequality (§17.3)");
            advance();  // consume `!=` so recovery can continue
            (void)parseUnaryExpression();  // consume the RHS to reduce cascades
            break;
        }
        const int prec = binaryPrecedence(current_);
        if (prec < minPrecedence) break;
        const Token opTok = current_;
        advance();

        // §17.4: `is not` is one logical operator with a two-token spelling.
        // The lexer emits `is` and `not` as separate Keyword tokens; we glue
        // them back together here when they appear in relational position.
        std::string_view opText = opTok.text;
        if (opTok.text == "is" && isKeyword("not")) {
            advance();
            opText = "is not";
        }

        // §17.4: `is` / `is not` / `as` take a Type on the right. `in` takes
        // an expression (it's a regular binary op).
        // §17.4 special: `is null` / `is not null` compare to the null
        // literal rather than naming a type — lower to an equality test.
        const bool isTypeTest =
            (opText == "is" || opText == "is not" || opText == "as");
        if (isTypeTest && isKeyword("null") &&
            (opText == "is" || opText == "is not")) {
            advance();  // past `null`
            auto bin = std::make_unique<ast::BinaryExpr>();
            bin->op = (opText == "is") ? "=" : "<>";
            bin->left = std::move(left);
            bin->right = [&]() {
                auto nl = std::make_unique<ast::NullLiteralExpr>();
                return std::unique_ptr<ast::Expr>(nl.release());
            }();
            left = std::unique_ptr<ast::Expr>(bin.release());
        } else if (isTypeTest) {
            auto tt = std::make_unique<ast::TypeTestExpr>();
            tt->operand = std::move(left);
            tt->op = opText;
            tt->testType = parseType();
            left = std::unique_ptr<ast::Expr>(tt.release());
        } else if (opText == ".." || opText == "..<") {
            // §A.7 RangeExpr. The right (Shift) is optional — `a..` is the
            // low-only form. An optional `step EXPR` may follow (contextual
            // keyword per App. B.2).
            auto re = std::make_unique<ast::RangeExpr>();
            re->inclusive = (opText == "..");
            re->lo = std::move(left);
            auto isStopTok = [&]() {
                if (current_.kind == TokenKind::EndOfFile) return true;
                if (current_.kind == TokenKind::Punctuator) {
                    return current_.text == ";" || current_.text == ")" ||
                           current_.text == "]" || current_.text == "}" ||
                           current_.text == "," || current_.text == "=>";
                }
                if (current_.kind == TokenKind::Keyword) {
                    // `then` joined the list at v0.208 (#2079): the `(if …)`
                    // atom's condition may end in an open range
                    // (`(if n in 1.. then a else b)`).
                    return current_.text == "do" || current_.text == "with" ||
                           current_.text == "when" ||
                           current_.text == "end" ||
                           current_.text == "else" ||
                           current_.text == "elif" ||
                           current_.text == "then";
                }
                if (current_.kind == TokenKind::Identifier &&
                    current_.text == "step") return true;
                return false;
            };
            if (!isStopTok()) {
                re->hi = parseBinaryExpression(prec + 1);
            }
            if (current_.kind == TokenKind::Identifier &&
                current_.text == "step") {
                advance();
                re->step = parseBinaryExpression(prec + 1);
            }
            // §17.1: a range bound that is an unparenthesized bitwise expression
            // (`0 & 1 .. n`) is a cross-category mix.
            checkMandatoryParens(opText, opTok.location, re->lo.get(),
                                 re->hi.get());
            left = std::unique_ptr<ast::Expr>(re.release());
        } else {
            const int nextMin =
                isRightAssociative(opText) ? prec : prec + 1;
            // §17.12: a `throw` expression has the bottom type and is valid
            // in *any* expression position — including a binary right
            // operand, most idiomatically `x ?? throw new E(...)` (spec
            // §A.7 example). The precedence climber does not descend to the
            // ThrowExpr level (A.7's grammar is informal; §17.12 governs),
            // so parse a leading `throw` as a full expression here.
            //
            // fix #3998 (§17.12, v0.295): call `parseExpression()`, not
            // `parseExpressionImpl()` — the latter builds the ThrowExpr node
            // but skips `stampSpan` (only `parseExpression()`'s wrapper at
            // line ~113 applies it), so a `throw` reached ONLY via this
            // binary-RHS path (e.g. `b ?? throw new E()`) kept its
            // default-constructed span (line 0, col 0). Any diagnostic
            // anchored to that node's own span — e.g. §22.15's pure-body
            // "`throw` is not admitted in a pure body" rejection inside a
            // §22.16 contract clause (PureChecker.cpp's `clauseMode_` arm,
            // which reports at `te->span.begin`) — printed at `0:0` instead
            // of the throw token's real location.
            auto right = isKeyword("throw") ? parseExpression()
                                            : parseBinaryExpression(nextMin);
            auto bin = std::make_unique<ast::BinaryExpr>();
            bin->op = opText;
            bin->left = std::move(left);
            bin->right = std::move(right);
            checkMandatoryParens(opText, opTok.location, bin->left.get(),
                                 bin->right.get());
            left = std::unique_ptr<ast::Expr>(bin.release());
        }
    }
    return left;
}

std::unique_ptr<ast::Expr> Parser::parseUnaryExpression() {
    // §17.1 level 2 prefix unary: `+ - ~ not await`. Right-associative.
    if (isPunctuator("+") || isPunctuator("-") || isPunctuator("~") ||
        isKeyword("not") || isKeyword("await")) {
        const Token opTok = current_;
        advance();
        auto u = std::make_unique<ast::UnaryExpr>();
        u->op = opTok.text;
        u->operand = parseUnaryExpression();
        return std::unique_ptr<ast::Expr>(u.release());
    }
    return parsePostfix(parsePrimaryExpression());
}

std::unique_ptr<ast::Expr> Parser::parsePrimaryExpression() {
    SourceLocation start = current_.location;
    return stampSpan(parsePrimaryExpressionImpl(), start, current_.location);
}

std::unique_ptr<ast::Expr> Parser::parsePrimaryExpressionImpl() {
    // Literals from the lexer (§18 forms covered in lexer slices 3 and 4).
    if (current_.kind == TokenKind::IntLiteral) {
        auto e = std::make_unique<ast::IntLiteralExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (current_.kind == TokenKind::FloatLiteral) {
        auto e = std::make_unique<ast::FloatLiteralExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (current_.kind == TokenKind::CharLiteral) {
        auto e = std::make_unique<ast::CharLiteralExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (current_.kind == TokenKind::StringLiteral) {
        auto e = std::make_unique<ast::StringLiteralExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (current_.kind == TokenKind::VerbatimStringLiteral) {
        auto e = std::make_unique<ast::VerbatimStringExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (current_.kind == TokenKind::InterpolatedString ||
        current_.kind == TokenKind::InterpolatedVerbatimString) {
        auto e = std::make_unique<ast::InterpolatedStringExpr>();
        e->isVerbatim =
            (current_.kind == TokenKind::InterpolatedVerbatimString);
        e->text = current_.text;
        // Record the token location before splitting so diagnostics reported by
        // splitInterpolation (empty hole, brace-in-spec) point at the string.
        e->span.begin = current_.location;
        e->span.end = current_.location;
        // §18.5 / §18.7: split the `$"..."` token into alternating literal
        // and expression parts. The lexer already validated balanced braces.
        splitInterpolation(*e);
        advance();
        // fix #3724 (§18.5 / §22.8 / §2.8.2, v0.286): CLOSE the span. The two
        // lines above pre-stamp `span.begin`/`span.end` at the SAME location so
        // splitInterpolation's own diagnostics have somewhere to point — but a
        // non-zero `span.begin.line` also makes `parsePrimaryExpression`'s
        // `stampSpan` wrapper (which only stamps a node whose span is still
        // unset, `span.begin.line == 0`) a NO-OP here. Every other primary gets
        // `[firstToken, tokenAfter)` from that wrapper; this one alone was left
        // ZERO-WIDTH, i.e. `span.end.offset == span.begin.offset`.
        //
        // That is not cosmetic: every SOURCE-SLICING consumer guards on
        // `span.begin.offset < span.end.offset` and silently falls back when it
        // fails. ModuleMetadataBuilder's `fieldInitText` carriage (#2860/#2871/
        // #3723, ModuleMetadataBuilder.cpp) therefore never wrote a single byte
        // for an interpolated-string field initializer, so the importer's §22.8
        // step-2 replay (`importedInstanceFieldInitExpr`, #2874, consumed by
        // `emitOwnFieldInit` for a class and `buildRecordValue` for a record)
        // had nothing to reconstruct AND the #2860 construction-site reject had
        // no text to fire on: `public bInterp: string := $"v{H.Get()}";` on an
        // imported generic/record left the non-nullable `string` slot on its
        // §7.9 calloc-zero null, and `.Length`/`+` on it SEGFAULT because a
        // non-nullable slot is assumed by construction never to hold null.
        // The same guard fronts `canonicalClauseText` (contract-clause
        // carriage, same file), whose fallback is the literal text
        // "<expression>"; that consumer is not reachable for this node today
        // — a `requires`/`ensures`/`invariant` clause is boolean-typed, so a
        // bare interpolation cannot BE one — but it is the same guard over the
        // same authority. Closing the span here repairs that authority once,
        // instead of teaching each consumer a second spelling of "where does
        // this expression end".
        //
        // `current_.location` is the token AFTER the string — exactly the upper
        // bound `stampSpan` would have supplied, so the slice matches every
        // other expression's convention.
        e->span.end = current_.location;
        return std::unique_ptr<ast::Expr>(e.release());
    }
    // §A.7 high-only and fully-open RangeExpr forms: `..b`, `..<b`, `..`.
    // (Low-only `a..` and the with-bound forms `a..b` / `a..<b` arrive
    // through parseBinaryExpression's range path.)
    if (isPunctuator("..") || isPunctuator("..<")) {
        auto re = std::make_unique<ast::RangeExpr>();
        re->inclusive = (current_.text == "..");
        advance();
        auto isStopTok = [&]() {
            if (current_.kind == TokenKind::EndOfFile) return true;
            if (current_.kind == TokenKind::Punctuator) {
                return current_.text == ";" || current_.text == ")" ||
                       current_.text == "]" || current_.text == "}" ||
                       current_.text == "," || current_.text == "=>";
            }
            if (current_.kind == TokenKind::Keyword) {
                // `then` per v0.208 (#2079) — see the twin list above.
                return current_.text == "do" || current_.text == "with" ||
                       current_.text == "when" || current_.text == "end" ||
                       current_.text == "else" || current_.text == "elif" ||
                       current_.text == "then";
            }
            if (current_.kind == TokenKind::Identifier &&
                current_.text == "step") return true;
            return false;
        };
        if (!isStopTok()) {
            // Use binary precedence 11+1 (one above range itself) so the
            // hi-bound parses as a Shift expression per A.7.
            re->hi = parseBinaryExpression(12);
        }
        if (current_.kind == TokenKind::Identifier &&
            current_.text == "step") {
            advance();
            re->step = parseBinaryExpression(12);
        }
        return std::unique_ptr<ast::Expr>(re.release());
    }

    // §18.2 boolean and null keyword literals.
    if (isKeyword("true") || isKeyword("false")) {
        auto e = std::make_unique<ast::BoolLiteralExpr>();
        e->value = (current_.text == "true");
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (isKeyword("null")) {
        advance();
        return std::unique_ptr<ast::Expr>(
            std::make_unique<ast::NullLiteralExpr>().release());
    }

    if (current_.kind == TokenKind::Identifier) {
        auto e = std::make_unique<ast::IdentifierExpr>();
        e->name = current_.text;
        advance();
        // per §14.1 / §22.12 (issue #520): optional explicit type arguments
        // on an identifier in receiver position, e.g. `Registry<int>.Make(7)`.
        // Only parsed when the disambiguating scan confirms `< TypeArgList > .`
        // (dot follows the closing `>`) — the static-member-access receiver
        // form.  Relational chains (`a < b > c`) have a non-dot token after
        // the `>`, so the scan returns false and leaves `<` for the binary
        // expression parser to handle as a relational operator.
        if (isPunctuator("<") && tokensLookLikeReceiverTypeArgs()) {
            advance(); // past `<`
            e->typeArgs.push_back(parseType());
            while (isPunctuator(",")) {
                advance(); // past `,`
                // per §1.6 / proposal 0059 (v0.214): optional trailing `,`
                // before the closing angle — consumeClosingAngle below
                // handles `>` / `>>` / `>=` / `>>=`.
                if (current_.kind == TokenKind::Punctuator &&
                    !current_.text.empty() &&
                    current_.text.front() == '>') {
                    break;
                }
                e->typeArgs.push_back(parseType());
            }
            if (!consumeClosingAngle()) {
                diagnostics_.report(Severity::Error, current_.location,
                    "expected '>' to close receiver type-argument list");
            }
        }
        return std::unique_ptr<ast::Expr>(e.release());
    }

    // §A.7 typeof / nameof primary expressions.
    if (isKeyword("typeof")) {
        advance();
        auto te = std::make_unique<ast::TypeOfExpr>();
        if (!consumePunctuator("(")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '(' after 'typeof'");
        }
        te->queriedType = parseType();
        if (!consumePunctuator(")")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ')' to close 'typeof'");
        }
        return std::unique_ptr<ast::Expr>(te.release());
    }
    if (isKeyword("nameof")) {
        advance();
        auto ne = std::make_unique<ast::NameOfExpr>();
        if (!consumePunctuator("(")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '(' after 'nameof'");
        }
        ne->name = parseQualifiedName();
        if (!consumePunctuator(")")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ')' to close 'nameof'");
        }
        return std::unique_ptr<ast::Expr>(ne.release());
    }
    // §A.7 `DefaultExpr ::= "default" "(" Type ")"`.
    if (isKeyword("default")) {
        advance();
        auto de = std::make_unique<ast::DefaultExpr>();
        if (!consumePunctuator("(")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '(' after 'default'");
        }
        de->type = parseType();
        if (!consumePunctuator(")")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ')' to close 'default'");
        }
        return std::unique_ptr<ast::Expr>(de.release());
    }
    // §A.7 CollectionLiteral — `[ ... ]`. Two forms (sequence / dict)
    // discriminated by what follows the first element: `:` means dict,
    // `,`/`]` means sequence. An empty `[]` is the empty sequence form
    // (the empty dict has no syntactic shape and must be expressed via
    // `default(IDictionary<K,V>)` or a target-type-explicit factory call).
    if (consumePunctuator("[")) {
        auto cl = std::make_unique<ast::CollectionLiteralExpr>();
        if (consumePunctuator("]")) {
            return std::unique_ptr<ast::Expr>(cl.release());
        }
        auto first = parseExpression();
        if (consumePunctuator(":")) {
            cl->isDict = true;
            cl->keys.push_back(std::move(first));
            cl->elements.push_back(parseExpression());
            while (consumePunctuator(",")) {
                if (isPunctuator("]")) break; // trailing `,` permitted
                cl->keys.push_back(parseExpression());
                if (!consumePunctuator(":")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ':' in dictionary literal entry");
                }
                cl->elements.push_back(parseExpression());
            }
        } else {
            cl->elements.push_back(std::move(first));
            while (consumePunctuator(",")) {
                if (isPunctuator("]")) break;
                cl->elements.push_back(parseExpression());
            }
        }
        if (!consumePunctuator("]")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ']' to close collection literal");
        }
        return std::unique_ptr<ast::Expr>(cl.release());
    }

    // A.7 `MoveExpr ::= "move" PrimaryExpr` — primary-level prefix per §28.6
    // for owned-field move-out.
    if (isKeyword("move")) {
        advance();
        auto m = std::make_unique<ast::UnaryExpr>();
        m->op = "move";
        m->operand = parsePostfix(parsePrimaryExpression());
        return std::unique_ptr<ast::Expr>(m.release());
    }

    // §22.3 `this` and `base` — reserved keywords (App. B.1) that name the
    // current instance and the base-class projection. The AST stores them as
    // IdentifierExpr with the keyword as the lexeme; sema interprets.
    if (isKeyword("this") || isKeyword("base")) {
        auto e = std::make_unique<ast::IdentifierExpr>();
        e->name = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }

    // §5.2 explicit conversion `int(x)`, `double(m)`, … — a primitive type
    // keyword in expression position names a conversion applied with
    // call syntax. Emit it as an IdentifierExpr so parsePostfix forms the
    // `(arg)` call; sema/codegen interpret the call as a conversion. (This
    // also ensures a stray primitive keyword is consumed rather than left
    // to spin the enclosing statement-list parser.)
    if (current_.kind == TokenKind::Keyword &&
        isPrimitiveTypeKeyword(current_.text)) {
        auto e = std::make_unique<ast::IdentifierExpr>();
        e->name = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }

    // §19.1/§19.7 (v0.208, #2079): the expression form of `case`/`when` is
    // the parenthesized atom (`(case … of …)` / `(when …)`, parsed in the
    // paren branch below). A BARE `case`/`when` reaching expression position
    // is the retired pre-v0.208 `end`-closed expression spelling — report the
    // targeted migration diagnostic and parse the old form for recovery.
    if (isKeyword("case")) {
        return parseCaseExpr(/*parenAtom=*/false);
    }
    if (isKeyword("when")) {
        return parseWhenExpr(/*parenAtom=*/false);
    }

    // §A.7 MarkedTransfer (v0.209, #2080): `"owned" Identifier` — the K3.4
    // transfer marker as a PrimaryAtom. `owned` is reserved and begins no
    // other expression, so the reading commits at its first token; the
    // binding/assignment RHS forms are site-parsed BEFORE parseExpression
    // (the v0.180 consumeKeyword("owned") hooks), so this atom forms only in
    // the remaining expression positions — sema enforces the positional
    // admission (if-expression branch / expression-`case`/`when` arm result /
    // the §35.5 case-scrutinee production; everywhere else rejected naming
    // axiom A7's narrowed bound).
    if (isKeyword("owned")) {
        auto mt = std::make_unique<ast::MarkedTransferExpr>();
        mt->span.begin = current_.location;
        advance();  // past `owned`
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected an identifier after the `owned` transfer marker "
                "\xe2\x80\x94 the marker names a local binding or `owned` "
                "parameter (`owned x`); field paths use `move x.f` "
                "(\xc2\xa7""28.8.1) (A.7 MarkedTransfer, \xc2\xa7""28.3, "
                "v0.209)");
        } else {
            mt->name = current_.text;
            mt->span.end = current_.location;
            advance();
        }
        return std::unique_ptr<ast::Expr>(mt.release());
    }

    // §A.5 NewExpr — `new Type "(" ArgList? ")"` or shorthand `new "(" … ")"`.
    if (isKeyword("new")) {
        advance(); // past `new`
        auto n = std::make_unique<ast::NewExpr>();
        if (!isPunctuator("(")) {
            n->type = parseType();
        }
        if (!consumePunctuator("(")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected '(' to begin constructor argument list");
        } else {
            n->args = parseArgList();
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close constructor argument list");
            }
        }
        return std::unique_ptr<ast::Expr>(n.release());
    }

    // Lambda: `"async"? "lambda" "(" LambdaParams? ")" (":" Type)? "=>"
    // LambdaBody` (§27.3 / A.7, v0.217). Keyword-led: the reserved `lambda`
    // begins no other expression or statement, so the reading commits at its
    // first token (the second, after `async`) — no scan to the matching `)`
    // is ever needed, and a parenthesized construct followed by `=>` (a
    // when-guard arm, a grouped expression) can never be read as a lambda
    // prefix. This cutover deleted the pre-v0.217 `tokensLookLikeLambda`
    // buffered lookahead and the #1256 `suppressTopLevelLambda_` carve.
    const bool asyncLambdaCandidate =
        isKeyword("async") &&
        peekAt(1).kind == TokenKind::Keyword && peekAt(1).text == "lambda";
    // v0.217 hard cutover: the retired `async (params) => body` spelling is
    // recognized here only to issue the targeted migration diagnostic; it is
    // then parsed AS a lambda so one old spelling yields one error.
    const bool retiredAsyncLambda =
        isKeyword("async") &&
        peekAt(1).kind == TokenKind::Punctuator && peekAt(1).text == "(";

    if (asyncLambdaCandidate || retiredAsyncLambda || isKeyword("lambda")) {
        auto lam = std::make_unique<ast::LambdaExpr>();
        if (asyncLambdaCandidate || retiredAsyncLambda) {
            lam->isAsync = true;
            if (retiredAsyncLambda) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "`async (params) => body` is the retired pre-v0.217 "
                    "lambda spelling \xe2\x80\x94 a lambda is keyword-led: "
                    "insert `lambda` after `async` "
                    "(`async lambda(...) => ...`) (\xc2\xa7""27.3, v0.217)");
            }
            advance(); // past `async`
        }
        // Past `lambda` (absent in the retired `async (…)` spelling).
        if (isKeyword("lambda")) advance();
        if (!consumePunctuator("(")) {
            // §27.3 (v0.217): "a parenthesized parameter list (the empty
            // pair for zero parameters; there is no paren-free form)".
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected '(' after `lambda` \xe2\x80\x94 the parameter "
                "list is parenthesized and there is no paren-free form; "
                "zero parameters spell the empty pair (`lambda() => ...`) "
                "(\xc2\xa7""27.3, v0.217)");
            // Recovery: absorb the paren-free one-param shape (`lambda x =>`)
            // so a single diagnostic is produced instead of a cascade.
            if (current_.kind == TokenKind::Identifier &&
                peekAt(1).kind == TokenKind::Punctuator &&
                peekAt(1).text == "=>") {
                ast::Param p;
                p.name = current_.text;
                advance();
                lam->params.push_back(std::move(p));
            }
        } else if (!consumePunctuator(")")) {
            // Non-empty LambdaParams.
            lam->params.push_back(parseLambdaParam());
            while (consumePunctuator(",")) {
                // per §1.6 / proposal 0059 (v0.214): optional trailing `,`
                // before the `)` that closes the lambda parameter list.
                if (isPunctuator(")")) break;
                lam->params.push_back(parseLambdaParam());
            }
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close lambda parameter list");
            }
        }
        // §27.3 (v0.217): the optional return-type ascription `":" Type`
        // between the parameter list and `=>`. No Type production contains
        // `=>` (function types use `->`, A.5), so the ascription ends
        // deterministically before the arrow.
        if (consumePunctuator(":")) {
            lam->retType = parseType();
        }
        if (!consumePunctuator("=>")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '=>' in lambda expression");
        }
        if (consumeKeyword("do")) {
            lam->isBlockBody = true;
            while (!isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto s = parseStatement();
                if (s) {
                    lam->stmts.push_back(std::move(s));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
            }
            if (!consumeKeyword("end")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected 'end' to close lambda block body");
            }
        } else {
            lam->exprBody = parseExpression();
        }
        return std::unique_ptr<ast::Expr>(lam.release());
    }

    if (consumePunctuator("(")) {
        // §20 / A.7 IfExpr (v0.208, #2079): "(" followed by the reserved
        // `if` — which begins no other expression — commits the conditional-
        // atom reading at the second token.
        if (isKeyword("if")) {
            return parseIfExprAtom();
        }
        // §19.1/§19.7 / A.7 CaseExpr/WhenExpr (v0.208, #2079): "(case" /
        // "(when" likewise commit at the second token; the atom's `)` is the
        // closer (no `end`), and arms are Expression-bodied only.
        if (isKeyword("case")) {
            return parseCaseExpr(/*parenAtom=*/true);
        }
        if (isKeyword("when")) {
            return parseWhenExpr(/*parenAtom=*/true);
        }
        // §17.15 / A.7 BindExpr (v0.216, #2098): "(" followed by the reserved
        // `var` — which begins no other expression — commits the binding-atom
        // reading at the second token, like the other paren atoms.
        if (isKeyword("var")) {
            return parseBindExprAtom();
        }
        // §A.7 unit value `()` — empty parens at PrimaryAtom level.
        if (consumePunctuator(")")) {
            return std::unique_ptr<ast::Expr>(
                std::make_unique<ast::UnitExpr>().release());
        }
        // No ParenExpr node: source-level parens only group the parse; the
        // resulting tree shape encodes the grouping directly. See Node.hpp.
        auto inner = parseExpression();
        if (!consumePunctuator(")")) {
            // v0.217 cutover (§27.3, #2101): a multi-parameter or type-
            // ascribed head of the RETIRED lambda spelling reaches here as a
            // failed parenthesized expression (`(x, y) => …`, `(x: int) =>
            // …`). On this already-failed path only, scan to the matching
            // `)` and test for `=>` so the diagnostic names the migration
            // instead of a bare "expected ')'". (Error-path aid only — the
            // grammar itself needs no lookahead; see Parser.hpp.)
            if ((isPunctuator(",") || isPunctuator(":")) &&
                retiredLambdaHeadAhead()) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "`(params) => body` is the retired pre-v0.217 lambda "
                    "spelling \xe2\x80\x94 a lambda is keyword-led: insert "
                    "`lambda` before the parameter list "
                    "(`lambda(...) => ...`) (\xc2\xa7""27.3, v0.217)");
                // Recover: swallow the rest of the head, the arrow, and the
                // body, so one retired lambda yields one diagnostic.
                std::size_t depth = 1;
                while (current_.kind != TokenKind::EndOfFile) {
                    if (current_.kind == TokenKind::Punctuator) {
                        if (current_.text == "(") {
                            ++depth;
                        } else if (current_.text == ")") {
                            if (--depth == 0) break;
                        }
                    }
                    advance();
                }
                consumePunctuator(")");
                consumePunctuator("=>");
                skipRetiredLambdaBody();
            } else {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close parenthesized expression");
            }
        }
        // §17.1 (#1086): mark the group so the mandatory-parenthesization check
        // treats `(a & 1) = 0` as explicitly parenthesized (legal) rather than a
        // bare cross-category mix.
        if (inner) inner->parenthesized = true;
        return inner;
    }

    // §17.13 (v0.244, #3049): "a bare `_` in expression position (`_` is not a
    // value — nothing binds it, so no such read can resolve)". A.1 makes this
    // grammatical: no Expression production contains the bare token, so this
    // is the one place it can surface, and it gets its own diagnostic instead
    // of the generic "expected expression".
    if (atDiscard()) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "`_` is not a value — it is the reserved discard token, and "
            "nothing binds it, so no read of `_` can resolve (§1.3, §17.13)");
        advance();
        return nullptr;
    }

    diagnostics_.report(Severity::Error, current_.location,
                        "expected expression");
    return nullptr;
}

// §27.3 (v0.217, #2101): see Parser.hpp — error-path-only scan for the
// retired `(params) => body` spelling. current_ is inside an already-failed
// parenthesized expression at depth 1; peek to the matching `)` and test for
// `=>`. Bails out on EOF.
bool Parser::retiredLambdaHeadAhead() {
    std::size_t depth = 1;
    std::size_t k = 0;
    for (;;) {
        const Token& t = peekAt(k);
        if (t.kind == TokenKind::EndOfFile) return false;
        if (t.kind == TokenKind::Punctuator) {
            if (t.text == "(") ++depth;
            else if (t.text == ")") {
                if (--depth == 0) {
                    const Token& after = peekAt(k + 1);
                    return after.kind == TokenKind::Punctuator &&
                           after.text == "=>";
                }
            }
        }
        ++k;
    }
}

// §27.3 (v0.217, #2101): consume and discard the body of a retired-spelling
// lambda after its `=>` has been consumed, so one old lambda yields one
// targeted diagnostic instead of a cascade of generic ones.
void Parser::skipRetiredLambdaBody() {
    if (consumeKeyword("do")) {
        while (!isKeyword("end") && current_.kind != TokenKind::EndOfFile) {
            auto s = parseStatement();
            if (!s && current_.kind != TokenKind::EndOfFile) advance();
        }
        consumeKeyword("end");
    } else {
        parseExpression();
    }
}

// §27.3 (v0.217, #2101): see Parser.hpp — called at generic-error sites when
// the unexpected token may be the `=>` of a retired-spelling lambda whose
// head parsed as an ordinary parenthesized expression (`(x) => …`,
// `() => …`, i.e. the shapes the paren-expression grammar accepts cleanly).
bool Parser::checkRetiredParenLambda() {
    if (!isPunctuator("=>")) return false;
    diagnostics_.report(
        Severity::Error, current_.location,
        "`(params) => body` is the retired pre-v0.217 lambda spelling "
        "\xe2\x80\x94 a lambda is keyword-led: insert `lambda` before the "
        "parameter list (`lambda(...) => ...`) (\xc2\xa7""27.3, v0.217)");
    advance(); // past `=>`
    skipRetiredLambdaBody();
    return true;
}

// per §14.1 (issue #509): disambiguate `Method<T>(…)` (explicit method type
// args) from `expr < T > (expr)` (relational chain).  peekAt(0) is the `<`
// token after the member identifier has been consumed.
//
// Strategy: scan forward counting angle-bracket depth.  A token sequence that
// matches `< (TypeArgToken | ",")* > (` is a type-arg list; anything else
// (EOF, statement-terminator keyword, relational-expression operand like a
// number or operator) is a relational `<`.
//
// "TypeArgToken" is conservative: identifier, primitive-type keyword, `?`
// (nullable suffix), `[` / `]` (inline-array prefix), `<` / `>` (nested
// generic args).  Operators other than `<`, `>`, `?`, `[`, `]`, `,` are
// not valid inside a type-arg list and cause us to bail out.
//
// per §A.5 / §22.12 (issue #2191): a GenericArg is a Type, and a Type
// includes the function-type spelling `( FuncParamType ("," …)* ","? )
// "->" "owned"? Type` (and the unit type `()`).  The scan therefore also
// admits the function-type tokens — `(` / `)` (depth-tracked: the spec's
// "balanced <…> group respecting nesting" covers the function type's
// parameter list), the `->` separator, the `:` of a named function-type
// parameter, and the reserved FuncParamMods keywords `owned` / `ref` /
// `out` (`scoped` is a contextual identifier, already accepted).  Those
// keywords cannot appear as expression operands, so admitting them cannot
// turn a relational `<` into a false type-arg reading; an ill-formed
// combination still reaches parseType, the authority.
bool Parser::tokensLookLikeMethodTypeArgs() {
    // peekAt(0) must be `<`.
    std::size_t k = 1;                // start scanning after the opening `<`
    std::size_t depth = 1;            // nesting level of `<` / `>`
    std::size_t parenDepth = 0;       // function-type parameter-list nesting (#2191)
    static const std::size_t kLimit = 64; // prevent scanning unbounded input
    for (std::size_t guard = 0; guard < kLimit; ++guard, ++k) {
        const Token& t = peekAt(k);
        if (t.kind == TokenKind::EndOfFile) return false;
        if (t.kind == TokenKind::Punctuator) {
            if (t.text == "<") { ++depth; continue; }
            if (t.text == ">>" ) {
                // Lexer emits ">>" as one token; consuming two `>`s.
                if (depth <= 2) {
                    // The outer `>` closes this generic-arg list and the
                    // inner `>` would be left to the caller.  Treat this
                    // as closing: after the `>>`, we need `(`.
                    if (parenDepth != 0) return false; // `>>` inside function `(…)`
                    const Token& after = peekAt(k + 1);
                    if (depth == 2) {
                        // both `>` consumed here; we need `(` next.
                        return after.kind == TokenKind::Punctuator &&
                               after.text == "(";
                    }
                    // depth == 1: `>>` closes this list twice (malformed),
                    // bail out.
                    return false;
                }
                depth -= 2; continue;
            }
            if (t.text == ">") {
                if (parenDepth != 0) return false; // `>` inside function-type `(…)`
                --depth;
                if (depth == 0) {
                    // The closing `>` of the arg list.  Confirm `(` follows.
                    const Token& after = peekAt(k + 1);
                    return after.kind == TokenKind::Punctuator &&
                           after.text == "(";
                }
                continue;
            }
            // §A.5 FunctionType `(...) -> owned? Type` (and the unit `()`) is
            // a GenericArg (#2191): track the parameter-list nesting so the
            // scan stays sound (an unmatched `)` cannot be a type-arg list).
            if (t.text == "(") { ++parenDepth; continue; }
            if (t.text == ")") {
                if (parenDepth == 0) return false;
                --parenDepth; continue;
            }
            // `->` separates a function type's parameters from its return;
            // `:` precedes a named function-type parameter's type (`(x: int)`).
            if (t.text == "->" || t.text == ":") continue;
            // Punctuators valid inside a type arg: `,`, `?`, `[`, `]`.
            if (t.text == "," || t.text == "?" ||
                t.text == "[" || t.text == "]") continue;
            // Any other punctuator (e.g. `+`, `-`, `*`, `=`,
            // `..`) cannot appear inside a type arg; bail out.
            return false;
        }
        if (t.kind == TokenKind::Keyword) {
            // Primitive-type keywords (int, string, bool, …) are valid type
            // names.  The function-type parameter modifiers (§A.5
            // FuncParamMods: `owned` / `ref` / `out`, the latter also part
            // of `out owned`) are reserved keywords (Appendix B.1) admitted
            // so a function-type GenericArg scans correctly (#2191); they
            // cannot appear as expression operands.  Other keywords (if,
            // while, return, …) cannot appear inside a type arg.
            if (isPrimitiveTypeKeyword(t.text)) continue;
            if (t.text == "owned" || t.text == "ref" || t.text == "out" ||
                t.text == "env") continue;
            return false;
        }
        if (t.kind == TokenKind::Identifier) continue; // type name component
        // Numeric/char/string literals cannot be type args.
        return false;
    }
    return false; // exceeded scan limit
}

// per §14.1 / §22.12 (issue #520): disambiguate `GenericClass<T>.Member`
// (explicit receiver type args, static access) from a relational `<` chain.
// peekAt(0) is the `<` token after the identifier has been consumed.
//
// Identical strategy to tokensLookLikeMethodTypeArgs() except that the
// closing `>` must be followed by `.` (dot) — the start of the `.Member`
// access — rather than `(` (an argument list).  This allows:
//   `Registry<int>.Make(7)`   → receiver type-arg list (> followed by .)
// while still leaving:
//   `a < b > 0`               → relational chain (> followed by numeric/ident)
//   `Wrapper.Identity<int>(v)` → method type-arg list (handled by #509)
// both parsed correctly.
bool Parser::tokensLookLikeReceiverTypeArgs() {
    std::size_t k = 1;                // start scanning after the opening `<`
    std::size_t depth = 1;
    std::size_t parenDepth = 0;       // function-type parameter-list nesting (#2191)
    static const std::size_t kLimit = 64;
    for (std::size_t guard = 0; guard < kLimit; ++guard, ++k) {
        const Token& t = peekAt(k);
        if (t.kind == TokenKind::EndOfFile) return false;
        if (t.kind == TokenKind::Punctuator) {
            if (t.text == "<") { ++depth; continue; }
            if (t.text == ">>") {
                if (depth <= 2) {
                    if (parenDepth != 0) return false; // `>>` inside function `(…)`
                    const Token& after = peekAt(k + 1);
                    if (depth == 2) {
                        return after.kind == TokenKind::Punctuator &&
                               after.text == ".";
                    }
                    return false;
                }
                depth -= 2; continue;
            }
            if (t.text == ">") {
                if (parenDepth != 0) return false; // `>` inside function-type `(…)`
                --depth;
                if (depth == 0) {
                    // The closing `>` of the arg list.  Confirm `.` follows —
                    // a static-member access, not a call-argument list.
                    const Token& after = peekAt(k + 1);
                    return after.kind == TokenKind::Punctuator &&
                           after.text == ".";
                }
                continue;
            }
            // §A.5 FunctionType (and unit `()`) as a GenericArg (#2191):
            // track the parameter-list nesting so the scan stays sound.
            if (t.text == "(") { ++parenDepth; continue; }
            if (t.text == ")") {
                if (parenDepth == 0) return false;
                --parenDepth; continue;
            }
            if (t.text == "->" || t.text == ":") continue;
            if (t.text == "," || t.text == "?" ||
                t.text == "[" || t.text == "]") continue;
            return false;
        }
        if (t.kind == TokenKind::Keyword) {
            if (isPrimitiveTypeKeyword(t.text)) continue;
            // §A.5 FuncParamMods (`owned` / `ref` / `out`) inside a
            // function-type GenericArg (#2191). `env` is also valid here
            // (it appears in function-type param lists inside extern decls).
            if (t.text == "owned" || t.text == "ref" || t.text == "out" ||
                t.text == "env") continue;
            return false;
        }
        if (t.kind == TokenKind::Identifier) continue;
        return false;
    }
    return false; // exceeded scan limit
}

ast::Param Parser::parseLambdaParam() {
    // A.7 `LambdaParam ::= FuncParamMods? Identifier (":" Type)? |
    // LambdaDestructure`. Type is optional on the Identifier form (unlike
    // A.4 method ParamList).
    ast::Param p;
    std::string_view mod;
    bool hasMod = false;
    SourceLocation modLoc = current_.location;
    if (consumeKeyword("ref")) {
        mod = "ref";
        hasMod = true;
    } else if (consumeKeyword("out")) {
        hasMod = true;
        if (consumeKeyword("owned")) {
            mod = "out owned";
        } else {
            mod = "out";
        }
    } else if (consumeKeyword("owned")) {
        mod = "owned";
        hasMod = true;
    } else if (current_.kind == TokenKind::Identifier &&
               current_.text == "scoped" &&
               (peekAt(1).kind == TokenKind::Identifier ||
                peekAt(1).kind == TokenKind::Discard)) {
        // per §27 / A.7 (v0.228): `scoped` is a CONTEXTUAL modifier (lexed
        // as Identifier, not a reserved keyword) listed in FuncParamMods and
        // therefore admissible on a LambdaParam, exactly mirroring the
        // method-param path (§22.3 ParamModifiers, A.4; Parser_Decls.cpp).
        // It is the modifier exactly when the next token is the parameter's
        // name (an Identifier) or the bare ignore `_` (the Discard token —
        // matched here so the "no modifier composes with a bare ignore" rule
        // below reports it, rather than reading `scoped` as the name); a
        // parameter NAMED `scoped` is followed by
        // `:` and is not matched here, and a destructure head `scoped(` is
        // followed by `(` (a Punctuator) and likewise falls through to the
        // LambdaDestructure alternative below. Function-typed-ness and the
        // §28.14 storage-bounded-capture semantics for a lambda's own
        // parameter are checked in sema — the parser only admits the token
        // the grammar admits (#2344).
        mod = "scoped";
        hasMod = true;
        advance();
    }
    // per §27.4 (v0.183)/A.7: the LambdaDestructure alternative is
    // `QualifiedName "(" ...`, discriminated from the Identifier alternative
    // by a `(` immediately following the (possibly dotted) name.
    if (tokensLookLikeLambdaDestructure()) {
        if (hasMod) {
            // §27.4: "The destructuring parameter admits no modifiers (`ref`,
            // `scoped`) and no separate type annotation — the head carries
            // the type." A dedicated diagnostic, not the generic "expected
            // lambda parameter name" a modifier consumed here would produce.
            diagnostics_.report(
                Severity::Error, modLoc,
                "a destructuring lambda parameter admits no modifier "
                "(`" + std::string(mod) +
                "`) \xe2\x80\x94 the destructuring head names the exact "
                "parameter type and carries no separate annotation "
                "(\xc2\xa7""27.4)");
        }
        p.destructure = parseLambdaDestructure();
        return p;
    }
    p.modifier = mod;
    // A.7 `LambdaParam ::= FuncParamMods? Identifier (":" Type)? | "_" |
    // LambdaDestructure` (§27.3 / §17.13): the bare `"_"` alternative is the
    // parameter IGNORE — accepted, binds nothing, still contributes its slot
    // to the function type, and duplicates are legal because nothing is named.
    // This is the Q2 asymmetry with method parameters, which cannot be named
    // `_` at all (A.4 Param): a lambda parameter is not call-site-addressable
    // API surface. "No FuncParamMods and no annotation compose with it — a
    // typed ignore slot is spelled by the target type (§27.4)."
    if (atDiscard()) {
        if (hasMod) {
            diagnostics_.report(
                Severity::Error, modLoc,
                "no parameter modifier composes with the bare ignore `_` "
                "(`" + std::string(mod) +
                "` here) — a `ref` parameter reads and writes through its "
                "operand, which an ignore cannot supply, and a `scoped` "
                "parameter that binds nothing stores nothing (§17.13, §27.3)");
            p.modifier = {};
        }
        p.name = "_";
        advance();
        if (isPunctuator(":")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "a type annotation is not permitted on the bare ignore `_` — "
                "a bare ignore takes its type from context, never an "
                "annotation; a typed ignore slot is spelled by the target "
                "type (§17.13, §27.3, §27.4)");
            advance();            // past `:`
            p.type = parseType(); // consume it for recovery
        }
        return p;
    }
    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected lambda parameter name");
    } else {
        p.name = current_.text;
        advance();
    }
    if (consumePunctuator(":")) {
        p.type = parseType();
    }
    return p;
}

bool Parser::tokensLookLikeLambdaDestructure() {
    // `QualifiedName "("`: an Identifier, optionally dotted, immediately
    // followed by `(`. peekAt(0) is current_.
    if (current_.kind != TokenKind::Identifier) return false;
    std::size_t k = 1;
    for (;;) {
        const Token& dot = peekAt(k);
        if (!(dot.kind == TokenKind::Punctuator && dot.text == ".")) break;
        const Token& id = peekAt(k + 1);
        if (id.kind != TokenKind::Identifier) return false;
        k += 2;
    }
    const Token& open = peekAt(k);
    return open.kind == TokenKind::Punctuator && open.text == "(";
}

std::unique_ptr<ast::DestructuringPattern> Parser::parseLambdaDestructure() {
    // per §27.4 (v0.183, #1408) / A.7: `LambdaDestructure ::= QualifiedName
    // "(" LambdaSlot ("," LambdaSlot)* ")"`. Reuses the §19.3 case-pattern
    // destructuring parse (parseQualifiedName + parsePositionalPattern) —
    // same spelling, parameter-position restrictions enforced afterward by
    // validateLambdaDestructureSlots.
    SourceLocation start = current_.location;
    auto dp = std::make_unique<ast::DestructuringPattern>();
    dp->typeName = parseQualifiedName();
    consumePunctuator("("); // confirmed by tokensLookLikeLambdaDestructure
    if (!isPunctuator(")")) {
        dp->subPatterns.push_back(parsePositionalPattern());
        while (consumePunctuator(",")) {
            // per §1.6 / proposal 0059 (v0.214): optional trailing `,` before
            // the `)` that closes the destructuring pattern slot list.
            if (isPunctuator(")")) break;
            dp->subPatterns.push_back(parsePositionalPattern());
        }
    }
    if (!consumePunctuator(")")) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected ')' to close destructuring lambda parameter");
    }
    dp->span.begin = start;
    dp->span.end = current_.location;
    validateLambdaDestructureSlots(*dp);
    return dp;
}

void Parser::validateLambdaDestructureSlots(const ast::DestructuringPattern& dp) {
    // per §27.4 (v0.183, #1408): "A destructuring parameter is a binding
    // form, not a test, so only irrefutable elements are admitted. Each slot
    // is a naked identifier binding, a discard `_`, or a nested destructuring
    // meeting the same rules... Constants, ranges, relationals, type tests,
    // `var`-form bindings, and alternatives are compile errors in parameter
    // position." parsePositionalPattern (shared with §19.3 case patterns)
    // parses the full pattern grammar, so any of those disallowed shapes may
    // come back here; reject them with a dedicated diagnostic rather than
    // letting them silently through to sema as if this were a `case` arm.
    for (const auto& sp : dp.subPatterns) {
        if (!sp.nakedBinding.empty()) continue; // naked identifier: OK
        if (dynamic_cast<const ast::DiscardPattern*>(sp.pattern.get())) {
            continue; // `_`: OK
        }
        if (auto* nested =
                dynamic_cast<const ast::DestructuringPattern*>(sp.pattern.get())) {
            validateLambdaDestructureSlots(*nested); // nested LambdaDestructure
            continue;
        }
        SourceLocation loc =
            sp.pattern ? sp.pattern->span.begin : dp.span.begin;
        diagnostics_.report(
            Severity::Error, loc,
            "a destructuring lambda-parameter element must be a binding "
            "identifier, `_`, or a nested destructuring \xe2\x80\x94 "
            "constants, ranges, relationals, type tests, `var`-form "
            "bindings, and alternatives are refutable and not admitted in "
            "parameter position (a parameter list is not a `case`, "
            "\xc2\xa7""27.4)");
    }
}

// ===== Slice 38: patterns and case expressions (§A.7) =====================

// §19.3 / §A.7 AltPattern (v0.167, #1371): `AltPattern ::= Pattern ("|"
// Pattern)*`. The `|` alternative separator replaces the pre-v0.167 comma —
// the comma now means exactly one thing in pattern context (the arm/slot
// separator), so no `=>` lookahead is needed to disambiguate an alternatives
// list from an arm boundary, and alternatives become available *per positional
// slot* as well as at arm level. `|` never collides with bitwise-or: the only
// expression positions embedded in patterns (relational operand, range bounds)
// parse at Shift level or tighter, below `|` (§19.3). Every member must resolve
// to a CONSTANT (§19.3: a literal, an enum constant, or a named `const`); the
// members are collected into a single ConstantPattern so the existing
// OR-of-tests lowering (codegen `patternMatches` over ConstantPattern) applies
// unchanged at both arm level and inside a destructuring slot. A structurally
// non-constant member (a binding, range, relational, primitive type, or `_`)
// is rejected here; a QualifiedName that resolves to a *type* is rejected in
// sema (the type-vs-constant distinction is a name-resolution question, §A.7).
std::unique_ptr<ast::Pattern> Parser::parsePattern(bool inPositional) {
    SourceLocation start = current_.location;
    auto first = stampSpan(parsePrimaryPattern(inPositional), start,
                           current_.location);
    if (!isPunctuator("|")) return first;
    // Collect every `|`-joined primary pattern first, then choose the
    // representation from their kinds.
    std::vector<std::unique_ptr<ast::Pattern>> members;
    members.push_back(std::move(first));
    while (consumePunctuator("|")) {
        SourceLocation memStart = current_.location;
        members.push_back(stampSpan(parsePrimaryPattern(inPositional), memStart,
                                    current_.location));
    }
    // §19.3 (v0.183, #1584): ranges and relationals are now admitted as `|`
    // alternative members. If any member is a range/relational pattern, the
    // list is a heterogeneous OR of membership tests and is kept as an
    // AltPattern of the individual primary patterns. Otherwise every member is
    // constant-shaped and collapses into a single ConstantPattern — the v0.167
    // form, reused unchanged by all downstream constant-alternative logic (enum
    // exhaustiveness, duplicate detection, the OR-of-equals codegen lowering).
    bool anyExtent = false;
    for (const auto& m : members)
        if (dynamic_cast<ast::RangePattern*>(m.get()) ||
            dynamic_cast<ast::RelOpPattern*>(m.get())) {
            anyExtent = true;
            break;
        }
    if (!anyExtent) {
        auto cp = std::make_unique<ast::ConstantPattern>();
        cp->span.begin = start;
        for (auto& m : members) appendAlternativeMember(*cp, std::move(m));
        cp->span.end = current_.location;
        return std::unique_ptr<ast::Pattern>(cp.release());
    }
    // §19.3 (v0.183, #1584): build the AltPattern. Each member is normalized to
    // one of the three binding-free kinds the downstream passes handle
    // uniformly: a constant-shaped member (a single-constant ConstantPattern, a
    // NamedPattern, or `null`) becomes a single-constant ConstantPattern — the
    // same expression form the pure-constant collapse produces, so a member that
    // resolves to a *type* is caught in sema (checkAltMembersConstant) and a
    // mismatched member is caught by the §19 comparison-class check. A range or
    // relational member is kept as-is. A binding-bearing member (`var x`, a
    // naked binding, `_`), a primitive type-test, or a destructuring member is
    // NOT binding-free and is rejected here — the list binds nothing regardless
    // of member kind.
    auto ap = std::make_unique<ast::AltPattern>();
    ap->span.begin = start;
    for (auto& m : members) {
        if (!m) continue;
        if (dynamic_cast<ast::ConstantPattern*>(m.get()) ||
            dynamic_cast<ast::RangePattern*>(m.get()) ||
            dynamic_cast<ast::RelOpPattern*>(m.get())) {
            ap->members.push_back(std::move(m));
            continue;
        }
        if (auto* np = dynamic_cast<ast::NamedPattern*>(m.get())) {
            auto cp = std::make_unique<ast::ConstantPattern>();
            cp->span = m->span;
            auto e = qualifiedNameToExpr(std::move(np->name));
            if (e) e->span = m->span;
            cp->constants.push_back(std::move(e));
            ap->members.push_back(std::unique_ptr<ast::Pattern>(cp.release()));
            continue;
        }
        if (dynamic_cast<ast::NullPattern*>(m.get())) {
            auto cp = std::make_unique<ast::ConstantPattern>();
            cp->span = m->span;
            auto nl = std::make_unique<ast::NullLiteralExpr>();
            nl->span = m->span;
            cp->constants.push_back(std::unique_ptr<ast::Expr>(nl.release()));
            ap->members.push_back(std::unique_ptr<ast::Pattern>(cp.release()));
            continue;
        }
        diagnostics_.report(
            Severity::Error, m->span.begin,
            "a pattern alternative member must be binding-free \xe2\x80\x94 a "
            "constant, a range pattern, or a relational pattern "
            "(\xc2\xa7""19.3); a binding or a type may not appear in a `|` "
            "alternative list");
    }
    ap->span.end = current_.location;
    return std::unique_ptr<ast::Pattern>(ap.release());
}

// §19.3 / §A.7 (v0.167, #1371): fold one already-parsed primary pattern into an
// alternatives ConstantPattern as a constant member. Constant-shaped members
// (a ConstantPattern, a NamedPattern's QualifiedName, or `null`) contribute
// their constant expression(s); any other shape is a structurally non-constant
// member and is rejected with the §19.3 membership diagnostic.
void Parser::appendAlternativeMember(ast::ConstantPattern& cp,
                                     std::unique_ptr<ast::Pattern> p) {
    if (!p) return;
    if (auto* inner = dynamic_cast<ast::ConstantPattern*>(p.get())) {
        for (auto& c : inner->constants)
            if (c) {
                if (c->span.begin.line == 0) c->span = p->span;
                cp.constants.push_back(std::move(c));
            }
        return;
    }
    if (auto* np = dynamic_cast<ast::NamedPattern*>(p.get())) {
        auto e = qualifiedNameToExpr(std::move(np->name));
        if (e) e->span = p->span;
        cp.constants.push_back(std::move(e));
        return;
    }
    if (dynamic_cast<ast::NullPattern*>(p.get())) {
        auto nl = std::make_unique<ast::NullLiteralExpr>();
        nl->span = p->span;
        cp.constants.push_back(std::unique_ptr<ast::Expr>(nl.release()));
        return;
    }
    // §19.3: a binding, a type, or any other non-constant is not admissible in
    // an alternative list. (`var`/naked bindings and primitive types are caught
    // here; a user-type QualifiedName reaches sema as a ConstantPattern member.)
    diagnostics_.report(
        Severity::Error, p->span.begin,
        "a pattern alternative member must be a constant \xe2\x80\x94 a "
        "literal, an enum constant, or a named `const` (\xc2\xa7""19.3); a "
        "binding or a type may not appear in a `|` alternative list");
}

std::unique_ptr<ast::Pattern> Parser::parsePrimaryPattern(
    [[maybe_unused]] bool inPositional) {
    // §A.7 (v0.167, #1371): one primary (non-alternative) pattern. `|`-joined
    // alternatives are handled by the parsePattern wrapper; the comma is
    // exclusively the arm/slot separator, so `inPositional` no longer changes
    // this routine's behavior (it is retained for call-site symmetry with the
    // naked-binding recognition in parsePositionalPattern).
    // A.7 `Pattern ::= "_"` — the wildcard, matching any value without
    // binding. Since v0.244 `_` is the reserved discard token (A.1), not an
    // Identifier.
    if (atDiscard()) {
        advance();
        return std::make_unique<ast::DiscardPattern>();
    }
    // `null` literal pattern.
    if (consumeKeyword("null")) {
        return std::make_unique<ast::NullPattern>();
    }
    // `var IDENT (: TYPE)?` binding pattern.
    if (consumeKeyword("var")) {
        auto bp = std::make_unique<ast::BindingPattern>();
        if (atDiscard()) {
            // §17.13 / §19.3 (v0.244, #3049): the pattern-BINDING spelling of
            // `_` is a rejected position — "the bare wildcard already means
            // match-without-binding (write `_`)". Recover as that wildcard so
            // the arm keeps its shape.
            reportDiscardNotDeclarable(
                "pattern binding",
                "the bare wildcard `_` already matches without binding — drop "
                "the `var` (§19.3)");
            if (isPunctuator(":")) { advance(); parseType(); }
            return std::make_unique<ast::DiscardPattern>();
        }
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected identifier in `var` binding pattern");
        } else {
            bp->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) bp->type = parseType();
        return std::unique_ptr<ast::Pattern>(bp.release());
    }
    // Relational pattern: `<`, `>`, `<=`, `>=` followed by a Shift-level
    // operand. §19.3 (v0.167, #1371): the operand parses at the Shift level or
    // tighter (parseBinaryExpression(12)) — below `|` — so a trailing `|` is the
    // alternative separator, not bitwise-or absorbed into the operand. A
    // bitwise operand must be parenthesized (§17.2).
    if (isPunctuator("<") || isPunctuator(">") ||
        isPunctuator("<=") || isPunctuator(">=")) {
        auto rop = std::make_unique<ast::RelOpPattern>();
        rop->op = current_.text;
        advance();
        SourceLocation opStart = current_.location;
        rop->expr = stampSpan(parseBinaryExpression(12), opStart,
                              current_.location);
        return std::unique_ptr<ast::Pattern>(rop.release());
    }
    // §16.2 / §19.3 (#726, #744): both open-ended range-pattern forms are
    // supported. A TRAILING-open pattern (`31..`) is handled by the catch-all
    // below (the leading expression is parsed, then `..` makes it a RangeExpr
    // with an absent upper bound). A LEADING-open pattern (`..<EXPR` / `..EXPR`)
    // is now unambiguous: with the scrutinee-less form spelled `when` (#744),
    // `case` always carries a scrutinee parsed above the range level (≥ 12), so
    // the scrutinee never swallows a following `..`/`..<`. A leading `..`/`..<`
    // therefore begins this arm's pattern — fall through to the catch-all,
    // where parsePrimaryExpression parses it as a high-only RangeExpr (§A.7)
    // that wraps into a RangePattern.
    // Primitive-type-led TypePattern. Other types (named) fall through to
    // the Identifier branch as NamedPattern — sema disambiguates Constant
    // vs Type by name resolution per the §A.7 note.
    //
    // Per §5.2 / §A.7: `int(expr)` is an explicit conversion call (a
    // PrimaryExpr), NOT a TypePattern. Only a bare primitive keyword NOT
    // followed by `(` is a TypePattern here; when `(` follows, fall through
    // to the catch-all expression path so `int(this) < int(other)` and
    // similar conversion expressions in scrutinee-less case arms parse as
    // ConstantPattern over a BinaryExpr. Issue #657 / spec_0026.
    if (isKeyword("int")     || isKeyword("long")  ||
        isKeyword("short")   || isKeyword("byte")  ||
        isKeyword("sbyte")   || isKeyword("ushort")||
        isKeyword("uint")    || isKeyword("ulong") ||
        isKeyword("float")   || isKeyword("double")||
        isKeyword("decimal") || isKeyword("bool")  ||
        isKeyword("char")    || isKeyword("string")) {
        // current_ is the keyword; peekAt(1) is the next token.
        // If the next token is `(`, this is a conversion call expression,
        // not a type-test pattern — fall through to the catch-all.
        const bool nextIsParen =
            peekAt(1).kind == TokenKind::Punctuator &&
            peekAt(1).text == "(";
        if (!nextIsParen) {
            auto tp = std::make_unique<ast::TypePattern>();
            tp->type = parseType();
            return std::unique_ptr<ast::Pattern>(tp.release());
        }
        // Fall through to catch-all: parse int(expr) as expression.
    }
    // Identifier → QualifiedName. `(` after means DestructuringPattern.
    // A `,` followed by another Constant means a multi-Constant alternatives
    // pattern (slice 44). Otherwise it's a NamedPattern (sema resolves
    // Constant-vs-Type-test).
    if (current_.kind == TokenKind::Identifier) {
        auto qn = parseQualifiedName();
        if (consumePunctuator("(")) {
            auto dp = std::make_unique<ast::DestructuringPattern>();
            dp->typeName = std::move(qn);
            if (!isPunctuator(")")) {
                dp->subPatterns.push_back(parsePositionalPattern());
                while (consumePunctuator(",")) {
                    // per §1.6 / proposal 0059 (v0.214): optional trailing `,`
                    // before the `)` that closes the case destructuring slot
                    // list (PositionalPatternList).
                    if (isPunctuator(")")) break;
                    dp->subPatterns.push_back(parsePositionalPattern());
                }
            }
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close destructuring pattern");
            }
            return std::unique_ptr<ast::Pattern>(dp.release());
        }
        // §A.7 (v0.167, #1371): a QualifiedName that is not a destructuring
        // head is a NamedPattern (sema disambiguates Constant-vs-Type by name
        // resolution). Any `|`-joined alternatives are collected by the
        // parsePattern wrapper, not here — the comma is now exclusively the
        // arm/slot separator, so no `,`-alternatives branch remains.
        auto np = std::make_unique<ast::NamedPattern>();
        np->name = std::move(qn);
        return std::unique_ptr<ast::Pattern>(np.release());
    }
    // Catch-all: parse a Constant / RangePattern. §19.3 (v0.167, #1371): the
    // pattern-embedded expression parses at the RANGE level (precedence 11) —
    // its Shift-level bounds/value (12+) are tighter than `|` (bitwise-or, 8),
    // so a `|` at pattern level is left for the alternative separator, never
    // absorbed as bitwise-or. A bitwise-or *value* pattern must be
    // parenthesized (`(A | B)`), per the §17.2 mandatory-parenthesization
    // discipline. A RangeExpr at the root is a RangePattern (slice 52);
    // otherwise the expression is a single ConstantPattern member (the
    // parsePattern wrapper collects any `|`-joined alternatives).
    SourceLocation exprStart = current_.location;
    auto e = stampSpan(parseBinaryExpression(11), exprStart, current_.location);
    if (dynamic_cast<ast::RangeExpr*>(e.get())) {
        auto rp = std::make_unique<ast::RangePattern>();
        rp->rangeExpr = std::move(e);
        return std::unique_ptr<ast::Pattern>(rp.release());
    }
    auto cp = std::make_unique<ast::ConstantPattern>();
    cp->constants.push_back(std::move(e));
    // §A.7 (v0.167, #1371): a single Constant pattern. `|`-joined alternatives
    // are collected by the parsePattern wrapper; the comma is exclusively the
    // arm/slot separator, so no `,`-alternatives loop remains here.
    return std::unique_ptr<ast::Pattern>(cp.release());
}

ast::PositionalPattern Parser::parsePositionalPattern() {
    ast::PositionalPattern pp;
    // A.7 PositionalPattern: a Pattern OR a naked binding Identifier (valid
    // only inside destructuring position). Naked binding is detected as a
    // single Identifier followed immediately by `,` or `)`. Any other shape
    // (Identifier . ..., Identifier ( ..., literal, `var`, etc.) takes the
    // Pattern path — including the `_` wildcard slot, which since v0.244 is
    // the reserved Discard token and so can no longer be an Identifier here.
    if (current_.kind == TokenKind::Identifier) {
        const Token& nxt = peekAt(1);
        if (nxt.kind == TokenKind::Punctuator &&
            (nxt.text == "," || nxt.text == ")")) {
            pp.nakedBinding = current_.text;
            // #1271: mint a real BindingPattern Node as the naked binding's
            // dynamic_cast-safe anchor (sema/codegen key its slot on this).
            pp.nakedAnchor = std::make_unique<ast::BindingPattern>();
            pp.nakedAnchor->name = current_.text;
            pp.nakedAnchor->span.begin = current_.location;
            pp.nakedAnchor->span.end = current_.location;
            advance();
            return pp;
        }
    }
    pp.pattern = parsePattern(/*inPositional=*/true);
    return pp;
}

ast::CaseArm Parser::parseCaseArm(bool scrutineeLess) {
    ast::CaseArm arm;
    if (scrutineeLess) {
        // §19.1 scrutinee-less form: each arm's "pattern" is a full boolean
        // expression (e.g. `int(this) < int(other)`, `score >= 90`, `_ `).
        // The `_` catch-all is the only conventional pattern that remains
        // as-is; since v0.244 it is the reserved Discard token (A.1).
        if (atDiscard()) {
            advance();
            arm.pattern = std::make_unique<ast::DiscardPattern>();
        } else {
            // Parse a full expression as the boolean pattern. Wrap it as a
            // ConstantPattern so the rest of the arm pipeline is uniform.
            auto boolExpr = parseExpression();
            auto cp = std::make_unique<ast::ConstantPattern>();
            cp->constants.push_back(std::move(boolExpr));
            arm.pattern = std::move(cp);
        }
    } else {
        arm.pattern = parsePattern();
    }
    if (consumeKeyword("when")) {
        // §19.4/§17: a parenthesised guard like `(t > 0)` is followed by the
        // case-arm `=>` separator. Since v0.217 (§27.3) a lambda is keyword-
        // led (`lambda(...)`), so the guard's `(…) =>` token sequence can no
        // longer be mis-read as a lambda head — the #1256 suppression flag
        // this site used to set is deleted with the old spelling.
        arm.guard = parseExpression();
    }
    if (!consumePunctuator("=>")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected '=>' after case-arm pattern");
    }
    // Slice 38: only the Expression ArmBody form. Statement-form ArmBody is
    // valid only in statement-position case (also deferred); the `do … end`
    // ArmBody form lands with the statement-position case slice.
    arm.bodyKind = ast::CaseArmBodyKind::Expression;
    arm.exprBody = parseExpression();
    return arm;
}

// #744 / §19: the scrutinee-less bare-`case` detection (isScruntineeLessCase)
// has been deleted — the scrutinee-less condition-dispatch form is now spelled
// `when` (parseWhenExpr / parseWhenStmt), so `case` is unconditionally
// scrutinee'd and the parser no longer consults source line numbers anywhere.

// §19.1 / A.7 CaseExpr (v0.166, #1370): the `of` delimiter separates the case
// scrutinee from the first arm. `of` is a CONTEXTUAL keyword — Appendix B does
// not reserve it, so the lexer emits it as an ordinary Identifier and it is
// recognized only here, in the scrutinee-terminating position. Programs may
// still use `of` as an identifier anywhere else. A missing `of` is a syntax
// error at the first arm token; the diagnostic names the delimiter.
bool Parser::expectOfDelimiter() {
    if (current_.kind == TokenKind::Identifier && current_.text == "of") {
        advance();
        return true;
    }
    diagnostics_.report(Severity::Error, current_.location,
                        "expected `of` after the case scrutinee");
    return false;
}

std::unique_ptr<ast::Expr> Parser::parseCaseExpr(bool parenAtom) {
    if (!parenAtom) {
        // v0.208 cutover (§19.7, #2079): the retired `end`-closed expression
        // spelling. One targeted diagnostic; the old form is then parsed for
        // recovery so downstream sees a well-formed CaseExpr.
        diagnostics_.report(
            Severity::Error, current_.location,
            "an expression-position `case \xe2\x80\xa6 of \xe2\x80\xa6 end` "
            "is the retired pre-v0.208 spelling \xe2\x80\x94 the expression "
            "form is the parenthesized atom: wrap it and drop `end` "
            "(`(case \xe2\x80\xa6 of arm, \xe2\x80\xa6)`) "
            "(\xc2\xa7""19.1/\xc2\xa7""19.7, v0.208)");
    }
    advance(); // past `case`
    auto ce = std::make_unique<ast::CaseExpr>();

    // §19.1 / A.7 CaseExpr (v0.166, #1370): `case Expression "of" ...`. `case`
    // ALWAYS takes a scrutinee; the scrutinee-less condition-dispatch form is
    // spelled `when` (parseWhenExpr). The `of` delimiter terminates the
    // scrutinee, so the scrutinee is a FULL Expression — no precedence
    // restriction and no parenthesization rule. The old multiplicative-level
    // restriction (`parseBinaryExpression(14)`) that existed only to serve the
    // missing boundary (#1233/#1320) is removed; a first pattern may begin with
    // `-`, `..`, `..<`, or a relational token with no possibility of the
    // scrutinee absorbing it, because `of` marks the boundary explicitly.
    SourceLocation start = current_.location;
    ce->scrutinee = stampSpan(parseExpression(), start, current_.location);
    expectOfDelimiter();

    // §19.7 (v0.208): the atom closes with `)`; the retired recovery form
    // with `end` (+ optional `case` tail, v0.170).
    auto atCloser = [&]() {
        return parenAtom ? isPunctuator(")") : isKeyword("end");
    };
    ce->arms.push_back(parseCaseArm(/*bareForm=*/false));
    while (consumePunctuator(",")) {
        // Trailing `","?` per the grammar — `,` followed immediately by the
        // closer terminates the list.
        if (atCloser()) break;
        ce->arms.push_back(parseCaseArm(/*bareForm=*/false));
    }
    if (parenAtom) {
        if (!consumePunctuator(")")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ')' to close the `(case \xe2\x80\xa6)` expression "
                "atom (\xc2\xa7""19.7, v0.208)");
        }
    } else {
        consumeKeyword("end");
        consumeOptionalEndKeyword("case"); // §1.6 / v0.170 (#1374)
    }
    return std::unique_ptr<ast::Expr>(ce.release());
}

// #744 / §19: `when Arm ("," Arm)* ","? end` — the dedicated scrutinee-less
// condition-dispatch form. Each arm's "pattern" is a full boolean expression;
// the first whose condition is true is taken. It produces the same
// null-scrutinee CaseExpr the bare-`case` form does today (sema/codegen's
// scrutinee-less path is reused), but with NO layout-significant disambiguation
// — `when` is unambiguous. (Migrating bare `case` → `when` and deleting the
// heuristic is a later stage.)
std::unique_ptr<ast::Expr> Parser::parseWhenExpr(bool parenAtom) {
    if (!parenAtom) {
        // v0.208 cutover (§19.7, #2079): the retired `end`-closed expression
        // spelling — see parseCaseExpr.
        diagnostics_.report(
            Severity::Error, current_.location,
            "an expression-position `when \xe2\x80\xa6 end` is the retired "
            "pre-v0.208 spelling \xe2\x80\x94 the expression form is the "
            "parenthesized atom: wrap it and drop `end` "
            "(`(when arm, \xe2\x80\xa6)`) "
            "(\xc2\xa7""19.1/\xc2\xa7""19.7, v0.208)");
    }
    advance();  // past `when`
    auto ce = std::make_unique<ast::CaseExpr>();
    auto atCloser = [&]() {
        return parenAtom ? isPunctuator(")") : isKeyword("end");
    };
    // Scrutinee stays null: `when` is always the condition-dispatch form.
    ce->arms.push_back(parseCaseArm(/*bareForm=*/true));
    while (consumePunctuator(",")) {
        if (atCloser()) break;  // trailing `,?`
        ce->arms.push_back(parseCaseArm(/*bareForm=*/true));
    }
    if (parenAtom) {
        if (!consumePunctuator(")")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ')' to close the `(when \xe2\x80\xa6)` expression "
                "atom (\xc2\xa7""19.7, v0.208)");
        }
    } else {
        consumeKeyword("end");
        consumeOptionalEndKeyword("when"); // §1.6 / v0.170 (#1374)
    }
    return std::unique_ptr<ast::Expr>(ce.release());
}

// §20 / A.7 IfExpr (v0.208, #2079): `"(" "if" Expression "then" Expression
// ("elif" Expression "then" Expression)* "else" Expression ")"`. The caller
// consumed the `(`; current_ is `if`. Each condition and branch derives a
// FULL Expression (a bare `throw` branch is admitted anywhere — parse-
// ExpressionImpl handles the leading `throw`); `else` is mandatory. The
// elif chain right-nests into the ConditionalExpr representation, so the
// selection/typing/ownership semantics are the pre-v0.208 machinery
// unchanged (v0.208: "semantics untouched").
std::unique_ptr<ast::Expr> Parser::parseIfExprAtom() {
    advance(); // past `if`
    struct Seg { std::unique_ptr<ast::Expr> cond, value; };
    std::vector<Seg> segs;
    auto parseSeg = [&]() {
        Seg s;
        s.cond = parseExpression();
        if (!consumeKeyword("then")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected 'then' after the `(if \xe2\x80\xa6)` atom's "
                "condition (\xc2\xa7""20, v0.208)");
        }
        s.value = parseExpression();
        segs.push_back(std::move(s));
    };
    parseSeg();
    while (consumeKeyword("elif")) parseSeg();
    std::unique_ptr<ast::Expr> elseValue;
    if (!consumeKeyword("else")) {
        // §20: "the `else` branch is mandatory (an expression produces a
        // value on every path)".
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected 'else' \xe2\x80\x94 the `(if \xe2\x80\xa6)` atom's "
            "`else` branch is mandatory: an expression produces a value on "
            "every path (\xc2\xa7""20, v0.208)");
    } else {
        elseValue = parseExpression();
    }
    if (!consumePunctuator(")")) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected ')' to close the `(if \xe2\x80\xa6)` expression atom "
            "(\xc2\xa7""20, v0.208)");
    }
    // Right-nest the elif chain.
    std::unique_ptr<ast::Expr> tail = std::move(elseValue);
    for (std::size_t i = segs.size(); i-- > 0;) {
        auto c = std::make_unique<ast::ConditionalExpr>();
        c->condition = std::move(segs[i].cond);
        c->thenValue = std::move(segs[i].value);
        c->elseValue = std::move(tail);
        tail = std::unique_ptr<ast::Expr>(c.release());
    }
    return tail;
}

// §17.15 / A.7 BindExpr (v0.216, #2098): `"(" BindClause+ Expression ")"`,
// `BindClause ::= "var" Identifier (":" Type)? ":=" Expression ";"`. The
// caller consumed the `(`; current_ is `var` (so at least one clause is
// guaranteed). Each clause is represented as an embedded plain-`var`
// LocalDecl (isBindAtomClause set) so sema's §21.1 declaration typing and
// codegen's lowerLocalDecl are reused verbatim; the `;` terminates the
// clause (no expression contains a top-level `;`, so the extent is
// unambiguous). Clause initializers admit the `owned` binding-position
// transfer marker (K3.4 / §28.3, the same consumeKeyword("owned") hook the
// A.6 LocalDecl statement uses); the body is one full Expression and the
// atom supplies its own `)`.
std::unique_ptr<ast::Expr> Parser::parseBindExprAtom() {
    auto bx = std::make_unique<ast::BindExpr>();
    while (isKeyword("var")) {
        auto clause = std::make_unique<ast::LocalDecl>();
        clause->isBindAtomClause = true;
        clause->span.begin = current_.location;
        advance();  // past `var`
        if (atDiscard()) {
            // §17.13 / §17.15 (v0.244, #3049): the binding-atom clause is one
            // of the three positions the gate added to the rejected list, and
            // its remedy is deliberately NOT the statement pair — "`_ := e;`"
            // and "`using var _ := e;`" are statements and are not spellable
            // inside the `(var … )` atom.
            reportDiscardNotDeclarable(
                "binding-atom clause",
                "name the binding or restructure the expression — the "
                "statement discards (`_ := e;`, `using var _ := e;`) are not "
                "spellable inside the `(var …)` atom (§17.15)");
            clause->name = "_";  // recovery: keep parsing this clause
        } else if (current_.kind != TokenKind::Identifier) {
            // Appendix B.1 / #2090 sibling: a keyword in the clause-name slot
            // is almost certainly an attempted binding name; diagnose the
            // cause directly so one typo does not cascade.
            if (current_.kind == TokenKind::Keyword) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    std::string("`") + std::string(current_.text) +
                        "` is a reserved keyword and cannot be used as an "
                        "identifier (Appendix B.1); rename it");
                clause->name = current_.text;
                advance();
            } else {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected a binding name after `var` in the "
                    "`(var \xe2\x80\xa6)` binding atom (\xc2\xa7""17.15, "
                    "v0.216)");
            }
        } else {
            clause->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            clause->type = parseType();
        }
        if (!consumePunctuator(":=")) {
            // §17.15: a clause's initializer is mandatory (A.7 BindClause has
            // no initializer-less form — a binding is initialized exactly
            // once, at its clause).
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected `:=` \xe2\x80\x94 a binding-atom clause requires "
                "an initializer (`var x := E;`) (\xc2\xa7""17.15, v0.216)");
        } else {
            // §28.3 / §17.15: the `owned` binding-position transfer marker —
            // `var y := owned x;` is K3.4's marked binding transfer inside
            // the atom exactly as at statement level (#1962).
            if (consumeKeyword("owned"))
                clause->initializerHasOwnedMarker = true;
            clause->initializer = parseExpression();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected `;` to terminate this binding-atom clause "
                "(\xc2\xa7""17.15, v0.216)");
        }
        clause->span.end = current_.location;
        bx->clauses.push_back(std::move(clause));
    }
    // The body is one full Expression supplying the atom's value and type.
    bx->body = parseExpression();
    if (!consumePunctuator(")")) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected ')' to close the `(var \xe2\x80\xa6)` binding atom "
            "(\xc2\xa7""17.15, v0.216)");
    }
    return std::unique_ptr<ast::Expr>(bx.release());
}

// §20 (v0.208, #2079): see Parser.hpp — the retired postfix conditional
// `x if c else y`, detected at generic-error sites by its stray `if`. The
// error-path scan confirms a top-level `else` before `then`/`do`/`;` so a
// legitimate if STATEMENT after a merely-missing `;` is not mislabeled.
bool Parser::checkRetiredPostfixConditional() {
    if (!isKeyword("if")) return false;
    std::size_t k = 1;
    int depth = 0;
    for (;;) {
        const Token& t = peekAt(k);
        if (t.kind == TokenKind::EndOfFile) return false;
        if (t.kind == TokenKind::Punctuator) {
            if (t.text == "(" || t.text == "[") ++depth;
            else if (t.text == ")" || t.text == "]") {
                if (depth == 0) return false;
                --depth;
            } else if (t.text == ";" && depth == 0) {
                return false;
            }
        } else if (t.kind == TokenKind::Keyword && depth == 0) {
            if (t.text == "then" || t.text == "do") return false;
            if (t.text == "else") break;
        }
        ++k;
    }
    diagnostics_.report(
        Severity::Error, current_.location,
        "the postfix conditional `x if c else y` is the retired pre-v0.208 "
        "spelling \xe2\x80\x94 the conditional is the condition-first "
        "parenthesized atom `(if c then x else y)` (\xc2\xa7""20, v0.208)");
    // Swallow `if c else y` so one retired conditional yields one diagnostic.
    advance(); // past `if`
    parseExpression();
    consumeKeyword("else");
    parseExpression();
    return true;
}

// Umbrella for the retired-spelling checks at statement/argument error
// sites: the v0.217 paren lambda (#2101) and the v0.208 postfix conditional
// (#2079). At most one fires per site.
bool Parser::checkRetiredExprSpelling() {
    if (checkRetiredParenLambda()) return true;
    return checkRetiredPostfixConditional();
}

// §A.7 (v0.167, #1371): helper for folding a NamedPattern into a
// ConstantPattern alternative member.

std::unique_ptr<ast::Expr> Parser::qualifiedNameToExpr(
    std::unique_ptr<ast::QualifiedName> qn) {
    if (!qn || qn->parts.empty()) return nullptr;
    auto first = std::make_unique<ast::IdentifierExpr>();
    first->name = qn->parts[0];
    std::unique_ptr<ast::Expr> cur(first.release());
    for (std::size_t i = 1; i < qn->parts.size(); ++i) {
        auto ma = std::make_unique<ast::MemberAccessExpr>();
        ma->target = std::move(cur);
        ma->member = qn->parts[i];
        cur = std::unique_ptr<ast::Expr>(ma.release());
    }
    return cur;
}

// Slice 39: statement-position `case`.

std::unique_ptr<ast::Expr> Parser::parsePostfix(std::unique_ptr<ast::Expr> head) {
    // fix #3582: `parsePrimaryExpression` returns a NULL expression at exactly
    // the two points where it has ALREADY reported a located error — the
    // generic "expected expression" and §17.13's bare `_` — and that null is
    // the parser's established "no expression here" signal: the
    // expression-statement fallthrough in Parser_Stmts.cpp reads it to hand
    // the offending token back to the statement-list loop, which advances past
    // it (Parser_Decls.cpp's `advance(); // crude recovery`). A suffix chain
    // has nothing to attach to, and every arm below reads `head->span.begin`
    // and moves `head` into a suffix node, so the null must be propagated, not
    // dereferenced. Before this the `.` / `?.` / `!` arms dereferenced it and
    // the compiler SEGV'd with no diagnostic. The token is deliberately NOT
    // consumed here: the diagnostic is already on the record and
    // resynchronisation belongs to the statement-list loop, whose null-return
    // contract guarantees forward progress.
    if (!head) return nullptr;
    // §A.5 / §17.1 level 1 postfix chain: `.member`, `(args)`, `?.member`,
    // `?(args)`, `[indices]`, `!` (postfix null-forgiving). All bind to the
    // expression on the left.
    for (;;) {
        // per §14.1 (issue #561): explicit method-level type arguments on a bare
        // identifier call — `Id<string>("hello")` — where the identifier is in
        // callee position (no leading dot). The disambiguating scan confirms
        // `< TypeArgList > (` so this is a generic call, not a relational chain.
        // Store the type args on the IdentifierExpr so sema and codegen can
        // substitute T→concrete when typing and emitting the call.
        if (isPunctuator("<") && tokensLookLikeMethodTypeArgs()) {
            if (auto* idHead =
                    dynamic_cast<ast::IdentifierExpr*>(head.get())) {
                if (idHead->typeArgs.empty()) {
                    advance(); // past `<`
                    idHead->typeArgs.push_back(parseType());
                    while (isPunctuator(",")) {
                        advance(); // past `,`
                        // per §1.6 / proposal 0059 (v0.214): optional
                        // trailing `,` before the closing angle.
                        if (current_.kind == TokenKind::Punctuator &&
                            !current_.text.empty() &&
                            current_.text.front() == '>') {
                            break;
                        }
                        idHead->typeArgs.push_back(parseType());
                    }
                    if (!consumeClosingAngle()) {
                        diagnostics_.report(
                            Severity::Error, current_.location,
                            "expected '>' to close method type-argument list");
                    }
                    continue; // loop back — the `(` call-suffix follows
                }
            }
        }
        if (consumePunctuator(".")) {
            if (current_.kind != TokenKind::Identifier) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected member name after '.'");
                break;
            }
            // fix #3214 (v0.254): see NullCondMemberExpr branch below.
            SourceLocation suffixBegin = head->span.begin;
            auto access = std::make_unique<ast::MemberAccessExpr>();
            access->target = std::move(head);
            access->member = current_.text;
            advance();
            // per §14.1 (issue #509): optional explicit method-level type
            // arguments `<T1, T2, ...>` following the member name.  Only
            // parsed when the disambiguating scan confirms the `<` is a
            // generic-arg list (closes with `>` followed by `(`), not a
            // relational `<` operator.
            if (isPunctuator("<") && tokensLookLikeMethodTypeArgs()) {
                advance(); // past `<`
                access->typeArgs.push_back(parseType());
                while (isPunctuator(",")) {
                    advance(); // past `,`
                    // per §1.6 / proposal 0059 (v0.214): optional trailing
                    // `,` before the closing angle.
                    if (current_.kind == TokenKind::Punctuator &&
                        !current_.text.empty() &&
                        current_.text.front() == '>') {
                        break;
                    }
                    access->typeArgs.push_back(parseType());
                }
                if (!consumeClosingAngle()) {
                    diagnostics_.report(Severity::Error, current_.location,
                        "expected '>' to close method type-argument list");
                }
            }
            access->span.begin = suffixBegin;
            access->span.end = current_.location;
            head = std::unique_ptr<ast::Expr>(access.release());
        } else if (consumePunctuator("?.")) {
            if (current_.kind != TokenKind::Identifier) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected member name after '?.'");
                break;
            }
            // fix #3214 (v0.254): every postfix-suffix node carries a span
            // [target.begin, current_.location-after-suffix], so diagnostics
            // keyed on the new node's span (call, index, member, null-cond
            // variants — 100+ sites in ExprTypecheck_Calls.cpp alone) report
            // the call/member/index site instead of 0:0.
            SourceLocation suffixBegin = head->span.begin;
            auto a = std::make_unique<ast::NullCondMemberExpr>();
            a->target = std::move(head);
            a->member = current_.text;
            advance();
            // per §14.1/§22.12 (issue #1278): optional explicit method-level
            // type arguments `<T1, T2, ...>` following the member name on a
            // null-conditional access — `obj?.Method<T>(...)`. Mirrors the
            // `.` branch above verbatim: only parsed when the disambiguating
            // scan confirms the `<` closes with `>` followed by `(`, not a
            // relational `<` operator (§22.12's generics-vs-less-than rule,
            // which the grammar states applies identically to `?.`).
            if (isPunctuator("<") && tokensLookLikeMethodTypeArgs()) {
                advance(); // past `<`
                a->typeArgs.push_back(parseType());
                while (isPunctuator(",")) {
                    advance(); // past `,`
                    // per §1.6 / proposal 0059 (v0.214): optional trailing
                    // `,` before the closing angle.
                    if (current_.kind == TokenKind::Punctuator &&
                        !current_.text.empty() &&
                        current_.text.front() == '>') {
                        break;
                    }
                    a->typeArgs.push_back(parseType());
                }
                if (!consumeClosingAngle()) {
                    diagnostics_.report(Severity::Error, current_.location,
                        "expected '>' to close method type-argument list");
                }
            }
            a->span.begin = suffixBegin;
            a->span.end = current_.location;
            head = std::unique_ptr<ast::Expr>(a.release());
        } else if (consumePunctuator("(")) {
            // fix #3214 (v0.254): see NullCondMemberExpr branch above.
            SourceLocation suffixBegin = head->span.begin;
            auto call = std::make_unique<ast::CallExpr>();
            call->callee = std::move(head);
            call->args = parseArgList();
            if (!consumePunctuator(")")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected ')' to close call argument list");
            }
            call->span.begin = suffixBegin;
            call->span.end = current_.location;
            head = std::unique_ptr<ast::Expr>(call.release());
        } else if (consumePunctuator("?(")) {
            // fix #3214 (v0.254): see NullCondMemberExpr branch above.
            SourceLocation suffixBegin = head->span.begin;
            auto call = std::make_unique<ast::NullCondCallExpr>();
            call->callee = std::move(head);
            call->args = parseArgList();
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close '?(' argument list");
            }
            call->span.begin = suffixBegin;
            call->span.end = current_.location;
            head = std::unique_ptr<ast::Expr>(call.release());
        } else if (consumePunctuator("[")) {
            // fix #3214 (v0.254): see NullCondMemberExpr branch above.
            SourceLocation suffixBegin = head->span.begin;
            auto idx = std::make_unique<ast::IndexExpr>();
            idx->target = std::move(head);
            // §A.7 PrimarySuffix `[ ArgList ]` — share the call-arg grammar
            // (slice 55). parseArgList stops on `)`; here we drive it until
            // `]` instead by inlining the empty-check.
            if (!isPunctuator("]")) {
                idx->indices = parseArgList();
            }
            if (!consumePunctuator("]")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected ']' to close index list");
            }
            idx->span.begin = suffixBegin;
            idx->span.end = current_.location;
            head = std::unique_ptr<ast::Expr>(idx.release());
        } else if (consumePunctuator("!")) {
            // fix #3214 (v0.254): see NullCondMemberExpr branch above.
            SourceLocation suffixBegin = head->span.begin;
            auto bang = std::make_unique<ast::PostfixBangExpr>();
            bang->operand = std::move(head);
            bang->span.begin = suffixBegin;
            bang->span.end = current_.location;
            head = std::unique_ptr<ast::Expr>(bang.release());
        } else if (isKeyword("with") &&
                   (!disableWithSuffix_ ||
                    (peekAt(1).kind == TokenKind::Punctuator &&
                     peekAt(1).text == "(" &&
                     peekAt(2).kind == TokenKind::Identifier &&
                     peekAt(3).kind == TokenKind::Punctuator &&
                     peekAt(3).text == ":="))) {
            // §9.5 / A.7 `with` PrimarySuffix (v0.208, #2079):
            //   "with" "(" InitAssign ("," InitAssign)* ","? ")"
            //   InitAssign ::= Identifier ":=" Expression
            // Source-type determines the semantic (record copy-with-overrides
            // vs object initializer on `new C(...)`); sema disambiguates.
            // In an `await for` header (disableWithSuffix_), §30.7's stated
            // rule applies: header-`with` followed by `( Identifier :=` is
            // this update suffix on the source; any other `with` is left for
            // the for-statement's cancellation clause.
            advance(); // past `with`
            auto w = std::make_unique<ast::WithExpr>();
            w->source = std::move(head);
            auto parseInitAssign = [&]() -> ast::WithExpr::InitAssign {
                ast::WithExpr::InitAssign ia;
                if (current_.kind != TokenKind::Identifier) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected identifier in `with` init-assignment");
                } else {
                    ia.name = current_.text;
                    advance();
                }
                if (!consumePunctuator(":=")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ':=' in `with` init-assignment");
                }
                ia.value = parseExpression();
                return ia;
            };
            if (consumePunctuator("(")) {
                // The v0.208 keyword-attached-parentheses form.
                w->assigns.push_back(parseInitAssign());
                while (consumePunctuator(",")) {
                    if (isPunctuator(")")) break; // trailing `,?`
                    w->assigns.push_back(parseInitAssign());
                }
                if (!consumePunctuator(")")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ')' to close the `with(\xe2\x80\xa6)` "
                        "update suffix (\xc2\xa7""9.5, v0.208)");
                }
            } else {
                // v0.208 cutover: the retired `with Name := value, \xe2\x80\xa6
                // end` spelling. One targeted diagnostic, then the old list is
                // parsed for recovery.
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "`with Name := value, \xe2\x80\xa6 end` is the retired "
                    "pre-v0.208 record-update spelling \xe2\x80\x94 the "
                    "suffix carries keyword-attached parentheses and no "
                    "`end`: `with(Name := value, \xe2\x80\xa6)` "
                    "(\xc2\xa7""9.5, v0.208)");
                w->assigns.push_back(parseInitAssign());
                while (consumePunctuator(",")) {
                    if (isKeyword("end")) break; // trailing `,?`
                    w->assigns.push_back(parseInitAssign());
                }
                consumeKeyword("end");
            }
            head = std::unique_ptr<ast::Expr>(w.release());
        } else {
            break;
        }
    }
    return head;
}

} // namespace bezel
