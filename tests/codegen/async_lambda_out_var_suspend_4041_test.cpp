// Spec-derived regression tests for issue #4041 — a `string`-typed `out var`
// (or `out using var`) recipient bound inside an ASYNC LAMBDA body, read or
// disposed after a real `await` inside that SAME lambda, SEGVs at runtime
// (not merely leaks). This is the same defect family fix #3981 closed for
// the sync iterator, async iterator, and async-METHOD venues — but
// `emitAsyncLambda` (`LlvmBackend_Async.cpp`) is a FOURTH, SEPARATE
// state-machine-frame-building venue: fix #3499's own comment there
// (~L1818, "this is the FOURTH state-machine frame venue and the only one
// that does not go through buildStateMachineFrame") already documented the
// duplication that let this gap survive #3981 untouched. Confirmed NOT a
// regression from #3981: the crash reproduces byte-for-byte identically on
// `origin/main` before AND after cherry-picking #3981's own fix (be46eda) —
// #3981's `OutVarSlotCollector::expr` deliberately declines to descend into
// a nested `LambdaExpr`'s body ("that body compiles as its own function with
// its own suspend/frame domain ... its out-var call sites are that frame's
// business, not this one's"), so `emitAsyncLambda`'s own frame was never in
// scope for that fix at all.
//
// THE MECHANISM (established via BEZEL_DUMP_IR=1 on the issue's own repro).
// `emitAsyncLambda` builds its own `StateMachineFrame`-shaped struct BY HAND
// (`LlvmBackend_Async.cpp`, the `StateMachineFrame frame;` local ~L2158),
// populating `bodyLocals` — and from it, `frame.localFields` /
// `smLocalFields_`, published via the SHARED `emitResumeDispatch` — purely
// from `collectSuspendLocals(lam.stmts, bodyLocals)`. It never called
// `collectOutVarSuspendLocals`, #3981's new pre-pass, so an inline out-var
// recipient's identifier was never in `smLocalFields_` for an async lambda's
// own resume function: `ensureOutVarSlots`/`registerOutVarStringRelease`'s
// `mapSuspendLocalSlot(&id, slot)` call looked it up, missed, and silently
// no-op'd — exactly as it would have for the OTHER three venues before
// #3981. Unlike a plain leak, this is a hard crash: the recipient's bare
// `entryAlloca` is a stack slot in the FIRST activation of
// `bz$asynclam$N$Resume`; the `await` suspends and returns; the runtime
// re-enters `Resume` as a FRESH activation (a different worker thread, per
// the `BzScheduler`/`pthread_create` frames in the crash trace) whose fresh
// `entryAlloca` was just reset to null by the dispatch; the deferred
// scope-end release then dereferences that null/garbage `bz_string` --
// `AddressSanitizer: SEGV ... in bz$asynclam$0$Resume`, address 0x8 (the
// `bz_string` length-field offset past a null base).
//
// THE FIX (`LlvmBackend_Async.cpp`, `emitAsyncLambda`'s frame-building
// block). Call `collectOutVarSuspendLocals(lam.stmts, bodyLocals)` right
// after `collectSuspendLocals(lam.stmts, bodyLocals)`, mirroring
// `buildStateMachineFrame`'s own pairing (LlvmBackend_Iterators.cpp)
// exactly. The four `mapSuspendLocalSlot` call sites #3981 added
// (`registerOutVarStringRelease`, `registerCtorOutVarStringRelease`,
// `ensureOutVarSlots`'s and `ensureCtorOutVarSlots`'s `out using var` arms)
// are venue-agnostic — they read whatever `smLocalFields_` happens to hold,
// and `emitAsyncLambda` already publishes `frame.localFields` into
// `smLocalFields_` via the shared `emitResumeDispatch` — so they need no
// change at all; only the RESERVATION was missing for this venue.
//
// SCOPE NOTE — the EXPRESSION-BODIED async lambda arm (`lam.exprBody`) is a
// SEPARATE, DEEPER gap NOT fixed here: `registerOutVarStringRelease`'s own
// gate 4 (`outVarStringReleaseEligible`) requires `stringReleaseAnalyzedBody_`
// to be pinned to a STATEMENT LIST (`collectStringReleaseEligible`'s side
// effect), which never runs for an expression body (block-bodied guard,
// `if (!lam.exprBody) { collectStringReleaseEligible(lam.stmts); ... }`) — so
// an out-var recipient's release is never even REGISTERED in that venue,
// suspend or no suspend (measured: a plain, unconditional 20500 B / 1000
// allocation LEAK with NO await at all). This test file adds an expression-
// entry `collectOutVarSuspendLocals` overload (for forward-compatibility;
// presently inert in the expression-bodied venue for the reason above) but
// does not attempt the `outVarStringReleaseEligible` fix, which touches a
// different, safety-critical mechanism (the string escape walker) and
// deserves its own dedicated fix + tests. Filed as a follow-up issue.
//
// per §22.3, §22.4, §28.9, §28.11, §28.13, §29.2, §29.7, §30.1, §30.2, v0.294

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ASan + LSan oracle: a leak / double-free / UAF / SEGV report at exit
// surfaces as ran=false, which this flips to the -999 sentinel so a plain
// int comparison serves as the assertion.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// -----------------------------------------------------------------------------
// Issue #4041's own repro, loop-amplified: 500 invocations of an async
// lambda whose body binds a `string` `out var` recipient via a plain static
// method call, suspends at a REAL `await Task.Delay(0);`, and reads the
// recipient (`nm.Length`) only after resuming — so the recipient's release
// is necessarily deferred to the RESUMED activation. "abc" + "-tail" = 8
// chars, so a correct run totals 500*8 = 4000; asserted INTERNALLY
// (`return (if t = 4000 then 0 else 1)`) rather than via the exit code
// directly (LANE_RULES §11: exit code truncates to 8 bits). Pre-fix: SEGV
// inside `bz$asynclam$0$Resume` on the very first invocation (a hard crash,
// not merely a leak — this test's oracle is "did the process survive and
// answer correctly" as much as "did LSan stay quiet"). Post-fix: clean,
// exit 0.
// per §22.3, §22.4, §28.9, §30.1, §30.2
// -----------------------------------------------------------------------------
BEZEL_TEST(async_lambda_out_var_suspend_4041,
           single_call_out_var_string_recipient_in_async_lambda_is_safe) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do "
        "      var f := async lambda(): Task<int> => do "
        "        var ok := W.Make(\"abc\", out var nm); "
        "        _ := ok; "
        "        await Task.Delay(0); "
        "        return nm.Length; "
        "      end; "
        "      using var task := f(); "
        "      task.Wait(); "
        "      t := t + task.Result; "
        "    end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// DOUBLE-FREE HUNT — a WHILE loop INSIDE the async lambda body reuses the
// out-var recipient's slot across MULTIPLE real suspend/resume cycles within
// ONE lambda invocation: 5 rebinds of the SAME frame field, each spilled,
// released, and re-bound across its own `await`. "x" + "-tail" = 6 chars; 5
// rebinds per invocation x 500 invocations = 500*5*6 = 15000. If the slot
// were not nulled after release (`Disposal::nullSlotAfterRelease`), the
// NEXT bind's callee-side load would read already-freed memory — a
// double-release ASan reports rather than tolerates.
// per §22.3, §22.4, §28.9, §30.1, §30.2
// -----------------------------------------------------------------------------
BEZEL_TEST(async_lambda_out_var_suspend_4041,
           loop_reused_out_var_slot_across_real_suspends_in_async_lambda) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do "
        "      var f := async lambda(): Task<int> => do "
        "        var total := 0; "
        "        var k := 0; "
        "        while k < 5 do "
        "          var ok := W.Make(\"x\", out var nm); "
        "          _ := ok; "
        "          await Task.Delay(0); "
        "          total := total + nm.Length; "
        "          k := k + 1; "
        "        end; "
        "        return total; "
        "      end; "
        "      using var task := f(); "
        "      task.Wait(); "
        "      t := t + task.Result; "
        "    end; "
        "    return (if t = 15000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// CONSTRUCTOR SIBLING — `registerCtorOutVarStringRelease` (the `new T(out
// var n)` form, fix #3305) shares the identical activation-crossing gap
// inside an async lambda body: a `string` `out var` recipient of a
// CONSTRUCTOR call, bound before a real `await` and read after, also SEGVs
// pre-fix for the same reason as the static-method form above.
// per §22.3, §22.4, §28.9, §30.1, §30.2
// -----------------------------------------------------------------------------
BEZEL_TEST(async_lambda_out_var_suspend_4041,
           ctor_out_var_string_recipient_in_async_lambda_is_safe) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; "
        "  end constructor; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do "
        "      var f := async lambda(): Task<int> => do "
        "        var w := new W(\"abc\", out var nm); "
        "        _ := w; "
        "        await Task.Delay(0); "
        "        return nm.Length; "
        "      end; "
        "      using var task := f(); "
        "      task.Wait(); "
        "      t := t + task.Result; "
        "    end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// SIBLING FORM — `out using var`, the scope-bound OWNED-recipient form
// (§22.4/§28.11/§28.13), inside an async lambda body across a real `await`:
// `OwningStack<Box>.TryPop(out using var c)` binds a moved-out `Box` and
// registers an unconditional `dynamicDispose` — allocated identically to
// `out var`'s `stringRelease` (`entryAlloca(ptrTy())`, unconditionally) and
// therefore exposed to the identical gap. `Box`'s destructor prints
// "dispose" so exactly-once disposal is independently observable in
// `stdoutText`, not just LSan-clean. 500 invocations, one Box(7) per
// invocation: 500*7 = 3500 and exactly 500 "dispose" lines.
// per §22.4, §28.9, §28.11, §28.13, §30.1, §30.2
// -----------------------------------------------------------------------------
BEZEL_TEST(async_lambda_out_var_suspend_4041,
           out_using_var_owned_recipient_in_async_lambda_is_safe) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class Box "
        "  public v: int; "
        "  public constructor(v: int) this.v := v; end constructor; "
        "  destructor Console.WriteLine(\"dispose\"); end destructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do "
        "      var f := async lambda(): Task<int> => do "
        "        using var s := new OwningStack<Box>(); "
        "        s.Push(new Box(7)); "
        "        var total := 0; "
        "        if s.TryPop(out using var c) then "
        "          await Task.Delay(0); "
        "          total := c!.v; "
        "        end; "
        "        return total; "
        "      end; "
        "      using var task := f(); "
        "      task.Wait(); "
        "      t := t + task.Result; "
        "    end; "
        "    return (if t = 3500 then 0 else 1); "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    std::size_t n = 0, pos = 0;
    while ((pos = r.stdoutText.find("dispose", pos)) != std::string::npos) {
        ++n;
        pos += 7;
    }
    BEZEL_EXPECT_EQ(static_cast<int>(n), 500);
}
