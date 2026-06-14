#include "parser/Parser.hpp"

#include <algorithm>
#include <string_view>

namespace bezel {

namespace {

// §3.4: the lowercase primitive-type aliases. These are unconditionally
// reserved keywords (Appendix B.1) but they ARE valid type-name first
// components — `int`, `string`, etc. The PascalCase forms (`Object`, `String`,
// `Array`) are ordinary identifiers and need no special handling here.
bool isPrimitiveTypeKeyword(std::string_view text) {
    return text == "bool"   || text == "byte"  || text == "sbyte" ||
           text == "short"  || text == "ushort" || text == "int"  ||
           text == "uint"   || text == "long"  || text == "ulong" ||
           text == "float"  || text == "double" || text == "decimal" ||
           text == "char"   || text == "string";
}

} // namespace

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
    // §A.7 ConditionalExpr ::= LogicalOr ("if" Expression "else" Expression)?
    //                         | ThrowExpr | CaseExpr (later slice)
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
    auto left = parseBinaryExpression(1);
    // Conditional suffix. Now that statement constructs that contain an
    // Expression-followed-by-Statement (`if cond then …`, `while cond do …`,
    // `for var x in expr do …`) have body-start keywords, naive `if`-suffix
    // parsing here can't gobble a statement's `if` — it'd just fail to find
    // `else` and report a parse error.
    if (consumeKeyword("if")) {
        auto cond = parseExpression();
        if (!consumeKeyword("else")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected 'else' after conditional 'if' condition");
        }
        auto right = parseExpression();
        auto c = std::make_unique<ast::ConditionalExpr>();
        c->thenValue = std::move(left);
        c->condition = std::move(cond);
        c->elseValue = std::move(right);
        return std::unique_ptr<ast::Expr>(c.release());
    }
    return left;
}

std::unique_ptr<ast::Expr> Parser::parseBinaryExpression(int minPrecedence) {
    // Precedence climbing. Right-associative operators (`??`) recurse with the
    // same minPrec; left-associative ones recurse with prec + 1 so same-prec
    // ops associate to the left. The keyword relationals `is`, `is not`, and
    // `as` have a Type on the right-hand side and produce TypeTestExpr; `in`
    // takes a regular expression on the right.
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
                    return current_.text == "do" || current_.text == "with" ||
                           current_.text == "when" ||
                           current_.text == "end" ||
                           current_.text == "else" ||
                           current_.text == "elif";
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
            auto right = isKeyword("throw") ? parseExpressionImpl()
                                            : parseBinaryExpression(nextMin);
            auto bin = std::make_unique<ast::BinaryExpr>();
            bin->op = opText;
            bin->left = std::move(left);
            bin->right = std::move(right);
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
        // §18.5 / §18.7: split the `$"..."` token into alternating literal
        // and expression parts. The lexer already validated balanced braces.
        splitInterpolation(*e);
        advance();
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
                return current_.text == "do" || current_.text == "with" ||
                       current_.text == "when" || current_.text == "end" ||
                       current_.text == "else" || current_.text == "elif";
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

    // §A.7 CaseExpr — `"case" Expression? CaseArm ("," CaseArm)* ","? "end"`.
    // The bare form (no scrutinee) is admitted by the grammar; slice 38
    // requires the scrutinee form (the bare-form lookahead requires deciding
    // "is the next token a pattern or an expression" without backtracking,
    // which lands later).
    if (isKeyword("case")) {
        return parseCaseExpr();
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

    // Lambda: `async? "(" LambdaParams? ")" "=>" LambdaBody` (A.7). The
    // discriminator is "what follows the matching `)`?": `=>` means lambda,
    // anything else means parenthesised expression. We need to scan past the
    // matching `)` before deciding — that is the parser's one use of
    // unbounded buffered lookahead (peekAt).
    const bool asyncLambdaCandidate =
        isKeyword("async") &&
        peekAt(1).kind == TokenKind::Punctuator && peekAt(1).text == "(" &&
        tokensLookLikeLambda(1);
    const bool plainLambdaCandidate =
        isPunctuator("(") && tokensLookLikeLambda(0);

    if (asyncLambdaCandidate || plainLambdaCandidate) {
        auto lam = std::make_unique<ast::LambdaExpr>();
        if (asyncLambdaCandidate) {
            lam->isAsync = true;
            advance(); // past `async`
        }
        consumePunctuator("("); // confirmed by lookahead
        if (!consumePunctuator(")")) {
            // Non-empty LambdaParams.
            lam->params.push_back(parseLambdaParam());
            while (consumePunctuator(",")) {
                lam->params.push_back(parseLambdaParam());
            }
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close lambda parameter list");
            }
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
        // §A.7 unit value `()` — empty parens at PrimaryAtom level.
        if (consumePunctuator(")")) {
            return std::unique_ptr<ast::Expr>(
                std::make_unique<ast::UnitExpr>().release());
        }
        // No ParenExpr node: source-level parens only group the parse; the
        // resulting tree shape encodes the grouping directly. See Node.hpp.
        auto inner = parseExpression();
        if (!consumePunctuator(")")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ')' to close parenthesized expression");
        }
        return inner;
    }

    diagnostics_.report(Severity::Error, current_.location,
                        "expected expression");
    return nullptr;
}

bool Parser::tokensLookLikeLambda(std::size_t startK) {
    // peekAt(startK) must be `(`. Scan forward through balanced parens to
    // find the matching `)`, then return true iff `=>` follows. Brackets
    // `[]` and angles `<>` need not be balanced separately: they nest
    // *inside* the outer `(...)` regardless, and we only care about which
    // `)` matches the leading `(`. Bails out on EOF.
    std::size_t depth = 1;
    std::size_t k = startK + 1;
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
bool Parser::tokensLookLikeMethodTypeArgs() {
    // peekAt(0) must be `<`.
    std::size_t k = 1;                // start scanning after the opening `<`
    std::size_t depth = 1;            // nesting level of `<` / `>`
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
                --depth;
                if (depth == 0) {
                    // The closing `>` of the arg list.  Confirm `(` follows.
                    const Token& after = peekAt(k + 1);
                    return after.kind == TokenKind::Punctuator &&
                           after.text == "(";
                }
                continue;
            }
            // Punctuators valid inside a type arg: `,`, `?`, `[`, `]`.
            if (t.text == "," || t.text == "?" ||
                t.text == "[" || t.text == "]") continue;
            // Any other punctuator (e.g. `+`, `-`, `*`, `(`, `)`, `=`,
            // `..`) cannot appear inside a type arg; bail out.
            return false;
        }
        if (t.kind == TokenKind::Keyword) {
            // Primitive-type keywords (int, string, bool, …) are valid type
            // names.  Other keywords (if, while, return, …) cannot appear
            // inside a type arg.
            if (isPrimitiveTypeKeyword(t.text)) continue;
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
    static const std::size_t kLimit = 64;
    for (std::size_t guard = 0; guard < kLimit; ++guard, ++k) {
        const Token& t = peekAt(k);
        if (t.kind == TokenKind::EndOfFile) return false;
        if (t.kind == TokenKind::Punctuator) {
            if (t.text == "<") { ++depth; continue; }
            if (t.text == ">>") {
                if (depth <= 2) {
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
            if (t.text == "," || t.text == "?" ||
                t.text == "[" || t.text == "]") continue;
            return false;
        }
        if (t.kind == TokenKind::Keyword) {
            if (isPrimitiveTypeKeyword(t.text)) continue;
            return false;
        }
        if (t.kind == TokenKind::Identifier) continue;
        return false;
    }
    return false; // exceeded scan limit
}

ast::Param Parser::parseLambdaParam() {
    // A.7 `LambdaParam ::= FuncParamMods? Identifier (":" Type)?`. Type is
    // optional (unlike A.4 method ParamList).
    ast::Param p;
    if (consumeKeyword("ref")) {
        p.modifier = "ref";
    } else if (consumeKeyword("out")) {
        if (consumeKeyword("owned")) {
            p.modifier = "out owned";
        } else {
            p.modifier = "out";
        }
    } else if (consumeKeyword("owned")) {
        p.modifier = "owned";
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

// ===== Slice 38: patterns and case expressions (§A.7) =====================

std::unique_ptr<ast::Pattern> Parser::parsePattern(bool inPositional) {
    // `_` discard. `_` is an Identifier per the lexer.
    if (current_.kind == TokenKind::Identifier && current_.text == "_") {
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
    // Relational pattern: `<`, `>`, `<=`, `>=` followed by Expression.
    if (isPunctuator("<") || isPunctuator(">") ||
        isPunctuator("<=") || isPunctuator(">=")) {
        auto rop = std::make_unique<ast::RelOpPattern>();
        rop->op = current_.text;
        advance();
        rop->expr = parseExpression();
        return std::unique_ptr<ast::Pattern>(rop.release());
    }
    // §16.2 / §19.3 (#726): a TRAILING-open range pattern (`31..`) is handled by
    // the catch-all below (the leading expression is parsed, then `..` makes it a
    // RangeExpr with an absent upper bound; codegen now matches the open bound).
    // A LEADING-open range pattern (`..<EXPR`) cannot be parsed unambiguously
    // here: in the scrutinee form `case x ..<10` the scrutinee parser greedily
    // consumes `x ..< 10` as a range expression. Use the equivalent relational
    // pattern (`< EXPR` / `<= EXPR`), which is supported, instead.
    if (isPunctuator("..") || isPunctuator("..<")) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "leading-open range pattern (`..<x` / `..x`) is not supported as a "
            "case pattern (it is ambiguous with a `scrutinee..hi` range); use "
            "the equivalent relational pattern `< x` / `<= x` (§19.3)");
        advance();
        return nullptr;
    }
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
        if (isPunctuator(",") && isConstantStart(peekAt(1))) {
            // Convert qn → IdentifierExpr/MemberAccess chain and continue
            // as ConstantPattern alternatives.
            auto cp = std::make_unique<ast::ConstantPattern>();
            cp->constants.push_back(qualifiedNameToExpr(std::move(qn)));
            while (consumePunctuator(",")) {
                cp->constants.push_back(parseConstantExpr());
                if (!(isPunctuator(",") && isConstantStart(peekAt(1)))) break;
            }
            return std::unique_ptr<ast::Pattern>(cp.release());
        }
        auto np = std::make_unique<ast::NamedPattern>();
        np->name = std::move(qn);
        return std::unique_ptr<ast::Pattern>(np.release());
    }
    // Catch-all: parse an Expression and classify. A RangeExpr at the root
    // is a RangePattern (slice 52 — replaces the earlier BinaryExpr-based
    // representation); otherwise wrap as a ConstantPattern with one or
    // more `,`-separated constants (slice 44).
    auto e = parseExpression();
    if (dynamic_cast<ast::RangeExpr*>(e.get())) {
        auto rp = std::make_unique<ast::RangePattern>();
        rp->rangeExpr = std::move(e);
        return std::unique_ptr<ast::Pattern>(rp.release());
    }
    auto cp = std::make_unique<ast::ConstantPattern>();
    cp->constants.push_back(std::move(e));
    // §A.7: ConstantPattern alternatives (`,` list) are only valid at the
    // case-arm level, NOT inside DestructuringPattern sub-patterns — the `,`
    // there is the sub-pattern separator. Suppress the alternatives loop when
    // parsing a positional sub-pattern (inPositional=true).
    if (!inPositional) {
        while (isPunctuator(",") && isConstantStart(peekAt(1))) {
            advance(); // past `,`
            cp->constants.push_back(parseConstantExpr());
        }
    }
    return std::unique_ptr<ast::Pattern>(cp.release());
}

ast::PositionalPattern Parser::parsePositionalPattern() {
    ast::PositionalPattern pp;
    // A.7 PositionalPattern: a Pattern OR a naked binding Identifier (valid
    // only inside destructuring position). Naked binding is detected as a
    // single Identifier (not `_`) followed immediately by `,` or `)`. Any
    // other shape (Identifier . ..., Identifier ( ..., literal, `var`, etc.)
    // takes the Pattern path.
    if (current_.kind == TokenKind::Identifier &&
        current_.text != "_") {
        const Token& nxt = peekAt(1);
        if (nxt.kind == TokenKind::Punctuator &&
            (nxt.text == "," || nxt.text == ")")) {
            pp.nakedBinding = current_.text;
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
        // The `_` discard is the only conventional pattern that remains as-is.
        if (current_.kind == TokenKind::Identifier && current_.text == "_") {
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

// §19.1 / §A.7 `Expression?`: detect the scrutinee-less bare-`case` form.
// Returns true when either:
//   (a) the first arm's first token is on a different (later) source line
//       than the `case` keyword — the canonical written style for bare case;
//   (b) a bounded forward scan to the first `=>` at depth 0 finds an
//       operator that cannot appear in a Pattern (equality `=`/`<>`, logical
//       `or`/`and`) — an unambiguous signal that the arms are full boolean
//       expressions rather than patterns.
// Called immediately after advance() past `case`; `caseKeyLine` is the
// source line of the consumed `case` token.
bool Parser::isScruntineeLessCase(uint32_t caseKeyLine) const {
    // Heuristic (a): `case` keyword on a different line than first arm token.
    if (current_.location.line != caseKeyLine) return true;
    // Heuristic (b): scan for non-pattern operators before first `=>`.
    // Track nesting depth for `(`, `)`, `[`, `]`; bail at `end`/EOF.
    int depth = 0;
    for (std::size_t i = 0; ; ++i) {
        const Token& t = const_cast<Parser*>(this)->peekAt(i);
        if (t.kind == TokenKind::EndOfFile) break;
        if (t.kind == TokenKind::Punctuator) {
            if (t.text == "(" || t.text == "[") { depth++; continue; }
            if (t.text == ")" || t.text == "]") { depth--; continue; }
            if (depth == 0) {
                if (t.text == "=>") break;  // end of first arm's pattern region
            }
        }
        if (t.kind == TokenKind::Keyword && depth == 0) {
            if (t.text == "end") break;  // empty or malformed case
            // `or` / `and` — logical operators, not in Pattern
            if (t.text == "or" || t.text == "and") return true;
        }
    }
    return false;
}

std::unique_ptr<ast::Expr> Parser::parseCaseExpr() {
    const uint32_t caseKeyLine = current_.location.line;
    advance(); // past `case`
    auto ce = std::make_unique<ast::CaseExpr>();

    // §19.1 / §A.7 `Expression?`: detect scrutinee-less form. In the bare
    // form (no scrutinee), each arm's pattern is a full boolean expression
    // and no scrutinee value exists.
    //
    // §19: in scrutinee form, the scrutinee is limited to operators above
    // the relational level (precedence ≥ 8) so that relop patterns
    // (`>=90`, `<0`, etc.) are recognised as arm patterns rather than
    // consumed as part of a boolean scrutinee. Users needing a full boolean
    // expression as scrutinee can wrap it in parentheses.
    const bool bareForm = isScruntineeLessCase(caseKeyLine);
    if (!bareForm) {
        SourceLocation start = current_.location;
        ce->scrutinee = stampSpan(parseBinaryExpression(8), start, current_.location);
    }
    // ce->scrutinee remains null for bare form (grammar: Expression?).

    ce->arms.push_back(parseCaseArm(bareForm));
    while (consumePunctuator(",")) {
        // Trailing `","?` per the grammar — `,` followed immediately by
        // `end` terminates the list.
        if (isKeyword("end")) break;
        ce->arms.push_back(parseCaseArm(bareForm));
    }
    if (!consumeKeyword("end")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'end' to close case expression");
    }
    return std::unique_ptr<ast::Expr>(ce.release());
}

// Slice 44: helpers for ConstantPattern alternatives.

bool Parser::isConstantStart(const Token& t) const {
    if (t.kind == TokenKind::IntLiteral ||
        t.kind == TokenKind::FloatLiteral ||
        t.kind == TokenKind::CharLiteral ||
        t.kind == TokenKind::StringLiteral ||
        t.kind == TokenKind::VerbatimStringLiteral) return true;
    if (t.kind == TokenKind::Keyword &&
        (t.text == "true" || t.text == "false" || t.text == "null")) {
        return true;
    }
    // Identifier — could begin a QualifiedName Constant — but `_` is the
    // DiscardPattern, not a Constant.
    return t.kind == TokenKind::Identifier && t.text != "_";
}

std::unique_ptr<ast::Expr> Parser::parseConstantExpr() {
    // A.8 Constant. Literals go through parsePrimaryExpression; QualifiedName
    // is rebuilt as the equivalent IdentifierExpr / MemberAccessExpr chain.
    // Note: `null` is a keyword the lexer emits as TokenKind::Keyword, and
    // parsePrimaryExpression handles it as NullLiteral.
    if (current_.kind == TokenKind::Identifier) {
        auto qn = parseQualifiedName();
        return qualifiedNameToExpr(std::move(qn));
    }
    return parsePrimaryExpression();
}

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
                    access->typeArgs.push_back(parseType());
                }
                if (!consumeClosingAngle()) {
                    diagnostics_.report(Severity::Error, current_.location,
                        "expected '>' to close method type-argument list");
                }
            }
            head = std::unique_ptr<ast::Expr>(access.release());
        } else if (consumePunctuator("?.")) {
            if (current_.kind != TokenKind::Identifier) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected member name after '?.'");
                break;
            }
            auto a = std::make_unique<ast::NullCondMemberExpr>();
            a->target = std::move(head);
            a->member = current_.text;
            advance();
            head = std::unique_ptr<ast::Expr>(a.release());
        } else if (consumePunctuator("(")) {
            auto call = std::make_unique<ast::CallExpr>();
            call->callee = std::move(head);
            call->args = parseArgList();
            if (!consumePunctuator(")")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected ')' to close call argument list");
            }
            head = std::unique_ptr<ast::Expr>(call.release());
        } else if (consumePunctuator("?(")) {
            auto call = std::make_unique<ast::NullCondCallExpr>();
            call->callee = std::move(head);
            call->args = parseArgList();
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close '?(' argument list");
            }
            head = std::unique_ptr<ast::Expr>(call.release());
        } else if (consumePunctuator("[")) {
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
            head = std::unique_ptr<ast::Expr>(idx.release());
        } else if (consumePunctuator("!")) {
            auto bang = std::make_unique<ast::PostfixBangExpr>();
            bang->operand = std::move(head);
            head = std::unique_ptr<ast::Expr>(bang.release());
        } else if (!disableWithSuffix_ && consumeKeyword("with")) {
            // §A.7 `with` PrimarySuffix:
            //   "with" InitAssign ("," InitAssign)* ","? "end"
            //   InitAssign ::= Identifier ":=" Expression
            // Source-type determines the semantic (record copy-with-overrides
            // vs object initializer on `new C(...)`); sema disambiguates.
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
            w->assigns.push_back(parseInitAssign());
            while (consumePunctuator(",")) {
                if (isKeyword("end")) break; // trailing `,?`
                w->assigns.push_back(parseInitAssign());
            }
            if (!consumeKeyword("end")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected 'end' to close `with` initializer block");
            }
            head = std::unique_ptr<ast::Expr>(w.release());
        } else {
            break;
        }
    }
    return head;
}

} // namespace bezel
