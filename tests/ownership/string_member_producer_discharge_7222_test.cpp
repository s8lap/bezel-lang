// Regression tests for #7222 — the §9.6 `string`-member family did NOT
// discharge the `value` argument's PRODUCER reference, so the argument leaked.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * language spec §28.3.4 (Producers, v0.344), the "Parameter `p` (no
//     modifier)" row: a plain parameter is "Borrowed (source: caller)" — so
//     the ARGUMENT's reference is the CALLER's to discharge, once the call is
//     over.
//   * language spec §28.6 (v0.344): "the callee receives a parameter-sourced
//     borrow ... the callee further borrows for the duration of the call" —
//     the callee owns NO reference to the argument, so the callee must not
//     release it either.
//   * language spec §28.9 (v0.344): `string` is a runtime-managed type, and
//     "the implementation reclaims the production; how is not specified" — so
//     a production the implementation drops on the floor is a LEAK.
//   * stdlib §9.6 (v0.344) declares the members with BORROW parameters —
//     `public pure method Insert(startIndex: int, value: string): string;`
//     and `public pure method Replace(oldValue: string, newValue: string):
//     string;` carry no `owned` marker.
//
// => THE PRODUCER REFERENCE OF A `string` ARGUMENT IS THE CALLER'S TO
// DISCHARGE, ONCE, WHEN THE CALL IS OVER.
//
// THE DEFECT (MEASURED on the base tree c9dfae6c, `--sanitize=address`).
//
//   | program                                | base arm              |
//   |----------------------------------------|-----------------------|
//   | ("a"+"b").Insert(1, "x"+"y")           | 35 B in 2 allocations |
//   | ("a"+"b").Insert(1, "z")  (a literal)  | clean                 |
//   | ("a"+"b").Replace("a", "x"+"y")        | 35 B in 2 allocations |
//   | ("a"+"b").Insert(999, "x"+"y") (RAISE) | 35 B in 2 allocations |
//
//   The literal control is the TELL: a literal argument carries NO producer
//   reference to discharge, so it never leaked. The difference between the
//   two rows is the PRODUCER, not the argument.
//
//   => IT IS A FAMILY DEFECT, NOT ONE ARM'S: `Replace` — an arm no earlier
//   lane touched — leaked exactly as `Insert` did, and so did every other
//   §9.6 member that takes a `string` argument. MEASURED on the base arm,
//   each of these leaked 35 B in 2 allocations:
//     Contains · StartsWith · EndsWith · IndexOf (both arities) ·
//     LastIndexOf · CompareTo · Equals · Replace · Insert · Split ·
//     CountOf
//   (the two STATIC members with a `string` argument — `string.Join` and
//   `string.IsNullOrWhiteSpace` — leaked 35 B in 2 allocations as well; they
//   are lowered in other files and are NOT covered by this change.)
//
// ORACLE CHOICE. The observable is a LEAK, so every row here runs on the
// `runAotChecked` oracle (generated-code ASan + LeakSanitizer armed): a
// plain `runAot` row runs with `detect_leaks=0` and would stay GREEN on the
// broken code (the program still exits 0 and prints the right text) — a
// vacuous oracle for this defect. `runAotChecked` FAILS the row when the
// sanitizer fires, so the row has teeth. The conformance corpus runner arms
// NO sanitizer at all, so it cannot witness this defect either.
//
// The LITERAL row is the CONTROL that proves the rows discriminate the
// producer rather than the argument: it is green on BOTH arms, and it would
// stay green if the discharge were simply dropped for every argument.
//
// The RAISE row covers the SECOND edge: a member that raises unwinds past the
// normal-return discharge, so the throw edge must discharge the argument's
// producer reference too (§28.9 — the same shape as the receiver's release
// on the throw edge, the #6979/#7159 idiom).

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A program that binds a §9.6 member call's result to a local and prints it.
//
// ⚠ The result is bound to a LOCAL on purpose: a fresh runtime-managed value
// is discharged by its holder's release point (§28.3), so the local's
// scope-end release accounts for the RESULT. What these rows measure is the
// ARGUMENT's producer reference — the one the member path used to drop.
std::string stringProg(const std::string& expr) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var t: string := " + expr + ";\n"
           "    Console.WriteLine(t);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same shape for an `int`-returning member (`IndexOf` / `LastIndexOf` /
// `CountOf`): the result is rendered through `ToString()` so the row can
// assert the member still computes the right answer.
std::string intProg(const std::string& expr) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var t: int := " + expr + ";\n"
           "    Console.WriteLine(t.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The RAISE shape: the member's guard raises `IndexOutOfRangeException`
// (stdlib §3.1's `StringBuilder` twin contract, which decides `String.Insert`
// — §9.6 declares no contract paragraph of its own), and the catch arm
// returns 42 so the row can tell "raised" from "returned".
std::string raisingProg(const std::string& expr) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      var t: string := " + expr + ";\n"
           "      Console.WriteLine(t);\n"
           "      return 0;\n"
           "    catch IndexOutOfRangeException => return 42;\n"
           "    end;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// (A) `Insert` — a producer `value` argument.
//
// Pre-fix: 35 B leaked in 2 allocations (the `value` concatenation's +1
// reference, never discharged). The program still printed `axyb` and exited
// 0, so only the CHECKED oracle could see it.
// ===========================================================================
BEZEL_TEST(string_member_producer_discharge_7222,
           insert_producer_value_argument_is_discharged_stdlib_9_6) {
    JitResult r = runAotChecked(
        stringProg("(\"a\" + \"b\").Insert(1, \"x\" + \"y\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // no ASan report, no LSan leak
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "axyb\n");
}

// ===========================================================================
// (B) `Replace` — the SECOND member, and the proof that the defect was the
// FAMILY's and not one arm's.
//
// `Replace` is an arm NO earlier lane touched, and it leaked exactly as
// `Insert` did. Pre-fix: 35 B in 2 allocations.
// ===========================================================================
BEZEL_TEST(string_member_producer_discharge_7222,
           replace_producer_value_argument_is_discharged_stdlib_9_6) {
    JitResult r = runAotChecked(
        stringProg("(\"a\" + \"b\").Replace(\"a\", \"x\" + \"y\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "xyb\n");
}

// BOTH of `Replace`'s `string` arguments as producers — the two-argument
// shape, where the OLD operand is a producer too. Pre-fix: 35 B in 2
// allocations (one producer reference was discharged, the other was not —
// which is exactly the "one arm, one leak" shape the family fix removes).
BEZEL_TEST(string_member_producer_discharge_7222,
           replace_producer_old_and_new_value_arguments_are_discharged_stdlib_9_6) {
    JitResult r = runAotChecked(
        stringProg("(\"a\" + \"b\").Replace(\"a\" + \"b\", \"x\" + \"y\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "xy\n");
}

// ===========================================================================
// (C) THE LITERAL CONTROL.
//
// A literal argument carries NO producer reference to discharge, so this row
// is green on BOTH arms — and it stays green if the discharge were dropped
// for every argument. That is what makes it the control: it proves the rows
// above discriminate the PRODUCER, not the argument.
// ===========================================================================
BEZEL_TEST(string_member_producer_discharge_7222,
           insert_literal_value_argument_stays_clean_stdlib_9_6) {
    JitResult r = runAotChecked(
        stringProg("(\"a\" + \"b\").Insert(1, \"z\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "azb\n");
}

// ===========================================================================
// (D) A THIRD MEMBER — `IndexOf` (an `int`-returning member, so its result
// is a VALUE and the argument's reference is the only reference in play).
//
// Pre-fix: 35 B in 2 allocations.
// ===========================================================================
BEZEL_TEST(string_member_producer_discharge_7222,
           indexof_producer_value_argument_is_discharged_stdlib_9_6) {
    JitResult r = runAotChecked(
        intProg("(\"a\" + \"b\").IndexOf(\"a\" + \"b\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// ===========================================================================
// (E) THE RAISE EDGE.
//
// A raise unwinds past the member's normal-return discharge, so the throw
// edge must discharge the argument's producer reference too (§28.9 — the
// same shape as the receiver's release on the throw edge).
//
// Pre-fix: 35 B in 2 allocations; the program still returned 42, so only the
// CHECKED oracle could see it.
// ===========================================================================
BEZEL_TEST(string_member_producer_discharge_7222,
           insert_producer_value_argument_is_discharged_on_the_raise_edge_stdlib_3_1) {
    JitResult r = runAotChecked(
        raisingProg("(\"a\" + \"b\").Insert(999, \"x\" + \"y\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (F) AN INTERPOLATION producer — the other spelling of a +1 producer.
//
// §28.9's producer reference is not a concatenation's alone: an interpolated
// string is a fresh runtime-managed value too, and it carries the same
// undischarged reference when it is an argument.
//
// Pre-fix: 35 B in 2 allocations.
// ===========================================================================
BEZEL_TEST(string_member_producer_discharge_7222,
           insert_interpolated_value_argument_is_discharged_stdlib_9_6) {
    JitResult r = runAotChecked(
        stringProg("(\"a\" + \"b\").Insert(1, $\"x{1}\")"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ax1b\n");
}
