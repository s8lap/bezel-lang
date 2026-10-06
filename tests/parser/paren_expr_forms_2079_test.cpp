// Spec-derived parser tests for the v0.208 paren-delimited expression forms
// (proposal 0057 graduation; impl issue #2079) — §20 / §19.1 / §19.7 / §9.5,
// grammar A.7 IfExpr/CaseExpr/WhenExpr + A.6 CaseStmt/WhenStmt:
//
//   IfExpr   ::= "(" "if" Expression "then" Expression
//                ("elif" Expression "then" Expression)* "else" Expression ")"
//   CaseExpr ::= "(" "case" "owned"? Expression "of" CaseArm ("," CaseArm)* ","? ")"
//   WhenExpr ::= "(" "when" WhenArm ("," WhenArm)* ","? ")"
//   PrimarySuffix … | "with" "(" InitAssign ("," InitAssign)* ","? ")"
//
// The postfix conditional `x if c else y` is retired (its targeted
// diagnostics are lit-tested in tests/lit/paren_forms_retired_*_2079.bzl);
// `end` is statement-only — the A.6 CaseStmt/WhenStmt statement forms are
// unchanged in surface. The atom commits at its second token ("(if"/"(case"/
// "(when") and no conditional level exists in the §17.1 table.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::exprToString;
using bezel::test::parseAll;

namespace {

std::string exprFromStmt(const std::string& exprSrc) {
    const std::string src =
        "module M; class C public method M() _ := " + exprSrc +
        "; end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto& topDecls = r.sourceFile->topDecls;
    if (topDecls.empty()) return "<no-topdecls>";
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topDecls[0].get());
    if (!td || td->members.empty()) return "<no-member>";
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return "<no-stmt>";
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        md->stmts[0].get());
    if (!ds || !ds->value) return "<not-discard>";
    return exprToString(*ds->value);
}

bool parsesClean(const std::string& src) {
    auto r = parseAll(src);
    return !r.diagnostics.hasErrors();
}

} // namespace

// §20 v0.208: the two-branch conditional atom.
BEZEL_TEST(parser_paren_forms_2079, spec_20_if_atom_basic) {
    BEZEL_EXPECT_EQ(exprFromStmt("(if a > b then a else b)"),
                    std::string("(if (a > b) then a else b)"));
}

// §20 v0.208: the elif chain right-nests into the ConditionalExpr chain
// (rendered structure-true by the printer).
BEZEL_TEST(parser_paren_forms_2079, spec_20_if_atom_elif_chain) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(if c1 then 1 elif c2 then 2 else 3)"),
        std::string("(if c1 then 1 else (if c2 then 2 else 3))"));
}

// §20 v0.208: a `throw` branch is admitted in ANY branch position — the
// v0.182 true-branch parse trap is deleted, not repaired.
BEZEL_TEST(parser_paren_forms_2079, spec_20_if_atom_throw_true_branch) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method M(n: int): int "
        "return (if n < 0 then throw new ArgumentException(\"neg\") else n); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method M(n: int): int "
        "return (if n > 0 then n else throw new ArgumentException(\"neg\")); "
        "end M; end C;"));
}

// §20 v0.208: in call-argument position the atom supplies its own pair —
// `f((if c then a else b))`.
BEZEL_TEST(parser_paren_forms_2079, spec_20_if_atom_in_argument) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method F(x: int): int return x; end F; "
        "public method M(c: bool): int "
        "return F((if c then 1 else 2)); "
        "end M; end C;"));
}

// §19.1/§19.7 v0.208: the expression-position case atom (the spec's own
// grade example) — `)` closes it, no `end`.
BEZEL_TEST(parser_paren_forms_2079, spec_19_1_case_atom) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(case score of 90..100 => \"A\", _ => \"F\")"),
        std::string("case score of { (90 .. 100) => \"A\" | _ => \"F\" } end"));
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired — the replacement is the parenthesized if-expression.
// This test previously verified the when expression's AST; now verifies rejection.
BEZEL_TEST(parser_paren_forms_2079, spec_19_1_when_atom) {
    auto r = parseAll(
        "module M; class C public method M(score: int): string "
        "  return (when score >= 90 => \"A\", _ => \"F\"); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §19.7 v0.208: trailing comma before the atom's closer (§1.6). The `when` form
// is retired (v0.289, proposal 0122 L1) — test the case form and expect an error
// for the when form.
BEZEL_TEST(parser_paren_forms_2079, spec_19_7_atom_trailing_comma) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method M(x: int): int "
        "return (case x of 1 => 10, _ => 20,); "
        "end M; end C;"));
    // v0.289: the `(when x > 0 => 1, _ => 0,)` expression is retired.
    auto r = parseAll(
        "module M; class C public method M(x: int): int "
        "return (when x > 0 => 1, _ => 0,); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §19.7 v0.208: the atom composes as another arm's body and in argument
// position (`f((case x of 1 => a, 2 => b), y)`). The `when` form is retired
// (v0.289, proposal 0122 L1) — test the case form and expect an error for the
// when-in-case-arm form.
BEZEL_TEST(parser_paren_forms_2079, spec_19_7_atom_composes) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method F(a: int, b: int): int return a; "
        "end F; public method M(x: int): int "
        "return F((case x of 1 => 10, _ => 20), x); "
        "end M; end C;"));
    // v0.289: the `(when x > 0 => 1, _ => 0)` arm body is retired.
    auto r = parseAll(
        "module M; class C public method M(x: int): int "
        "return (case x of 1 => (when x > 0 => 1, _ => 0), _ => 3); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §19 v0.208: statement-position case is UNCHANGED — `end case?;` with
// statement arm bodies (A.6 CaseStmt). The `when` statement form is retired
// (v0.289, proposal 0122 L1) — test the case form and expect an error for
// the when form.
BEZEL_TEST(parser_paren_forms_2079, spec_19_7_statement_forms_unchanged) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method M(x: int) "
        "case x of 1 => Console.WriteLine(\"one\"), _ => do end end case; "
        "end M; end C;"));
    // v0.289: the `when x > 0 => ... end when;` statement is retired.
    auto r = parseAll(
        "module M; class C public method M(x: int) "
        "when x > 0 => Console.WriteLine(\"pos\"), _ => do end end when; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.5 v0.208: the record update suffix with keyword-attached parentheses,
// including the trailing comma and suffix chaining past its `)`.
BEZEL_TEST(parser_paren_forms_2079, spec_9_5_with_suffix) {
    BEZEL_EXPECT_EQ(exprFromStmt("p with(X := 10)"),
                    std::string("p with(X := 10)"));
    BEZEL_EXPECT_EQ(exprFromStmt("p with(X := 1, Y := 2,)"),
                    std::string("p with(X := 1, Y := 2)"));
    BEZEL_EXPECT_EQ(exprFromStmt("p with(X := 1) with(Y := 2)"),
                    std::string("p with(X := 1) with(Y := 2)"));
}

// §20 v0.208: the atom is a PrimaryAtom — it composes as an operand without
// further parenthesization (`(if c then 1 else 2) + 3`).
BEZEL_TEST(parser_paren_forms_2079, spec_20_if_atom_as_operand) {
    BEZEL_EXPECT_EQ(exprFromStmt("(if c then 1 else 2) + 3"),
                    std::string("((if c then 1 else 2) + 3)"));
}
