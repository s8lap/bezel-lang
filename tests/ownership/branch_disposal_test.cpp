// Spec-derived tests for the branch-join SOUNDNESS fix (issue #338) —
// conditional disposal on an if-branch (§28.3 branch joins; §28.6 leak
// prevention).
//
// §28.3 "Owned-local discharge requirement", verbatim: "An owned local must
// reach an owned slot before it goes out of scope, or be explicitly
// disposed. ... The compiler errors when an owned local is dropped from a
// *normal-exit* control-flow path without reaching one of the above."
//
// §28.3 branch joins: "owned ⊓ owned = owned (with the discharge obligation
// carried forward)." The discharge obligation is therefore PER-PATH: a value
// disposed on only SOME incoming edges is still Owned on the others, so at
// scope end on those edges it leaks (§28.6).
//
// The bug (issue #338): the ownership meet treated Owned ⊓ Disposed as
// Poisoned, which the rest of the checker read as "no remaining obligation."
// A value disposed on only the `then` branch was wrongly accepted even
// though the `else`/fall-through path leaks. These tests pin the corrected
// behaviour: the obligation survives on the undischarged path and is
// diagnosed, while the dispose-on-all-paths case stays clean.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// `cond` is a bool parameter; `Stream` is a disposable class; `source` is a
// field that yields borrows for the borrow-branch cases. `Sink.Take` accepts
// an `owned` parameter so a branch can transfer ownership out (MovedFrom).
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

} // namespace

// ---- §28.6: dispose on the THEN branch only → leak on the else path -------

// The exact issue #338 program. `x` is Owned on entry to the `if`; the `then`
// branch disposes it; the implicit `else`/fall-through edge leaves it Owned.
// At the merge the value is Disposed on one edge and Owned on the other, so
// the obligation survives on the undischarged path and the leak is diagnosed.
BEZEL_TEST(own_branch_disposal, dispose_then_only_leaks_else_path_28_6) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Symmetric with an explicit (empty) else: dispose only in `else`, fall
// through Owned on the `then` edge.
BEZEL_TEST(own_branch_disposal, dispose_else_only_leaks_then_path_28_6) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then else x.Dispose(); end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- §28.3: dispose on BOTH branches → clean (no leak, no double-dispose) --

// Both branches discharge `x`; the merge is Disposed ⊓ Disposed = Disposed.
// No obligation survives → no leak, and the meet must NOT synthesize a false
// double-dispose error for the dispose-on-all-paths case.
BEZEL_TEST(own_branch_disposal, dispose_in_both_branches_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); else x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Dispose in an if/elif/else, all three arms discharge → clean.
BEZEL_TEST(own_branch_disposal, dispose_in_all_three_arms_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if a then x.Dispose(); "
        "elif b then x.Dispose(); "
        "else x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// One of three arms omits the dispose → leak on that arm.
BEZEL_TEST(own_branch_disposal, dispose_two_of_three_arms_leaks_28_6) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if a then x.Dispose(); "
        "elif b then x.Dispose(); "
        "else end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- §28.3: dispose on then, discharge differently on else → clean --------

// `then` disposes; `else` transfers ownership out via an owned argument
// (MovedFrom). Both edges discharge `x` by a recognised means, so no
// obligation survives the merge → clean. This exercises Disposed ⊓ MovedFrom
// (both no-obligation), which must remain clean.
BEZEL_TEST(own_branch_disposal, dispose_then_move_else_both_discharge_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); else sink.Take(owned x); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `then` moves the owned value out (MovedFrom); `else` does nothing, so the
// value is still Owned on the else edge → leak. This is the MovedFrom ⊓ Owned
// form of the soundness fix (a move on one branch only).
BEZEL_TEST(own_branch_disposal, move_then_only_leaks_else_path_28_6) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(owned x); end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- §28.3: dispose then reassign — the value after the merge is owned ----

// `then` disposes `x` then binds it to a fresh owned value; the `else` edge
// leaves the original Owned. Both edges reach the merge Owned, and nothing
// discharges after the merge → leak (Owned ⊓ Owned = Owned with the
// obligation carried forward, per §28.3).
BEZEL_TEST(own_branch_disposal, dispose_then_reassign_else_owned_leaks_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); x := new Stream(); end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Same shape, but discharged after the merge on the (now uniformly Owned)
// value → clean.
BEZEL_TEST(own_branch_disposal, dispose_then_reassign_discharged_after_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); x := new Stream(); end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.10 + §28.3: using var in one branch ------------------------------

// A `using var` acquired and discharged WITHIN the `then` branch carries its
// own scope-bound discharge; the outer `x` is untouched by the branch and is
// discharged after the merge → clean. (The branch's using-var does not affect
// `x`'s join state.)
BEZEL_TEST(own_branch_disposal, using_var_in_then_branch_outer_discharged_clean_28_10) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then using var scratch := new Stream(); end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- §28.6: the IN-LOOP conditional form ----------------------------------

// An owned local acquired in a loop body, disposed only on the `then` branch
// inside the body: the per-iteration normal-exit (fall-through) edge leaves it
// Owned on the else path → leak. The fixpoint preserves the obligation across
// the back-edge / zero-trip-skip merge.
BEZEL_TEST(own_branch_disposal, in_loop_dispose_then_only_leaks_28_6) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  var x := new Stream(); "
        "  if a then x.Dispose(); end; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// In-loop conditional disposal on BOTH branches → clean each iteration.
BEZEL_TEST(own_branch_disposal, in_loop_dispose_both_branches_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  var x := new Stream(); "
        "  if a then x.Dispose(); else x.Dispose(); end; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
