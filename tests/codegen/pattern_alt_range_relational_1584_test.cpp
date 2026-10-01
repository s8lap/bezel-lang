// Codegen value tests for v0.183 (#1584): §19.3 admits RANGE and RELATIONAL
// members in a `|` pattern alternative list. Per §19.3 v0.183: "every member
// must be binding-free: a constant (a literal, an enum constant, or a named
// `const`), a range pattern, or a relational pattern — ranges and relationals
// admitted in v0.183 ... so `1..5 | 10 => …` and `< 0 | > 100 => …` are single
// arms." The list binds nothing regardless of member kind, so a binding member
// remains rejected; a type member remains a compile error. Matching is the OR
// of the members' individual membership tests — verified here via the int exit
// code. Pure-constant alternatives (v0.167, #1371) still collapse and work.
//
// Covered (each derives from the §19.3 v0.183 rule text):
//   - a `1..5 | 10` arm selects for a value INSIDE the range and for the
//     singleton, and falls through for a value in neither,
//   - a `< 0 | > 100` relational arm selects on both sides and falls through,
//   - a half-open range member (`1..<5`) excludes its upper bound,
//   - an open-ended range member (`..<0`) coexists with a constant member,
//   - a `null | 1..5` alternative over a nullable value scrutinee matches null,
//     an in-range present value, and falls through for an out-of-range value,
//   - a binding member in an alternative list is still rejected (§19.3),
//   - a pure-constant `|` alternative (no range/relational) still works.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runProg(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A `case` over an int classifier: a range+singleton alternative arm, then a
// two-sided relational alternative arm, else 0. // per §19.3 (v0.183, #1584)
int classify(const std::string& scrut) {
    return runProg(
        std::string("module M; class C public static method Main(): int ") +
        "  var x := " + scrut + "; "
        "  return (case x of "
        "    1..5 | 10 => 1, "     // range OR singleton constant
        "    < 0 | > 100 => 2, "   // two relational members
        "    _ => 0); end Main; end C;");
}

} // namespace

// ---- `1..5 | 10` selects for the range interior and for the singleton -------

BEZEL_TEST(codegen_alt_range_1584, range_member_matches_interior) {
    BEZEL_EXPECT_EQ(classify("3"), 1);    // 3 in 1..5
}

BEZEL_TEST(codegen_alt_range_1584, range_member_matches_lower_bound) {
    BEZEL_EXPECT_EQ(classify("1"), 1);    // inclusive lower bound
}

BEZEL_TEST(codegen_alt_range_1584, range_member_matches_upper_bound) {
    BEZEL_EXPECT_EQ(classify("5"), 1);    // inclusive upper bound
}

BEZEL_TEST(codegen_alt_range_1584, singleton_member_matches) {
    BEZEL_EXPECT_EQ(classify("10"), 1);   // the `| 10` constant member
}

BEZEL_TEST(codegen_alt_range_1584, neither_range_nor_singleton_falls_through) {
    BEZEL_EXPECT_EQ(classify("7"), 0);    // not in 1..5, not 10, not </>
}

// ---- `< 0 | > 100` relational alternative selects on both sides ------------

BEZEL_TEST(codegen_alt_relational_1584, less_than_member_matches) {
    BEZEL_EXPECT_EQ(classify("0 - 4"), 2);   // -4 < 0
}

BEZEL_TEST(codegen_alt_relational_1584, greater_than_member_matches) {
    BEZEL_EXPECT_EQ(classify("200"), 2);     // 200 > 100
}

BEZEL_TEST(codegen_alt_relational_1584, relational_gap_falls_through) {
    BEZEL_EXPECT_EQ(classify("50"), 0);      // 0 <= 50 <= 100, not in 1..5/10
}

// ---- a half-open range member excludes its upper bound ---------------------

BEZEL_TEST(codegen_alt_range_1584, half_open_member_excludes_upper) {
    const std::string prog =
        "module M; class C public static method Main(): int "
        "  var x := 5; "
        "  return (case x of 1..<5 | 20 => 1, _ => 0); "  // [1,5) excludes 5
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProg(prog), 0);
}

// ---- an open-ended range member coexists with a constant member ------------

BEZEL_TEST(codegen_alt_range_1584, open_ended_member_with_constant) {
    // `..<0 | 100`: everything below 0, or exactly 100.
    const std::string base =
        "module M; class C public static method Grade(x: int): int "
        "  return (case x of ..<0 | 100 => 9, _ => 0); "
        "end Grade; public static method Main(): int return ";
    BEZEL_EXPECT_EQ(runProg(base + "Grade(0 - 7); end Main; end C;"), 9);   // <0
    BEZEL_EXPECT_EQ(runProg(base + "Grade(100); end Main; end C;"), 9);     // 100
    BEZEL_EXPECT_EQ(runProg(base + "Grade(50); end Main; end C;"), 0);      // neither
}

// ---- `null | 1..5` over a nullable value scrutinee -------------------------
// §19.3 / §16.6 / §7.11 (v0.286, #3298) — REJECTION, reversing the three
// acceptance rows that used to live here. §19.3 admits a range as an
// alternative MEMBER, and it separately requires a non-nullable scrutinee at
// any range or relational pattern; the rejection is keyed on the member's
// pattern KIND, so it reaches an ordering member inside an alternative list
// exactly as it reaches a top-level one. A rule that stopped at the top level
// would make the verdict depend on the spelling of the arm — the "same shape,
// opposite answers, decided by spelling" defect proposal 0114 exists to remove.
//
// The alternative-list machinery #1584 installed is unaffected: it is still
// exercised over a NON-nullable scrutinee by every row above, and by the
// pure-constant row below.

namespace {
bool altRangeOverNullableRejected(const std::string& prog) {
    auto r = runAot(prog);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("pattern requires a non-nullable scrutinee") ==
                std::string::npos)
            continue;
        if (m.find("19.3") == std::string::npos) continue;
        return true;
    }
    return false;
}
}  // namespace

BEZEL_TEST(codegen_alt_range_1584, null_or_range_member_over_nullable_rejected) {
    // fix #5173 / §19.3 (v0.310, #5143 ruling): an UN-narrowed nullable
    // scrutinee (a bare parameter here) still rejects. The narrowed
    // variant is admitted — covered by `_admitted_5173` below.
    BEZEL_EXPECT_TRUE(altRangeOverNullableRejected(
        "module M; class C public static method Main(x: int?): int "
        "  return (case x of null | 1..5 => 8, _ => 0); "
        "end Main; end C;"));
}

// fix #5173 / §19.3 (v0.310, #5143 ruling): narrowed nullable scrutinee
// with a `null | 1..5` alternative list. The `null` member is unaffected
// (§19.3 constant-path); the `1..5` member runs the narrowed range pattern
// via patternMatches's `isNullableVal` branch.
//
// `Main(x: int?)` is NOT a §33.2 entry-point signature (only `Main()` or
// `Main(args: Array<string>)` are); Helper.Get() returns `int?` so
// `valueProvesNonNull` returns false and `x` is NOT narrowed at the
// declaration — the `if x <> null then` block narrows `x` for the case
// form (§7.4; §19.1).
BEZEL_TEST(codegen_alt_range_1584,
           null_or_range_member_over_narrowed_nullable_admitted_5173) {
    auto r = runAot(
        "module M; class Helper public static method Get(): int? "
        "  return 3; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var x: int? := Helper.Get(); "
        "  if x <> null then "
        "    return (case x of null | 1..5 => 8, _ => 0); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

// The absent-case member alone is still admitted: `null` is not an ordering
// pattern (§19.3's rejection is kind-keyed), and a constant member is §8.4
// equality, which is total on `null`.
BEZEL_TEST(codegen_alt_range_1584, null_or_constant_member_over_nullable_accepted) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C public static method Main(): int "
        "  var x: int? := null; "
        "  return (case x of null | 3 => 8, _ => 0); "
        "end Main; end C;"), 8);
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C public static method Main(): int "
        "  var x: int? := 3; "
        "  return (case x of null | 3 => 8, _ => 0); "
        "end Main; end C;"), 8);
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C public static method Main(): int "
        "  var x: int? := 99; "
        "  return (case x of null | 3 => 8, _ => 0); "
        "end Main; end C;"), 0);
}

// ---- a binding member in an alternative list is still rejected -------------
// §19.3: an alternative list binds nothing regardless of member kind, so a
// `var` binding cannot appear alongside a range member.

BEZEL_TEST(codegen_alt_range_1584, binding_member_still_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var x := 3; "
        "  return (case x of var y | 1..5 => y, _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool sawAltError = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("alternative list") != std::string::npos)
            sawAltError = true;
    BEZEL_EXPECT_TRUE(sawAltError);
}

// ---- pure-constant `|` alternatives (no range/relational) still work -------

BEZEL_TEST(codegen_alt_range_1584, pure_constant_alternatives_regression) {
    const std::string prog =
        "module M; class C public static method Main(): int "
        "  var x := 8; "
        "  return (case x of 1 | 2 | 3 => 10, 7 | 8 | 9 => 20, _ => 30); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProg(prog), 20);
}
