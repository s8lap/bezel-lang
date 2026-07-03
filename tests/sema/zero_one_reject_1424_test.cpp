// Spec-derived sema tests for #1424 — `Zero` and `One` as direct primitive
// statics are declared ONLY on decimal (stdlib §9.3); the prior accept on
// every numeric primitive keyword was an invented API (CLAUDE.md rule 2).
//
// Per stdlib §9.1/§9.2/§9.3:
//   int/long/short/byte/… → MaxValue, MinValue only (§9.1)
//   double/float          → MaxValue, MinValue, NaN, PositiveInfinity,
//                           NegativeInfinity, Epsilon (§9.2)
//   decimal               → MaxValue, MinValue, Zero, One (§9.3)
//
// The §11.8 constraint path (T.Zero() inside a `where T: INumber<T>` body,
// #807) is SEPARATE — it is not affected by this narrowing. Tests in
// tests/codegen/static_interface_dispatch_807_test.cpp cover that path.
//
// Fix site: ExprTypecheck_Calls.cpp, ~line 7413 — the primitive-keyword
// static-member branch now accepts Zero/One only when pt == Decimal.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult checkBody(std::string_view body) {
    std::string src =
        std::string("module M; class C public static method Main(): int ") +
        std::string(body) + " end Main; end C;";
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

}  // namespace

// ===========================================================================
// NEGATIVE — direct `int.Zero()`, `double.One()`, etc. must be rejected.
// Before fix #1424 these were accepted (typed as the primitive) even though
// Zero/One are not in the stdlib's §9.1/§9.2 static member sets.
// ===========================================================================

// §9.1 / #1424: int has no Zero static (stdlib §9.1 lists only MaxValue, MinValue).
BEZEL_TEST(sema_zero_one_reject_1424, int_zero_is_rejected_spec_9_1) {
    auto r = checkBody("var x := int.Zero(); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.1 / #1424: int has no One static.
BEZEL_TEST(sema_zero_one_reject_1424, int_one_is_rejected_spec_9_1) {
    auto r = checkBody("var x := int.One(); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.1 / #1424: long.Zero — same closed set as int.
BEZEL_TEST(sema_zero_one_reject_1424, long_zero_is_rejected_spec_9_1) {
    auto r = checkBody("var x := long.Zero(); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2 / #1424: double.Zero is not in the stdlib §9.2 static set.
BEZEL_TEST(sema_zero_one_reject_1424, double_zero_is_rejected_spec_9_2) {
    auto r = checkBody("var x := double.Zero(); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.2 / #1424: double.One is not in the stdlib §9.2 static set.
BEZEL_TEST(sema_zero_one_reject_1424, double_one_is_rejected_spec_9_2) {
    auto r = checkBody("var x := double.One(); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// POSITIVE — decimal.Zero and decimal.One must still be accepted (§9.3),
// and MaxValue/MinValue on integer types must not be regressed.
// ===========================================================================

// §9.3: decimal.Zero is the ONLY numeric keyword that has Zero — must accept.
BEZEL_TEST(sema_zero_one_reject_1424, decimal_zero_still_accepted_spec_9_3) {
    auto r = checkBody("var z := Decimal.Zero; return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.3: decimal.One — must accept.
BEZEL_TEST(sema_zero_one_reject_1424, decimal_one_still_accepted_spec_9_3) {
    auto r = checkBody("var o := Decimal.One; return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.1: int.MaxValue must still be accepted (not collateral damage).
BEZEL_TEST(sema_zero_one_reject_1424, int_maxvalue_still_accepted_spec_9_1) {
    auto r = checkBody("var m := int.MaxValue; return m;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.1: int.MinValue must still be accepted.
BEZEL_TEST(sema_zero_one_reject_1424, int_minvalue_still_accepted_spec_9_1) {
    auto r = checkBody("var m := int.MinValue; return m;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.2: double.MaxValue must still be accepted.
BEZEL_TEST(sema_zero_one_reject_1424, double_maxvalue_still_accepted_spec_9_2) {
    auto r = checkBody("var m := double.MaxValue; return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
