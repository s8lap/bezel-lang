// Spec-derived tests for the maybe-consumed branch-join lattice state
// (issue #1626, §28.3 v0.180).
//
// §28.3 "Local variables, ownership, and borrows" — branch joins:
//
//   "A join of owned with moved-from (or explicitly disposed) yields
//    maybe-consumed (v0.180): the variable may not be read, borrowed from,
//    or consumed after the join, and may not flow around a loop back edge
//    into any use; scope-end auto-drop applies per-path (disposal runs
//    only on the still-owned path)."
//
// The bug (issue #1626): the ownership meet resolved an Owned ⊓ MovedFrom
// join to a plain, unrestricted Owned state — so a post-join
// read/borrow/consume/Dispose compiled clean and DOUBLE-DISPOSED (use-after-
// freed) at runtime on the branch that had transferred ownership out. There
// was no lattice state representing "owned on one branch, moved-from on the
// other -> maybe-consumed -> may not be re-used." The fix adds a `maybeConsumed`
// flag (orthogonal to the retained Owned state, so #338's per-path leak
// obligation is preserved) that the use sites — checkRead (read/borrow),
// markMovedByName (consume/move), markDisposedByName (explicit Dispose) —
// each reject.
//
// Rework (reconciling §28.3 with §28.2 / #601): the marker is set ONLY for
// the Owned ⊓ MOVED-FROM split — the move/`owned`-transfer freed the object
// under its new owner, so any post-join use is a use-after-free. The Owned ⊓
// DISPOSED split is NOT maybe-consumed: an explicit Dispose() is idempotent
// (§28.2 — "a further Dispose() call remains a no-op, not an error"), and the
// local still OWNS the reference on both branches (Disposed is a logical
// state, not a transfer; the object is freed only at the per-path scope-end
// auto-drop). That join resolves to plain Owned, so the post-join Dispose is
// the proper discharge on the owned path and an idempotent no-op on the
// disposed path — exactly #601's spec_28_2_one_branch_no_warn one-branch
// discharge pattern. (The literal §28.3 "(or explicitly disposed)" clause
// collides with §28.2's idempotent-Dispose guarantee there; the marker is the
// move/transfer danger only.) These tests pin the fix: the asymmetric
// Owned/moved-from join is rejected at every use, the Owned/disposed join
// discharges clean (§28.2), the symmetric forms (owned on both branches;
// discharged on both branches; reassign-after-join) stay clean, and the
// undischarged-path leak (#338) still fires when there is no post-join use.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// `cond`/`a`/`b` are bool parameters; `Stream` is a disposable class; `sink`
// accepts an `owned` parameter so a branch can transfer ownership out
// (leaving the local MovedFrom). Mirrors the harness in
// branch_disposal_test.cpp (#338's tests) so the interaction with that fix
// is exercised on the same shapes.
std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class T end T; "
        "class Stream "
        "  public Inner: Stream := null; "
        "  public Other: Stream := null; "
        "end Stream; "
        "class Sink "
        "  public method Take(owned s: Stream) "
        "    using var hold := move s; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public source: Stream := null; "
        "  public method M(cond: bool, a: bool, b: bool, sink: Sink) "
        ) + body + " end M; end C;";
}

// True iff any diagnostic is the §28.3 maybe-consumed rejection. Pins the
// specific fix (the join must produce the maybe-consumed restriction, not a
// generic clash/leak).
bool hasMaybeConsumedError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("may have already been consumed on another branch") !=
            std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// ---- §28.3 (#1626): the asymmetric Owned ⊓ MovedFrom join, then a use ----

// The exact issue #1626 shape: `x` is Owned on entry; the `then` branch
// transfers it out (MovedFrom); the `else` leaves it Owned. After the join
// `x` is maybe-consumed, so an explicit `.Dispose()` — which would double-
// dispose on the consuming branch — is rejected. Pre-fix this compiled clean
// (the double-dispose).
BEZEL_TEST(own_maybe_consumed, dispose_after_owned_moved_join_rejected_1626) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(owned x); else end; "
        "x.Dispose();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMaybeConsumedError(r));
}

// Owned ⊓ Disposed join + post-join Dispose — the §28.2/#601 one-branch
// discharge pattern, which must compile CLEAN (reworked from the original
// #1626, which wrongly rejected it). The maybe-consumed marker applies only
// to the MOVED-FROM split (a transfer freed the object; re-use is a
// use-after-free). The DISPOSED split is different: an explicit Dispose() is
// IDEMPOTENT — §28.2: "a further Dispose() call remains a no-op, not an error,
// in both modes" — and the local still OWNS the reference on both branches
// (Disposed is a logical state, not a transfer; the object is freed only at
// the per-path scope-end auto-drop). So the join resolves to plain Owned with
// NO marker: the post-join `x.Dispose()` is the proper discharge on the
// fall-through (owned) path and an idempotent no-op on the then (disposed)
// path. This is exactly the shape #601's spec_28_2_one_branch_no_warn pins
// (dispose on one branch, the final Dispose is the proper discharge); the two
// sections coexist by scoping maybe-consumed to the move/transfer danger.
BEZEL_TEST(own_maybe_consumed, dispose_after_owned_disposed_join_clean_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); else end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMaybeConsumedError(r));
}

// A READ of a maybe-consumed local is forbidden (on the consuming branch the
// value is already gone). `_ := x` is a discard read of `x`.
BEZEL_TEST(own_maybe_consumed, read_after_owned_moved_join_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(owned x); else end; "
        "_ := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMaybeConsumedError(r));
}

// A BORROW (unmarked alias binding `var y := x`) of a maybe-consumed local is
// forbidden — it reads `x`.
BEZEL_TEST(own_maybe_consumed, borrow_after_owned_moved_join_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(owned x); else end; "
        "var y := x; "
        "y.Dispose();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMaybeConsumedError(r));
}

// A CONSUME (a second `owned`-argument transfer) of a maybe-consumed local is
// forbidden — on the consuming branch the value was already moved.
BEZEL_TEST(own_maybe_consumed, consume_after_owned_moved_join_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(owned x); else end; "
        "sink.Take(owned x);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMaybeConsumedError(r));
}

// ---- §28.3: the symmetric forms stay clean (no over-rejection) -----------

// Owned on BOTH branches + Dispose after the merge: the join is Owned ⊓ Owned
// = Owned (no maybe-consumed marker), and the Dispose satisfies the
// obligation. Pattern 2 of §28.3.
BEZEL_TEST(own_maybe_consumed, owned_on_both_branches_dispose_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := new Stream(); else x := new Stream(); end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Disposed on BOTH branches: Disposed ⊓ Disposed = Disposed (same-state short
// circuit). No maybe-consumed marker, no double-dispose error.
BEZEL_TEST(own_maybe_consumed, disposed_on_both_branches_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); else x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Reassigning the local to a fresh owned value after the mixed join clears
// the maybe-consumed marker: the local then holds a definite value and may be
// discharged. (The reassignment is inside the consuming branch, so both
// branches reach the merge Owned.)
BEZEL_TEST(own_maybe_consumed, reassign_after_join_clears_marker_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(owned x); x := new Stream(); else end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.6 (#338): no post-join use -> the per-path leak still fires ------

// A maybe-consumed local with NO post-join use must still surface the
// undischarged-path leak (§28.6): on the branch that never discharged it, the
// value reaches scope end Owned. The maybe-consumed marker must NOT suppress
// that leak diagnostic (it restricts re-use, not the per-path obligation).
// This is the #338 soundness fix, preserved by keeping the Owned state.
BEZEL_TEST(own_maybe_consumed, no_post_join_use_still_leaks_338) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(owned x); else end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The leak diagnostic, NOT a maybe-consumed rejection (there is no use).
    BEZEL_EXPECT_FALSE(hasMaybeConsumedError(r));
}
