// fix #856 / §19.5: a typed-binding pattern `var n: string` never matched when
// the scrutinee was `string?` (or a boxed `Object` holding a string). A string
// lowers to a (possibly-null) pointer, not the {i8,T} tagged struct, so a
// `string?` scrutinee fell into the boxed-primitive type-test path whose
// `bz_obj_is_string` short-circuit returns FALSE for a real string (correct for
// `var n: int` — a string is not an int — but wrong for `var n: string`). Per
// §19.5 a typed binding matches a non-null value of the type; a non-null
// `string?` IS a `string`. The fix matches iff the value is a non-null string
// (an Object scrutinee uses a runtime is-string test guarded by non-null).
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// bug13: a non-null `string?` matches `var n: string` (was taking the `_` arm).
BEZEL_TEST(typed_binding_string_pattern_856, nullable_string_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"hello\"; "
        "  return (case s of var n: string => 1, _ => 2); "  // 1
        "end Main; end C;"), 1);
}

// The bound name is the non-null string — usable in the arm body.
BEZEL_TEST(typed_binding_string_pattern_856, binds_unwrapped_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"hello\"; "
        "  return (case s of var n: string => n.Length, _ => 0); "  // 5
        "end Main; end C;"), 5);
}

// §19.5: a typed binding never matches null — a null `string?` takes `_`.
BEZEL_TEST(typed_binding_string_pattern_856, null_string_takes_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string? := null; "
        "  return (case s of var n: string => 1, _ => 2); "  // 2
        "end Main; end C;"), 2);
}

// An `Object` holding a string matches `var n: string` (runtime is-string test).
BEZEL_TEST(typed_binding_string_pattern_856, object_holding_string_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hi\"; "
        "  return (case o of var n: string => n.Length, _ => 0); "  // 2
        "end Main; end C;"), 2);
}

// An `Object` holding a non-string (int) does NOT match `var n: string`.
BEZEL_TEST(typed_binding_string_pattern_856, object_holding_int_no_string_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := 42; "
        "  return (case o of var n: string => 1, _ => 9); "  // 9
        "end Main; end C;"), 9);
}

// Regression: `var n: int` against an `Object` holding a string still takes `_`
// (the boxed-primitive is-string short-circuit is unchanged for non-string).
BEZEL_TEST(typed_binding_string_pattern_856, int_pattern_vs_object_string_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var o: Object := \"hi\"; "
        "  return (case o of var n: int => 1, _ => 7); "  // 7
        "end Main; end C;"), 7);
}
