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
// Rework (v0.180 era): the `maybeConsumed` marker is set ONLY for the
// Owned ⊓ MOVED-FROM split — the move/`owned`-transfer freed the object under
// its new owner, so any post-join use is a use-after-free. The Owned ⊓ DISPOSED
// split is tracked by a separate `disposedOnSomePath` flag and joins to plain
// Owned, because an explicit Dispose() nulls the slot, which makes the per-path
// scope-end auto-drop null-safe.
//
// ── UPDATED 2026-07-29 for issue #2270 ─────────────────────────────────────
//
// That rework also concluded that a post-join USE of the DISPOSED split should
// be ACCEPTED, on the reasoning that §28.2's idempotent-Dispose guarantee ("a
// further Dispose() call remains a no-op, not an error") makes the post-join
// `x.Dispose()` harmless. The current spec says the opposite, in two places:
//
//   * §28.2 (v0.228, "Use after disposal"), immediately after the mandatory
//     straight-line warning: "A *conditional* kill is treated more strictly: at
//     a join of an explicitly-disposed path with a still-owned path the
//     variable is **maybe-consumed** and a later use is a compile error
//     (§28.3)." The idempotent-Dispose sentence it was weighed against is about
//     the RUNTIME checked/unchecked modes, not the compile-time join rule.
//   * K4.2 (the kernel, v0.228 ~line 6241): "A *conditional* kill of either
//     kind joins to maybe-consumed (K7) and later use is an error." "Either
//     kind" is exactly the disposing/transferring split — the disposed side is
//     named, not exempted.
//   * K7.4 itself: the variable "may not be read, borrowed from, or consumed
//     after the join". `x.Dispose()` reads the receiver and is a K4.2 announced
//     kill — a consume — so it is doubly covered.
//
// So `dispose_after_owned_disposed_join_*` below is flipped: the post-join
// Dispose is a compile ERROR, carrying the conditional-discharge diagnostic.
// What is accepted is the same join with NO post-join use — "scope-end disposal
// per-path" — and those cells live in branch_disposal_test.cpp and
// conditional_discharge_1883_test.cpp.
//
// These tests pin: the asymmetric Owned/moved-from join is rejected at every
// use, the Owned/disposed join is rejected at a post-join use, the symmetric
// forms (owned on both branches; discharged on both branches;
// reassign-after-join) stay clean, and the moved-from split with no post-join
// use is still hard-rejected (#338/#1626).

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
        "  public Inner: Stream? := null; "
        "  public Other: Stream? := null; "
        "end Stream; "
        "class Sink "
        "  public method Take(owned s: Stream) "
        "    using var hold := move s; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public source: Stream? := null; "
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

// True iff any diagnostic is the §28.2 / K7.4 conditional-discharge rejection —
// the DISPOSED-split sibling of hasMaybeConsumedError above (the checker keeps
// the two messages distinct because the hazard differs: use-after-free for the
// moved-from split, use-after-dispose for the disposed split).
bool hasConditionalDischargeError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("may have already been disposed on another branch") !=
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

// Owned ⊓ Disposed join + post-join Dispose. §28.2 (v0.228): "A *conditional*
// kill is treated more strictly: at a join of an explicitly-disposed path with
// a still-owned path the variable is maybe-consumed and a later use is a
// compile error (§28.3)" — and K7.4 lists the forbidden post-join operations as
// "read, borrowed from, or consumed", all three of which `x.Dispose()` is (it
// reads the receiver and is a K4.2 announced kill). REJECTED, with the
// conditional-discharge diagnostic rather than the moved-from one: the checker
// distinguishes the disposed split (use-after-dispose) from the moved-from
// split (use-after-free).
//
// (#2270 / d2ecf82: this cell asserted the program was CLEAN, on the v0.180-era
// reading that §28.2's idempotent-Dispose guarantee exempted the disposed
// split. That guarantee is about the runtime checked/unchecked modes; the
// compile-time join rule is the sentence quoted above. The accepted shape is
// the same join with NO post-join use — branch_disposal_test.cpp.)
BEZEL_TEST(own_maybe_consumed, dispose_after_owned_disposed_join_rejected_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); else end; "
        "x.Dispose();"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConditionalDischargeError(r));
    // The disposed split, not the moved-from split.
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
        "if cond then sink.Take(move x); x := new Stream(); else end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.6 (#338): no post-join use, MOVED-FROM split -> still rejected ---

// A moved-from-split maybe-consumed local with NO post-join use is still hard-
// rejected: the moved-out plain `var` slot is not nulled, so the per-path
// scope-end auto-drop that the DISPOSED split gets (K7.4 "scope-end disposal
// per-path", exercised in branch_disposal_test.cpp) cannot be applied here
// without risking a double free. This is deliberately more conservative than
// K7.4's literal text, which names moved-from and explicitly-disposed
// symmetrically; see #2270.
BEZEL_TEST(own_maybe_consumed, no_post_join_use_still_leaks_338) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(owned x); else end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The leak diagnostic, NOT a maybe-consumed rejection (there is no use).
    BEZEL_EXPECT_FALSE(hasMaybeConsumedError(r));
}
