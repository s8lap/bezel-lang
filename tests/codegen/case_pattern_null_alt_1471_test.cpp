// fix #1471 / §19.5: a `null` member folded into a `|`-alternative over a
// nullable VALUE-type scrutinee (`1 | null`, `Red | null`, `true | null`).
//
// §19.3 (grammar A.8): `null` is a Constant and an admissible member of an
// alternative list; §19.5: `null` is matched by the explicit `null` pattern.
// For a nullable value scrutinee (a tagged {i8 hasValue; T value} struct), the
// codegen ConstantPattern loop lowered the `null` member as a value — a
// ConstantPointerNull — and fed it to emitEquals against the unwrapped inner
// value, building an ICmp on mismatched types (ptr vs iN) that aborted the
// backend (LLVM `Both operands to ICmp instruction are not of the same type!`).
// The standalone NullPattern branch was never reached because `|` folds `null`
// into a ConstantPattern's constants, not a NullPattern. The fix detects the
// NullLiteralExpr member and routes it through the same has-no-value test
// (`!hasValue`) the standalone NullPattern uses.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19.5: `int?` scrutinee is null → `1 | null` matches → returns 111.
BEZEL_TEST(case_pattern_null_alt_1471, int_nullable_null_matches_alt) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class App public static method Main(): int "
        "  var x: int? := null; "
        "  var r := case x of 1 | null => 111, _ => 999 end; "
        "  return r; "
        "end Main; end App;"), 111);
}

// §19.5: `int?` scrutinee is a PRESENT value equal to the literal member `1` →
// `1 | null` still matches → returns 111. (Non-null path unregressed.)
BEZEL_TEST(case_pattern_null_alt_1471, int_nullable_literal_member_matches) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class App public static method Main(): int "
        "  var x: int? := 1; "
        "  var r := case x of 1 | null => 111, _ => 999 end; "
        "  return r; "
        "end Main; end App;"), 111);
}

// §19.5: `int?` scrutinee is a PRESENT value matching NEITHER member → the
// `_` catch-all fires → returns 42. (Guards against the alternative wrongly
// swallowing non-null non-matching values.)
BEZEL_TEST(case_pattern_null_alt_1471, int_nullable_present_nonmatch_falls_through) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class App public static method Main(): int "
        "  var x: int? := 2; "
        "  var r := case x of 1 | null => 111, _ => 42 end; "
        "  return r; "
        "end Main; end App;"), 42);
}

// §19.5: `enum?` scrutinee is null → `Red | null` matches → returns 111.
BEZEL_TEST(case_pattern_null_alt_1471, enum_nullable_null_matches_alt) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class App public static method Main(): int "
        "  var x: Color? := null; "
        "  var r := case x of Red | null => 111, _ => 999 end; "
        "  return r; "
        "end Main; end App;"), 111);
}

// §19.5: `enum?` scrutinee is the enum constant member → `Red | null` matches.
BEZEL_TEST(case_pattern_null_alt_1471, enum_nullable_constant_member_matches) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class App public static method Main(): int "
        "  var x: Color? := Color.Red; "
        "  var r := case x of Red | null => 111, _ => 999 end; "
        "  return r; "
        "end Main; end App;"), 111);
}

// §19.5: `bool?` scrutinee is null → `true | null` matches → returns 111.
BEZEL_TEST(case_pattern_null_alt_1471, bool_nullable_null_matches_alt) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class App public static method Main(): int "
        "  var x: bool? := null; "
        "  var r := case x of true | null => 111, _ => 999 end; "
        "  return r; "
        "end Main; end App;"), 111);
}
