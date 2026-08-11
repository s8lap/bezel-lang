// Codegen/runtime — §30.9 v0.179 fault ownership (#1601, residual item 4 /
// #1021): an abandoned still-running faulted task's `error_obj` MUST be disposed
// by the fire-and-forget continuation that frees the task — not just the
// DisposedAccessException sentinel, ALL faults.
//
// Bug (#1601 / #1021). §30.9 (v0.179) says a task owns its fault exception and
// must dispose it "exactly once, by the task that owns it, when the task is
// disposed". A `using var t := Task.SomeFaultingOp();` (or plain `var t` after
// v0.190 SB-01 D2, #1868) bound in an inner scope, where the scope ends before
// `t` settles, triggers the bz_task_fire_and_forget slow path: it registers a
// continuation that calls bz_task_free when `t` later completes. Pre-fix, that
// continuation only inspected the fault for the DisposedAccessException
// backstop (§30.10 checked-disposal backstop) — any OTHER fault's `error_obj`
// leaked (no Dispose() call ran; the task struct + error_msg were reclaimed by
// bz_task_free, but the Bezel exception object's destructor was never invoked).
//
// SPEC §30.9 v0.179 (cited per fix #1528 / #1601): "a task owns its fault
// exception; on disposal the task disposes the exception object exactly once".
// The D2 admission (#1868) for plain-`var` across-a-suspend makes the leak
// reachable from BOTH `using var` AND `var` shape — codegen's scope-end path
// skips the error_obj disposal whenever bz_task_is_error(t) is false at scope
// end (the "still running" case), deferring it to the runtime's later settle.
// The runtime path that performs the deferred disposal must therefore fire for
// EVERY settled fault, not only the DisposedAccessException sentinel.
//
// Repro. `using var t := SlowFault();` (or plain `var` under #1868): at scope
// end `t` is still running, so codegen emits bz_task_fire_and_forget(t) — slow
// path; `t` later faults via the worker pool, completing with error_obj set.
// Pre-fix the slow-path continuation calls bz_task_free, which reclaims the
// struct + error_msg but NOT the error_obj -> 16-byte leak (the 16-byte Bezel
// exception object allocated by the throwing resume function via calloc).
//
// Oracle. A custom `Exc` class with a static live-count (ctor ++, dtor --).
// After the abandoned task settles and Main returns the count: 0 == dispose-
// exactly-once, >0 == leak, <0 == double-free. runAotChecked attaches ASan +
// LSan so a leak is a hard sanitizer failure (r.ran==false -> -999) in
// addition to the count oracle.
//
// Scope. This test pins the runtime-side disposal fix (item 4 of #1601). The
// other #1601 residuals (SetException `owned` signatures, aggregate
// re-raise-by-borrow, etc.) are independent and not regressed by this change.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999, distinct
// from any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Exception with a static live-count — the standard leak / double-free oracle.
const char* kExc =
    "class Exc: Exception public static n: int := 0; "
    "  public constructor() Exc.n := Exc.n + 1; end constructor; "
    "  destructor Exc.n := Exc.n - 1; end destructor; "
    "end Exc; ";

}  // namespace

// ---------------------------------------------------------------------------
// §30.9 v0.179 / #1601 (item 4) / #1021 — the canonical leak repro. A
// `using var t := SlowFault();` whose body faults AFTER the using-scope ends:
// at scope end `t` is still running, codegen's scope-end path skips
// error_obj disposal (#1601 acknowledges this), the slow-path continuation
// registered by bz_task_fire_and_forget later calls bz_task_free when `t`
// settles. Pre-fix the continuation does not dispose error_obj -> the 16-byte
// BzExc leaks. Pre-fix return = -999 (ASan LSan); post-fix return = 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_faulted_task_error_obj_disposal_1601,
           using_var_still_running_at_scope_end_faults_later) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kExc) +
        "class C "
        "  public static async method SlowFault(): Task<int> "
        "    await Task.Delay(30); "
        "    throw new Exc(); "
        "  end SlowFault; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var t := C.SlowFault(); "   // still running at scope end
        "      await Task.Delay(5); "            // different suspend — t live across it
        "    end; "
        "    await Task.Delay(120); "            // let the abandoned task settle + dispose
        "    return Exc.n; "                     // expect 0 (Exc disposed exactly once)
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// §30.9 v0.179 / #1601 (item 4) — same shape via plain `var t := ...` after the
// v0.190 SB-01 D2 admission (#1868). The plain-`var` auto-drop also fires
// bz_task_fire_and_forget at scope end (the §30.5 admission D2 newly admits),
// so the same slow-path continuation runs and the same error_obj leak fires.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_faulted_task_error_obj_disposal_1601,
           plain_var_still_running_at_scope_end_faults_later) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kExc) +
        "class C "
        "  public static async method SlowFault(): Task<int> "
        "    await Task.Delay(30); "
        "    throw new Exc(); "
        "  end SlowFault; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      var t := C.SlowFault(); "         // plain var — SB-01 D2 (#1868) owns + drops
        "      await Task.Delay(5); "
        "    end; "
        "    await Task.Delay(120); "
        "    return Exc.n; "                     // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// §30.9 v0.179 / #1601 (item 4) — sanity control: the already-done-at-scope-end
// arm. The slow-fault task finishes its delay BEFORE the using-scope ends, so
// scope-end disposal runs synchronously on an already-settled faulted task.
// Pre-fix this arm is also correct (codegen's bz_task_is_error(t) branch
// disposes the error_obj, then bz_task_fire_and_forget fast-path bz_task_free
// sees a null error_obj) — pinning it guards against regressing the fast path
// while fixing the slow path.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_faulted_task_error_obj_disposal_1601,
           using_var_already_faulted_at_scope_end) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kExc) +
        "class C "
        "  public static async method FastFault(): Task<int> "
        "    await Task.Delay(5); "
        "    throw new Exc(); "
        "  end FastFault; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var t := C.FastFault(); "   // faults inside the scope (await)
        "      await Task.Delay(120); "           // let the fault complete while t is bound
        "    end; "
        "    return Exc.n; "                      // expect 0 (sync-fast-path disposal)
        "  end Main; "
        "end C;"),
        0);
}
