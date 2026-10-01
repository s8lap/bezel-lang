// Codegen/AOT pins for #5188's residual (§19.3 / Appendix C.1, v0.313): a
// concrete value-typed `case` scrutinee against a type pattern naming a
// different value type is a compile-time error (real location, specific
// diagnostic); where a type parameter stands on the scrutinee side and both
// types are value types at the instantiation, the arm is decided per
// instantiation and the program runs (identical types match, otherwise the
// arm is admitted and never selected).

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, aot_int_vs_enum_rejected) {
    auto r = runAot(
        "module M; enum Color Red, Green; end Color; "
        "class C public static method Main(): int "
        "var n: int := 5; return (case n of Color => 1, _ => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "naming a different value type"));
}

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, aot_int_vs_long_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "var n: int := 5; return (case n of long => 1, _ => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "naming a different value type"));
}

// §19.3 v0.313 admit control (passes pre-fix too: a guard, not a fail-
// without pin): `case v of long` with `v: T` is admitted, and at T = long the
// identical types match (1). NOT pinned here: at T = int the spec answer is
// "never matches" (9) but codegen answers 1 (the arm matches by widening) —
// a separate pre-existing codegen gap on the scrutinee-side per-instantiation
// decision, reported with this commit rather than pinned wrong.
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, aot_generic_scrutinee_long_at_long_matches) {
    auto r = runAot(
        "module M; class C "
        "public static method Pick<T>(v: T): int "
        "return (case v of long => 1, _ => 9); end Pick; "
        "public static method Main(): int "
        "return C.Pick<long>(5L); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
