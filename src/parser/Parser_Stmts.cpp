#include "parser/Parser.hpp"

#include <algorithm>
#include <string>
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


// A.6 `DiscardStmt ::= "_" ":=" Expression ";"` — see Parser.hpp.
std::unique_ptr<ast::Stmt> Parser::parseDiscardStmt(bool semicolonOptional) {
    const SourceLocation start = current_.location;
    advance();  // past the reserved `_`
    if (!isPunctuator(":=")) {
        // §17.13: `_` is a sink, not a name and not an lvalue, so `_` followed
        // by anything but `:=` (`_;`, `_ += e;`, `_.M();`, `_++;`) has no
        // reading at all. Name the two statement-position spellings.
        diagnostics_.report(
            Severity::Error, start,
            "`_` is the reserved discard token — it is not a value and not an "
            "lvalue, so the only statement it spells is the discard "
            "`_ := expression;` (evaluate, then release at end of statement); "
            "to hold the value anonymously until scope end write "
            "`using var _ := expression;` (§1.3, §17.13, §28.10)");
        return nullptr;
    }
    advance();  // past `:=`
    auto d = std::make_unique<ast::DiscardStmt>();
    d->span.begin = start;
    d->value = parseExpression();
    if (!consumePunctuator(";") && !semicolonOptional) {
        // v0.217 (§27.3, #2101): retired lambda spelling.
        if (!checkRetiredExprSpelling() || !consumePunctuator(";"))
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after discard statement");
    }
    return std::unique_ptr<ast::Stmt>(d.release());
}

ast::CaseArm Parser::parseCaseStmtArm(bool scrutineeLess) {
    ast::CaseArm arm;
    if (scrutineeLess) {
        // §19.1 scrutinee-less: arm pattern is a full boolean expression.
        // `_` is the always-true catch-all (A.7 `WhenArm ::= (Expression |
        // "_") "=>" ArmBody`) and remains special. Since v0.244 it arrives as
        // the reserved Discard token rather than an Identifier (A.1).
        if (atDiscard()) {
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
        //
        // §17.13 / A.6 (v0.244): `_ := e;` is committed on its leading token —
        // `_` is the reserved discard token, never an Identifier, so it can no
        // longer arrive here as a leading IdentifierExpr. §19.7 makes the
        // arm's `;` optional (`,`/`end` may follow directly).
        if (atDiscard()) {
            if (auto d = parseDiscardStmt(/*semicolonOptional=*/true))
                arm.stmts.push_back(std::move(d));
            return arm;
        }
        SourceLocation start = current_.location;
        auto lhs = parseExpression();
        bool isInc = current_.kind == TokenKind::Punctuator &&
                     (current_.text == "++" || current_.text == "--");
        bool isAssign = current_.kind == TokenKind::Punctuator &&
                        (current_.text == ":="  || current_.text == "+="  ||
                         current_.text == "-="  || current_.text == "*="  ||
                         current_.text == "/="  || current_.text == "mod=" ||
                         current_.text == "&="  ||
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
            // (The former `_ := expr` recovery — an IdentifierExpr LHS named
            // `_` — is gone: since v0.244 the discard is committed on its own
            // token above, so nothing named `_` can reach this branch.)
            auto a = std::make_unique<ast::AssignmentStmt>();
            a->target = std::move(lhs);
            a->op = current_.text;
            a->span.begin = start;
            advance();
            // §28.3 / §28.7 / §28.10 (v0.180, #1965): the `owned`
            // assignment-position transfer marker — `y := owned x;` — in a
            // case/when arm body. See parseStatementImpl for the rationale.
            if (a->op == ":=" && consumeKeyword("owned"))
                a->valueHasOwnedMarker = true;
            a->value = parseExpression();
            consumePunctuator(";");
            arm.stmts.push_back(std::move(a));
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
    // ALWAYS takes a scrutinee; the condition-dispatch form formerly spelled
    // `when` is retired (v0.289, proposal 0122 L1). The `of` delimiter terminates the scrutinee, so the
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
    consumeOptionalEndKeyword("case"); // §1.6 / v0.170 (#1374)
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end' of case statement");
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

    // §21.5 / A.6 LoopLabel (v0.201, #2051): a loop label prefixes a loop
    // statement — `scan: for …` / `scan: while …` / `scan: await for …`. Two
    // tokens of lookahead at statement start: `Identifier ":"`. No v0.197
    // statement begins that way (`:=` is one lexeme; every other statement-
    // leading identifier is followed by `.`/`[`/`(`/an operator/`;`/`=>`, never a
    // bare `:`), so this is unambiguous (grammar A.6 commentary). After the label
    // the next token MUST introduce a loop; anything else is a compile error.
    std::string_view loopLabel;
    if (current_.kind == TokenKind::Identifier &&
        peekNext().kind == TokenKind::Punctuator && peekNext().text == ":") {
        loopLabel = current_.text;
        advance(); // Identifier
        advance(); // ":"
        const bool isLoopStart =
            isKeyword("while") || isKeyword("for") ||
            (isKeyword("await") && peekNext().kind == TokenKind::Keyword &&
             peekNext().text == "for");
        if (!isLoopStart) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "a loop label may prefix only a loop statement — `for`, "
                "`while`, or `await for` (§21.5)");
            loopLabel = {}; // drop it; parse whatever follows for recovery
        }
    } else if (atDiscard() && peekNext().kind == TokenKind::Punctuator &&
               peekNext().text == ":") {
        // §17.13 / §21.5 (v0.244, #3049): "`_` is not a valid loop label: a
        // label is a declaration, and `_` cannot be declared." (`_ :=` is one
        // lexeme, so this cannot collide with the discard statement below.)
        reportDiscardNotDeclarable(
            "loop label",
            "a label is a declaration, and `_` cannot be declared (§21.5)");
        consumePunctuator(":");  // drop the `:` too; parse the loop unlabeled
    }

    // A.6 DiscardStmt (§17.13): `_ := Expression ;`. Committed on the leading
    // reserved token — §17.13's "the form is recognized lexically by the
    // leading `_` token" — because since v0.244 `_` is not an Identifier and
    // therefore not an expression, so the old parse-an-expression-then-check
    // recovery has nothing to recover from.
    if (atDiscard()) return parseDiscardStmt(/*semicolonOptional=*/false);

    // A.6 LocalConstDecl (slice 53):
    //   "const" Identifier (":" Type)? ":=" ConstExpr ";"
    if (isKeyword("const")) {
        advance(); // past `const`
        auto lc = std::make_unique<ast::LocalConstDecl>();
        if (atDiscard()) {
            // §21.1 / §17.13 (v0.244, #3049): "`const _ := e;` is likewise
            // rejected (a never-readable constant is useless by construction)".
            // The two-remedy tail is the same as `var _`'s: nothing about a
            // `const` changes which sanctioned spelling replaces it.
            reportDiscardNotDeclarable(
                "local const",
                "a never-readable constant is useless by construction — write "
                "`_ := e;` to evaluate and release the value at end of "
                "statement, or `using var _ := e;` to hold it anonymously "
                "until scope end (§21.1, §28.10)");
            lc->name = "_";  // recovery: keep parsing this declaration
        } else if (current_.kind != TokenKind::Identifier) {
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
            // v0.217 (§27.3, #2101): retired `(params) => body` spelling.
            if (!checkRetiredExprSpelling() || !consumePunctuator(";"))
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
        if (atDiscard()) {
            // A.6 LocalDecl (v0.244, #3049): the `"_"` alternative exists ONLY
            // on the `using var` / `await using var` forms — the anonymous
            // owning binding of §17.13 ("the one `_` form that owns storage":
            // scope-end disposal, no name bound), whose `(":" Type)?`
            // ascription slot is deliberately RETAINED (`using var _: Stream
            // := e;`). Plain `var _ := e;` does not parse as a LocalDecl and
            // is rejected with §21.1's two-remedy diagnostic.
            if (decl->isUsing) {
                decl->name = "_";
                advance();
            } else {
                reportDiscardNotDeclarable(
                    "local",
                    "write `_ := e;` to evaluate and release the value at end "
                    "of statement, or `using var _ := e;` to hold it "
                    "anonymously until scope end (§21.1, §28.10)");
                decl->name = "_";  // recovery: keep parsing this declaration
            }
        } else if (current_.kind != TokenKind::Identifier) {
            // Appendix B.1 (v0.215) / #2090: a keyword in the identifier
            // slot is almost certainly an attempted local name. Diagnose the
            // cause directly and consume it so one typo does not cascade into
            // opaque expression and terminator errors.
            if (current_.kind == TokenKind::Keyword) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    std::string("`") + std::string(current_.text) +
                        "` is a reserved keyword and cannot be used as an "
                        "identifier (Appendix B.1); rename it");
                decl->name = current_.text;
                advance();
            } else {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected local-variable name");
            }
        } else {
            decl->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            decl->type = parseType();
        }
        if (consumePunctuator(":=")) {
            // §28.3 / §28.10 (v0.180, #1962): the `owned` binding-position
            // transfer marker — `var y := owned x;` / `using var y := owned x;`.
            // This parallels the call-site marker (§22.3, `M(owned x)`): the
            // keyword announces that the initializer MOVES a named owned binding
            // (leaving it moved-from) rather than borrowing it. `owned` is a
            // modifier keyword that can never begin an expression, so consuming
            // it here is unambiguous. The ownership pass validates the source and
            // records the move; the flag routes the binding to the same
            // move-transfer handling as an `owned` call argument.
            if (consumeKeyword("owned"))
                decl->initializerHasOwnedMarker = true;
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
            // v0.217 (§27.3, #2101): `var f := (x) => …;` — the retired
            // lambda spelling parses its head as a parenthesized expression
            // and trips here on the `=>`; name the migration instead.
            if (!checkRetiredExprSpelling() || !consumePunctuator(";"))
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
    // v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
    // statement is retired — `when` remains reserved and appears in two positions
    // only: the arm guard (§19.4) and the out-null postcondition clause (§22.4);
    // both are still valid. A `when`-led statement is a syntax error naming the
    // replacement (the if/elif/else statement).
    if (isKeyword("when")) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "a `when`-led statement is retired (\xc2\xa7""19 / \xc2\xa7""22.4, "
            "v0.289) \xe2\x80\x94 use the `if`/`elif`/`else` statement "
            "(`if c1 then \xe2\x80\xa6 elif c2 then \xe2\x80\xa6 else \xe2\x80\xa6 end;`)");
        return nullptr;
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
        consumeOptionalEndKeyword("if"); // §1.6 / v0.170 (#1374)
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'if'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // WhileStmt (A.6): `LoopLabel? "while" Expression "do" Statement*
    //                   "end" ("while" | Identifier)? ";"`.
    if (isKeyword("while")) {
        advance();
        auto stmt = std::make_unique<ast::WhileStmt>();
        stmt->label = loopLabel; // §21.5 (v0.201): empty when unlabeled
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
        // §1.6 / §21.5 (v0.201): a labeled loop is a NAMED construct — its
        // closer is `end scan;` (or bare `end;`); the keyword closer `end while;`
        // applies only to unlabeled loops.
        consumeOptionalLoopEndLabel("while", loopLabel);
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
                    if (atDiscard()) {
                        // §17.13 / §31.2 (v0.244, #3049): the catch-arm
                        // BINDING spelling of `_` is a rejected position —
                        // "write … the bindingless `catch E =>`". Recover as
                        // exactly that (drop the binding, keep the type).
                        reportDiscardNotDeclarable(
                            "catch-arm binding",
                            "write the bindingless form `catch E => …`, which "
                            "already binds nothing (§31.2)");
                        arm.hasBinding = false;
                    } else if (current_.kind != TokenKind::Identifier) {
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
                } else if (atDiscard() &&
                           peekAt(1).kind == TokenKind::Punctuator &&
                           peekAt(1).text == ":") {
                    // The `var`-less binding alias `catch name: Type` with the
                    // reserved `_` in the name slot — same rejection, same
                    // remedy (§17.13, §31.2).
                    reportDiscardNotDeclarable(
                        "catch-arm binding",
                        "write the bindingless form `catch E => …`, which "
                        "already binds nothing (§31.2)");
                    advance();  // past ":"
                } else if (current_.kind == TokenKind::Identifier &&
                           peekAt(1).kind == TokenKind::Punctuator &&
                           peekAt(1).text == ":") {
                    // `catch name: Type` without `var` — accepted silently.
                    arm.hasBinding = true;
                    arm.varName = current_.text;
                    advance(); // past name
                    advance(); // past ":"
                }
                // §31.2.1 (v0.243, #3046) — SPAN STAMPING. `Parser::parseType`
                // populates no span, so every `ast::Type` node carries the
                // degenerate 0:0 and a diagnostic anchored on a catch-arm TYPE
                // (the #1939 erasure guard, and the v0.243 arm-type rule)
                // reported `0:0` — the #1307/#2496 degenerate-span class,
                // measured on this very shape. Stamp it HERE, in the catch-arm
                // parse only, so no other Type-anchored diagnostic's location
                // can move. `stampSpan` is a no-op on an already-stamped node.
                // The start location is read BEFORE parseType consumes the
                // token and the end AFTER, in separate statements — argument
                // evaluation order is unspecified, so this must not be one call.
                {
                    SourceLocation tstart = current_.location;
                    auto ty = parseType();
                    arm.types.push_back(
                        stampSpan(std::move(ty), tstart, current_.location));
                }
                // §31.2.1 / §A.6 (v0.167, #1371): multi-type alternatives are
                // now `|`-separated — the same alternatives spelling as case
                // patterns. The comma is exclusively the arm separator, so no
                // lookahead past the comma is needed to tell an added type from
                // the next arm. `IOException | TimeoutException` and
                // `var e: IOException | TimeoutException` both collect here.
                while (consumePunctuator("|")) {
                    SourceLocation tstart = current_.location;
                    auto ty = parseType();
                    arm.types.push_back(
                        stampSpan(std::move(ty), tstart, current_.location));
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
                } else if (atDiscard()) {
                    // §17.13 / A.6 (v0.244): `_ := e;` in a catch-arm body.
                    // The discard is committed on its leading token, so it can
                    // no longer arrive as an IdentifierExpr in the
                    // expression-leading path below.
                    arm.body = parseDiscardStmt(/*semicolonOptional=*/false);
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
                         current_.text == "/="  || current_.text == "mod=" ||
                         current_.text == "&="  ||
                         current_.text == "|="  || current_.text == "^="  ||
                         current_.text == "<<=" || current_.text == ">>=" ||
                         current_.text == "??=")) {
                        // (The former `_ := expr` recovery — an IdentifierExpr
                        // LHS named `_` — is gone: since v0.244 the discard is
                        // committed on its own token above.)
                        auto a = std::make_unique<ast::AssignmentStmt>();
                        a->target = std::move(lhs);
                        a->op = current_.text;
                        advance();
                        // §28.3 / §28.7 / §28.10 (v0.180, #1965): the `owned`
                        // assignment-position transfer marker `y := owned x;`
                        // in a catch-arm body. See parseStatementImpl.
                        if (a->op == ":=" && consumeKeyword("owned"))
                            a->valueHasOwnedMarker = true;
                        a->value = parseExpression();
                        if (!consumePunctuator(";")) {
                            diagnostics_.report(
                                Severity::Error, current_.location,
                                "expected ';' after assignment");
                        }
                        arm.body = std::move(a);
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
        consumeOptionalEndKeyword("try"); // §1.6 / v0.170 (#1374)
        // §31.2 (#746 R41-G): "At least one of catch or finally must be
        // present." A bare `try ... end;` with neither was silently accepted.
        //
        // The wording carries the spec's own clause verbatim ("at least one of
        // `catch` or `finally` must be present"), not a paraphrase of it. The
        // pre-fix text said "at least one `catch` arm or a `finally` block",
        // which is the same rule in different words — and a diagnostic that
        // does not name the rule in the rule's language cannot be matched back
        // to it. The rule corpus measures exactly that: case `s31_2_0001`
        // authors `diagnostic: must be present` straight out of §31.2 and the
        // runner reported the row under "reject/warn cases whose diagnostic
        // does not carry the rule's wording" even while its verdict read
        // `pass` (the verdict is exit-code-only). per §31.2, v0.307
        if (stmt->catches.empty() && !stmt->hasFinally) {
            diagnostics_.report(Severity::Error, tryLoc,
                                "a `try` statement must have a `catch` block, "
                                "a `finally` block, or both — at least one of "
                                "`catch` or `finally` must be present (§31.2)");
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
        stmt->label = loopLabel; // §21.5 (v0.201): empty when unlabeled
        if (awaitFor) {
            stmt->isAwait = true;
            advance(); // past `await`
        }
        advance(); // past `for`
        if (!consumeKeyword("var")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'var' after 'for'");
        }
        // A.6 ForStmt `(Identifier | "_")` (§21.4 / §17.13): the `_`
        // alternative is the loop-variable IGNORE — "iterates for the body's
        // effects with the element unbound". It binds nothing readable (since
        // v0.244 no read of `_` can even be spelled), and the grammar's note
        // says the ascription it over-admits is rejected semantically: "a bare
        // ignore takes its type from context, never an annotation".
        const bool ignoreLoopVar = atDiscard();
        if (ignoreLoopVar) {
            stmt->varName = "_";
            advance();
            if (isPunctuator(":")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "a type ascription is not permitted on the loop-variable "
                    "ignore `_` — a bare ignore takes its type from context, "
                    "never an annotation; write `for var _ in …` (§17.13, "
                    "§21.4)");
                advance();          // past `:`
                stmt->varType = parseType();  // consume it for recovery
            }
        } else if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected loop variable name");
        } else {
            stmt->varName = current_.text;
            advance();
        }
        if (!ignoreLoopVar && consumePunctuator(":")) {
            stmt->varType = parseType();
        }
        if (!consumeKeyword("in")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'in' in for statement");
        }
        // §9.5/§30.7 (v0.208, #2079): in this header, `with` followed by
        // `( Identifier :=` is the record-update suffix on the source (the
        // expression parser consumes it even under this flag); any other
        // `with` is the cancellation-token clause and is left here for the
        // for-statement (the content-based resolution the spec states).
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
        // §1.6 / §21.5 (v0.201): labeled loop closes with `end scan;` (or bare
        // `end;`); `end for;` applies only to unlabeled loops.
        consumeOptionalLoopEndLabel("for", loopLabel);
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
        consumeOptionalEndKeyword("do"); // §1.6 / v0.170 (#1374)
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'do' block");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // BreakStmt / ContinueStmt (A.6, v0.201): `"break" Identifier? ";"` /
    // `"continue" Identifier? ";"`. The optional identifier is the target loop
    // label (§21.5); sema resolves it against the enclosing labeled loops.
    if (consumeKeyword("break")) {
        auto stmt = std::make_unique<ast::BreakStmt>();
        if (current_.kind == TokenKind::Identifier) {
            stmt->label = current_.text;
            advance();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'break'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }
    if (consumeKeyword("continue")) {
        auto stmt = std::make_unique<ast::ContinueStmt>();
        if (current_.kind == TokenKind::Identifier) {
            stmt->label = current_.text;
            advance();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'continue'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
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
            // v0.217 (§27.3, #2101): retired `(params) => body` spelling.
            if (!checkRetiredExprSpelling() || !consumePunctuator(";"))
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

    // A.6 LocalTypeDecl (§21.10): `record` or `enum` as a statement.
    // §21.10 restricts this to record/enum only (not class/interface/choice),
    // with no visibility modifier and no generic params.
    // §21.10: a visibility keyword on a local type is a compile error.
    if (isKeyword("public") || isKeyword("private") ||
        isKeyword("internal") || isKeyword("protected")) {
        // Check if the next token is record or enum (a visibility keyword
        // preceding a local type declaration).
        auto visLoc = current_.location;
        std::string_view visWord = current_.text;
        advance(); // past the visibility keyword
        if (isKeyword("record") || isKeyword("enum")) {
            diagnostics_.report(
                Severity::Error, visLoc,
                std::string("a local type may not have a visibility modifier "
                            "(§21.10)"));
            // Parse and discard the local type so we don't cascade errors.
            (void)parseTypeOrInterfaceDecl(ast::Visibility::Private, {});
            return nullptr;
        }
        // Not a local type - visibility keyword in statement position is
        // a parse error (fall through to expression parsing which will
        // produce a more appropriate error).
        // Note: we cannot put the token back, so we just proceed to
        // expression parsing which will fail with "expected expression".
    }
    if (isKeyword("record") || isKeyword("enum")) {
        auto typeDecl = parseTypeOrInterfaceDecl(ast::Visibility::Private, {});
        // §21.10: only record and enum are allowed as local types.
        if (typeDecl &&
            typeDecl->kind != ast::TypeDeclKind::Record &&
            typeDecl->kind != ast::TypeDeclKind::Enum) {
            diagnostics_.report(
                Severity::Error, typeDecl->span.begin,
                "a local type may be `record` or `enum` only — `class`, "
                "`interface`, and `choice` may not be declared locally "
                "(§21.10)");
        }
        // §21.10: generic params are not permitted on a local type.
        if (typeDecl && !typeDecl->genericParams.empty()) {
            diagnostics_.report(
                Severity::Error, typeDecl->span.begin,
                "a local type may not have type parameters (§21.10)");
        }
        if (!typeDecl) return nullptr;
        auto localType = std::make_unique<ast::LocalTypeDecl>();
        localType->decl = std::move(typeDecl);
        return std::unique_ptr<ast::Stmt>(localType.release());
    }

    // Otherwise parse an expression and decide between IncrementStmt (`++`/
    // `--`), AssignmentStmt (assignment ops), and ExpressionStmt (fallback).
    // (DiscardStmt is committed on its leading `_` token above, §17.13.)
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
         current_.text == "/="  || current_.text == "mod=" ||
         current_.text == "&="  ||
         current_.text == "|="  || current_.text == "^="  ||
         current_.text == "<<=" || current_.text == ">>=" ||
         current_.text == "??=")) {
        // (The former discard recovery — an IdentifierExpr LHS named `_` — is
        // gone: since v0.244 `_` is the reserved discard token, so the
        // DiscardStmt is committed on its leading token at the top of this
        // function and never reaches the expression path.)
        auto a = std::make_unique<ast::AssignmentStmt>();
        a->target = std::move(lhs);
        a->op = current_.text;
        advance();
        // §28.3 / §28.7 / §28.10 (v0.180, #1965): the `owned` assignment-position
        // transfer marker — `y := owned x;`. The assignment-position analog of
        // the binding marker (`var y := owned x;`, #1962) and the call-site
        // marker (`M(owned x)`, §22.3): the keyword announces that the value
        // MOVES a named owned binding (leaving it moved-from) rather than
        // borrowing it. Only for a plain `:=` (a compound assign never takes an
        // ownership transfer). `owned` is a modifier keyword that can never begin
        // an expression, so consuming it here is unambiguous.
        if (a->op == ":=" && consumeKeyword("owned"))
            a->valueHasOwnedMarker = true;
        a->value = parseExpression();
        if (!consumePunctuator(";")) {
            // v0.217 (§27.3, #2101): retired `(params) => body` spelling.
            if (!checkRetiredExprSpelling() || !consumePunctuator(";"))
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
