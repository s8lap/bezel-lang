// Spec-derived parser tests for §22.16 contract clauses (v0.203, proposal
// 0043, #2053) — Slice 1: clause-region PARSE + AST.
//
// Grammar (Appendix A.4):
//   ContractClause  ::= "static"? ("requires" | "ensures") Expression ";"
//   InvariantDecl   ::= "static"? "invariant" Expression ";"
//   MethodDecl      ::= ... ContractClause* MethodBody
//   ConstructorDecl ::= ... ContractClause* CtorChain? Statement* "end" ...
//   IfaceMethod     ::= ... ( ";" | ContractClause+ )
//
// The marker-wins rule (Appendix B.2): in the clause region, the keyword
// reading of `requires`/`ensures` always wins; elsewhere they are ordinary
// identifiers. The body-less-member closure (§22.16): a clause-bearing
// member may be body-less, terminated at its last clause's `;`.
//
// Slice 1 covers these Appendix C.15 bullets:
//   * "Clauses never distinguish overloads; `invariant` members appear only
//     in classes; `old`/`result` appear only where the rules above admit
//     them." (the parse tier — invariant placement and clause-region entry)
//   * the production-grammar admission of `requires`/`ensures`/`invariant`
//     markers and the `static` qualifier
//
// Subsequent slices cover clause typing, evaluation lowering, and the
// enforcement modes. The tests here are PARSE-ONLY — they verify the
// clause-region parse + AST shape and that zero-clause members parse
// exactly as before (additive).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

// Drill into the first class's first member of a specific kind. Test
// fixtures commonly place a field before the member under test, so a fixed
// index would alias the wrong member.
template <typename MemberT>
const MemberT* firstMemberOf(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td) return nullptr;
    for (const auto& m : td->members) {
        if (auto* p = dynamic_cast<const MemberT*>(m.get())) return p;
    }
    return nullptr;
}

const bezel::ast::MethodDecl* firstMethod(const bezel::test::ParseResult& r) {
    return firstMemberOf<bezel::ast::MethodDecl>(r);
}

const bezel::ast::ConstructorDecl* firstCtor(
    const bezel::test::ParseResult& r) {
    return firstMemberOf<bezel::ast::ConstructorDecl>(r);
}

const bezel::ast::InvariantDecl* firstInvariant(
    const bezel::test::ParseResult& r) {
    return firstMemberOf<bezel::ast::InvariantDecl>(r);
}

// Count clauses of a kind on a method/ctor.
template <typename NodeT>
std::size_t countClauses(const NodeT* n, bezel::ast::ContractClause::Kind k) {
    if (!n) return 0;
    std::size_t c = 0;
    for (const auto& cl : n->contractClauses)
        if (cl.kind == k) ++c;
    return c;
}

}  // namespace

// =========================================================================
// §22.16 example: a method with `requires` + `ensures` + a block body
// parses cleanly. Additive — zero-clause methods continue to parse.
// =========================================================================
BEZEL_TEST(parser_contract_clauses_2053,
           method_with_requires_and_ensures_parses_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  private _balance: decimal := 0m; "
        "  public method Withdraw(amount: decimal) "
        "    requires amount > 0m; "
        "    requires amount <= _balance; "
        "    ensures _balance = old(_balance) - amount; "
        "    _balance := _balance - amount; "
        "  end Withdraw; "
        "end C;");
    const auto* m = firstMethod(r);
    BEZEL_EXPECT_TRUE(m != nullptr);
    BEZEL_EXPECT_EQ(m->contractClauses.size(), std::size_t{3});
    BEZEL_EXPECT_EQ(countClauses(m, bezel::ast::ContractClause::Kind::Requires),
                    std::size_t{2});
    BEZEL_EXPECT_EQ(countClauses(m, bezel::ast::ContractClause::Kind::Ensures),
                    std::size_t{1});
    BEZEL_EXPECT_EQ(m->bodyKind, bezel::ast::MethodBodyKind::Block);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 / A.4: clauses on a method with an EXPRESSION body parse cleanly.
BEZEL_TEST(parser_contract_clauses_2053,
           method_clauses_with_expression_body_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public method Identity(x: int): int "
        "    requires x > 0; "
        "    => x; "
        "end C;");
    const auto* m = firstMethod(r);
    BEZEL_EXPECT_TRUE(m != nullptr);
    BEZEL_EXPECT_EQ(m->contractClauses.size(), std::size_t{1});
    BEZEL_EXPECT_EQ(m->bodyKind, bezel::ast::MethodBodyKind::Expression);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 "a clause-bearing abstract method ends at its last clause's `;`":
// no extra `;` after the clauses. Body is body-less (Abstract).
BEZEL_TEST(parser_contract_clauses_2053,
           clause_bearing_bodyless_method_clauses_terminate_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public method M(x: int): int "
        "    requires x > 0; "
        "    ensures result > 0; "
        "  public method Next(): int => 1; "
        "end C;");
    const auto* m = firstMethod(r);
    BEZEL_EXPECT_TRUE(m != nullptr);
    BEZEL_EXPECT_EQ(m->contractClauses.size(), std::size_t{2});
    BEZEL_EXPECT_EQ(m->bodyKind, bezel::ast::MethodBodyKind::Abstract);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 lenient: a clause-bearing method followed by an explicit `;` is
// also accepted as body-less (the grammar's `;` MethodBody alternative).
BEZEL_TEST(parser_contract_clauses_2053,
           clause_bearing_bodyless_method_with_semicolon_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public method M(x: int): int "
        "    requires x > 0; ; "
        "end C;");
    const auto* m = firstMethod(r);
    BEZEL_EXPECT_TRUE(m != nullptr);
    BEZEL_EXPECT_EQ(m->contractClauses.size(), std::size_t{1});
    BEZEL_EXPECT_EQ(m->bodyKind, bezel::ast::MethodBodyKind::Abstract);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 / A.4: the `static` qualifier demands the proven tier. Parsing
// admits it textually on `requires` and `ensures` (sema enforces the
// proven-tier demand in a later slice).
BEZEL_TEST(parser_contract_clauses_2053,
           static_qualifier_on_requires_and_ensures_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public method M(x: int) "
        "    static requires x > 0; "
        "    static ensures true; "
        "    => 0; "
        "end C;");
    const auto* m = firstMethod(r);
    BEZEL_EXPECT_TRUE(m != nullptr);
    BEZEL_EXPECT_EQ(m->contractClauses.size(), std::size_t{2});
    BEZEL_EXPECT_TRUE(m->contractClauses[0].isStatic);
    BEZEL_EXPECT_TRUE(m->contractClauses[1].isStatic);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16: clauses on a CONSTRUCTOR parse, between `)` and the CtorChain/body.
BEZEL_TEST(parser_contract_clauses_2053,
           constructor_with_clauses_parses_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  private _limit: decimal := 0m; "
        "  public constructor(limit: decimal) "
        "    requires limit >= 0m; "
        "    _limit := limit; "
        "  end constructor; "
        "end C;");
    const auto* c = firstCtor(r);
    BEZEL_EXPECT_TRUE(c != nullptr);
    BEZEL_EXPECT_EQ(c->contractClauses.size(), std::size_t{1});
    BEZEL_EXPECT_EQ(
        countClauses(c, bezel::ast::ContractClause::Kind::Requires),
        std::size_t{1});
    BEZEL_EXPECT_FALSE(c->body.empty());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 "constructor `ensures` evaluates at body completion": an ensures
// clause on a constructor parses cleanly.
BEZEL_TEST(parser_contract_clauses_2053,
           constructor_with_ensures_clause_parses_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public constructor(v: int) "
        "    requires v >= 0; "
        "    ensures _v = v; "
        "    _v := v; "
        "  end constructor; "
        "end C;");
    const auto* c = firstCtor(r);
    BEZEL_EXPECT_TRUE(c != nullptr);
    BEZEL_EXPECT_EQ(c->contractClauses.size(), std::size_t{2});
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 "Class invariants": an `invariant` member parses as an
// InvariantDecl in a class body, with the predicate captured.
BEZEL_TEST(parser_contract_clauses_2053,
           class_invariant_member_parses_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  private _balance: decimal := 0m; "
        "  private _limit: decimal := 0m; "
        "  invariant _balance + _limit >= 0m; "
        "end C;");
    const auto* inv = firstInvariant(r);
    BEZEL_EXPECT_TRUE(inv != nullptr);
    BEZEL_EXPECT_FALSE(inv->isStatic);
    BEZEL_EXPECT_TRUE(inv->expr != nullptr);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16: `static invariant` parses (it carries meaning when the prover
// fragment lands; sema in this revision rejects it as out-of-fragment).
BEZEL_TEST(parser_contract_clauses_2053,
           static_invariant_member_parses_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  static invariant _v >= 0; "
        "end C;");
    const auto* inv = firstInvariant(r);
    BEZEL_EXPECT_TRUE(inv != nullptr);
    BEZEL_EXPECT_TRUE(inv->isStatic);
    BEZEL_EXPECT_TRUE(inv->expr != nullptr);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 / A.4 InvariantDecl: visibility is not admitted — a `public
// invariant ...` is a parse error.
BEZEL_TEST(parser_contract_clauses_2053,
           invariant_with_visibility_is_parse_error_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public invariant _v >= 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.16: a method whose FIRST parameter is named `requires` parses as a
// normal parameter name — the marker-wins rule applies only inside the
// clause region, after the signature.
BEZEL_TEST(parser_contract_clauses_2053,
           requires_as_parameter_name_not_a_marker_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public method M(requires: int): int => requires; "
        "end C;");
    const auto* m = firstMethod(r);
    BEZEL_EXPECT_TRUE(m != nullptr);
    BEZEL_EXPECT_EQ(m->contractClauses.size(), std::size_t{0});
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 / A.4 IfaceMethod: an interface method may carry ContractClause+
// as its body-less form (the slot's contract written where the slot is
// declared). The declaration ends at its last clause's `;`.
BEZEL_TEST(parser_contract_clauses_2053,
           iface_method_with_clauses_parses_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "interface I "
        "  method M(x: int): int "
        "    requires x > 0; "
        "    ensures result > 0; "
        "end I;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* m = firstMethod(r);
    BEZEL_EXPECT_TRUE(m != nullptr);
    BEZEL_EXPECT_EQ(m->contractClauses.size(), std::size_t{2});
    BEZEL_EXPECT_EQ(m->bodyKind, bezel::ast::MethodBodyKind::Abstract);
}

// Additive baseline: a zero-clause method continues to parse EXACTLY as
// before — the clause region is empty and does not disturb existing code.
BEZEL_TEST(parser_contract_clauses_2053,
           zero_clause_method_unchanged_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public method M(x: int): int => x; "
        "end C;");
    const auto* m = firstMethod(r);
    BEZEL_EXPECT_TRUE(m != nullptr);
    BEZEL_EXPECT_EQ(m->contractClauses.size(), std::size_t{0});
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Additive baseline: a zero-clause constructor continues to parse.
BEZEL_TEST(parser_contract_clauses_2053,
           zero_clause_constructor_unchanged_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "end C;");
    const auto* c = firstCtor(r);
    BEZEL_EXPECT_TRUE(c != nullptr);
    BEZEL_EXPECT_EQ(c->contractClauses.size(), std::size_t{0});
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 evaluation order: requires clauses appear BEFORE ensures clauses
// textually per the spec's evaluation order. Multiple interleaved clauses
// preserve their textual order in the AST.
BEZEL_TEST(parser_contract_clauses_2053,
           clauses_preserve_declaration_order_spec_22_16) {
    auto r = parseAll(
        "module M; "
        "class C "
        "  public method M(x: int): int "
        "    requires x > 0; "
        "    ensures result > 0; "
        "    requires x < 100; "
        "    ensures result < 100; "
        "    => x; "
        "end C;");
    const auto* m = firstMethod(r);
    BEZEL_EXPECT_TRUE(m != nullptr);
    BEZEL_EXPECT_EQ(m->contractClauses.size(), std::size_t{4});
    BEZEL_EXPECT_EQ(m->contractClauses[0].kind,
                    bezel::ast::ContractClause::Kind::Requires);
    BEZEL_EXPECT_EQ(m->contractClauses[1].kind,
                    bezel::ast::ContractClause::Kind::Ensures);
    BEZEL_EXPECT_EQ(m->contractClauses[2].kind,
                    bezel::ast::ContractClause::Kind::Requires);
    BEZEL_EXPECT_EQ(m->contractClauses[3].kind,
                    bezel::ast::ContractClause::Kind::Ensures);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
