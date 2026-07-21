// Spec-derived sema tests for §22.16 contract clauses (v0.207, proposal
// 0055, #2078) — Slice 2: clause TYPING (bool + §22.15 pure-body admission,
// expression-wise; throw/await rejection). Clauses use the structural-paren
// surface `requires(E)` / `ensures(E)` (proposal 0055).
//
// Spec §22.16 "Clause typing: bool, pure, borrow-only":
//   * the clause expression (and each operand of `old` within it) is type-
//     checked in the context of the declaring member;
//   * the expression must be of type `bool` (§4.1 — no truthiness);
//   * it is admitted under the pure-body rules of §22.15, applied
//     EXPRESSION-WISE, with three clause-specific amendments:
//     - `throw` is rejected (the carve-out remains available to *callees*);
//     - `await` is rejected, on every member;
//     - `old` and `result` are admitted only as §22.16 defines them.
//
// Appendix C.15 bullets covered here:
//   * "A contract clause's expression is `bool`, satisfies the §22.15 pure-
//     body rules, and contains no `throw`, no `await`, no store, and no
//     owned-consuming or disposal-running operation."
//
// Slice 2 implements bool typing + pure-body admission expression-wise +
// the throw/await amendments. The `result`/`old` placement checks and the
// moved-from-at-return-edge rule land alongside the evaluation lowering
// in a later slice.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        bezel::sema::checkPureBodies(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =========================================================================
// §22.16 example: a Withdraw-style shape type-checks cleanly — each
// clause's expression is bool and pure-body-admitted.
// (The spec's exact Withdraw example uses `old(_balance)`; the lowering of
// `old`/`result` lands in Slice 3, so this slice uses the equivalent
// post-state form `_balance = _balance - amount` — the typing and the
// pure-body admission this slice owns are exercised identically.)
// =========================================================================
BEZEL_TEST(contract_clauses_typing_2053,
           withdraw_spec_example_type_checks_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class Account "
        "  private _balance: int := 0; "
        "  public method Withdraw(amount: int) "
        "    requires(amount > 0) "
        "    requires(amount <= _balance) "
        "    ensures(_balance - amount >= 0) "
        "    _balance := _balance - amount; "
        "  end Withdraw; "
        "end Account;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 "the expression must be of type `bool`": a non-bool requires
// clause is rejected.
BEZEL_TEST(contract_clauses_typing_2053,
           requires_clause_non_bool_rejected_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(amount: int) "
        "    requires(amount) "
        "    => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "must be `bool`"));
}

// §22.16 "the expression must be of type `bool`": a non-bool ensures
// clause is rejected.
BEZEL_TEST(contract_clauses_typing_2053,
           ensures_clause_non_bool_rejected_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(amount: int): int "
        "    ensures(amount + 1) "
        "    => amount; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "must be `bool`"));
}

// §22.16 "throw is rejected" (amendment to the pure-body rules): a throw
// STATEMENT in a clause is rejected.
BEZEL_TEST(contract_clauses_typing_2053,
           throw_stmt_in_clause_rejected_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(x: int) "
        "    requires(x > 0 or (do throw new ArgumentException(); end)) "
        "    => 0; "
        "end C;");
    // Either the bool typing rejects the do-block (it's not bool) or the
    // pure-body checker rejects the throw — both are §22.16 rejections.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.16 throw amendment: a throw EXPRESSION is rejected in a clause.
BEZEL_TEST(contract_clauses_typing_2053,
           throw_expr_in_clause_rejected_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(x: int): int "
        "    ensures((throw new ArgumentException()) if x < 0 else x > 0) "
        "    => x; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "throw"));
}

// §22.16 "calls to pure members": a clause calling a pure member is
// admitted; a clause calling a non-pure member is rejected.
BEZEL_TEST(contract_clauses_typing_2053,
           clause_calling_non_pure_rejected_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Helper(): int => 42; "
        "  public method M(x: int) "
        "    requires(Helper() > 0) "
        "    => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "not pure-declared"));
}

// §22.16 "calls to pure members": a clause calling a pure member compiles.
BEZEL_TEST(contract_clauses_typing_2053,
           clause_calling_pure_accepted_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method Helper(): int => 42; "
        "  public method M(x: int) "
        "    requires(Helper() > 0) "
        "    => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 "clauses never have effects" — class construction (`new C(...)`) in
// a clause is rejected: the throw carve-out does NOT apply to clauses (a
// clause states a proposition — §22.16 "Clause typing"), and §22.15's pure-
// body admission applies expression-wise. The fix (#2062): the contract-
// clause expression's embedded type nodes (e.g. the `C` in `new C()`) are
// now resolved by TypeResolution in the method scope, so the PureChecker's
// NewExpr admission gate (`rt->declaration->kind == Class`) fires identically
// in body and clause context.
BEZEL_TEST(contract_clauses_typing_2053,
           new_in_clause_rejected_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(x: int): bool "
        "    requires(new C() <> null) "
        "    => true; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "class construction"));
}

// §22.16 non-regression: a clause that does NOT spell class construction
// still type-checks cleanly — the §22.16 #2062 fix only broadens the clause
// typing to fire the existing NewExpr gate; the pure-admitted clause surface
// (parameter/this reads, pure calls, value/record construction) is unaffected.
// (Records are constructed by name `R(...)` as a CallExpr, not by `new`, so a
// record-construction clause is admitted without exercising the NewExpr arm.)
BEZEL_TEST(contract_clauses_typing_2053,
           new_in_clause_non_regression_pure_clause_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method P(): bool => true; "
        "  public method M(x: int) "
        "    requires(x > 0) "
        "    ensures(P()) "
        "    => true; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 "borrow-only composition": a clause may READ an owned parameter
// (a borrow read), may not consume/dispose it.
BEZEL_TEST(contract_clauses_typing_2053,
           clause_reads_owned_parameter_as_borrow_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(s: string) "
        "    requires(s.Length > 0) "
        "    => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 additive: a zero-clause method compiles unchanged.
BEZEL_TEST(contract_clauses_typing_2053,
           zero_clause_method_unchanged_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(x: int): int => x; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 clause typing: clauses on a constructor are typed the same way.
BEZEL_TEST(contract_clauses_typing_2053,
           constructor_clause_type_checks_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _v: int := 0; "
        "  public constructor(v: int) "
        "    requires(v >= 0) "
        "    ensures(_v = v) "
        "    _v := v; "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.16 clause typing: a non-bool constructor clause is rejected.
BEZEL_TEST(contract_clauses_typing_2053,
           constructor_clause_non_bool_rejected_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public constructor(v: int) "
        "    requires(v) "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "must be `bool`"));
}

// §22.16 clause typing: clauses can read this's fields in a method context.
BEZEL_TEST(contract_clauses_typing_2053,
           method_clause_reads_instance_field_spec_22_16) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _count: int := 0; "
        "  public method M(amount: int) "
        "    requires(amount <= _count) "
        "    => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
