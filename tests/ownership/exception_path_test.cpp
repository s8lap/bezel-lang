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

// §28.9 (v0.237, #2654): `T` declares a DESTRUCTOR so it is NON-trivial-
// dispose. This file's subject is §28.3 discharge on EXCEPTION/throw paths. A
// trivial-dispose class is exempt from the must-use auto-drop warning ("A
// `new` expression of a trivial-dispose class type … does *not* produce the
// standard ownership-discharge warning"), so with a bare `class T end T;` the
// must-use assertions here would test that exemption instead of the
// throw-path analysis, and the ABSENCE assertions would pass vacuously.
std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class T destructor end destructor; end T; "
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

// §28.3 (#1257): an owned local in a NESTED do-block that exits normally, then a
// later `throw`, must be flagged for scope-end auto-drop. It never reaches the
// function-exit state (its inner scope ended; the sole exit is the throw), so the
// function-exit auto-drop loop misses it and codegen's fallback unwind-only
// disposal cannot fire (the inner scope was popped before the throw unwinds) — a
// leak. The never-discharged plain owner must still be flagged (via
// everOwnedPlainLocals_) so its disposal fires at the inner-block scope end. We
// assert the auto-drop warning names `r` and no error is reported. (The runtime
// exactly-once disposal is verified AOT + ASan; the codegen_support runJit harness
// omits checkOwnership, so this ownership-pass flag is asserted at the sema level.)
BEZEL_TEST(own_exc, nested_block_owned_local_before_throw_auto_dropped_1257) {
    auto r = parseAndCheckOwnership(
        // §28.9 (#2654): `R` is destructor-bearing (NON-trivial-dispose) so
        // the auto-drop obligation asserted below is real, not an artifact of
        // which code path happens to reach the §28.9 exemption.
        "module M; class E end E; "
        "class R destructor end destructor; end R; "
        "class C public static method Main(): int "
        "  try "
        "    do var r := new R(); end; "
        "    throw new E(); "
        "  catch E => return 0; end; "
        "  return -1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "r"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 (#1257) guard: a nested owned local that IS moved out (discharged on a
// path, everDischarged > 0) is NOT auto-dropped — the move transfers ownership and
// auto-disposing would double-free. No auto-drop warning for `r` here.
BEZEL_TEST(own_exc, nested_block_moved_local_not_auto_dropped_1257) {
    auto r = parseAndCheckOwnership(
        // §28.9 (#2654): `R` MUST be destructor-bearing here. This fixture
        // asserts the ABSENCE of the auto-drop warning to prove the MOVE
        // discharged `r` (auto-dropping a moved-from local would double-free).
        // With a trivial-dispose `R` the §28.9 exemption would suppress the
        // warning regardless, so the assertion would pass even if the
        // move-discharge logic regressed — a vacuous test.
        "module M; class R destructor end destructor; end R; "
        "class C "
        "  public static method Take(owned x: R): int return 0; end Take; "
        "  public static method Main(): int "
        "    do var r := new R(); var _ := Take(move r); end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "r"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
