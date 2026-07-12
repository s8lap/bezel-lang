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
        "  return case x of "
        "    1..5 | 10 => 1, "     // range OR singleton constant
        "    < 0 | > 100 => 2, "   // two relational members
        "    _ => 0 "
        "  end; end Main; end C;");
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
        "  return case x of 1..<5 | 20 => 1, _ => 0 end; "  // [1,5) excludes 5
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProg(prog), 0);
}

// ---- an open-ended range member coexists with a constant member ------------

BEZEL_TEST(codegen_alt_range_1584, open_ended_member_with_constant) {
    // `..<0 | 100`: everything below 0, or exactly 100.
    const std::string base =
        "module M; class C public static method Grade(x: int): int "
        "  return case x of ..<0 | 100 => 9, _ => 0 end; "
        "end Grade; public static method Main(): int return ";
    BEZEL_EXPECT_EQ(runProg(base + "Grade(0 - 7); end Main; end C;"), 9);   // <0
    BEZEL_EXPECT_EQ(runProg(base + "Grade(100); end Main; end C;"), 9);     // 100
    BEZEL_EXPECT_EQ(runProg(base + "Grade(50); end Main; end C;"), 0);      // neither
}

// ---- `null | 1..5` over a nullable value scrutinee -------------------------
// §19.3 v0.183 / §19.5: `null` matches the absent slot; the range member
// matches a present in-range value; both are members of one binding-free arm.

BEZEL_TEST(codegen_alt_range_1584, null_or_range_matches_null) {
    const std::string prog =
        "module M; class C public static method Main(): int "
        "  var x: int? := null; "
        "  return case x of null | 1..5 => 8, _ => 0 end; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProg(prog), 8);
}

BEZEL_TEST(codegen_alt_range_1584, null_or_range_matches_in_range_present) {
    const std::string prog =
        "module M; class C public static method Main(): int "
        "  var x: int? := 3; "
        "  return case x of null | 1..5 => 8, _ => 0 end; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProg(prog), 8);
}

BEZEL_TEST(codegen_alt_range_1584, null_or_range_falls_through_out_of_range) {
    const std::string prog =
        "module M; class C public static method Main(): int "
        "  var x: int? := 99; "
        "  return case x of null | 1..5 => 8, _ => 0 end; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProg(prog), 0);
}

// ---- a binding member in an alternative list is still rejected -------------
// §19.3: an alternative list binds nothing regardless of member kind, so a
// `var` binding cannot appear alongside a range member.

BEZEL_TEST(codegen_alt_range_1584, binding_member_still_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var x := 3; "
        "  return case x of var y | 1..5 => y, _ => 0 end; "
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
        "  return case x of 1 | 2 | 3 => 10, 7 | 8 | 9 => 20, _ => 30 end; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProg(prog), 20);
}
