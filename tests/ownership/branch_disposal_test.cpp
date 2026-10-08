// Spec-derived tests for the branch-join SOUNDNESS fix (issue #338) —
// conditional disposal on an if-branch (§28.3 branch joins / K7; §28.6 leak
// prevention).
//
// The original bug (issue #338): the ownership meet treated Owned ⊓ Disposed
// as Poisoned, which the rest of the checker read as "no remaining
// obligation." A value disposed on only the `then` branch was accepted with
// the `else`/fall-through path silently dropping the obligation entirely —
// neither diagnosed nor disposed.
//
// ── REWRITTEN 2026-07-29 for issue #2270 (behaviour change in d2ecf82,
//    "fix #1900 #1883: owned reassignment restore + per-path conditional-
//    discharge") ───────────────────────────────────────────────────────────
//
// Six cells here asserted a hard ERROR for a one-branch dispose. That encoded
// the pre-auto-drop rule and is stale: two current spec rules stand under
// these shapes (spec v0.228).
//
//   * §28.3 "Auto-drop (affine)" (derives K3.14, v0.228 ~line 6515): "When an
//     owned local — or an `owned` parameter — reaches a *normal-exit* scope
//     end without having reached a discharge point, and is not in moved-from
//     state, it is **disposed implicitly at that scope end**" — with the
//     must-use WARNING of the next paragraph, "a **warning** (not an error)".
//     There is no longer a leak ERROR for a plain owned local left Owned at a
//     normal-exit scope end.
//   * K7.4 (the kernel join rule, v0.228 ~line 6270; restated verbatim at
//     §28.3 ~line 6328): "owned ⊓ moved-from and owned ⊓ explicitly-disposed
//     = **maybe-consumed** — unreadable after the join, no flow around a back
//     edge into a use; **scope-end disposal per-path**." So a value disposed
//     on some incoming edges and still owned on others is accepted: the
//     scope-end drop runs only on the still-owned path (an explicit
//     `Dispose()` nulls the slot, so the null-guarded drop skips the disposed
//     path). Verified at runtime on the shapes below — each disposes exactly
//     once per path, no double-free and no leak.
//
// What K7.4 still forbids is a USE after such a join — "the variable may not
// be read, borrowed from, or consumed after the join" — and §28.2 states it in
// the same words: "A *conditional* kill is treated more strictly: at a join of
// an explicitly-disposed path with a still-owned path the variable is
// **maybe-consumed** and a later use is a compile error." Those cells live in
// maybe_consumed_join_1626_test.cpp and use_after_dispose_warn_601_test.cpp;
// every program in THIS file has no post-join use.
//
// The MOVED-FROM split (Owned ⊓ MovedFrom) is deliberately NOT relaxed by
// d2ecf82 and stays a hard error here (`move_then_only_leaks_else_path_28_6`,
// `move_then_early_return_leaks_fallthrough_747`): a moved-out plain `var`
// slot is not nulled, so a per-path drop there would double-free. That is a
// conservative over-rejection against K7.4's literal text, tracked separately.
//
// Each rewritten cell pins BOTH halves of the current rule: no error, AND the
// §28.3 auto-drop warning is present. The warning is what preserves the #338 /
// #747 regression value — it is emitted only because the checker still sees
// the obligation alive on the undischarged edge. If that state were dropped
// from the merge again (the original defect), there would be no warning and no
// drop, and the value would really leak.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

namespace {

// `cond` is a bool parameter; `Stream` is a disposable class; `source` is a
// field that yields borrows for the borrow-branch cases. `Sink.Take` accepts
// an `owned` parameter so a branch can transfer ownership out (MovedFrom).
// §28.9 (v0.237, #2654): `Stream` declares a DESTRUCTOR, which is what makes
// it "a disposable class" as the comment above has always claimed — i.e. NON-
// trivial-dispose. This file's subject is the K7.4 branch/join disposal rule.
// A trivial-dispose class is exempt from the must-use auto-drop warning ("A
// `new` expression of a trivial-dispose class type … does *not* produce the
// standard ownership-discharge warning"), so a bare `class Stream` with no
// destructor would make every auto-drop assertion below test that exemption
// rather than the per-path join analysis it was written for.
std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class T end T; "
        "class Stream "
        "  destructor end destructor; "
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

} // namespace

// ---- K7.4: dispose on the THEN branch only → per-path auto-drop on else ---

// The exact issue #338 program. `x` is Owned on entry to the `if`; the `then`
// branch disposes it; the implicit `else`/fall-through edge leaves it Owned.
// At the merge the value is explicitly-disposed on one edge and owned on the
// other — K7.4's maybe-consumed join — and there is no post-join use, so
// "scope-end disposal per-path" applies: accepted, with the §28.3 auto-drop
// warning naming `x` (the still-owned path's implicit drop).
// (#2270 / d2ecf82: this cell asserted a hard error before the auto-drop and
// per-path-discharge rules landed.)
BEZEL_TEST(own_branch_disposal, dispose_then_only_auto_drops_else_path_k7_4) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}

// Symmetric with an explicit (empty) else: dispose only in `else`, fall
// through Owned on the `then` edge. Same K7.4 join, same per-path drop.
BEZEL_TEST(own_branch_disposal, dispose_else_only_auto_drops_then_path_k7_4) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then else x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
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

// One of three arms omits the dispose. The three-way merge is
// Disposed ⊓ Disposed ⊓ Owned = maybe-consumed (K7.4), and with no post-join
// use the auto-drop runs on the one still-owned arm → accepted with the
// warning. (#2270 / d2ecf82: previously asserted an error.)
BEZEL_TEST(own_branch_disposal, dispose_two_of_three_arms_auto_drops_third_k7_4) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if a then x.Dispose(); "
        "elif b then x.Dispose(); "
        "else end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}

// ---- §28.3: dispose on then, discharge differently on else → clean --------

// `then` disposes; `else` transfers ownership out via an owned argument
// (MovedFrom). Both edges discharge `x` by a recognised means, so no
// obligation survives the merge → clean. This exercises Disposed ⊓ MovedFrom
// (both no-obligation), which must remain clean.
BEZEL_TEST(own_branch_disposal, dispose_then_move_else_both_discharge_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); else sink.Take(move x); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `then` moves the owned value out (MovedFrom); `else` does nothing, so the
// value is still Owned on the else edge → leak. This is the MovedFrom ⊓ Owned
// form of the soundness fix (a move on one branch only).
BEZEL_TEST(own_branch_disposal, move_then_only_leaks_else_path_28_6) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(move x); end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- §28.3: dispose then reassign — the value after the merge is owned ----

// `then` disposes `x` then binds it to a fresh owned value; the `else` edge
// leaves the original Owned. This is NOT a K7.4 join: the reassignment happens
// BEFORE the merge, so both edges arrive Owned and K7.1 applies (owned ⊓ owned
// = owned, obligation carried forward) — the deciding rule here is §28.3
// auto-drop alone. Nothing discharges after the merge, so the surviving
// obligation is met by the implicit scope-end drop with the must-use warning;
// it is not an error. The `x := new Stream();` on the then edge is an ordinary
// §28.7 drop-on-overwrite onto a just-disposed (nulled) slot — a no-op there,
// not a double free.
// (#2270 / d2ecf82: previously asserted an error, from the pre-auto-drop rule.)
BEZEL_TEST(own_branch_disposal, dispose_then_reassign_else_owned_auto_drops_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); x := new Stream(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
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

// ---- K7.4: the IN-LOOP conditional form -----------------------------------

// An owned local acquired in a loop body, disposed only on the `then` branch
// inside the body. K7.4's back-edge clause ("may not flow around a loop back
// edge into any use") does NOT bite: `x` is declared INSIDE the body, so its
// scope — and its maybe-consumed state — ends at the body's end; the back edge
// carries no `x`, and the next iteration binds a fresh one. What applies is
// K7.4's other half, "scope-end disposal per-path", at the BODY's scope end,
// once per iteration. Accepted with the auto-drop warning; the fixpoint must
// still see the obligation alive on the else path (hence the warning
// assertion). Verified at runtime: a 3-iteration loop disposing on exactly one
// iteration reclaims 3 objects — exactly once each, no double-free, no leak.
// (#2270 / d2ecf82: previously asserted an error.)
BEZEL_TEST(own_branch_disposal, in_loop_dispose_then_only_auto_drops_k7_4) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  var x := new Stream(); "
        "  if a then x.Dispose(); end; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
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

// ---- §28.3 / #747 R42: dispose + EARLY RETURN in a branch ----------------
//
// When the `then` branch BOTH disposes `x` AND returns, the then-block flows
// straight to the function exit — it has no fall-through to the merge. So
// there is NO K7.4 join here at all: the merge has exactly one live incoming
// edge, the implicit fall-through, carrying `x = Owned`. The deciding rule is
// therefore plain §28.3 auto-drop — the fall-through path's obligation is met
// by the implicit scope-end drop, with the must-use warning.
//
// The #747 defect this cell pins is unchanged and is exactly what the warning
// assertion tests: the CFG builder's runInto() (used for branch bodies)
// returned a spurious empty dead-code block for the returning branch, which
// visitBranch wired as an extra orphan→merge edge. That orphan carried no
// locals, so the non-union if-merge meet dropped the `x = Owned` that arrived
// on the real fall-through edge. With the state dropped there is no obligation
// left to auto-drop — no warning AND no disposal, a genuine leak — so the
// warning is the live regression signal now that the leak is no longer an
// error. Verified at runtime: with the branch not taken, the fall-through path
// reclaims the object exactly once.
// (#2270 / d2ecf82: previously asserted an error.)
BEZEL_TEST(own_branch_disposal, dispose_then_early_return_fallthrough_auto_drops_747) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); return; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}

// Companion clean case: the `then` branch disposes + returns, and the
// fall-through path ALSO disposes. Every normal-exit edge discharges `x` →
// clean (no leak, no false double-dispose). Pins that the runInto fix does not
// over-reject the dispose-on-all-paths early-return shape.
BEZEL_TEST(own_branch_disposal, dispose_then_early_return_else_dispose_clean_747) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then x.Dispose(); return; end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The `then` branch transfers ownership out (MovedFrom) then returns; the
// fall-through leaves `x` Owned → leak. Exercises the orphan-edge fix for a
// move-discharge (not just Dispose) early-return branch.
BEZEL_TEST(own_branch_disposal, move_then_early_return_leaks_fallthrough_747) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Stream(); "
        "if cond then sink.Take(move x); return; end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
