// Ownership — a `for` loop's SOURCE expression (`ForStmt::iterable`, and the
// optional `with EXPR` clause) never reached `MethodOwnership::checkRead`, so
// a read of an already moved-from (or explicitly disposed) owned local in the
// loop source was undiagnosed in plain straight-line code, and the accepted
// program heap-use-after-frees.  Issue #3977.
//
// SPEC — settled, not underspecified.  §28.3 "Local variables, ownership, and
// borrows", K1 "Value classification", item 3 (v0.294), verbatim:
//
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
//
// §28.2 "The `IDisposable` contract" (v0.294): "The compiler additionally
// **must** emit a compile-time warning when a local variable is used (read,
// borrowed from, or consumed) after an explicit `Dispose()` call ... on the
// same variable that dominates the use site on every path".
//
// A `for` source is a read position; nothing in either section carves one
// out.
//
// MEASURED PRE-FIX (parent origin/main 8cb70ed = e240743 + #3971's own
// checkread_expression_walk_3971 fix, i.e. WITH the RangeExpr checkRead arm
// already in place).  Every body begins `var x := new Res(1); var t :=
// Sink.Take(move x);` — `Take` never discharges its `owned` parameter, so
// §28.3's auto-drop frees the moved-in value at the end of `Take` and `x` is
// provably dangling from the third line on:
//
//   | third line                                   | rc | diagnostics |
//   |-----------------------------------------------|----|-------------|
//   | `var r := x.tag..3;`             (control)     |  1 | §28.3 moved-out error |
//   | `for var i in 0..x.tag do n:=n+1; end;`         |  0 | **NONE** — heap-UAF |
//
// The `x.tag..3` control is REJECTED (the byte-identical read one node kind
// over — #3971's own RangeExpr arm already covers it); the identical range
// sitting in a `for` source compiled with zero diagnostics, because the
// `for`'s iterable is not part of any CFG block's `stmts` and nothing in the
// ownership pass ever called `checkRead` on it (`Ownership.cpp`'s
// `forOrigins` walk only calls `recordMovesIn`, which records OUT-transfers,
// never inspects a bare read).
//
// ROOT CAUSE: `Ownership.cpp`'s `block.forOrigins` walk ran `recordMovesIn`
// on `fs->iterable` / `fs->withExpr` and nothing else — no `checkRead` call
// on either anywhere in the pass.
//
// THE FIX feeds the existing authority rather than adding a second one:
// `checkRead(*fs->iterable, /*selfTarget=*/false)` and the same for
// `fs->withExpr`, in the `forOrigins` walk, BEFORE `recordMovesIn` so the
// state observed is the one at the top of the statement — mirroring #3949's
// assignment-RHS fix and #3864's const-initializer fix for the same family
// (#3591/#3687/#3864/#3931/#3949/#3971).  Guarded by the shared
// `isTopLevelConsumingForm` (#3941's fix, promoted to Ownership_impl.hpp by
// #3971): a `for var e in move coll do …` iterable must not be read-checked,
// or the more precise move/dispose diagnostic there would be stolen exactly
// as #3941 and #3979 describe.
//
// FALSE-REJECT COST: 0.  A `for` source that reads a live borrow, a
// reacquired local, a live collection, and a `for var e in move coll do …`
// iterable are byte-identical pre and post — pinned below as
// value-asserting positives.
//
// per §28.3 K1.3, §28.2, v0.294, #3941, #3971, #3977

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — the AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

bool diagWithSeverity(const OwnershipResult& r, bezel::Severity sev,
                      const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != sev) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool diagHasRealLocation(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) == std::string::npos) continue;
        if (d.location.line != 0 && d.location.column != 0) return true;
    }
    return false;
}

int diagCount(const OwnershipResult& r, const std::string& needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) ++n;
    return n;
}

const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "  public static method MakeRange(n: int): owned Array<int> "
    "    var a := new Array<int>(); a.Add(n); return a; "
    "  end MakeRange; "
    "end Sink; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// The shared first two lines of every use-after-MOVE row.
const char* kConsumed =
    "var x := new Res(1); "
    "var t := Sink.Take(move x); ";

const char* kMovedOut =
    "owned value in `x` was already moved out at this point (§28.3)";

}  // namespace

// ============================================================================
// CONTROL — the byte-identical read one node kind over (#3971's RangeExpr
// checkRead arm), which was ALREADY rejected before this fix. Anchors the
// asymmetry the negative rows below measure.
// ============================================================================

BEZEL_TEST(for_loop_source_read_after_consume_3977,
           control_var_initializer_range_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "var r := x.tag..3; return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// ============================================================================
// NEGATIVE — the `for` source spellings that were silently accepted.
// FAIL WITHOUT THE FIX: zero diagnostics, and the produced program
// heap-use-after-frees.
// ============================================================================

// The exact issue #3977 repro: a range whose LOW endpoint reads the
// moved-from local, sitting in the for-loop source.
BEZEL_TEST(for_loop_source_read_after_consume_3977,
           for_source_range_low_end_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) +
             "var n := 0; "
             "for var i in x.tag..3 do n := n + 1; end; "
             "return t + n;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_TRUE(diagHasRealLocation(r, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// The range's HIGH endpoint — checkRead's RangeExpr recursion must reach
// both operands, not just the low one.
BEZEL_TEST(for_loop_source_read_after_consume_3977,
           for_source_range_high_end_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) +
             "var n := 0; "
             "for var i in 0..x.tag do n := n + 1; end; "
             "return t + n;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// A CALL as the for-source, whose own ARGUMENT reads the moved-from local —
// this exercises the SAME single checkRead call recursing into checkRead's
// pre-existing CallExpr arm; no per-shape special-casing was needed.
BEZEL_TEST(for_loop_source_read_after_consume_3977,
           for_source_call_result_argument_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) +
             "var n := 0; "
             "for var i in Sink.MakeRange(x.tag) do n := n + i; end; "
             "return t + n;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagWithSeverity(r, bezel::Severity::Error, kMovedOut));
    BEZEL_EXPECT_EQ(diagCount(r, kMovedOut), 1);
}

// The AOT oracle's half of the same fact: the program that used to compile
// and then heap-use-after-free must now not compile at all.
//
// `ran == false` alone would be AMBIGUOUS (issue 3892 / the
// check_ambiguous_ran_false ratchet) — pin the REJECTION and the §28.3 text.
BEZEL_TEST(for_loop_source_read_after_consume_3977,
           aot_for_source_range_read_after_move_is_a_compile_error_28_3) {
    JitResult jr = runAot(wrap(
        std::string(kConsumed) +
        "var n := 0; "
        "for var i in x.tag..3 do n := n + 1; end; "
        "return t + n;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMovedOut));
}

// ============================================================================
// NEGATIVE — the DISPOSE half. §28.2's dominating-Dispose warning was
// likewise silent in a `for` source.
// ============================================================================

// The `await for ... with` sibling: `ForStmt::withExpr` shares the SAME
// `forOrigins` walk and the same `isAwait`-independent code path as
// `iterable`, so this is not a separately-guarded case — it is the same
// fix, exercised at the other field. Measured pre-fix: zero diagnostics
// (silent accept) for `cts.Token` reading a disposed CancellationTokenSource
// in the `with` clause; post-fix: the §28.2 warning, at the with-clause's
// own location.
BEZEL_TEST(for_loop_source_read_after_consume_3977,
           await_for_with_clause_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var cts := new CancellationTokenSource(); "
        "    cts.Dispose(); "
        "    var sum := 0; "
        "    await for var x in Gen() with cts.Token do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(diagWithSeverity(
        r, bezel::Severity::Warning,
        "use of `cts` after a dominating explicit Dispose() call"));
}

BEZEL_TEST(for_loop_source_read_after_consume_3977,
           for_source_read_after_dispose_warns_28_2) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        "var x := new Res(1); "
        "var t := 0; "
        "x.Dispose(); "
        "var n := 0; "
        "for var i in x.tag..3 do n := n + 1; end; "
        "return t + n;"));
    BEZEL_EXPECT_TRUE(diagWithSeverity(
        r, bezel::Severity::Warning,
        "use of `x` after a dominating explicit Dispose() call"));
}

// ============================================================================
// POSITIVE — the false-reject axis. Every one of these is a legitimate
// `for` source the new read-check must leave alone.
// ============================================================================

// A `for` source reading a live BORROW — the commonest for-source shape.
// `..` is INCLUSIVE on both ends (§16.3/§16.4, v0.294), so `0..4` yields
// 0,1,2,3,4 — 5 values.
BEZEL_TEST(for_loop_source_read_after_consume_3977,
           positive_for_source_reads_a_live_borrow_28_3) {
    JitResult jr = runAot(wrap(
        "using var src := new Res(4); "
        "var n := 0; "
        "for var i in 0..src.tag do n := n + 1; end; "
        "return n;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 5);
}

// A `for` source reading a REACQUIRED local — the moved-from state is gone,
// so the read is legal again (§28.3 K1.3 — "unreadable *until reassigned*").
// `0..6` inclusive yields 7 values.
BEZEL_TEST(for_loop_source_read_after_consume_3977,
           positive_for_source_reads_a_reacquired_local_28_3) {
    JitResult jr = runAot(wrap(
        std::string(kConsumed) +
        "x := new Res(6); "
        "var n := 0; "
        "for var i in 0..x.tag do n := n + 1; end; "
        "x.Dispose(); "
        "return t + n;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 8);  // 1 + 7
}

// A `for` source that is a live, undisposed collection — iterated normally.
BEZEL_TEST(for_loop_source_read_after_consume_3977,
           positive_for_source_iterates_a_live_collection) {
    JitResult jr = runAot(wrap(
        "using var arr := new Array<int>(); "
        "arr.Add(1); arr.Add(2); arr.Add(3); "
        "var n := 0; "
        "for var i in arr do n := n + i; end; "
        "return n;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);
}

// `for var e in move coll do …` — a TOP-LEVEL consuming form. It must NOT be
// read-checked: the more specific move check owns the diagnostic there, and
// the loop must still consume `coll` and run normally (§28.3 K3.4).
BEZEL_TEST(for_loop_source_read_after_consume_3977,
           positive_for_source_is_a_top_level_move_28_3_K3_4) {
    JitResult jr = runAot(wrap(
        "var arr := new Array<int>(); "
        "arr.Add(5); arr.Add(7); "
        "var n := 0; "
        "for var i in move arr do n := n + i; end; "
        "return n;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 12);
}
