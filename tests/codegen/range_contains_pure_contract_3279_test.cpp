// Spec-derived sema+codegen tests for fix #3279 (v0.263, stdlib §9.8 +
// spec §22.15): the `pure` marker on `Range<T>.Contains` is honored by the
// §22.15 calls-to-pure-members rule, so `x in r` over a Range<T> receiver is
// ADMITTED inside a contract clause (§22.16) and any other purity-gated
// callee position. The marker is what admits `requires(port in 0..65535)`
// (a §22.16 clause expression whose `in` call §22.15's rule permits).
//
// Negative cases — non-pure receivers of `in` are rejected in pure bodies and
// in contract clauses. Array<T>.Contains is not pure-declared (stdlib §4.4),
// so `x in arr` (Array<int>) is rejected by the same rule that rejects
// `arr.Contains(x)`. The pre-fix state let both through, since the `in`
// operator desugars to a range-member scan at codegen and the purity walker
// only walked operands without inspecting the callee.
//
// Citations: spec §22.15 (calls-to-pure-members rule, v0.263); stdlib §9.8
// (`Range<T>.Contains` is `pure`, intrinsic-purity contract); spec §22.16
// (contract-clause expression is checked under §22.15's pure-body rules);
// spec §17.5 (`in` operator semantics, calls `Contains` semantically).
//
// Done means:
//   - positive: a `requires(port in 0..65535)` clause compiles and the
//     runtime check fires (a value outside the range raises
//     ContractViolationException);
//   - negative: `requires(x in arr)` over an Array<int> is rejected at sema
//     with a §22.15 / "in over a non-pure receiver" diagnostic naming the
//     receiver;
//   - non-Range non-string non-span receivers of `in` in a PURE body are
//     rejected (the walker rejects the expression before codegen);
//   - sibling types whose Contains is pure-declared (string, span) continue
//     to be admitted, so the change is narrow: only `in` over a non-pure
//     receiver is newly rejected.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

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
// §9.8 / §22.15 / §22.16 / #3279: positive — `in` over Range<T> inside a
// contract clause (the v0.263 motivating example).
// =========================================================================

// Headline accept: `requires(port in 0..65535)` compiles and runs. The
// pure-body walker admits `in` over Range<T> because `Range<T>.Contains` is
// pure-declared (stdlib §9.8, intrinsic-purity contract per §22.15).
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_range_int_literal_compiles_and_runs_spec_22_15_issue_3279) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public method Foo(port: int): int "
        "    requires(port in 0..65535) "
        "    return port; "
        "  end Foo; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    var r: int := 0; "
        "    r := c.Foo(80); "
        "    return r; "
        "  end Main; "
        "end C;"), 80);
}

// A `requires(port in 0..65535)` clause raises ContractViolationException
// when the bound is violated (runtime check, §22.16 evaluation semantics).
// The purity classification enables the compilation; the runtime check
// enforces it.
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_range_int_literal_violation_raises_spec_22_16_issue_3279) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public method Foo(port: int): int "
        "    requires(port in 0..65535) "
        "    return port; "
        "  end Foo; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    var r: int := 0; "
        "    try r := c.Foo(70000); "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 99);
}

// `in` over a Range<int> VARIABLE (not a literal) — same purity rule. The
// receiver is `Range<int>` and Contains is pure-classified.
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_range_variable_compiles_spec_22_15_issue_3279) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public method Foo(port: int, r: Range<int>): int "
        "    requires(port in r) "
        "    return port; "
        "  end Foo; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    var r: int := 0; "
        "    r := c.Foo(80, 1..1024); "
        "    return r; "
        "  end Main; "
        "end C;"), 80);
}

// Same shape in a pure body: `in` over Range<T> is the §22.15
// calls-to-pure-members rule's canonical admitted callee, so it compiles
// without diagnostic. The body walks left and right operands (no violation
// on the operands themselves), then checks the implicit Contains — pure,
// so no rejection.
BEZEL_TEST(range_contains_pure_contract_3279,
           pure_body_in_range_compiles_spec_22_15_issue_3279) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public pure method Foo(port: int): bool "
        "    var r := 0..100; "
        "    return port in r; "
        "  end Foo; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// §22.15 / #3279: negative — `in` over Array<int> is REJECTED in a pure
// body and in a contract clause. Array<T>.Contains is not pure-declared
// (stdlib §4.4), so the `in` operator over an Array<T> receiver is an
// impure callee per §22.15's rule. The pre-fix state admitted it; this
// test pins the corrected behavior.
// =========================================================================

// Headline reject: a `requires(x in arr)` clause over an Array<int>
// receiver is rejected by the §22.15 calls-to-pure-members rule. The
// diagnostic names the receiver (`Array`) and cites §22.15.
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_array_rejected_spec_22_15_issue_3279) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Foo(x: int, a: Array<int>): int "
        "    requires(x in a) "
        "    return x; "
        "  end Foo; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.15"));
    // The diagnostic names the impure receiver so the user sees what to fix.
    BEZEL_EXPECT_TRUE(hasMsg(r, "Array"));
}

// Negative — same rule in a pure method body. A pure method whose body
// contains `x in arr` is rejected: the `in` operator's implicit Contains
// callee is not pure-classified on Array<T>.
BEZEL_TEST(range_contains_pure_contract_3279,
           pure_body_in_array_rejected_spec_22_15_issue_3279) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure method Foo(x: int, a: Array<int>): bool "
        "    return x in a; "
        "  end Foo; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.15"));
}

// Negative — `in` over an OwningArray<T> receiver is also rejected
// (OwningArray<T>.Contains is not pure-declared either; same rule).
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_owning_array_rejected_spec_22_15_issue_3279) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Foo(x: int, a: OwningArray<int>): int "
        "    requires(x in a) "
        "    return x; "
        "  end Foo; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.15"));
}

// =========================================================================
// §22.15 / #3279: a USER TYPE whose Contains is pure-declared is admitted
// by the same rule — this verifies the predicate does NOT special-case
// Range<T> by name (a regression guard for the broader predicate).
// =========================================================================

// User-defined pure-Contains type: a record (no member lookup complication)
// declaring `pure method Contains(value: T): bool`. `x in r` over this
// receiver compiles because the receiver's Contains is pure-declared.
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_user_pure_contains_compiles_spec_22_15_issue_3279) {
    auto r = parseSemaAll(
        "module M; "
        "record Bag<T> "
        "  public pure method Contains(value: T): bool "
        "    return true; "
        "  end Contains; "
        "end Bag; "
        "class C "
        "  public method Foo(x: int, b: Bag<int>): int "
        "    requires(x in b) "
        "    return x; "
        "  end Foo; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =========================================================================
// §22.15 / #3279 (post-review narrow): the by-name special-case set is
// exactly {Range<T>, string, ReadOnlyStringSpan} — the stdlib types whose
// Contains is `public pure method Contains(...)`. The Span<T> / ReadOnlySpan<T>
// family (§4.14) and ArraySpan<T> / ReadOnlyArraySpan<T> (§4.15) declare
// Contains as a plain `method`, NOT `pure`, so `x in spanVal` over those
// receivers is REJECTED — same shape as the Array<T> reject above.
//
// Pre-fix: the by-name list over-admitted Span / ReadOnlySpan / ArraySpan /
// ReadOnlyArraySpan / BitReader / BitWriter / ReadOnlyBitReader /
// ReadOnlyBitWriter. Post-fix: only the pure-declared-Contains set is
// admitted. These tests pin the corrected predicate scope.
// =========================================================================

// Negative — `requires(x in s)` over a ReadOnlySpan<int> receiver is
// REJECTED. ReadOnlySpan<T>.Contains is a plain method (stdlib §4.14),
// so `in` over a ReadOnlySpan receiver fails the
// §22.15 calls-to-pure-members rule.
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_readonly_span_rejected_spec_22_15_issue_3279) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Foo(x: int, s: ReadOnlySpan<int>): int "
        "    requires(x in s) "
        "    return x; "
        "  end Foo; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.15"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "ReadOnlySpan"));
}

// Negative — `requires(x in s)` over a Span<int> receiver is REJECTED.
// Span<T>.Contains is a plain method (stdlib §4.14),
// so `in` over a Span receiver fails the §22.15 rule.
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_span_rejected_spec_22_15_issue_3279) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Foo(x: int, s: Span<int>): int "
        "    requires(x in s) "
        "    return x; "
        "  end Foo; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.15"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "Span"));
}

// Negative — `requires(x in s)` over an ArraySpan<int> receiver is
// REJECTED. ArraySpan<T>.Contains is a plain method (stdlib §4.15),
// same rule.
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_array_span_rejected_spec_22_15_issue_3279) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Foo(x: int, s: ArraySpan<int>): int "
        "    requires(x in s) "
        "    return x; "
        "  end Foo; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.15"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "ArraySpan"));
}

// Negative — `requires(x in s)` over a ReadOnlyArraySpan<int> receiver
// is REJECTED. ReadOnlyArraySpan<T>.Contains is a plain method (stdlib
// §4.15), same rule.
BEZEL_TEST(range_contains_pure_contract_3279,
           requires_in_readonly_array_span_rejected_spec_22_15_issue_3279) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method Foo(x: int, s: ReadOnlyArraySpan<int>): int "
        "    requires(x in s) "
        "    return x; "
        "  end Foo; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.15"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "ReadOnlyArraySpan"));
}