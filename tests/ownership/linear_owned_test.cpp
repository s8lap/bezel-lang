// Spec-derived tests for Ownership slice O1 — linear owned-local
// lifecycle (§28.3).
//
// O1's contract: each `var x := new T();` introduces an Owned local
// with an explicit-discharge obligation. `using var x := EXPR;`
// carries implicit discharge. Failing to discharge a plain-`var` Owned
// local is a diagnostic; `using var` is always clean.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Wrap a method body with a preamble declaring the helper classes used
// in tests, so type resolution and identifier resolution don't emit
// unrelated diagnostics.
std::string wrap(const std::string& body, const std::string& sig = "M()") {
    return std::string("module M; "
        "class T end T; class U end U; "
        "class Source public method Read(): T => new T(); end Source; "
        "class C "
        "  public source: Source := null; "
        "  public method ") + sig + " " + body + " end; "
        "end C;";
}

} // namespace

BEZEL_TEST(own_linear, using_var_is_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var x := new T();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 v0.150 (#1008 Part B, affine auto-drop): a plain-`var` owned local left
// undischarged is no longer an error — it is auto-disposed at scope end and the
// pass emits the must-use WARNING (not an error).
BEZEL_TEST(own_linear, plain_var_undischarged_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

// §28.3 v0.150 (#1008 Part B) SOUNDNESS: a REASSIGNED owned local is NOT auto-
// dropped — auto-drop disposes only the final slot value, so the overwritten
// first value would leak. Such a local keeps the discharge error (the
// conservative pre-v0.150 behaviour), preventing a silent leak.
BEZEL_TEST(own_linear, reassigned_owned_local_not_auto_dropped) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x := new T();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "x"));
}

BEZEL_TEST(own_linear, plain_var_with_explicit_dispose_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 (#1500): the must-use auto-drop warning is suppressed "when it is
// explicitly disposed or discarded — including through the null-conditional
// and null-forgiving spellings `x?.Dispose()` / `x!.Dispose()`, required for
// nullable owned locals." `x!.Dispose()` parses as a MemberAccessExpr whose
// target is a PostfixBangExpr around the identifier; before the fix, only a
// bare-IdentifierExpr receiver was recognized as the explicit-disposal
// discharge, so this nullable-owned-local form (the repro from the issue:
// `var t := move pool[0]; pool[0] := null; t!.Dispose();`) still warned.
BEZEL_TEST(own_linear, nullable_var_with_bang_dispose_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: T? := new T(); x!.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "x"));
}

// §28.3 (#1500): the null-conditional receiver spelling `x?.Dispose()` is the
// other explicit-disposal form the spec names alongside `x!.Dispose()`; it
// already worked (recognized via NullCondMemberExpr, #860) — pinned here as a
// regression guard alongside the `!` fix above.
BEZEL_TEST(own_linear, nullable_var_with_null_cond_dispose_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: T? := new T(); x?.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "x"));
}

// §28.3 (#1500) NEGATIVE: a nullable owned local left undischarged must still
// draw the must-use auto-drop warning — the `!`/`?` recognition above must not
// over-suppress a genuinely undischarged local.
BEZEL_TEST(own_linear, nullable_var_undischarged_still_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: T? := new T();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

BEZEL_TEST(own_linear, plain_var_with_move_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); return move x;", "Make(): T"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_linear, plain_var_with_return_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); return x;", "Make(): T"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_linear, borrowed_var_not_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := source.Read();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 v0.150: `x` is explicitly disposed; the undischarged `y` is auto-dropped
// at scope end with the must-use warning — no error.
BEZEL_TEST(own_linear, multiple_owned_locals_some_discharged) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); var y := new U(); x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "y"));
}

BEZEL_TEST(own_linear, both_using_vars_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var a := new T(); using var b := new U();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_linear, null_initializer_is_polynull_no_obligation) {
    // #1084: a bare `var x := null` (typeless) is now a §7.1 reject (a null
    // literal carries no type to infer); the PolyNull ownership state it tests is
    // preserved with a nullable annotation (`var x: T? := null`).
    auto r = parseAndCheckOwnership(wrap(
        "var x: T? := null;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
