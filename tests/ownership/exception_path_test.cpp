// Spec-derived tests for Ownership slice O11 — exception-path
// implicit auto-dispose (§28.3 "Exception-path discharge").
//
// On exception unwinding the compiler auto-disposes still-Owned
// locals via the implicit try/finally. So a body that unconditionally
// throws doesn't need explicit discharge — the auto-dispose covers
// it. The slice-O11 (simple) approximation: if the body's lexically-
// last statement is a ThrowStmt, suppress the undischarged-owned-
// local diagnostic.

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

BEZEL_TEST(own_exc, body_ending_normally_still_diagnoses) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_exc, throw_in_middle_does_not_suppress_later_check) {
    // The slice-O11 simple model only honors a TRAILING throw. A throw
    // in the middle of the body, with normal-exit code afterwards,
    // doesn't suppress the check.
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); throw new Error(); var y := 0;"));
    // The trailing stmt is a LocalDecl (not throw), so the
    // undischarged-x check fires.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_exc, throw_after_dispose_is_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x.Dispose(); throw new Error();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
