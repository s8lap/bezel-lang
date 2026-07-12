// fix #1472 / §19.3 (grammar A.7 `RangePattern ::= ".."`): a bare `..` range
// pattern is an unbounded range that matches every value.
//
// §19.3 / A.7: `..` with no bounds and no step is a valid RangePattern; an
// unbounded range is vacuously satisfied by every value (equivalently `_`).
// The codegen guard `if (range && !range->step && (range->lo || range->hi))`
// excluded the both-bounds-absent case — contradicting its own comment "A
// `..`-only pattern reduces to true" — so `patternMatches` returned nullptr.
// In expression position that left the `case.end` block unterminated → IR
// verification failed → the compiler aborted; in statement position the arm
// was silently never taken. The fix drops the `(lo || hi)` clause so a bare
// `..` flows through the existing bounds logic, where an absent lower bound
// leaves geL = true and an absent upper bound leaves leH = true, yielding an
// unconditional match. §19.5: on a NULLABLE scrutinee a range pattern matches
// every PRESENT value but never null, so `..` there reduces to `hasValue`.

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

// §19.3 / A.7: expression-position `case x of .. => 7 end` — the bare `..`
// matches, the case returns 7. (The #1472 expr-position repro; previously an
// IR-verification abort.)
BEZEL_TEST(case_range_dotdot_1472, expr_bare_dotdot_matches) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class App public static method Main(): int "
        "  var x := 999; "
        "  return case x of .. => 7 end; "
        "end Main; end App;"), 7);
}

// §19.3 / A.7: statement-position `case x of .. => ... end` — the bare `..`
// arm FIRES (previously silently never taken).
BEZEL_TEST(case_range_dotdot_1472, stmt_bare_dotdot_fires) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class App public static method Main(): int "
        "  var x := 999; "
        "  var r := 0; "
        "  case x of .. => r := 42 end; "
        "  return r; "
        "end Main; end App;"), 42);
}

// §19.3: a bare `..` is unconditional — it wins over a later `_` and matches
// regardless of the scrutinee value.
BEZEL_TEST(case_range_dotdot_1472, dotdot_matches_any_value) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class App public static method Main(): int "
        "  var x := -7; "
        "  return case x of .. => 3, _ => 4 end; "
        "end Main; end App;"), 3);
}

// §19.5: on a nullable scrutinee holding a PRESENT value, `..` matches (a range
// pattern matches every present value).
BEZEL_TEST(case_range_dotdot_1472, nullable_present_value_matches_dotdot) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class App public static method Main(): int "
        "  var x: int? := 5; "
        "  return case x of .. => 7, null => 9 end; "
        "end Main; end App;"), 7);
}

// §19.5: on a nullable scrutinee holding `null`, `..` does NOT match (a range
// pattern never matches null) — the explicit `null` arm fires instead.
BEZEL_TEST(case_range_dotdot_1472, nullable_null_value_does_not_match_dotdot) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class App public static method Main(): int "
        "  var x: int? := null; "
        "  return case x of .. => 7, null => 9 end; "
        "end Main; end App;"), 9);
}
