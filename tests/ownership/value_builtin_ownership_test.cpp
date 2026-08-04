// Spec-derived tests for issue #434 — ownership checker must NOT impose a
// §28.3 disposal obligation on the builtin VALUE types DateTime, TimeSpan,
// Guid, and CancellationToken.
//
// §2.5 / §2.6 / §6.1 / §30.8 / §28:
//   DateTime  — scalar i64 tick count; value type, no heap allocation.
//   TimeSpan  — scalar i64 tick count; value type, no heap allocation.
//   Guid      — 128-bit struct { i64 lo, i64 hi }; value type, no heap.
//   CancellationToken — thin handle (borrowed pointer); value type.
//   CancellationTokenSource — owned class, IS disposable; must remain owned.
//
// Regression contract (no-over, no-under):
//   VALUE locals (DateTime/TimeSpan/Guid/CancellationToken):
//     - `var x := new T(...)` or `var x := T.Method()` → no error.
//     - Multi-step arithmetic via locals → no error.
//   OWNED class locals (CancellationTokenSource, user-defined class T):
//     - `var x := new OwnedT()` without disposal → STILL an error.
//     - `using var x := new CancellationTokenSource()` → still implicitly
//       discharged (regression guard).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Minimal program wrapper: a static Main that returns int.
std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int "
           + body +
           " end Main; "
           "end C;";
}

// Ownership-only wrapper (no JIT): checks ownership diagnostics.
std::string own(const std::string& body) {
    return prog(body);
}

} // namespace

// ---------------------------------------------------------------------------
// §2.5 DateTime — value type, no disposal obligation (issue #434).
// ---------------------------------------------------------------------------

// §28 / §2.5: `var dt := new DateTime(2024, 3, 15); return dt.Day;`
// DateTime is a value type; the ownership checker MUST NOT require disposal.
BEZEL_TEST(own_value_builtin_434, datetime_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "var dt := new DateTime(2024, 3, 15); "
        "return dt.Day; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28 / §2.5: local TimeSpan via static factory — no disposal obligation.
BEZEL_TEST(own_value_builtin_434, timespan_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "var ts := TimeSpan.FromSeconds(30); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28 / §2.5: DateTime arithmetic via intermediate locals — no error.
// Mirrors the exact repro from issue #434.
BEZEL_TEST(own_value_builtin_434, datetime_arithmetic_locals_no_error) {
    auto r = parseAndCheckOwnership(own(
        "var d1 := new DateTime(2024, 3, 15); "
        "var d2 := new DateTime(2024, 3, 16); "
        "var diff := d2.Subtract(d1); "
        "return diff.Days; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28 / §2.5: TimeSpan constructor via new — no disposal obligation.
BEZEL_TEST(own_value_builtin_434, timespan_ctor_local_no_error) {
    auto r = parseAndCheckOwnership(own(
        "var ts := new TimeSpan(1, 30, 0); "
        "return ts.Hours; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28 / §2.5: DateTime.UtcNow static property via local — no error.
BEZEL_TEST(own_value_builtin_434, datetime_utcnow_local_no_error) {
    auto r = parseAndCheckOwnership(own(
        "var now := DateTime.UtcNow; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §2.6 Guid — value type, no disposal obligation (issue #434).
// ---------------------------------------------------------------------------

// §28 / §2.6: `var g := Guid.NewGuid()` — no disposal obligation.
BEZEL_TEST(own_value_builtin_434, guid_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "var g := Guid.NewGuid(); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28 / §2.6: Guid.Empty static property via local — no error.
BEZEL_TEST(own_value_builtin_434, guid_empty_local_no_error) {
    auto r = parseAndCheckOwnership(own(
        "var a := Guid.Empty; "
        "var b := Guid.Empty; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §6.1 / §30.8 CancellationToken — value type, no disposal obligation.
// ---------------------------------------------------------------------------

// §28 / §6.1: CancellationToken local from cts.Token — no disposal obligation.
BEZEL_TEST(own_value_builtin_434, cancellation_token_local_no_dispose_error) {
    auto r = parseAndCheckOwnership(own(
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28 / §6.1: CancellationToken.None static property via local — no error.
BEZEL_TEST(own_value_builtin_434, cancellation_token_none_local_no_error) {
    auto r = parseAndCheckOwnership(own(
        "var tok := CancellationToken.None; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Regression: owned class locals STILL require disposal (no-under).
// ---------------------------------------------------------------------------

// §28.3: CancellationTokenSource is an OWNED disposable class.
// A plain `var` without `using` or explicit Dispose MUST still be diagnosed.
// §28.3 v0.150 (#1008 Part B): a plain-`var` CancellationTokenSource left
// undischarged auto-drops at scope end (must-use warning), no error.
BEZEL_TEST(own_value_builtin_434, cancellation_token_source_still_owned) {
    auto r = parseAndCheckOwnership(own(
        "var cts := new CancellationTokenSource(); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "cts"));
}

// §28.3: using var CancellationTokenSource is implicitly discharged — clean.
BEZEL_TEST(own_value_builtin_434, cancellation_token_source_using_var_clean) {
    auto r = parseAndCheckOwnership(own(
        "using var cts := new CancellationTokenSource(); "
        "return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 v0.150: a user-defined class local without disposal auto-drops at scope
// end (must-use warning), no error. This is the counterweight to the value-type
// cases above — #434's contract is "no-over, no-under", and this is the
// no-UNDER half.
//
// §28.9 (v0.237, #2654): `T` declares a DESTRUCTOR so it is NON-trivial-
// dispose, which is what makes it the "OWNED class local" the file header
// names. A trivial-dispose class is exempt from this very warning ("A `new`
// expression of a trivial-dispose class type … does *not* produce the standard
// ownership-discharge warning"), so the previous bare `class T end T;` made
// this fixture assert the opposite of what §28.9 requires.
BEZEL_TEST(own_value_builtin_434, user_class_still_owned) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class T destructor end destructor; end T; "
        "class C "
        "  public static method Main(): int "
        "    var x := new T(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

// §28.3: `using var` for a user class IS implicitly discharged — clean.
// `T` is destructor-bearing (see above), so this is a real discharge of a real
// obligation rather than a vacuous pass on a §28.9-exempt trivial class.
BEZEL_TEST(own_value_builtin_434, user_class_using_var_clean) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class T destructor end destructor; end T; "
        "class C "
        "  public static method Main(): int "
        "    using var x := new T(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
