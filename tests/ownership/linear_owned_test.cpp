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
//
// §28.9 (v0.237, #2654): `T` and `U` declare a DESTRUCTOR so that they are
// NON-trivial-dispose. This file's subject is the §28.3 linear discharge
// obligation, not §28.9's trivial-dispose classification: a trivial-dispose
// class is exempt from the must-use auto-drop warning ("A `new` expression of
// a trivial-dispose class type … does *not* produce the standard ownership-
// discharge warning"), so with the previous bare `class T end T;` every
// must-use assertion below would silently test the exemption instead of the
// discharge analysis it was written for. The destructors keep the obligation
// real and these fixtures on their intended subject.
//
// §28.3 announcement table row 4 (v0.307): `Source.Read()` must classify as
// a BORROW at the caller (the `borrowed_var_not_diagnosed` test's own
// subject) — but `Read(): T => new T();`, a plain (non-`owned`) instance
// return handing back a FRESH production, is itself now correctly rejected
// at its OWN declaration by that same rule (the fresh-owned-value-through-a-
// non-owned-instance-return check this file's preamble had silently violated
// since it was written — nothing tested `Source` itself, so the violation
// was invisible until that check existed). Give `Source` a genuine
// receiver-sourced borrow instead — an owned field read back unmarked — so
// `Read()` stays legally Borrowed-classified at every call site exactly as
// this file's tests need, without tripping the declaration-site rule.
// // per §28.3, v0.307
std::string wrap(const std::string& body, const std::string& sig = "M()") {
    return std::string("module M; "
        "class T destructor end destructor; end T; "
        "class U destructor end destructor; end U; "
        "class Source "
        "  public owned _t: T; "
        "  public constructor() _t := new T(); end constructor; "
        "  public method Read(): T => _t; "
        "end Source; "
        "class C(source: Source) "
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

// §28.7 drop-on-overwrite (v0.161) composed with §28.3 affine auto-drop (#1465):
// a REASSIGNED plain owned class local is now ACCEPTED, not rejected. Codegen
// disposes the displaced first value at the overwrite and the final value auto-
// drops at scope end (each exactly once), so only the must-use WARNING fires.
// Historically this was rejected — auto-drop disposes only the final slot value,
// and before drop-on-overwrite landed the first value would have leaked — the
// pre-v0.161 conservatism the consume-site-freeze split (#1537) removed. The
// BORROWED reassignment case is instead rejected by the consume-site freeze
// (§28.3, see consume_site_freeze_1537_test).
BEZEL_TEST(own_linear, reassigned_owned_local_auto_dropped_1465) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x := new T();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
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

// §28.3 announcement table row 4 (v0.307): a plain (non-`owned`) instance
// return admits only a receiver-sourced borrow, so `return x;` of a FRESH
// owned local through `Make(): T` is itself now rejected at the declaration
// (the exact s28_3_0029 corpus shape). This test's actual subject —
// returning a plain owned local discharges it, no auto-drop warning needed
// — needs an `owned` return type to admit the transfer in the first place;
// `owned` is the announcement `move x`'s sibling test just above this one
// implicitly relies on too. // per §28.3, v0.307
BEZEL_TEST(own_linear, plain_var_with_return_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); return x;", "Make(): owned T"));
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
