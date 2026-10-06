// Spec-derived parser tests for the v0.209 arm-position transfer marker
// (proposal 0056 graduation as narrowed; impl issue #2080) — grammar A.7:
//
//   MarkedTransfer ::= "owned" Identifier
//
// "The K3.4 transfer marker as an expression atom (v0.209). `owned` is
// reserved and begins no other expression, so the form is additive.
// Admission is positional, enforced semantically (§28.3)" — the PARSER
// accepts the atom in any expression position (the rejects are sema's, lit-
// tested in tests/lit/marked_transfer_positions_2080.bzl); the RHS-of-
// binding/assignment forms remain site-parsed flags and never form the atom.

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

// §20 v0.209: the marker as an if-expression BRANCH — the spec's own
// one-expression equalization `(if useFresh then new Stream() else owned
// cached)` shape (§28.3 Pattern 2).
BEZEL_TEST(parser_marked_transfer_2080, spec_28_3_if_branch_marker) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(if useFresh then fresh else owned cached)"),
        std::string("(if useFresh then fresh else (owned cached))"));
}

// §19.2 v0.209: the marker as an expression-`case` ARM RESULT. The `when` arm
// is retired (v0.289, proposal 0122 L1) and is tested separately.
BEZEL_TEST(parser_marked_transfer_2080, spec_19_2_case_when_arm_marker) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(case n of 1 => owned a, _ => owned b)"),
        std::string(
            "case n of { 1 => (owned a) | _ => (owned b) } end"));
    // v0.289: the `(when c => owned a, _ => owned b)` expression is retired.
    auto r = parseAll(
        "module M; class C public method M(c: bool) "
        "_ := (when c => owned a, _ => owned b); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A.7 v0.209: `owned` requires an identifier — a field path is `move x.f`
// (§28.8.1), not a marker operand.
BEZEL_TEST(parser_marked_transfer_2080, spec_a7_identifier_required) {
    BEZEL_EXPECT_FALSE(parsesClean(
        "module M; class C public method M(): int "
        "return (if C.T() then owned 3 else 0); "
        "end M; end C;"));
}

// A.7 v0.209 / §28.3 v0.180: the binding/assignment RHS forms stay the
// site-parsed FLAG (LocalDecl::initializerHasOwnedMarker), never the atom —
// `var y := owned x;` parses identically to the pre-v0.209 surface.
BEZEL_TEST(parser_marked_transfer_2080, spec_28_3_rhs_forms_unchanged) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method M(owned x: Res) "
        "var y := owned x; "
        "y.Dispose(); "
        "end M; end C; class Res end Res;"));
}

// §35.5 (v0.209/#2080 F4): the `(case owned x of …)` atom's SCRUTINEE
// marker parses (the scrutinee is a full Expression, so the marker forms at
// its root) — its §35.5 semantic routing is lit-tested.
BEZEL_TEST(parser_marked_transfer_2080, spec_35_5_scrutinee_marker_parses) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(case owned o of 1 => 1, _ => 0)"),
        std::string("case (owned o) of { 1 => 1 | _ => 0 } end"));
}
