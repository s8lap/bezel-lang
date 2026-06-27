// Regression tests for #1104 (§17.8): `a ?? b` where both operands are
// value-type nullables (`int? ?? int?`) crashed codegen with an LLVM PHI
// same-type assertion (SIGABRT) — lowerCoalesce built the result PHI with one
// incoming the unwrapped `T` and the other the `{i8,T}` nullable struct. Two
// fixes: (1) sema types `T? ?? T?` as `T?` (not the non-nullable `T`) — the
// result CAN be null when both are null; (2) lowerCoalesce wraps the non-null
// LHS payload into `{i8,T}` so both PHI incomings match when the RHS is nullable.
// Found by adversarial discovery sweep #8.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1104: `int? ?? int?` compiles (was SIGABRT) and the non-null RHS is selected
// when the LHS is null.
BEZEL_TEST(coalesce_value_nullable_rhs_1104, null_lhs_picks_nullable_rhs) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int? := null; "
        "  var b: int? := 5; "
        "  var c := a ?? b; "                             // int? = 5
        "  return c ?? -1; "                              // 5
        "end Main; end C;"), 5);
}

// The result of `T? ?? T?` is nullable: chaining `(a ?? b) ?? d` coalesces to the
// final default when both a and b are null.
BEZEL_TEST(coalesce_value_nullable_rhs_1104, both_null_chains_to_default) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int? := null; "
        "  var b: int? := null; "
        "  var c := a ?? b; "                             // int? = null
        "  return c ?? 9; "                               // 9
        "end Main; end C;"), 9);
}

// Non-null LHS short-circuits (the nullable RHS is not consulted).
BEZEL_TEST(coalesce_value_nullable_rhs_1104, non_null_lhs_short_circuits) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int? := 3; "
        "  var b: int? := 7; "
        "  var c := a ?? b; "                             // 3
        "  return c ?? -1; "                              // 3
        "end Main; end C;"), 3);
}

// Regression guard: a NON-nullable RHS still narrows the result to `T`.
BEZEL_TEST(coalesce_value_nullable_rhs_1104, non_nullable_rhs_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int? := null; "
        "  var b := a ?? 42; "                            // int = 42
        "  var x: int? := 7; "
        "  return b + (x ?? 0); "                         // 42 + 7 = 49
        "end Main; end C;"), 49);
}

// A nullable enum `??` nullable enum is likewise fine.
BEZEL_TEST(coalesce_value_nullable_rhs_1104, nullable_enum_coalesce) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var a: Color? := null; "
        "  var b: Color? := Color.Blue; "
        "  var c := a ?? b; "                             // Color? = Blue (ord 2)
        "  return int(c ?? Color.Red); "                  // 2
        "end Main; end C;"), 2);
}
