// Spec-derived tests for §28.2 v0.131: compile-time warning for use of a
// local variable after an explicit Dispose() call that dominates the use site
// on every CFG path — the static companion to the runtime checked-disposal
// mode (issue #601).
//
// §28.2 v0.131 (exact text):
//   "Implementations should additionally emit a compile-time warning when a
//    local variable is used after an explicit Dispose() call on the same
//    variable that dominates the use site on every path — the
//    statically-decidable subset of use-after-dispose."
//
// Covered cases (each test cites the spec sentence and the specific
// CFG-dominance property it exercises):
//
//   1. Straight-line use-after-dispose → WARNS.
//   2. Member-call use-after-dispose → WARNS.
//   3. Dispose in one if-branch only → does NOT warn (no domination).
//   4. Dispose in both branches then use → WARNS (join point is dominated).
//   5. Use before dispose → does NOT warn (correct program order).
//   6. Reassignment after dispose then use → does NOT warn (reinitialises).
//   7. `using var` implicit end-of-scope disposal → does NOT trigger it
//      (explicit Dispose() only per §28.2 wording).
//   8. Dispose inside loop body + use after loop → does NOT warn (the loop
//      may execute zero times, so the Dispose does not dominate the post-loop
//      use on the zero-trip path).
//   9. Dispose and use entirely within the loop body (straight-line each
//      iteration) → WARNS each iteration.
//  10. Double Dispose (second call after first) → first Dispose → second
//      is a separate ownership ERROR (double-dispose), not a use-after-dispose
//      warning; no confusion between the two diagnostics.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Standard class scaffold.  T is a non-trivial-dispose class (it IS a class,
// so it has a compiler-generated Dispose(); method calls are valid).
// `cond`, `a`, `b` are bool parameters for branching tests.
// `source` is a field of type T so we can produce borrows.
std::string wrap(const std::string& body, const std::string& sig = "M()") {
    return std::string("module M; "
        "class T end T; "
        "class C "
        "  public source: T := null; "
        "  public method ") + sig + " " + body + " end; "
        "end C;";
}

std::string wrapBranch(const std::string& body) {
    return std::string("module M; "
        "class T end T; "
        "class C "
        "  public method M(cond: bool, a: bool, b: bool) ")
        + body + " end; "
        "end C;";
}

// True iff any diagnostic in `r` is the §28.2 use-after-dispose warning.
bool hasUseAfterDisposeWarning(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("after a dominating explicit Dispose") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace


// ── Case 1: straight-line use-after-dispose (§28.2 "dominates on every
//    path") ──────────────────────────────────────────────────────────────────

// §28.2 v0.131: explicit Dispose() immediately before a read on the same
// straight-line path dominates the use → WARN.
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_straight_line_warns) {
    // var x := new T(); x.Dispose(); _ := x;
    // The Dispose() at the single predecessor block dominates the read.
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x.Dispose(); _ := x;"));
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ── Case 2: member-call use-after-dispose (§28.2 "used ... member access,
//    method call, property/indexer accessor, argument-pass, borrow source")
// ─────────────────────────────────────────────────────────────────────────────

// §28.2 v0.131: calling an instance method on x after disposing it is a
// use-after-dispose; the receiver x is read as part of the call site.
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_method_call_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x.Dispose(); _ := x.ToString();"));
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ── Case 3: dispose in only one if-branch → no domination, no warning ──────

// §28.2 v0.131: the warning fires only when Dispose() dominates the use on
// EVERY path.  If x is disposed only on the `then` branch, the `else`/
// fall-through path leaves x live at the join — the Dispose does NOT dominate
// the post-if use site.  No warning.
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_one_branch_no_warn) {
    // x.Dispose() only in the then branch; use after the if. The
    // ownership meet of Disposed (then) ⊓ Owned (fall-through) is Owned
    // (the discharge obligation is retained on the fall-through edge per
    // §28.6), so the state at the use site is not Disposed → no warning.
    auto r = parseAndCheckOwnership(wrapBranch(
        "var x := new T(); "
        "if cond then x.Dispose(); end; "
        "x.Dispose();"));  // final Dispose is the proper discharge
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ── Case 4: dispose in BOTH branches then use → dominates, WARN ──────────────

// §28.2 v0.131: if x is disposed in both the `then` and `else` branches,
// the state at the join is Disposed (Disposed ⊓ Disposed = Disposed per
// §28.3 branch join), so every path into the post-if use site passed through
// a Dispose() — the Dispose dominates, and we warn.
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_both_branches_warns) {
    auto r = parseAndCheckOwnership(wrapBranch(
        "var x := new T(); "
        "if cond then x.Dispose(); else x.Dispose(); end; "
        "_ := x;"));  // x is Disposed on every path to here → warn
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ── Case 5: use before dispose → no warning (correct program order) ──────────

// §28.2 v0.131: the warning fires only when the Dispose() DOMINATES the use.
// A use before the Dispose() is a correct use; the Dispose() does not yet
// dominate any predecessor of the use site.
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_use_before_dispose_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); _ := x.ToString(); x.Dispose();"));
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ── Case 6: reassignment after dispose then use → no warning ─────────────────

// §28.2 v0.131: the spec addresses "an explicit Dispose() call on the same
// variable."  A reassignment `x := new T();` after the Dispose() re-initialises
// x — it is a fresh owned value at that point, not the disposed one.  A
// subsequent use of x is a use of the NEW value, which is live and owned,
// so the Dispose() that was applied to the OLD value does NOT dominate the
// use in any meaningful sense.  The ownership state after reassignment is
// Owned, not Disposed, so no warning fires.
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_reassign_after_dispose_no_warn) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x.Dispose(); x := new T(); _ := x.ToString(); x.Dispose();"));
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ── Case 7: using-var implicit end-of-scope disposal → no warning ────────────

// §28.2 v0.131: the warning is specifically for an explicit `x.Dispose()`
// call. A `using var x` disposes x implicitly at scope end — there is no
// explicit call site and x goes out of scope at the same time, so there is
// no valid use-site after the implicit disposal.  Verify that a well-formed
// `using var` scope does not trigger the warning (the implicit disposal is
// not an explicit Dispose() call; the variable ceases to be accessible at
// the scope boundary anyway).
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_using_var_implicit_no_warn) {
    auto r = parseAndCheckOwnership(wrap(
        "using var x := new T(); _ := x.ToString();"));
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ── Case 8: dispose inside loop body + use AFTER loop → no warning ───────────

// §28.2 v0.131: the warning requires that the Dispose() dominates the use
// on EVERY path.  A Dispose() inside a loop body is only reachable on paths
// that execute the loop at least once; on the zero-trip path the loop body
// never runs and the local is still in its pre-loop state (Owned).  Therefore
// the loop-body Dispose() does NOT dominate a post-loop use — no warning.
//
// Shape: `var x := new T(); while cond do x.Dispose(); x := new T(); end;
//         x.Dispose();`
// The Dispose() inside the body is followed by a reassignment, so the local
// is freshly Owned on every iteration's next use. The final post-loop
// x.Dispose() is the correct discharge on all paths (including zero-trip).
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_dispose_in_loop_no_warn_post_loop) {
    auto r = parseAndCheckOwnership(wrapBranch(
        "var x := new T(); "
        "while cond do "
        "  x.Dispose(); "         // dispose previous value each iteration
        "  x := new T(); "        // reinitialise; Owned again
        "end; "
        "x.Dispose();"));         // final discharge (safe on all paths)
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ── Case 9: dispose + use WITHIN loop body (straight-line per iteration) ─────

// §28.2 v0.131: within a single loop iteration, if x is disposed and then
// used in the same straight-line body segment, the Dispose() dominates the
// in-body use.  The warning fires inside the loop body (same as the
// straight-line case 1).
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_dispose_then_use_in_loop_body_warns) {
    // Dispose x inside the loop, then read it in the same iteration. The
    // Dispose() on the straight-line path inside the body dominates the read.
    auto r = parseAndCheckOwnership(wrapBranch(
        "while cond do "
        "  var x := new T(); "
        "  x.Dispose(); "
        "  _ := x.ToString(); "   // use after dispose inside the same iteration
        "end;"));
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ── Case 10: double-Dispose is a WARNING, not an error (§28.2) ───────────────

// §28.2: `Dispose()` is idempotent — "a further Dispose() call remains a no-op,
// not an error, in both modes." A statically-provable second Dispose() on a
// known-disposed local is therefore a compile-time WARNING (the use-after-
// dispose subset §28.2 prescribes a warning for), NOT a hard error: rejecting
// it would refuse the valid idempotent pattern the language guarantees. (Owner
// decision 2026-06-15; this was previously a hard error.)
BEZEL_TEST(own_use_after_dispose_warn_601, spec_28_2_double_dispose_is_warning_not_error) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); x.Dispose(); x.Dispose();"));
    // The program is well-formed — idempotent disposal is guaranteed (§28.2).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The redundant second Dispose() is flagged with a warning.
    bool warned = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("disposed again") != std::string::npos)
            warned = true;
    BEZEL_EXPECT_TRUE(warned);
}
