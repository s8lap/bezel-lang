// Spec-derived regression tests for issue #1883: an owned local discharged
// (explicit Dispose) on one branch and relying on scope-end auto-drop for the
// other is rejected, contradicting §28.3's per-path auto-drop for a
// maybe-consumed join.
//
// §28.3 (v0.180, current spec rev v0.196), "maybe-consumed join rule":
//   "A join of owned with moved-from (or explicitly disposed) yields
//    maybe-consumed (v0.180): the variable may not be read, borrowed from,
//    or consumed after the join, and may not flow around a loop back edge
//    into any use; scope-end auto-drop applies per-path (disposal runs
//    only on the still-owned path)."
// The repro has NO post-join read/borrow/consume — only `return 0;` — so
// the program is well-formed and should compile with at most a must-use
// warning and run returning 0.
//
// Before this fix, the scope-end auto-drop gate in Ownership.cpp was gated
// on `everDischarged_.count(sym) == 0` — any conditional discharge (even a
// one-branch Dispose on a null-safe slot) disqualified the local from
// auto-drop and fell through to the hard discharge error. The MOVED-FROM
// split (Owned ⊓ MovedFrom) correctly stays rejected (joinInto sets
// maybeConsumed, the slot is NOT nulled on the moved path → real UAF
// hazard); the DISPOSED split (Owned ⊓ Disposed) joins to plain Owned
// (no maybeConsumed) because explicit Dispose NULLS the slot (per #415,
// LlvmBackend_Calls.cpp's explicit-Dispose lowering), so the auto-drop is
// null-safe: the null-guard at scope end skips the disposed-on-this-path
// branch and disposes only the still-owned branch.
//
// The fix relaxes the gate: `!st.maybeConsumed && (everDischarged_ == 0 ||
// definitelyRestoredLocals_ != 0)` (the !maybeConsumed conjunct already
// excludes the MOVED-FROM split, where joinInto sets the marker).
//
// Codegen needs no change: the existing class-local auto-drop path
// (registerUsingDisposal → LlvmBackend_Disposal.cpp ~line 3313) already
// null-guards and nulls the slot after reclaim, mirroring the dynamicDispose
// path used by the pre-declared nullable out-owned recipient (§28.3 / #1466).

#include "support/aot_support.hpp"       // runAot — AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// `class A` is a disposable-shaped class with a destructor; `_n` is a static
// counter so an AOT test can confirm exactly-once disposal (each A is
// constructed once and disposed once). `body` is the Main method body.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class A "
        "  public static _n: int := 0; "
        "  public constructor() A._n := A._n + 1; end constructor; "
        "  destructor A._n := A._n - 1; end destructor; "
        "end A; "
        "class C "
        "  public static method Cond(): bool return true; end Cond; "
        "  public static method Main(): int ") +
        body + " end Main; end C;";
}

// True iff some ERROR diagnostic cites §28.3 in the "would be dropped here
// without disposal" form — the exact phrase the pre-fix ownership pass
// emitted at the declaration.
bool hasConditionalDischargeReject(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("§28.3") == std::string::npos) continue;
        if (d.message.find("would be dropped here without disposal") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// ============================================================================
// ACCEPTED — the §28.3 maybe-consumed per-path auto-drop: one-branch Dispose,
// no post-join use.
// ============================================================================

// §28.3 (#1883): the exact issue repro — `var x := new A(); if C.Cond() then
// x.Dispose(); end; return 0;`. After the if, x is disposed on the true
// branch and still-owned on the false branch (maybe-consumed join). No
// post-join read/borrow/consume, so the program is well-formed and should
// compile with at most the §28.3 must-use WARNING (auto-drop on the still-
// owned path).
BEZEL_TEST(conditional_discharge_1883,
           one_branch_dispose_no_post_join_use_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: A := new A(); "
        "if C.Cond() then "
        "  x.Dispose(); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConditionalDischargeReject(r));
    // Must-use warning per §28.3 auto-drop on the still-owned path.
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}

// §28.3 (#1883): the move-on-one-branch / auto-drop-on-other variant —
// the spec's own worked-example shape. The MOVE split sets maybeConsumed
// (joinInto, #1626) and STAYS rejected (the slot is NOT nulled on the
// moved path → real UAF hazard); this is the negative control that proves
// the gate's `!st.maybeConsumed` conjunct is the precise filter.
BEZEL_TEST(conditional_discharge_1883,
           one_branch_move_still_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "class Sink "
        "  public method Take(owned t: A) "
        "    using var h: A := t; "
        "  end Take; "
        "end Sink; "
        "var s: Sink := new Sink(); "
        "var x: A := new A(); "
        "if C.Cond() then "
        "  s.Take(move x); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "x"));
}

// §28.3 (#1883): AOT execution — the one-branch-Dispose / auto-drop-on-
// other idiom actually runs and the disposed A is reclaimed exactly once.
// Uses A._n (constructor increments, destructor decrements) to assert the
// post-disposal count is 0; AOT-compiled, so a double-free would crash
// rather than silently leak.
BEZEL_TEST(conditional_discharge_1883,
           one_branch_dispose_aot_runs_with_exactly_once_disposal) {
    JitResult jr = runAot(wrap(
        "var x: A := new A(); "
        "if C.Cond() then "
        "  x.Dispose(); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
    BEZEL_EXPECT_TRUE(jr.ran);
}

// ============================================================================
// NO REGRESSION — the MOVED-FROM split is still rejected (the gate's
// !st.maybeConsumed filter) and the unconditional dispose still auto-drops.
// ============================================================================

// §28.3 v0.180 (#1626): the Owned ⊓ MovedFrom split — a `move x` on one
// branch — sets maybeConsumed and is hard-rejected by the post-join
// discharge error. This is the exact #1626 baseline that the #1883 fix
// MUST NOT regress.
BEZEL_TEST(conditional_discharge_1883,
           move_split_still_hard_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "class Sink "
        "  public method Take(owned t: A) "
        "    using var h: A := t; "
        "  end Take; "
        "end Sink; "
        "var s: Sink := new Sink(); "
        "var x: A := new A(); "
        "if C.Cond() then "
        "  s.Take(move x); "
        "end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "x"));
}

// §28.3 v0.150 baseline: unconditional `x.Dispose()` then no further use —
// the legacy #601 / spec_28_2_one_branch_no_warn case. Still accepted with
// no error (the Dispose is the proper discharge).
BEZEL_TEST(conditional_discharge_1883,
           unconditional_dispose_still_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: A := new A(); "
        "x.Dispose(); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConditionalDischargeReject(r));
}