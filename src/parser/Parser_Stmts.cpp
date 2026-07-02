#include "parser/Parser.hpp"

#include <algorithm>
#include <string_view>

namespace bezel {

namespace {

// Issue #135: span-stamping wrappers.
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


ast::CaseArm Parser::parseCaseStmtArm(bool scrutineeLess) {
    ast::CaseArm arm;
    if (scrutineeLess) {
        // §19.1 scrutinee-less: arm pattern is a full boolean expression.
        // `_` is the discard pattern and remains special.
        if (current_.kind == TokenKind::Identifier && current_.text == "_") {
            advance();
            arm.pattern = std::make_unique<ast::DiscardPattern>();
        } else {
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
    // ArmBody in statement position: §A.7 allows three forms:
    //   (1) `do … end` block of statements
    //   (2) A single Statement whose terminating `;` ends the arm body
    //   (3) An Expression (value discarded; per §19.7 spec examples,
    //       no `;` is required when the expression fills the arm body
    //       and `,` or `end` follows directly)
    //
    // Disambiguation:
    //   `do` keyword → form (1).
    //   Keywords that start statements only (`return`, `var`, …) → form (2).
    //   Everything else → parse as expression (form 3); if `;` follows,
    //   consume it (compatible with explicit-`;` statement style from §19.2).
    arm.bodyKind = ast::CaseArmBodyKind::Block;
    if (consumeKeyword("do")) {
        // Block form.
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto s = parseStatement();
            if (s) {
                arm.stmts.push_back(std::move(s));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected 'end' to close 'do' arm body");
        }
    } else if (current_.kind == TokenKind::Keyword &&
               (current_.text == "return"  || current_.text == "var"     ||
                current_.text == "using"   || current_.text == "for"     ||
                current_.text == "while"   || current_.text == "if"      ||
                current_.text == "break"   || current_.text == "continue"||
                current_.text == "try"     || current_.text == "yield"   ||
                current_.text == "await")) {
        // Statement-only keywords → parse as statement (form 2). The
        // statement consumes its own `;`, leaving `,` or `end` for the
        // outer case-arm loop.
        auto s = parseStatement();
        if (s) arm.stmts.push_back(std::move(s));
    } else {
        // Identifier-led body (forms 2/3): parse the leading expression, then —
        // mirroring parseStatement — promote it to an IncrementStmt (`x++`/`x--`)
        // or AssignmentStmt (`x := …`, compound assigns; `_ := …` → DiscardStmt)
        // when an assignment/increment operator follows. §19.2 admits a single
        // Statement here, so an identifier-led assignment `result := 30;` is
        // valid; previously it fell to the bare-expression path, which stopped
        // at `:=` and cascaded a parse error (#932). The terminating `;` is
        // OPTIONAL — `,`/`end` may follow directly (§19.7). The arm is uniformly
        // represented as a Block with one statement.
        SourceLocation start = current_.location;
        auto lhs = parseExpression();
        bool isInc = current_.kind == TokenKind::Punctuator &&
                     (current_.text == "++" || current_.text == "--");
        bool isAssign = current_.kind == TokenKind::Punctuator &&
                        (current_.text == ":="  || current_.text == "+="  ||
                         current_.text == "-="  || current_.text == "*="  ||
                         current_.text == "/="  || current_.text == "&="  ||
                         current_.text == "|="  || current_.text == "^="  ||
                         current_.text == "<<=" || current_.text == ">>=" ||
                         current_.text == "??=");
        if (isInc) {
            auto inc = std::make_unique<ast::IncrementStmt>();
            inc->target = std::move(lhs);
            inc->op = current_.text;
            inc->span.begin = start;
            advance();
            consumePunctuator(";");
            arm.stmts.push_back(std::move(inc));
        } else if (isAssign) {
            auto* idLhs = current_.text == ":="
                              ? dynamic_cast<ast::IdentifierExpr*>(lhs.get())
                              : nullptr;
            if (idLhs && idLhs->name == "_") {
                // Discard form `_ := expr`.
                advance();
                auto d = std::make_unique<ast::DiscardStmt>();
                d->span.begin = start;
                d->value = parseExpression();
                consumePunctuator(";");
                arm.stmts.push_back(std::move(d));
            } else {
                auto a = std::make_unique<ast::AssignmentStmt>();
                a->target = std::move(lhs);
                a->op = current_.text;
                a->span.begin = start;
                advance();
                a->value = parseExpression();
                consumePunctuator(";");
                arm.stmts.push_back(std::move(a));
            }
        } else {
            // Expression body (form 3). Optional trailing `;`.
            consumePunctuator(";");
            if (lhs) {
                auto es = std::make_unique<ast::ExpressionStmt>();
                es->expr = std::move(lhs);
                es->span.begin = start;
                arm.stmts.push_back(std::move(es));
            }
        }
    }
    return arm;
}

std::unique_ptr<ast::Stmt> Parser::parseCaseStmt() {
    advance(); // past `case`
    auto cs = std::make_unique<ast::CaseStmt>();
    // §19.1 / A.7 CaseExpr (v0.166, #1370): `case Expression "of" ...`. `case`
    // ALWAYS takes a scrutinee (scrutinee-less dispatch is `when`,
    // parseWhenStmt). The `of` delimiter terminates the scrutinee, so the
    // scrutinee is a FULL Expression — no precedence restriction and no
    // parenthesization rule (mirrors parseCaseExpr). The old multiplicative-
    // level restriction (#1233/#1320) that served the missing boundary is
    // removed; a first pattern may begin with `-`, `..`, `..<`, or a
    // relational token without the scrutinee absorbing it.
    SourceLocation start = current_.location;
    cs->scrutinee = stampSpan(parseExpression(), start, current_.location);
    expectOfDelimiter();
    cs->arms.push_back(parseCaseStmtArm(/*scrutineeLess=*/false));
    while (consumePunctuator(",")) {
        if (isKeyword("end")) break;
        cs->arms.push_back(parseCaseStmtArm(/*scrutineeLess=*/false));
    }
    if (!consumeKeyword("end")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'end' to close case statement");
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end' of case statement");
    }
    return std::unique_ptr<ast::Stmt>(cs.release());
}

// #744 / §19: statement-position `when … end;` — the scrutinee-less
// condition-dispatch form. Produces a null-scrutinee CaseStmt (sema/codegen
// reuse the existing scrutinee-less path); no layout-significant disambiguation.
std::unique_ptr<ast::Stmt> Parser::parseWhenStmt() {
    advance();  // past `when`
    auto cs = std::make_unique<ast::CaseStmt>();
    // Scrutinee stays null: `when` is always the condition-dispatch form.
    cs->arms.push_back(parseCaseStmtArm(/*scrutineeLess=*/true));
    while (consumePunctuator(",")) {
        if (isKeyword("end")) break;  // trailing `,?`
        cs->arms.push_back(parseCaseStmtArm(/*scrutineeLess=*/true));
    }
    if (!consumeKeyword("end")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'end' to close `when` statement");
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end' of `when` statement");
    }
    return std::unique_ptr<ast::Stmt>(cs.release());
}

std::unique_ptr<ast::Stmt> Parser::parseStatement() {
    SourceLocation start = current_.location;
    return stampSpan(parseStatementImpl(), start, current_.location);
}

std::unique_ptr<ast::Stmt> Parser::parseStatementImpl() {
    // §A.6 Statement dispatch. Slice 11 covers LocalDecl, ReturnStmt, and
    // the expression-led shapes (AssignmentStmt vs ExpressionStmt, decided
    // after parsing the leading expression).

    // A.6 LocalConstDecl (slice 53):
    //   "const" Identifier (":" Type)? ":=" ConstExpr ";"
    if (isKeyword("const")) {
        advance(); // past `const`
        auto lc = std::make_unique<ast::LocalConstDecl>();
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected name in local const declaration");
        } else {
            lc->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            lc->type = parseType();
        }
        if (!consumePunctuator(":=")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "local const requires an initializer ':=' (A.6 LocalConstDecl)");
        }
        lc->value = parseExpression();
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after local const declaration");
        }
        return std::unique_ptr<ast::Stmt>(lc.release());
    }

    // LocalDecl (A.6): `var Identifier (":" Type)? (":=" Expression)? ";"`,
    // `using var ...`, or the async `await using var ...` form. The async
    // form is detected by one-token lookahead on the leading `await` —
    // without it, `await` would otherwise begin a unary-expression
    // ExpressionStmt.
    const bool awaitUsing =
        isKeyword("await") &&
        peekNext().kind == TokenKind::Keyword &&
        peekNext().text == "using";
    if (isKeyword("var") || isKeyword("using") || awaitUsing) {
        auto decl = std::make_unique<ast::LocalDecl>();
        if (awaitUsing) {
            decl->isUsing = true;
            decl->isAwaitUsing = true;
            advance(); // past `await`
            consumeKeyword("using"); // confirmed by lookahead
            if (!consumeKeyword("var")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected 'var' after 'await using'");
            }
        } else if (consumeKeyword("using")) {
            decl->isUsing = true;
            if (!consumeKeyword("var")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected 'var' after 'using'");
            }
        } else {
            advance(); // past `var`
        }
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected local-variable name");
        } else {
            decl->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            decl->type = parseType();
        }
        if (consumePunctuator(":=")) {
            decl->initializer = parseExpression();
        } else if (decl->isUsing) {
            diagnostics_.report(
                Severity::Error, current_.location,
                decl->isAwaitUsing
                    ? "`await using var` requires an initializer "
                      "(A.6 LocalDecl)"
                    : "`using var` requires an initializer (A.6 LocalDecl)");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after local declaration");
        }
        return std::unique_ptr<ast::Stmt>(decl.release());
    }

    // CaseStmt (A.6). Statement-position `case` admits Statement-form and
    // `do … end` ArmBodies, unlike the Expression-position CaseExpr. We
    // intercept before the expression fallthrough so the `;` terminator and
    // the richer body forms work correctly.
    if (isKeyword("case")) {
        return parseCaseStmt();
    }
    // #744 / §19: statement-position `when` (scrutinee-less condition dispatch).
    if (isKeyword("when")) {
        return parseWhenStmt();
    }

    // IfStmt (A.6).
    if (isKeyword("if")) {
        advance(); // past `if`
        auto stmt = std::make_unique<ast::IfStmt>();
        stmt->ifBranch.condition = parseExpression();
        if (!consumeKeyword("then")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'then' after 'if' condition");
        }
        while (!isKeyword("elif") && !isKeyword("else") &&
               !isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto inner = parseStatement();
            if (inner) {
                stmt->ifBranch.body.push_back(std::move(inner));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        while (consumeKeyword("elif")) {
            ast::IfStmt::Branch elif;
            elif.condition = parseExpression();
            if (!consumeKeyword("then")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected 'then' after 'elif' condition");
            }
            while (!isKeyword("elif") && !isKeyword("else") &&
                   !isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto inner = parseStatement();
                if (inner) {
                    elif.body.push_back(std::move(inner));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
            }
            stmt->elifBranches.push_back(std::move(elif));
        }
        if (consumeKeyword("else")) {
            stmt->hasElse = true;
            while (!isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto inner = parseStatement();
                if (inner) {
                    stmt->elseBody.push_back(std::move(inner));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'if' statement");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'if'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // WhileStmt (A.6): `"while" Expression "do" Statement* "end" ";"`.
    if (isKeyword("while")) {
        advance();
        auto stmt = std::make_unique<ast::WhileStmt>();
        stmt->condition = parseExpression();
        if (!consumeKeyword("do")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'do' to begin 'while' body");
        }
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto inner = parseStatement();
            if (inner) {
                stmt->body.push_back(std::move(inner));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'while' statement");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'while'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // TryStmt (A.6). Slice 22 covers the binding catch arm only —
    // `catch var Identifier : Type => Statement` — plus optional finally.
    if (isKeyword("try")) {
        auto tryLoc = current_.location;  // for the §31.2 diagnostic below
        advance(); // past `try`
        auto stmt = std::make_unique<ast::TryStmt>();
        while (!isKeyword("catch") && !isKeyword("finally") &&
               !isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto s = parseStatement();
            if (s) {
                stmt->body.push_back(std::move(s));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        // §A.6: CatchBlock ::= "catch" CatchArm ("," CatchArm)* ","?
        // One "catch" keyword followed by one or more comma-separated arms.
        // Also support multiple "catch" keywords (one per arm) as alias.
        if (consumeKeyword("catch")) {
            // Parse one arm: CatchPattern ("when" Guard)? "=>" ArmBody
            auto parseCatchArm = [&]() {
                ast::CatchArm arm;
                if (consumeKeyword("var")) {
                    arm.hasBinding = true;
                    if (current_.kind != TokenKind::Identifier) {
                        diagnostics_.report(
                            Severity::Error, current_.location,
                            "expected exception-binding name");
                    } else {
                        arm.varName = current_.text;
                        advance();
                    }
                    if (!consumePunctuator(":")) {
                        diagnostics_.report(
                            Severity::Error, current_.location,
                            "expected ':' after catch binding name");
                    }
                } else if (current_.kind == TokenKind::Identifier &&
                           peekAt(1).kind == TokenKind::Punctuator &&
                           peekAt(1).text == ":") {
                    // `catch name: Type` without `var` — accepted silently.
                    arm.hasBinding = true;
                    arm.varName = current_.text;
                    advance(); // past name
                    advance(); // past ":"
                }
                arm.types.push_back(parseType());
                // §31.2.1 / §A.6 (v0.167, #1371): multi-type alternatives are
                // now `|`-separated — the same alternatives spelling as case
                // patterns. The comma is exclusively the arm separator, so no
                // lookahead past the comma is needed to tell an added type from
                // the next arm. `IOException | TimeoutException` and
                // `var e: IOException | TimeoutException` both collect here.
                while (consumePunctuator("|")) {
                    arm.types.push_back(parseType());
                }
                if (consumeKeyword("when")) {
                    arm.guard = parseExpression();
                }
                if (!consumePunctuator("=>")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected '=>' after catch pattern");
                }
                // §31.2.2 / §A.6 ArmBody: three forms are valid in a catch arm —
                //   (1) Expression    — no `;`; `,` or `end`/`finally` follows
                //   (2) Statement     — with its terminating `;`
                //   (3) "do" … "end" — multi-statement block, no `;` after `end`
                //
                // (3) must NOT go through parseStatement() for DoEndBlock because
                // parseStatement's DoEndBlock requires a trailing `;` that is absent
                // in arm-body position. Parse it inline, as parseCaseStmtArm does.
                // (2) delegates to parseStatement() for statement-leading keywords.
                // (1) parses an expression, then checks whether `;` follows.
                // per §31.2.2
                if (isKeyword("do")) {
                    // Inline `do … end` block — no trailing `;` required (arm body
                    // form per §31.2.2), but accept optional `;` for compatibility
                    // with the statement-form `do … end;` that parseStatement() would
                    // normally produce. The `;` is consumed when present and silently
                    // omitted when absent (e.g. when `,` or `end` follows `end`).
                    advance(); // past `do`
                    auto blk = std::make_unique<ast::DoEndBlock>();
                    while (!isKeyword("end") && !isKeyword("catch") &&
                           !isKeyword("finally") &&
                           current_.kind != TokenKind::EndOfFile) {
                        auto inner = parseStatement();
                        if (inner) blk->body.push_back(std::move(inner));
                        else if (current_.kind != TokenKind::EndOfFile) advance();
                    }
                    if (!consumeKeyword("end")) {
                        diagnostics_.report(Severity::Error, current_.location,
                                            "expected 'end' to close catch arm 'do' block");
                    }
                    consumePunctuator(";"); // optional: present in statement form, absent in expression form
                    arm.body = std::move(blk);
                } else if (isKeyword("return") ||
                    isKeyword("var")      || isKeyword("const")  ||
                    isKeyword("yield")    || isKeyword("break")  ||
                    isKeyword("continue") || isKeyword("using")  ||
                    isKeyword("await")) {
                    // Statement-leading: delegate to parseStatement() as before.
                    arm.body = parseStatement();
                } else {
                    // Expression-leading (includes bare `throw` re-raise and
                    // `throw expr`). Parse the expression first, then detect
                    // the increment/assignment/expression-stmt forms.
                    auto lhs = parseExpression();
                    if (current_.kind == TokenKind::Punctuator &&
                        (current_.text == "++" || current_.text == "--")) {
                        auto inc = std::make_unique<ast::IncrementStmt>();
                        inc->target = std::move(lhs);
                        inc->op = current_.text;
                        advance();
                        if (!consumePunctuator(";")) {
                            diagnostics_.report(
                                Severity::Error, current_.location,
                                "expected ';' after increment/decrement");
                        }
                        arm.body = std::move(inc);
                    } else if (
                        current_.kind == TokenKind::Punctuator &&
                        (current_.text == ":="  || current_.text == "+="  ||
                         current_.text == "-="  || current_.text == "*="  ||
                         current_.text == "/="  || current_.text == "&="  ||
                         current_.text == "|="  || current_.text == "^="  ||
                         current_.text == "<<=" || current_.text == ">>=" ||
                         current_.text == "??=")) {
                        // Discard form: `_ := expr;`
                        bool isDiscard = false;
                        if (current_.text == ":=") {
                            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                    lhs.get())) {
                                if (id->name == "_") {
                                    isDiscard = true;
                                    advance(); // past `:=`
                                    auto d = std::make_unique<ast::DiscardStmt>();
                                    d->value = parseExpression();
                                    if (!consumePunctuator(";")) {
                                        diagnostics_.report(
                                            Severity::Error, current_.location,
                                            "expected ';' after discard statement");
                                    }
                                    arm.body = std::move(d);
                                }
                            }
                        }
                        if (!isDiscard) {
                            auto a = std::make_unique<ast::AssignmentStmt>();
                            a->target = std::move(lhs);
                            a->op = current_.text;
                            advance();
                            a->value = parseExpression();
                            if (!consumePunctuator(";")) {
                                diagnostics_.report(
                                    Severity::Error, current_.location,
                                    "expected ';' after assignment");
                            }
                            arm.body = std::move(a);
                        }
                    } else {
                        // Pure expression form (§31.2.2 form 1) or expression
                        // statement form (form 2, with optional `;`).
                        // `;` is consumed when present (statement form) and
                        // silently omitted when the next token is `,` or
                        // `end`/`finally` (expression form per §31.2.2).
                        if (lhs || isPunctuator(";")) {
                            auto es = std::make_unique<ast::ExpressionStmt>();
                            es->expr = std::move(lhs);
                            consumePunctuator(";"); // optional — present in stmt form, absent in expr form
                            arm.body = std::move(es);
                        }
                        // If lhs is null and no `;`, fall through with null body
                        // (the arm will have no body, which downstream handles gracefully).
                    }
                }
                stmt->catches.push_back(std::move(arm));
            };
            parseCatchArm();
            // Additional arms: comma-separated inline OR repeated `catch`
            // keywords (possibly after a comma). Handle both styles:
            //   Style A (spec): `catch Arm1, Arm2 end;`
            //   Style B (alias): `catch Arm1;, catch Arm2; end;`
            while (true) {
                bool hadComma = consumePunctuator(",");
                if (isKeyword("end") || isKeyword("finally") ||
                    current_.kind == TokenKind::EndOfFile) break;
                if (consumeKeyword("catch")) {
                    // Style B: next arm starts with `catch` keyword.
                    parseCatchArm();
                } else if (hadComma && (isKeyword("var") ||
                           current_.kind == TokenKind::Identifier)) {
                    // Style A: inline next arm (no `catch` keyword).
                    parseCatchArm();
                } else {
                    // No more arms.
                    break;
                }
            }
        }
        if (consumeKeyword("finally")) {
            stmt->hasFinally = true;
            while (!isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto s = parseStatement();
                if (s) {
                    stmt->finallyBody.push_back(std::move(s));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'try' statement");
        }
        // §31.2 (#746 R41-G): "At least one of catch or finally must be
        // present." A bare `try ... end;` with neither was silently accepted.
        if (stmt->catches.empty() && !stmt->hasFinally) {
            diagnostics_.report(Severity::Error, tryLoc,
                                "a `try` statement must have at least one "
                                "`catch` arm or a `finally` block (§31.2)");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'try'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // ForStmt (A.6). Synchronous `for var ... in EXPR do ... end ;` and the
    // async `await for var ... in EXPR (with EXPR)? do ... end ;` form
    // (slice 35). The async form is detected by one-token lookahead on the
    // leading `await`.
    const bool awaitFor =
        isKeyword("await") &&
        peekNext().kind == TokenKind::Keyword &&
        peekNext().text == "for";
    if (isKeyword("for") || awaitFor) {
        auto stmt = std::make_unique<ast::ForStmt>();
        if (awaitFor) {
            stmt->isAwait = true;
            advance(); // past `await`
        }
        advance(); // past `for`
        if (!consumeKeyword("var")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'var' after 'for'");
        }
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected loop variable name");
        } else {
            stmt->varName = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            stmt->varType = parseType();
        }
        if (!consumeKeyword("in")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'in' in for statement");
        }
        // Disable the postfix `with` suffix here so the for-stmt sees its
        // own `with EXPR` cancellation-token clause rather than letting the
        // expression parser consume it as a WithExpr (slice 50).
        const bool prevDisable = disableWithSuffix_;
        disableWithSuffix_ = true;
        stmt->iterable = parseExpression();
        disableWithSuffix_ = prevDisable;
        // Optional `with EXPR` cancellation-token clause — async form only
        // (§30.7). Sema rejects it on the sync form.
        if (consumeKeyword("with")) {
            if (!stmt->isAwait) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "'with' clause is only valid on 'await for' (§30.7)");
            }
            stmt->withExpr = parseExpression();
        }
        if (!consumeKeyword("do")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'do' to begin 'for' body");
        }
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto inner = parseStatement();
            if (inner) {
                stmt->body.push_back(std::move(inner));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'for' statement");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'for'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // DoEndBlock (A.6) — plain statement-sequencing block, no value.
    if (isKeyword("do")) {
        advance();
        auto stmt = std::make_unique<ast::DoEndBlock>();
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto inner = parseStatement();
            if (inner) {
                stmt->body.push_back(std::move(inner));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'do' block");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'do' block");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    if (consumeKeyword("break")) {
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'break'");
        }
        return std::unique_ptr<ast::Stmt>(
            std::make_unique<ast::BreakStmt>().release());
    }
    if (consumeKeyword("continue")) {
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'continue'");
        }
        return std::unique_ptr<ast::Stmt>(
            std::make_unique<ast::ContinueStmt>().release());
    }

    // YieldStmt (A.6): `yield return Expression ";"` or `yield break ";"`.
    if (isKeyword("yield")) {
        advance();
        auto y = std::make_unique<ast::YieldStmt>();
        if (consumeKeyword("break")) {
            y->isBreak = true;
        } else if (consumeKeyword("return")) {
            y->value = parseExpression();
        } else {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected 'return' or 'break' after 'yield'");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after yield statement");
        }
        return std::unique_ptr<ast::Stmt>(y.release());
    }

    // ReturnStmt: `return Expression? ";"`.
    if (isKeyword("return")) {
        advance();
        auto ret = std::make_unique<ast::ReturnStmt>();
        if (!isPunctuator(";")) {
            ret->value = parseExpression();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after return statement");
        }
        return std::unique_ptr<ast::Stmt>(ret.release());
    }

    // ThrowStmt (A.6): `throw Expression? ";"`.
    if (isKeyword("throw")) {
        advance();
        auto th = std::make_unique<ast::ThrowStmt>();
        if (!isPunctuator(";")) {
            th->value = parseExpression();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after throw statement");
        }
        return std::unique_ptr<ast::Stmt>(th.release());
    }

    // Otherwise parse an expression and decide between IncrementStmt (`++`/
    // `--`), DiscardStmt (LHS is identifier `_` and the op is `:=`),
    // AssignmentStmt (other assignment ops), and ExpressionStmt (fallback).
    auto lhs = parseExpression();

    if (current_.kind == TokenKind::Punctuator &&
        (current_.text == "++" || current_.text == "--")) {
        auto inc = std::make_unique<ast::IncrementStmt>();
        inc->target = std::move(lhs);
        inc->op = current_.text;
        advance();
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after increment/decrement statement");
        }
        return std::unique_ptr<ast::Stmt>(inc.release());
    }

    if (current_.kind == TokenKind::Punctuator &&
        (current_.text == ":="  || current_.text == "+="  ||
         current_.text == "-="  || current_.text == "*="  ||
         current_.text == "/="  || current_.text == "&="  ||
         current_.text == "|="  || current_.text == "^="  ||
         current_.text == "<<=" || current_.text == ">>=" ||
         current_.text == "??=")) {
        // Discard form: LHS is the bare `_` identifier and the op is `:=`.
        if (current_.text == ":=") {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(lhs.get())) {
                if (id->name == "_") {
                    advance(); // past `:=`
                    auto d = std::make_unique<ast::DiscardStmt>();
                    d->value = parseExpression();
                    if (!consumePunctuator(";")) {
                        diagnostics_.report(
                            Severity::Error, current_.location,
                            "expected ';' after discard statement");
                    }
                    return std::unique_ptr<ast::Stmt>(d.release());
                }
            }
        }
        auto a = std::make_unique<ast::AssignmentStmt>();
        a->target = std::move(lhs);
        a->op = current_.text;
        advance();
        a->value = parseExpression();
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after assignment");
        }
        return std::unique_ptr<ast::Stmt>(a.release());
    }

    // If the expression is null (e.g. a keyword token where an expression
    // was expected) and no `;` follows, return nullptr to allow the body-
    // parsing loop to advance past the problematic token. Returning a non-
    // null ExpressionStmt with null expr and no progress causes an infinite
    // loop in statement-list parsers.
    if (!lhs && !isPunctuator(";")) return nullptr;
    auto es = std::make_unique<ast::ExpressionStmt>();
    es->expr = std::move(lhs);
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after expression statement");
    }
    return std::unique_ptr<ast::Stmt>(es.release());
}

} // namespace bezel
