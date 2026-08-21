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
        "  return (case x of .. => 7); "
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
        "  return (case x of .. => 3, _ => 4); "
        "end Main; end App;"), 3);
}

// §19.3 / §16.6 (v0.286, #3298) — REJECTION, reversing the two acceptance rows
// that used to live here. These pinned proposal 0114's D5 reading (a range
// pattern over a nullable scrutinee matches every PRESENT value and never
// `null`), which v0.267 deliberately overrode and v0.273 declined to re-open:
// "A nullable scrutinee — value or reference — is a compile error at a
// relational pattern, and at a range pattern (§16.6) alike ... The rejection is
// keyed on the KIND of pattern, not on which bounds are written, so it reaches
// the open-ended forms and the fully open `..` with the closed ones" (§19.3).
// The bare `..` is inside the decline: v0.271's own accounting counted these
// two blocks among the eight the landed rule newly rejects.
//
// The non-nullable bare-`..` rows above are UNAFFECTED — #1472's actual
// subject is the both-bounds-absent codegen guard, and it stays fixed.
namespace {
// A rejection whose diagnostic is the §19.3 one, not merely "did not run".
bool rejectedAsNullableScrutineePattern(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("pattern requires a non-nullable scrutinee") ==
                std::string::npos)
            continue;
        if (m.find("19.3") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}
}  // namespace

BEZEL_TEST(case_range_dotdot_1472, nullable_present_value_dotdot_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableScrutineePattern(
        "module M; class App public static method Main(): int "
        "  var x: int? := 5; "
        "  return (case x of .. => 7, null => 9); "
        "end Main; end App;"));
}

BEZEL_TEST(case_range_dotdot_1472, nullable_null_value_dotdot_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableScrutineePattern(
        "module M; class App public static method Main(): int "
        "  var x: int? := null; "
        "  return (case x of .. => 7, null => 9); "
        "end Main; end App;"));
}
