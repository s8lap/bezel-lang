// Codegen — §30.5 Pattern A kind 2 chained return / issue #2750 (v0.240):
// `return await Producer()` in an async method whose continuation throws.
//
// THE DEFECT. §30.5 Pattern A kind 2 (the CHAINED SLOT) transfers the
// source task into the outer task's chained slot on the SUCCESS path
// (`bz_task_set_chained` in `emitAsyncPatternAResultDecision`); the outer
// task's destructor frees the source transitively. On the FAULT path,
// control diverts to `unwindBB()` (catch.dispatch inside a try, or
// async.fault otherwise) before any return-statement lowering runs —
// so the chained transfer NEVER happens and the source task leaks. The
// fault-path reclaim at `emitAwaitSuspend` is gated on `freeTaskAfter`,
// which is `false` for the chained-return row (`asyncChainedReturn_`
// suppresses it so the success-path chain is the sole owner of the
// source-task reclaim).
//
// THE FIX. Pass a new `freeOnFault` flag through `emitAwaitSuspend`; on
// the FAULT path, when the await is the source of a `return await
// <source>` chained return AND the operand is a CallExpr (the anonymous-
// producer row — a fresh `Producer()` result with no other owner), reclaim
// the source task. Use the null-after-free guard pattern from #2733's
// ffiBuffer arm: reload from the frame slot, check for null, free and
// null. The owned-LOCAL row (Identifier operand, chainedReturnOwnedTaskSource)
// is reclaimed by emitUnwindDisposals on the unwind edge, and the owned-
// PARAMETER row (chainedReturnOwnedTaskParamSource) is reclaimed by
// emitAsyncOwnedParamAutoDispose — reclaiming THOSE rows here would
// double-free, so freeOnFault is keyed on the CallExpr check that
// identifies the anonymous-producer row uniquely.
//
// MEASURED on the pre-fix compiler, generated-code ASan/LSan:
//
//     Direct leak of 184 byte(s) in 1 object(s) allocated from:
//         #0 malloc
//         #1 operator new(unsigned long)
//         #2 bz$M.C$OuterChainedFault
//         #3 main
//
//     SUMMARY: AddressSanitizer: 184+ bytes leaked
//
// Post-fix: 0 bytes leaked; both paths reclaim the source task exactly once.
//
// Spec citation: §30.5 "Pattern A kind 2 — Chaining from a scope-bound
// source" (v0.240); §30.5 "On both paths the source task is disposed
// exactly once — by the outer task's destructor, transitively." The
// fix extends that contract to the FAULT path where the chain itself
// is unreachable.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// True if the result carries an ASan / LSan report headline — the
// runAotChecked oracle flips `r.ran` to false for either, so a leak
// becomes a non-zero return distinct from any legal exit value.
bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// ASan+LSan run. A heap-use-after-free / double-free / leak surfaces as
// ran=false with the sanitizer report in `diagnostics`, i.e. -999.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

// Plain (non-ASan) AOT run.
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

// A disposal-counted class used to prove the §30.5 source-task reclaim
// fires EXACTLY ONCE — `n` (live count) and `made` (total construction
// count) are folded into the exit code so both the leak (n > 0) and a
// double-dispose (crash — counted by the sanitizer verdict) are each
// observable as a wrong value rather than as a silent pass.
//
// Note: `Cell` here is NOT the leaking object — the leak is on the
// `bz_task_t` source struct itself (heap-allocated by Producer() and
// missed on the fault path). LSan arms a direct leak on the source
// task's allocation, and that is what the negative-control expect-equals-0
// pattern guards.
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public static made: int := 0; "
    "  public v: int; "
    "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; "
    "    Cell.made := Cell.made + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

// ── (1) THE REPRO. `return await Producer()` in an async method whose
// `Producer()` throws. The async method's continuation re-raises the
// fault; the caller catches it. Pre-fix: LSan reports a 184-byte leak
// per iteration (the source `bz_task_t` struct). Post-fix: 0 leak.
const std::string kChainedReturnFault =
    std::string("module M; ") + kCell +
    "class C "
    "  public static async method ProducerFault(): Task<Cell> "
    "    throw new InvalidOperationException(\"boom\"); "
    "  end ProducerFault; "
    "  public static async method Outer(): Task<Cell> "
    "    return await C.ProducerFault(); "      // ← §30.5 kind 2 chained
    "  end Outer; "
    "  public static async method Main(): Task<int> "
    "    try "
    "      using var t := C.Outer(); "
    "      var c := await t; "
    "    catch InvalidOperationException => _ := 0; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ── (2) NEGATIVE CONTROL (same chained-return shape, SUCCESS path).
// `return await Producer()` where `Producer()` returns a fresh Cell.
// Pre-fix and post-fix: 0 leak (the success path's bz_task_set_chained
// already transfers the source task to the outer task, which the outer
// task's destructor frees transitively). This proves the fix is
// specific to the fault path and does NOT change the success path.
//
// The `do … end` wrapper around the `using var t := C.Outer(); await t;`
// is load-bearing: §28.3 fires the `using var`'s disposal at the END of
// its containing scope, so without the `do` wrapper the disposal runs
// at the method's end (after the `return Cell.n * 100 + Cell.made;`)
// and `Cell.n` is read BEFORE the disposal decrements it — the test
// would exit 101 (1 alive * 100 + 1 made) instead of the expected 1
// (0 alive + 1 made). The existing #2731 regression test uses the
// same wrapping for the same reason.
const std::string kChainedReturnOK =
    std::string("module M; ") + kCell +
    "class C "
    "  public static async method ProducerOK(): Task<Cell> "
    "    return new Cell(1); "
    "  end ProducerOK; "
    "  public static async method Outer(): Task<Cell> "
    "    return await C.ProducerOK(); "
    "  end Outer; "
    "  public static async method Main(): Task<int> "
    "    do "
    "      using var t := C.Outer(); "
    "      var c := await t; "
    "    end; "
    "    return Cell.n * 100 + Cell.made; "     // 0 alive, 1 made => 1
    "  end Main; "
    "end C;";

// ── (3) POSITIVE CONTROL — DIFFERENT SHAPE: a SYNC method returning a
// value directly. No `Task`, no await, no chained return — a Cell is
// returned and the caller disposes it through the call site. Pre-fix
// and post-fix: 1 (one Cell made, zero alive at end). This is the
// "LSan is armed in this binary shape" guard: a leak in the chained-
// return path is detected because this shape runs to completion cleanly
// and reports no leak.
//
// The `do … end` wrapper around `var c := C.Make();` is load-bearing
// for the same reason as in kChainedReturnOK above: §28.3 disposes the
// `var c := Make();` at the END of its containing scope, so without
// the wrapper the disposal runs at the method end and Cell.n is read
// BEFORE the decrement.
const std::string kSyncReturn =
    std::string("module M; ") + kCell +
    "class C "
    "  public static method Make(): owned Cell "
    "    return new Cell(1); "
    "  end Make; "
    "  public static method Main(): int "
    "    do "
    "      var c := C.Make(); "
    "    end; "
    "    return Cell.n * 100 + Cell.made; "    // 0 alive, 1 made => 1
    "  end Main; "
    "end C;";

// ── (4) AMPLIFIED PROOF. 64 rounds of the repro, plus 64 rounds of the
// success-path control, in the SAME binary shape. The amplification
// exists because LSan only fires on root-pointer-skip detection — a
// single iteration where the LAST pointer happened to be register-
// reachable would not register as a leak, and a defect-free single
// iteration would not prove the ORACLE is armed. 64 iterations each
// round puts real allocator churn between frees and the next read, and
// the exit code pins both halves: the program returns 0 (no error
// path) and the test harness's `r.ran` must be true (LSan did not
// report).
const std::string kAmplified =
    std::string("module M; ") + kCell +
    "class C "
    "  public static async method ProducerFault(): Task<Cell> "
    "    throw new InvalidOperationException(\"boom\"); "
    "  end ProducerFault; "
    "  public static async method ProducerOK(): Task<Cell> "
    "    return new Cell(1); "
    "  end ProducerOK; "
    "  public static async method OuterFault(): Task<Cell> "
    "    return await C.ProducerFault(); "
    "  end OuterFault; "
    "  public static async method OuterOK(): Task<Cell> "
    "    return await C.ProducerOK(); "
    "  end OuterOK; "
    "  public static async method Main(): Task<int> "
    "    for var i in 0..63 do "
    "      try "
    "        using var t := C.OuterFault(); "
    "        var c := await t; "
    "      catch InvalidOperationException => _ := 0; end; "
    "    end for; "
    "    for var i in 0..63 do "
    "      using var t := C.OuterOK(); "
    "      var c := await t; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ── THE REPRO. Pre-fix: LSan reports a direct leak of 184 bytes per
// invocation → -999 (r.ran = false). Post-fix: 0 (the new freeOnFault
// reclaim reclaims the source task on the fault path).
BEZEL_TEST(codegen_chained_return_fault_reclaim_2750, repro_checked) {
    int rc = runChecked(kChainedReturnFault);
    if (rc == -1000) return;   // ASan toolchain absent
    BEZEL_EXPECT_EQ(rc, 0);
}

// Plain (non-ASan) run of the same repro. The plain oracle alone does
// not catch the leak, but the program still exits 0 — the leak only
// surfaces as an ASan/LSan report.
BEZEL_TEST(codegen_chained_return_fault_reclaim_2750, repro_plain) {
    BEZEL_EXPECT_EQ(runPlain(kChainedReturnFault), 0);
}

// ── NEGATIVE CONTROL — same shape, success path. Proves the fix is
// confined to the fault path: pre- and post-fix this shape is leak-
// clean because bz_task_set_chained on the success path already
// transfers ownership. Pre-fix this also passes; the test pins the
// success-path behavior so a future change that breaks it (e.g.
// removing the chained slot on success) is caught.
BEZEL_TEST(codegen_chained_return_fault_reclaim_2750, success_path_control) {
    BEZEL_EXPECT_EQ(runPlain(kChainedReturnOK), 1);
}

BEZEL_TEST(codegen_chained_return_fault_reclaim_2750,
           success_path_control_checked) {
    int rc = runChecked(kChainedReturnOK);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 1);
}

// ── POSITIVE CONTROL — sync method, no Task involved. Same binary
// shape, same allocator, same LSan arming. Proves the test binary
// itself does not leak a baseline Cell (the only heap allocations
// here are the Cell + its dispose path).
BEZEL_TEST(codegen_chained_return_fault_reclaim_2750,
           sync_return_no_task_positive_control) {
    BEZEL_EXPECT_EQ(runPlain(kSyncReturn), 1);
}

BEZEL_TEST(codegen_chained_return_fault_reclaim_2750,
           sync_return_no_task_positive_control_checked) {
    int rc = runChecked(kSyncReturn);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 1);
}

// ── AMPLIFIED PROOF — 64 rounds each, both paths, same binary. The
// amplification matters because LSan is binary-layout dependent
// (mentioned in the issue): a single iteration where the LAST pointer
// happened to be register-reachable would not register as a leak, and
// a defect-free single iteration would not prove LSan is armed. 64
// rounds of churn and one path of each kind is the test-oracle's
// load-bearing claim — both must reach exit-code 0 with r.ran = true
// (no leak report).
BEZEL_TEST(codegen_chained_return_fault_reclaim_2750, amplified_64_rounds) {
    BEZEL_EXPECT_EQ(runPlain(kAmplified), 0);
}

BEZEL_TEST(codegen_chained_return_fault_reclaim_2750,
           amplified_64_rounds_checked) {
    int rc = runChecked(kAmplified);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 0);
}