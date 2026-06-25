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
    auto r = parseAndCheckOwnership(wrap(
        "var x := null;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
