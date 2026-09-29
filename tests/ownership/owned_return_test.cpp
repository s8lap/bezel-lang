// Spec-derived tests for Ownership slice O13 — method calls returning
// an `owned T` value (§22.3, §28.3).
//
// A method declared `method M(): owned T` returns ownership of the
// produced value; the caller incurs the same discharge obligation as
// for `new T()`. A method declared `method M(): T` (without owned)
// returns a borrowed reference and carries no obligation.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string("module M; ") + body;
}

} // namespace

BEZEL_TEST(own_return, owned_return_into_plain_var_undischarged_diagnoses) {
    auto r = parseAndCheckOwnership(mod(
        "class T end T; "
        "class C "
        "  public method Make(): owned T => new T(); "
        "  public method M() "
        "    var x := this.Make(); "
        "  end M; "
        "end C;"));
    // §28.3 v0.150 (#1008 Part B): the owned-returned value bound to a plain
    // `var` and left undischarged auto-drops at scope end (must-use warning).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

BEZEL_TEST(own_return, owned_return_into_using_var_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class T end T; "
        "class C "
        "  public method Make(): owned T => new T(); "
        "  public method M() "
        "    using var x := this.Make(); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 announcement table row 4 (v0.307): `Get()`'s own declaration must
// itself be legal — a plain (non-`owned`) instance return admits ONLY a
// receiver-sourced borrow, so `Get(): T => new T();` (a fresh production) is
// rejected at its OWN site since that rule was added. This test's subject is
// the CALLER-side classification (a call through a plain instance return is
// a borrow, no discharge obligation) — give `Get()` a genuine receiver-
// sourced borrow (a bare owned-field read) so it stays legally
// Borrowed-classified at the call site without tripping the declaration-site
// rule. // per §28.3, v0.307
BEZEL_TEST(own_return, borrowed_return_into_plain_var_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class T end T; "
        "class C "
        "  public owned _t: T; "
        "  public constructor() _t := new T(); end constructor; "
        "  public method Get(): T => _t; "
        "  public method M() "
        "    var x := this.Get(); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_return, owned_return_dispatched_on_param) {
    auto r = parseAndCheckOwnership(mod(
        "class T end T; "
        "class Source public method Read(): owned T => new T(); end Source; "
        "class C "
        "  public method M(src: Source) "
        "    var x := src.Read(); "
        "  end M; "
        "end C;"));
    // §28.3 v0.150: owned-returned value into a plain `var`, undischarged →
    // auto-drop at scope end (must-use warning), no error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

BEZEL_TEST(own_return, owned_return_explicit_dispose_clean) {
    // §28.2: Dispose() is compiler-synthesised; no user declaration needed.
    // The ownership checker must accept an explicit `x.Dispose()` call on an
    // owned local as a valid discharge (§28.3).
    auto r = parseAndCheckOwnership(mod(
        "class T end T; "
        "class C "
        "  public method Make(): owned T => new T(); "
        "  public method M() "
        "    var x := this.Make(); "
        "    x.Dispose(); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
