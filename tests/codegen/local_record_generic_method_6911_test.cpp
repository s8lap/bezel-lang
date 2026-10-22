// Regression tests for #6911 (spec v0.340): §21.10 "Restrictions (v1)" says
// "A local type may not reference an ENCLOSING generic parameter". A generic
// method declared ON the local record binds its own type parameter, which is
// not an enclosing one; the restriction flagged it. The genuine enclosing `T`
// must still be rejected (the last two rows).
//
// ORACLE: runAotChecked (real AOT path, generated-code ASan).

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #6911 rows.
BEZEL_TEST(local_record_generic_method_6911, spec_21_10_own_generic_method_on_local_record) {
    // Pick(7, 8) with A > 0 -> 7; A = 0 -> 8.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  record P(A: int) "
        "    public method Pick<T>(x: T, y: T): T return (if A > 0 then x else y); end Pick; "
        "  end P; "
        "  return new P(3).Pick<int>(7, 8) * 10 + new P(0).Pick(7, 8) - 62; "
        "end Main; end C; "), 16);
}
BEZEL_TEST(local_record_generic_method_6911, spec_21_10_own_generic_method_in_generic_enclosing_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "public static method Go<T>(t: T): int "
        "  record P(A: int) "
        "    public method Pick<U>(x: U, y: U): U return (if A > 0 then x else y); end Pick; "
        "  end P; "
        "  return new P(3).Pick<int>(7, 8) * 10 + new P(0).Pick(7, 8); "
        "end Go; "
        "public static method Main(): int return Go<string>(\"s\") - 70; end Main; end C; "), 8);
}
BEZEL_TEST(local_record_generic_method_6911, spec_9_top_level_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(A: int) "
        "  public method Pick<T>(x: T, y: T): T return (if A > 0 then x else y); end Pick; "
        "end P; class C public static method Main(): int "
        "  return new P(3).Pick<int>(7, 8) * 10 + new P(0).Pick(7, 8) - 62; "
        "end Main; end C; "), 16);
}
// The restriction still bites the ENCLOSING parameter: in a field type ...
BEZEL_TEST(local_record_generic_method_6911, spec_21_10_enclosing_param_in_field_still_rejected) {
    auto r = runAotChecked(
        "module M; class C public static method Go<T>(t: T): int "
        "  record P(A: T) public method Pick<U>(x: U): U return x; end Pick; end P; "
        "  return 0; end Go; "
        "public static method Main(): int return Go<int>(1); end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "enclosing generic parameter"));
}
// ... and in a signature of the local record's own generic method.
BEZEL_TEST(local_record_generic_method_6911, spec_21_10_enclosing_param_in_own_method_signature_still_rejected) {
    auto r = runAotChecked(
        "module M; class C public static method Go<T>(t: T): int "
        "  record P(A: int) public method Pick<U>(x: U): T return default(T); end Pick; end P; "
        "  return 0; end Go; "
        "public static method Main(): int return Go<int>(1); end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "enclosing generic parameter"));
}
