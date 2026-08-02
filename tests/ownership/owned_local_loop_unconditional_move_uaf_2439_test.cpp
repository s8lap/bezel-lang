// fix #2439 (K7.4 "The join rule" / K1.4 "maybe-consumed", §28.3 "Local
// variables, ownership, and borrows", spec v0.235) — a plain `owned` local
// declared OUTSIDE a loop, moved unconditionally into an owned-sink call
// INSIDE the loop body on every textual pass, was silently ACCEPTED with
// ZERO diagnostics — a confirmed heap-use-after-free at runtime (iteration 2
// re-consumes the value iteration 1 already freed).
//
// K7.4 states plainly: "a join of owned with moved-from ... yields
// maybe-consumed ...: the variable may not be read, borrowed from, or
// consumed after the join, and MAY NOT FLOW AROUND A LOOP BACK EDGE INTO ANY
// USE." This is exactly the shape a loop's back edge produces here: the
// loop header's entry state is the K7 meet of the pre-loop edge (`n`: owned)
// and the body's back edge (`n`: moved-from, from the previous iteration's
// `move`) — an Owned/MovedFrom split, which K7.4 names maybe-consumed. The
// very next iteration's `move n` is exactly the forbidden "flow ... into
// any use."
//
// ROOT CAUSE (src/ownership/Ownership_Checks.cpp, MethodOwnership::
// markMovedByName's `st->maybeConsumed` branch): the O3b loop fixpoint runs
// every transfer function TWICE — once per pass with diagnostics suppressed
// (`suppressDiag_`, see the field's doc comment in Ownership_impl.hpp, "a
// single final pass ... over the STABILIZED entry states emits each
// diagnostic exactly once") and once, unsuppressed, on the final pass. The
// maybeConsumed branch poisoned the record (`st->state =
// OwnershipState::Poisoned`) as an unconditional side effect of reporting —
// including during the SUPPRESSED passes. For a loop-carried local that is
// exactly this scenario, that Poisoned exit state becomes an INPUT to the
// header's entry on the next pass via the back edge; joinInto's Owned/
// Poisoned meet does not match the `ownedDischargedSplit` case (Poisoned is
// neither Disposed nor MovedFrom), so it falls through the generic "other
// mismatches" catch-all and poisons the header wholesale — discarding the
// Owned+maybeConsumed obligation. That Poisoned entry then reaches THIS SAME
// call on the next pass; `markMovedByName` has (by design — see
// OwnershipState::Poisoned's "suppress cascades" contract) no explicit
// Poisoned branch, so it falls through to the plain-move path and silently
// "revives" the poisoned local into a fresh, unpoisoned MovedFrom. This
// oscillates every pass (Owned+maybeConsumed -> Poisoned -> revived
// MovedFrom -> Owned+maybeConsumed -> ...) and never reaches the fixpoint's
// own definition of convergence, so it runs out the `maxPasses` defensive
// backstop and lands on whatever parity that happens to stop at — corrupted
// noise, not the correct stabilized Owned+maybeConsumed value — feeding the
// FINAL (diagnosing) pass's own entry state. Confirmed by instrumenting the
// fixpoint for the issue's own repro: it runs all `maxPasses` iterations
// without ever setting `changed = false`, and the final pass's entry for
// `n` is bare Poisoned, not Owned+maybeConsumed — so the maybeConsumed
// branch this fix targets never gets a chance to fire for real.
//
// FIX: only commit the Poisoned transition when NOT suppressed
// (`if (!suppressDiag_) st->state = OwnershipState::Poisoned;`). During the
// suppressed phase the record is left untouched (still Owned+maybeConsumed),
// so the loop fixpoint's per-pass entry/exit states become IDENTICAL once
// the back-edge fact has propagated once — the fixpoint genuinely converges
// on the correct value — and the final, unsuppressed pass observes that
// stabilized fact fresh and both reports the diagnostic and poisons for
// real, exactly the "poison after diagnosing" pattern every other branch of
// this function already follows, just deferred to the pass that keeps its
// diagnostic. Confirmed via AddressSanitizer (issue #2439's own repro): pre-
// fix this compiled clean and crashed with a heap-use-after-free at
// `bz$M.Sink$Take`, freed by iteration 1's call and read again by iteration
// 2's; post-fix the compile fails outright (see the companion codegen test),
// so the UAF binary can never be produced.
//
// Tests here assert on DIAGNOSTIC TEXT via `parseAndCheckOwnership` (which
// captures every diagnostic including warnings, unlike the AOT harness's
// JitResult on a successful compile) — the coarse "did it reject" check is
// insufficient to distinguish a genuine rejection from an unrelated crash or
// a different diagnostic altogether.
//
// SWEEP (per the lane protocol's "sweep the shape" step): the identical
// unconditional-poison-during-suppression shape was found — and mechanically
// CONFIRMED as an independent silent accept — in checkRead's two sibling
// maybeConsumed-family branches (Ownership_Checks.cpp, the `maybeConsumed &&
// state == Owned` and `disposedOnSomePath && state == Owned` branches): a
// loop-carried maybe-consumed local that is only ever READ (not moved/
// disposed) inside the loop body, and a loop-carried disposed-on-some-path
// local that is read inside the loop body, both compiled clean with zero
// diagnostics even with the markMovedByName fix alone in place. checkRead's
// own `state == Owned` guard makes its failure mode more direct than
// markMovedByName's: the record poisons on the VERY FIRST suppressed-pass
// evaluation (no oscillation needed) and then permanently fails that guard
// on the final pass. Both were fixed with the identical
// `if (!suppressDiag_) st->state = OwnershipState::Poisoned;` gate — see
// `reject_read_of_maybe_consumed_loop_carried_local` and
// `reject_read_of_disposed_on_some_path_loop_carried_local` below.
// markDisposedByName's own maybeConsumed branch has the identical code
// shape (unconditional poison + a Poisoned-reviving fallthrough a few lines
// later) and got the same defensive gate, but no live silent-accept was
// found for it specifically — every shape tried converged correctly before
// the final pass — so it is recorded here as INFERRED-FROM-SHAPE, not
// mechanically reproduced, unlike the three confirmed sites above.
//
// per K7.4, K1.4, §28.3, #2439, #2436, #1626

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContainsWithSeverity(const bezel::test::OwnershipResult& r,
                              bezel::Severity sev, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != sev) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

const char* kSink =
    "class Sink "
    "  public static method Take(owned x: Item) x.Dispose(); end Take; "
    "end Sink; ";

const char* kReader =
    "class Reader "
    "  public static method Peek(x: Item) end Peek; "
    "end Reader; ";

}  // namespace

// ===========================================================================
// REJECT — the issue's OWN repro, `for` form: `n` declared before the loop,
// moved unconditionally into `Sink.Take` on every iteration's textual pass.
// Pre-fix: zero diagnostics, silent accept (confirmed heap-use-after-free
// under ASan — see the companion codegen test). Post-fix: a hard compile
// error naming the maybe-consumed join (K7.4).
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           reject_unconditional_move_in_for_loop_body) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; ") + kSink +
        "class C "
        "  public method M() "
        "    var n := new Item(); "
        "    for var i in 0..3 do "
        "      Sink.Take(move n); "
        "    end for; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "cannot consume `n`: it may have already been consumed on another "
        "branch (maybe-consumed join, §28.3)"));
}

// ===========================================================================
// REJECT — the issue's OWN repro, `while` form. Identical mechanism (the
// loop back edge, not the `for`-specific desugaring) — locks in that the
// fix is not accidentally `for`-shaped only.
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           reject_unconditional_move_in_while_loop_body) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; ") + kSink +
        "class C "
        "  public method M() "
        "    var n := new Item(); "
        "    var i: int := 0; "
        "    while i < 3 do "
        "      Sink.Take(move n); "
        "      i := i + 1; "
        "    end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "cannot consume `n`: it may have already been consumed on another "
        "branch (maybe-consumed join, §28.3)"));
}

// ===========================================================================
// REJECT — nested loops: the recipient's declaring back edge is reached via
// the INNER loop, stressing the same convergence mechanism one level deeper
// (mirrors #2436's own nested-loop axis on the accept side).
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           reject_unconditional_move_in_nested_loop_body) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; ") + kSink +
        "class C "
        "  public method M() "
        "    var n := new Item(); "
        "    for var i in 0..2 do "
        "      for var j in 0..2 do "
        "        Sink.Take(move n); "
        "      end for; "
        "    end for; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "cannot consume `n`: it may have already been consumed on another "
        "branch (maybe-consumed join, §28.3)"));
}

// ===========================================================================
// REJECT — the CONDITIONAL variant: `n` is moved only on SOME iterations
// (guarded by an `if`), not every one. K7.4's text is unconditional on this
// point — "a join of owned with moved-from ... yields maybe-consumed ...
// and may not flow around a loop back edge into any use" — it does not
// carve out "unless the move was only conditional." Within the body's own
// first pass, the if/else merge (Owned on the not-taken path, MovedFrom on
// the taken path) already produces the maybe-consumed join in the SAME way
// the unconditional case's loop-header merge does; the loop back edge then
// carries that maybe-consumed fact to the next iteration's re-entry into the
// `if`, where the SAME `move n` statement is the "any use" K7.4 forbids.
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           reject_conditional_move_on_some_iterations_still_maybe_consumed) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; ") + kSink +
        "class C "
        "  public method M() "
        "    var n := new Item(); "
        "    for var i in 0..4 do "
        "      if i mod 2 = 0 then "
        "        Sink.Take(move n); "
        "      end; "
        "    end for; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "cannot consume `n`: it may have already been consumed on another "
        "branch (maybe-consumed join, §28.3)"));
}

// ===========================================================================
// ACCEPT, NO SPURIOUS DIAGNOSTIC — the fresh-per-iteration idiom: `n` is
// DECLARED INSIDE the loop body (not before it), so `handleLocalDecl`
// unconditionally re-seeds it Owned on every visit (the same established
// "fresh binding on every visit" semantics #2436 relies on for `out owned
// var`) — there is no pre-loop edge for the back edge to join AGAINST, so no
// maybe-consumed marker ever arises. This is the common, idiomatic real-
// world shape (a resource minted and consumed once per iteration) and must
// stay silent: the fix must not turn stricter loop-back-edge tracking into
// a false reject of the ordinary case.
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           accept_fresh_local_declared_inside_loop_moved_each_iteration) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; ") + kSink +
        "class C "
        "  public method M() "
        "    for var i in 0..3 do "
        "      var n := new Item(); "
        "      Sink.Take(move n); "
        "    end for; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().empty());
}

// ===========================================================================
// ACCEPT, NO SPURIOUS DIAGNOSTIC — non-loop baseline: a single straight-line
// move outside any loop must be completely unaffected by this fix (the
// `suppressDiag_`-gated branch only changes behavior for a symbol whose
// entry state differs between fixpoint passes because of a loop back edge;
// a non-loop CFG's blocks are evaluated the same way every pass regardless).
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           accept_straightline_single_move_no_loop_baseline) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; ") + kSink +
        "class C "
        "  public method M() "
        "    var n := new Item(); "
        "    Sink.Take(move n); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().empty());
}

// ===========================================================================
// SWEEP FINDING #1 (checkRead's maybeConsumed branch) — REJECT: a maybe-
// consumed local — the join is established ONCE, straight-line, BEFORE the
// loop (an ordinary if/else merge, no back edge involved in creating the
// marker itself) — that a loop reads on EVERY iteration and never itself
// moves or disposes. K7.4's own text — "may not be read, borrowed from, or
// consumed after the join" — names the read case explicitly, and pre-fix
// this was a SEPARATE silent accept from the issue's own move-based repro:
// with ONLY the markMovedByName fix in place (no checkRead fix), this exact
// program still compiled with zero diagnostics, because checkRead's own
// poison-during-suppression corrupts the READ block's own exit state, and
// since the read block sits on the loop's back edge (the loop's only
// operation on `n` is this read, repeated every iteration), that corruption
// propagates around the back edge and prevents the WHOLE loop from ever
// converging on the correct Owned+maybeConsumed value — confirmed by
// reverting just this one line (restoring the unconditional
// `st->state = OwnershipState::Poisoned;`) and re-probing: the program goes
// back to a silent accept. The move is deliberately placed OUTSIDE the loop
// (in a plain if/else) so this test isolates checkRead's own defect from
// markMovedByName's — nothing inside the loop body ever attempts a move, so
// there is no possibility of markMovedByName's own (already-fixed) branch
// producing this diagnostic instead.
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           reject_read_of_maybe_consumed_loop_carried_local) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; ") + kSink + kReader +
        "class C "
        "  public method M() "
        "    var n := new Item(); "
        "    var b: bool := true; "
        "    if b then "
        "      Sink.Take(move n); "
        "    end; "
        "    for var i in 0..3 do "
        "      Reader.Peek(n); "
        "    end for; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "local `n` may have already been consumed on another branch and "
        "may not be read here (maybe-consumed join, §28.3)"));
}

// ===========================================================================
// SWEEP FINDING #2 (checkRead's disposedOnSomePath branch) — REJECT: the
// §28.2/§1883 conditional-discharge sibling of the above — a loop-carried
// "disposed on some path" local read on every iteration. Same defect shape,
// different flag (`disposedOnSomePath` rather than `maybeConsumed`), same
// `state == Owned` guard that permanently misses once poisoned during
// suppression.
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           reject_read_of_disposed_on_some_path_loop_carried_local) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; ") + kReader +
        "class C "
        "  public method M() "
        "    var n := new Item(); "
        "    for var i in 0..4 do "
        "      if i = 0 then "
        "        n.Dispose(); "
        "      end; "
        "      Reader.Peek(n); "
        "    end for; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "local `n` may have already been disposed on another branch and "
        "may not be read here (conditional-discharge join, §28.3 / §28.2"));
}
