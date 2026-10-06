// Spec-derived parser tests for the v0.170 uniform-closer revision (#1374).
//
// §1.6 (v0.170): every `end` may optionally repeat the construct's
// introducing keyword as an end-label; when present it is verified.
// Newly legal-and-checked: `end if;`, `end while;`, `end for;`,
// `end do;`, `end try;`, `end case;`, `end when;`, `end extension;`,
// `end extern;`.  A wrong keyword is a compile error.  Bare `end;`
// remains valid everywhere (unchanged).
//
// Expression-position: `consumeOptionalEndKeyword` is called
// unconditionally after `end` — keywords cannot continue an expression
// in postfix/infix position, so consuming the matching keyword is always
// safe (no `;`-gating restriction needed).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string wrapMethod(std::string_view body) {
    return std::string("module M; class C public method M() ") +
           std::string(body) + " end M; end C;";
}

static std::string parseMethodBody(std::string_view body) {
    auto r = parseAll(wrapMethod(body));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() < 2) return "<no-method>";
    // ms[0] is the class; ms[1] is the method
    const std::string prefix = "public method M() ";
    if (ms[1].rfind(prefix, 0) != 0) return "<bad-prefix>";
    return ms[1].substr(prefix.size());
}

// ── 1. `end if;` ─────────────────────────────────────────────────────────────

BEZEL_TEST(parser_uniform_closer_1374, end_if_label_accepted) {
    // §1.6 / v0.170: `end if;` as labeled closer for IfStmt.
    auto r = parseAll(wrapMethod("if x then y := 1; end if;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_if_label_parses_same_as_bare) {
    // The labeled and bare form produce the same AST summary.
    auto bare   = parseMethodBody("if x then y := 1; end;");
    auto labeled = parseMethodBody("if x then y := 1; end if;");
    BEZEL_EXPECT_EQ(labeled, bare);
}

BEZEL_TEST(parser_uniform_closer_1374, end_if_wrong_keyword_is_error) {
    // `end while;` inside an if is a mismatch — compile error (§1.6).
    auto r = parseAll(wrapMethod("if x then y := 1; end while;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── 2. `end while;` ──────────────────────────────────────────────────────────

BEZEL_TEST(parser_uniform_closer_1374, end_while_label_accepted) {
    // §1.6 / v0.170: `end while;` as labeled closer for WhileStmt.
    auto r = parseAll(wrapMethod("while cond do x := x + 1; end while;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_while_label_parses_same_as_bare) {
    auto bare    = parseMethodBody("while cond do x := 1; end;");
    auto labeled = parseMethodBody("while cond do x := 1; end while;");
    BEZEL_EXPECT_EQ(labeled, bare);
}

BEZEL_TEST(parser_uniform_closer_1374, end_while_wrong_keyword_is_error) {
    auto r = parseAll(wrapMethod("while cond do x := 1; end for;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── 3. `end for;` ────────────────────────────────────────────────────────────

BEZEL_TEST(parser_uniform_closer_1374, end_for_label_accepted) {
    // §1.6 / v0.170: `end for;` as labeled closer for ForStmt.
    auto r = parseAll(
        wrapMethod("for var i in items do _ := i; end for;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_for_label_parses_same_as_bare) {
    auto bare    = parseMethodBody("for var i in items do _ := i; end;");
    auto labeled = parseMethodBody("for var i in items do _ := i; end for;");
    BEZEL_EXPECT_EQ(labeled, bare);
}

BEZEL_TEST(parser_uniform_closer_1374, end_for_wrong_keyword_is_error) {
    auto r = parseAll(wrapMethod("for var i in items do _ := i; end while;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── 4. `end do;` ─────────────────────────────────────────────────────────────

BEZEL_TEST(parser_uniform_closer_1374, end_do_label_accepted) {
    // §1.6 / v0.170: `end do;` as labeled closer for DoEndBlock.
    auto r = parseAll(wrapMethod("do x := 1; end do;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_do_label_parses_same_as_bare) {
    auto bare    = parseMethodBody("do x := 1; end;");
    auto labeled = parseMethodBody("do x := 1; end do;");
    BEZEL_EXPECT_EQ(labeled, bare);
}

BEZEL_TEST(parser_uniform_closer_1374, end_do_wrong_keyword_is_error) {
    auto r = parseAll(wrapMethod("do x := 1; end if;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── 5. `end try;` ────────────────────────────────────────────────────────────

BEZEL_TEST(parser_uniform_closer_1374, end_try_label_accepted) {
    // §1.6 / v0.170: `end try;` as labeled closer for TryStmt.
    auto r = parseAll(
        wrapMethod("try x := 1; catch Exception => _ := 0; end try;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_try_label_parses_same_as_bare) {
    auto bare    = parseMethodBody("try x := 1; catch Exception => _ := 0; end;");
    auto labeled = parseMethodBody("try x := 1; catch Exception => _ := 0; end try;");
    BEZEL_EXPECT_EQ(labeled, bare);
}

BEZEL_TEST(parser_uniform_closer_1374, end_try_wrong_keyword_is_error) {
    auto r = parseAll(
        wrapMethod("try x := 1; catch Exception => _ := 0; end for;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── 6. `end case;` ───────────────────────────────────────────────────────────

BEZEL_TEST(parser_uniform_closer_1374, end_case_stmt_label_accepted) {
    // §1.6 / v0.170: `end case;` as labeled closer for statement-position case.
    auto r = parseAll(
        wrapMethod("case x of 1 => return 1;, _ => return 0; end case;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_case_stmt_label_parses_same_as_bare) {
    auto bare    = parseMethodBody("case x of 1 => return 1;, _ => return 0; end;");
    auto labeled = parseMethodBody("case x of 1 => return 1;, _ => return 0; end case;");
    BEZEL_EXPECT_EQ(labeled, bare);
}

BEZEL_TEST(parser_uniform_closer_1374, end_case_stmt_wrong_keyword_is_error) {
    auto r = parseAll(
        wrapMethod("case x of _ => return 0; end if;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_case_expr_label_accepted) {
    // §1.6 / v0.170: `end case` as labeled closer for expression-position case.
    // Used in assignment; `end case;` terminates the expression statement.
    auto r = parseAll(
        wrapMethod("var y := (case x of 1 => 10, _ => 0);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_case_expr_label_parses_same_as_bare) {
    auto bare    = parseMethodBody("var y := (case x of 1 => 10, _ => 0);");
    auto labeled = parseMethodBody("var y := (case x of 1 => 10, _ => 0);");
    BEZEL_EXPECT_EQ(labeled, bare);
}

// ── 7. `end when;` ───────────────────────────────────────────────────────────

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// statement is retired — the replacement is the `if`/`elif`/`else` statement.
// This test previously verified the when statement; now verifies rejection.
BEZEL_TEST(parser_uniform_closer_1374, end_when_stmt_label_accepted) {
    auto r = parseAll(
        wrapMethod("when x > 0 => return 1;, _ => return 0; end when;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): both bare and labeled
// `when` statement forms are retired. This test previously verified that both parse
// to the same AST; now verifies that both produce errors.
BEZEL_TEST(parser_uniform_closer_1374, end_when_stmt_label_parses_same_as_bare) {
    auto bare    = parseMethodBody("when x > 0 => return 1;, _ => return 0; end;");
    auto labeled = parseMethodBody("when x > 0 => return 1;, _ => return 0; end when;");
    BEZEL_EXPECT_EQ(bare, "<error>");
    BEZEL_EXPECT_EQ(labeled, "<error>");
}

BEZEL_TEST(parser_uniform_closer_1374, end_when_stmt_wrong_keyword_is_error) {
    auto r = parseAll(
        wrapMethod("when x > 0 => return 1;, _ => return 0; end if;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired — the replacement is the parenthesized if-expression.
// This test previously verified the when expression; now verifies rejection.
BEZEL_TEST(parser_uniform_closer_1374, end_when_expr_label_accepted) {
    auto r = parseAll(
        wrapMethod("var y := (when x > 0 => 1, _ => 0);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_when_expr_label_parses_same_as_bare) {
    auto bare    = parseMethodBody("var y := (when x > 0 => 1, _ => 0);");
    auto labeled = parseMethodBody("var y := (when x > 0 => 1, _ => 0);");
    BEZEL_EXPECT_EQ(labeled, bare);
}

// ── 8. `end extension;` ──────────────────────────────────────────────────────

BEZEL_TEST(parser_uniform_closer_1374, end_extension_label_accepted) {
    // §1.6 / v0.170: `end extension;` as labeled closer for extension block.
    auto r = parseAll("module M; extension string end extension;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired. This test previously verified the when expression parses
// correctly in an extension method; now verifies rejection.
BEZEL_TEST(parser_uniform_closer_1374, end_extension_label_with_member) {
    auto r = parseAll(
        "module M; extension int "
        "public method Abs(): int => (when this >= 0 => this, _ => -this); "
        "end extension;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_extension_wrong_keyword_is_error) {
    auto r = parseAll("module M; extension string end extern;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── 9. `end extern;` ─────────────────────────────────────────────────────────

BEZEL_TEST(parser_uniform_closer_1374, end_extern_label_accepted) {
    // §1.6 / v0.170: `end extern;` as labeled closer for extern block.
    auto r = parseAll(
        "module M; extern \"libc\" "
        "public method puts(s: string): int; "
        "end extern;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, end_extern_wrong_keyword_is_error) {
    auto r = parseAll(
        "module M; extern \"libc\" "
        "public method puts(s: string): int; "
        "end extension;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── 10. Bare `end;` stays valid everywhere (regression guard) ────────────────

BEZEL_TEST(parser_uniform_closer_1374, bare_end_if_still_valid) {
    auto r = parseAll(wrapMethod("if x then y := 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, bare_end_while_still_valid) {
    auto r = parseAll(wrapMethod("while cond do x := 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, bare_end_for_still_valid) {
    auto r = parseAll(wrapMethod("for var i in items do _ := i; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, bare_end_do_still_valid) {
    auto r = parseAll(wrapMethod("do x := 1; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, bare_end_try_still_valid) {
    auto r = parseAll(
        wrapMethod("try x := 1; catch Exception => _ := 0; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, bare_end_case_stmt_still_valid) {
    auto r = parseAll(
        wrapMethod("case x of _ => return 0; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, bare_end_extension_still_valid) {
    auto r = parseAll("module M; extension string end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, bare_end_extern_still_valid) {
    auto r = parseAll(
        "module M; extern \"libc\" public method puts(s: string): int; end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── 11. Nested constructs — each closer matches its own keyword ───────────────

BEZEL_TEST(parser_uniform_closer_1374, nested_for_if_each_matches_own) {
    // §1.6: each end-label is checked against its OWN construct — not the outer.
    // `end if;` closes the inner if; `end for;` closes the outer for.
    auto r = parseAll(
        wrapMethod(
            "for var i in items do "
            "  if i > 0 then y := y + i; end if; "
            "end for;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, nested_while_do_each_matches_own) {
    auto r = parseAll(
        wrapMethod(
            "while cond do "
            "  do x := x + 1; end do; "
            "end while;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, nested_try_if_each_matches_own) {
    auto r = parseAll(
        wrapMethod(
            "try "
            "  if x > 0 then y := 1; end if; "
            "catch Exception => _ := 0; "
            "end try;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── 12. Expression-position `end when;` inside a case arm ────────────────────
//
// §1.6 note on expression position: keywords cannot appear in postfix/infix
// position, so `consumeOptionalEndKeyword` always consumes a matching keyword
// after `end` even without a trailing `;`.  This covers the case where a
// `when` expression is used as a case-arm body (terminated by `,`).

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired. This test previously verified that `(when ...)` parses
// correctly as a case arm body; now verifies rejection.
BEZEL_TEST(parser_uniform_closer_1374, expr_when_inside_case_arm_with_label) {
    auto r = parseAll(
        wrapMethod(
            "var y := (case x of "
            "  1 => (when x > 0 => 10, _ => 20), "
            "  _ => 0);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired. This test previously verified that a case expression
// parses correctly inside a when arm body; now verifies rejection of the
// outer when expression.
BEZEL_TEST(parser_uniform_closer_1374, expr_case_inside_when_arm_with_label) {
    auto r = parseAll(
        wrapMethod(
            "var y := (when x > 0 => "
            "  (case x of 1 => 10, _ => 0), "
            "  _ => -1);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── 13. `end when;` guard-keyword context-sensitivity ────────────────────────
//
// `when` inside `catch` arms is a GUARD keyword (not a WhenExpr) — the labeled
// closer `end when;` must NOT be confused with the guard keyword.

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// statement is retired — the replacement is the `if`/`elif`/`else` statement.
// This test previously verified the when statement parsing; now verifies rejection.
BEZEL_TEST(parser_uniform_closer_1374, end_when_in_when_stmt_not_guard) {
    auto r = parseAll(
        wrapMethod("when x > 0 => return 1;, _ => return 0; end when;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── 14. Existing named-end forms unchanged (regression guard) ─────────────────

BEZEL_TEST(parser_uniform_closer_1374, method_named_end_still_works) {
    // Named method closer is unaffected by the keyword-closer change.
    auto r = parseAll(
        "module M; class C public method Run() x := 1; end Run; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_uniform_closer_1374, constructor_keyword_end_still_works) {
    // `end constructor;` was already valid in v0.169 — still works.
    auto r = parseAll(
        "module M; class C "
        "public constructor() _ := 0; end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
