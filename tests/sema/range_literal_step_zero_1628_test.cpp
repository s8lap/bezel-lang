// fix #1628 (§16.4, v0.188): a LITERAL step of zero must be rejected at
// compile time. §16.4: "A literal step of zero is a compile error; a
// computed step of zero is admitted at construction and raises
// `ArgumentException` at `GetEnumerator()`." The computed/runtime case was
// fixed by #1623 (a runtime guard in Emitter::lowerRangeFor,
// LlvmBackend_Stmts.cpp). The literal case regressed: codegen's
// `if (cs && cs->isZero()) return false;` silently *deferred* rather than
// diagnosing, so `for var i in 0..10 step 0 do ... end;` compiled cleanly.
//
// THE FIX (ExprTC::visitExpr's RangeExpr case, src/sema/ExprTypecheck_Exprs.cpp)
// const-folds the step expression with foldIntConstExprTC — the same
// compile-time-constant discipline §16.4 already uses a few lines earlier in
// the spec for the direction/sign-mismatch warning ("both endpoints and step
// are constants or constant after folding") — and reports a compile error
// when the fold succeeds and yields zero. A non-constant (runtime-computed)
// step folds to nullopt and is left to the existing #1623 runtime guard.
//
// The check fires wherever a RangeExpr is constructed ("admitted at
// construction" — not just inside a `for`), matching §16.4's wording.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some Error diagnostic carries the literal-step-zero message.
bool hasStepZeroError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("step of zero") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// §16.4: `for var i in 0..10 step 0` — the exact issue #1628 repro. A
// literal step of zero inside a `for` iterable must be a compile error.
BEZEL_TEST(sema_range_literal_step_zero_1628,
           for_loop_literal_step_zero_rejected_spec_16_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var sum: int := 0; "
        "    for var i in 0..10 step 0 do sum := sum + i; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasStepZeroError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §16.4: the rule applies "at construction", not only inside a `for` —
// a bare Range-valued local with a literal step-0 must also be rejected.
BEZEL_TEST(sema_range_literal_step_zero_1628,
           bare_range_local_literal_step_zero_rejected_spec_16_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(): int "
        "    var r := 0..10 step 0; "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasStepZeroError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §16.4 "constant after folding": a step expression that is not a bare `0`
// literal but const-folds to zero (`2 - 2`) is still a compile-time
// constant and must be rejected the same as a bare literal — mirroring the
// identical "constants or constant after folding" discipline the spec uses
// for the sibling direction/sign-mismatch warning in the same section.
BEZEL_TEST(sema_range_literal_step_zero_1628,
           const_folded_step_zero_rejected_spec_16_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method M(): int "
        "    for var i in 0..10 step (2 - 2) do end; "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasStepZeroError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Control: `step 1` is a valid, non-zero literal step — must NOT error.
BEZEL_TEST(sema_range_literal_step_zero_1628,
           literal_step_one_accepted_spec_16_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var sum: int := 0; "
        "    for var i in 0..10 step 1 do sum := sum + i; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasStepZeroError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Control: `step 2` is a valid, non-zero literal step — must NOT error.
BEZEL_TEST(sema_range_literal_step_zero_1628,
           literal_step_two_accepted_spec_16_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var sum: int := 0; "
        "    for var i in 0..10 step 2 do sum := sum + i; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasStepZeroError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Control: `step -1` (a descending literal step) is valid and non-zero —
// must NOT error.
BEZEL_TEST(sema_range_literal_step_zero_1628,
           literal_step_negative_one_accepted_spec_16_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var sum: int := 0; "
        "    for var i in 10..0 step -1 do sum := sum + i; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasStepZeroError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §16.4 control: a COMPUTED (non-constant) step of zero is a separate,
// runtime concern (#1623 — ArgumentException at GetEnumerator()), NOT a
// compile error. foldIntConstExprTC cannot fold a plain local-variable
// read, so the literal-step-zero check here must stay silent and let the
// program compile; the codegen-side runtime guard is exercised by
// tests/codegen/for_range_runtime_step_test.cpp.
BEZEL_TEST(sema_range_literal_step_zero_1628,
           runtime_computed_step_zero_not_flagged_at_compile_time_spec_16_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var z: int := 0; var sum: int := 0; "
        "    for var i in 0..10 step z do sum := sum + i; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasStepZeroError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
