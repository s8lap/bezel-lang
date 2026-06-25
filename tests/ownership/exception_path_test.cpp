// Spec-derived tests for Ownership slice O11 — exception-path
// implicit auto-dispose (§28.3 "Exception-path discharge").
//
// On exception unwinding the compiler auto-disposes still-Owned
// locals via the implicit try/finally. So a value live-and-owned on an
// exception edge doesn't need explicit discharge — the auto-dispose
// covers it. O11-real consults the real CFG exception edges (a `throw`,
// or a throwable call inside a try body), so the suppression is
// path-precise rather than the O11-simple "lexically-last throw"
// approximation. Richer try/catch/finally cases live in
// exception_discharge_test.cpp.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class T end T; "
        "class Error end Error; "
        "class C public method M() ") + body + " end M; end C;";
}

} // namespace

BEZEL_TEST(own_exc, body_ending_in_throw_skips_undischarged_check) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); throw new Error();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 v0.150 (#1008 Part B): a body that ends normally with an undischarged
// plain-`var` owned local auto-drops it at scope end (must-use warning), no error.
BEZEL_TEST(own_exc, body_ending_normally_still_diagnoses) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

BEZEL_TEST(own_exc, throw_in_middle_auto_disposes_x_on_only_reachable_path) {
    // O11-real (§28.3 exception-path discharge): the throw is the only
    // reachable exit path. On that path `x` is live-and-owned, so the
    // implicit try/finally disposes it during stack unwinding — no leak.
    // The trailing `var y := 0;` is dead code (unreachable; sema diagnoses
    // dead code, not ownership) and `y` is an int with no owned obligation.
    // So §28.3 reports NO ownership error.
    //
    // OUTCOME CHANGE vs O11-simple: under the superseded approximation this
    // case was expected to ERROR, because that model only suppressed the
    // leak when a throw was the *lexically-last* statement. That was an
    // artifact of the approximation, not the spec; O11-real consults the
    // real CFG exception edges and gets the path-precise answer. (Reported
    // in the O11-real change for adjudication.)
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); throw new Error(); var y := 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_exc, throw_after_dispose_is_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x.Dispose(); throw new Error();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
