// CG27b: string comparison operators and case string patterns (§4.6, §19).
// String comparison is ordinal (code-point-by-code-point, per §4.6 and
// stdlib §9.6). Implemented via memcmp on the underlying byte data.
// String constant patterns in case arms use content equality.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// §4.6: ordinal comparison — `<` returns true when first is lexicographically
// before the second. "abc" < "abd" because 'c' < 'd'.
BEZEL_TEST(codegen_cg27b, string_less_than_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abc\"; var b := \"abd\"; "
        "  return (if a < b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §4.6: "abd" is NOT less than "abc".
BEZEL_TEST(codegen_cg27b, string_less_than_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abd\"; var b := \"abc\"; "
        "  return (if a < b then 1 else 0); "
        "end Main; end C;"), 0);
}

// §4.6: equal strings are not less than each other.
BEZEL_TEST(codegen_cg27b, string_less_than_equal_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abc\"; "
        "  return (if a < a then 1 else 0); "
        "end Main; end C;"), 0);
}

// §4.6: `>` (ordinal greater-than).
BEZEL_TEST(codegen_cg27b, string_greater_than_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"xyz\"; var b := \"abc\"; "
        "  return (if a > b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §4.6: `<=` — less than or equal.
BEZEL_TEST(codegen_cg27b, string_less_equal_same) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abc\"; "
        "  return (if a <= a then 1 else 0); "
        "end Main; end C;"), 1);
}

// §4.6: `>=` — greater than or equal.
BEZEL_TEST(codegen_cg27b, string_greater_equal_less) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abc\"; var b := \"abd\"; "
        "  return (if a >= b then 1 else 0); "
        "end Main; end C;"), 0);
}

// §4.6: shorter string is less than longer string with same prefix.
// "abc" < "abcd" because on the 4th byte "abc" is exhausted.
BEZEL_TEST(codegen_cg27b, string_less_than_shorter_prefix) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abc\"; var b := \"abcd\"; "
        "  return (if a < b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §19: string constant patterns in case expressions — exact match.
BEZEL_TEST(codegen_cg27b, case_string_match_first_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var r := (case s of \"hello\" => 1, \"world\" => 2, _ => 0); "
        "  return r; "
        "end Main; end C;"), 1);
}

// §19: second arm matches when first does not.
BEZEL_TEST(codegen_cg27b, case_string_match_second_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"world\"; "
        "  var r := (case s of \"hello\" => 1, \"world\" => 2, _ => 0); "
        "  return r; "
        "end Main; end C;"), 2);
}

// §19: discard arm matches when no string arm matches.
BEZEL_TEST(codegen_cg27b, case_string_discard_fallback) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"other\"; "
        "  var r := (case s of \"hello\" => 1, \"world\" => 2, _ => 0); "
        "  return r; "
        "end Main; end C;"), 0);
}

// §19: string alternative patterns `"a", "b"` (OR'd alternatives).
BEZEL_TEST(codegen_cg27b, case_string_alternative_pattern) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"yes\"; "
        "  var r := (case s of \"yes\" | \"ok\" | \"true\" => 1, _ => 0); "
        "  return r; "
        "end Main; end C;"), 1);
}

}  // namespace
