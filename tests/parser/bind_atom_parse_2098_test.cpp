// Spec-derived parser tests for the v0.216 binding atom (proposal 0061 §1
// graduation; impl issue #2098) — §17.15, grammar A.7:
//
//   BindExpr   ::= "(" BindClause+ Expression ")"
//   BindClause ::= "var" Identifier (":" Type)? ":=" Expression ";"
//
// "(" followed by the reserved `var` — which begins no other expression —
// commits the binding-atom reading at the second token, exactly as the
// v0.208 `(if …)`/`(case …)`/`(when …)` atoms commit (§17.15). Each clause
// introduces one single-assignment binding and is `;`-terminated; at least
// one clause is required (guaranteed by the dispatch — "(var" IS a clause
// head); the body is one full Expression; the atom supplies its own `)` in
// call- and index-argument position. Clause initializers admit the marked
// transfer `owned x` (K3.4 / §28.3); the atom takes no precedence-table
// entry (§17.1) and composes in any operand position.

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

// §17.15 v0.216: the one-clause atom — the spec's own first form
// `(var name := initializer; body)`.
BEZEL_TEST(parser_bind_atom_2098, spec_17_15_single_clause) {
    BEZEL_EXPECT_EQ(exprFromStmt("(var w := bounds.Width(); w * w)"),
                    std::string("(var w := bounds.Width(); (w * w))"));
}

// §17.15 v0.216: clauses chain — `(var a := E1; var b := E2; body)`; each
// later clause and the body see the earlier bindings.
BEZEL_TEST(parser_bind_atom_2098, spec_17_15_clause_chain) {
    BEZEL_EXPECT_EQ(exprFromStmt("(var a := 1; var b := a + 1; a + b)"),
                    std::string("(var a := 1; var b := (a + 1); (a + b))"));
}

// §17.15 v0.216: the ascribed clause form `var x: T := E;` (typed exactly as
// the §21.1 declaration statement it is spelled as).
BEZEL_TEST(parser_bind_atom_2098, spec_17_15_ascribed_clause) {
    BEZEL_EXPECT_EQ(exprFromStmt("(var n: long := 1; n)"),
                    std::string("(var n: long := 1; n)"));
}

// A.7 BindClause: a clause initializer admits the marked transfer
// `var y := owned x;` — K3.4's binding transfer, §28.3 (the body position
// does not; that reject is sema's, not the parser's).
BEZEL_TEST(parser_bind_atom_2098, spec_a7_owned_marker_clause) {
    BEZEL_EXPECT_EQ(exprFromStmt("(var y := owned x; y.Size())"),
                    std::string("(var y := owned x; y.Size())"));
}

// §17.15 v0.216: the atom composes in any operand position — here as a
// binary-operator operand (the spec's own `var area := (var w := …; w * w);`
// example) and nested inside another atom's clause initializer.
BEZEL_TEST(parser_bind_atom_2098, spec_17_15_operand_position) {
    BEZEL_EXPECT_EQ(exprFromStmt("(var w := 2; w * w) + 1"),
                    std::string("((var w := 2; (w * w)) + 1)"));
    BEZEL_EXPECT_EQ(
        exprFromStmt("(var a := (var b := 1; b + b); a * 3)"),
        std::string("(var a := (var b := 1; (b + b)); (a * 3))"));
}

// §17.15 v0.216: in call-argument position the atom supplies its own pair —
// `F((var n := 1; n))`.
BEZEL_TEST(parser_bind_atom_2098, spec_17_15_in_argument) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method F(x: int): int return x; end F; "
        "public method M(): int "
        "return F((var n := 1; n + n)); "
        "end M; end C;"));
}

// §17.15 v0.216: the spec's own second example — a v0.208 paren atom as the
// atom's body (`(var n := items.Count; (if n = 1 then … else …))`).
BEZEL_TEST(parser_bind_atom_2098, spec_17_15_paren_atom_body) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(var n := c; (if n = 1 then \"one\" else \"many\"))"),
        std::string(
            "(var n := c; (if (n = 1) then \"one\" else \"many\"))"));
}

// §17.15 v0.216: in statement position the atom is an ordinary expression
// statement under §21.9's discard regime.
BEZEL_TEST(parser_bind_atom_2098, spec_17_15_statement_position) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method G(x: int): int return x; end G; "
        "public method M() "
        "_ := (var n := 1; G(n)); "
        "end M; end C;"));
}

// §17.15 v0.216: a missing clause initializer is a parse error — A.7
// BindClause has no initializer-less form (a binding is initialized exactly
// once, at its clause).
BEZEL_TEST(parser_bind_atom_2098, spec_a7_clause_requires_initializer) {
    BEZEL_EXPECT_FALSE(parsesClean(
        "module M; class C public method M(): int "
        "return (var n; 3); "
        "end M; end C;"));
}

// §17.15 v0.216: the body is mandatory — `(var x := E;)` derives no
// Expression before the closer.
BEZEL_TEST(parser_bind_atom_2098, spec_a7_body_required) {
    BEZEL_EXPECT_FALSE(parsesClean(
        "module M; class C public method M(): int "
        "return (var n := 1;); "
        "end M; end C;"));
}

// §17.15 v0.216: the unit value `()` and grouping parens are unaffected —
// the atom commits only on "(" + `var`.
BEZEL_TEST(parser_bind_atom_2098, spec_17_15_no_false_commit) {
    BEZEL_EXPECT_EQ(exprFromStmt("(a + b) * c"),
                    std::string("((a + b) * c)"));
}
