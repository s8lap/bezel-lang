// Codegen/runtime — §30.9 v0.240 fault ownership (#3030):
// `Task.FireAndForget(owned task)`'s already-done FAST path must dispose the
// task's owned fault exception BEFORE `bz_task_free` runs. The slow path does
// (that is #1601 item 4 / #1021); the fast path used to call `bz_task_free`
// and nothing else, leaking the entire exception object — and with it,
// anything the exception owns.
//
// Bug (#3030). §30.9 (v0.240) says a task owns its fault exception and
// "disposes it exactly once, by the task that owns it, when the task is
// disposed". The slow path's `bz_task_then` continuation does the right
// thing: it inspects `error_obj` under the `!fault_borrowed` guard and calls
// `bz_dispose_class_obj` before `bz_task_free`. The FAST path (the
// `already_done` arm of `bz_task_fire_and_forget`) had no such logic — the
// `bz_task_free` reclaim deliberately ignores `error_obj` (its own comment
// scopes it to the *result*, not the fault), and the prior comment on the
// FAST path claimed "codegen's scope-end error_obj disposal runs before this
// call" — which is true for the `emitTaskDisposal` scope-end path (the
// preamble does dispose `error_obj` for faulted tasks BEFORE calling
// `bz_task_fire_and_forget`), but is FALSE for the explicit
// `Task.FireAndForget(t)` user-facing call (LlvmBackend_Calls.cpp §6.7 arm
// in `LlvmBackend_Calls.cpp` calls `bz_task_fire_and_forget` directly, with
// no preamble). On the FAST path with a task that is already-done AND
// faulted (the synchronous-throw shape), the exception object — a Bezel
// heap allocation distinct from the task struct — never had its destructor
// run, and was leaked.
//
// SPEC §30.9 v0.240 (cited per fix #3030): "A faulted task owns its fault
// exception ... disposed exactly once, by the task that owns it, when the
// task is disposed." Nothing in §30.9 or §30.10 conditions that on whether
// the task had settled by the time ownership transferred. The fix mirrors
// the slow path's guarded disposal in the FAST path before `bz_task_free`,
// with the same `!fault_borrowed` guard so a `WhenAll`/`WhenAll<T>`
// aggregate that BORROWED its fault from an input task (§30.10 / #1529) is
// still left alone.
//
// Repro. A synchronous-throw async method (no `await` before the `throw`)
// is already complete with `error_obj` set when `Task.FireAndForget` takes
// it — the FAST path. Pre-fix: ASan/LSan reports a Direct leak of the
// exception object (40 bytes for a plain `new Exception("plain")`; 112 bytes
// for `new Exception("wrap", root)` because the cause chain on the
// exception is itself owned). Post-fix: leak-free.
//
// Oracle. A custom `Exc` class with a static live-count (ctor ++, dtor --).
// After the abandoned task settles and Main returns the count: 0 ==
// dispose-exactly-once, >0 == leak, <0 == double-free. runAotChecked
// attaches ASan + LSan so a leak is a hard sanitizer failure
// (r.ran==false -> -999) on top of the count oracle.
//
// Scope. This test pins the FAST-path `bz_task_fire_and_forget` fix for
// #3030. The slow-path disposal is separately pinned by the
// `async_faulted_task_error_obj_disposal_1601` test family
// (tests/codegen/async_faulted_task_error_obj_disposal_1601_test.cpp). The
// fix is deliberately scoped to the FAST path; the SLOW path already had
// the right disposal from #1601 and is unchanged.
//
// Axes covered (verifies the AXIS GRID pre-flight):
//   1. FAULT + FAST (the bug)        — synchronous `throw` + FireAndForget
//   2. NO fault + FAST (control)     — Task.FromResult + FireAndForget
//   3. FAULT + FAST, Task<T> shape   — Task.FromException<int> + FireAndForget
//   4. FAULT + SLOW (control)        — already-done at scope-end (the path
//        #1601's "using_var_already_faulted_at_scope_end" pins, restated
//        under the explicit FireAndForget semantics to confirm the SLOW
//        path is unaffected by the fix). For completeness, the slow path
//        via an `await Task.Delay` before the fault is the #1601 family
//        and is not duplicated here.

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
// Reclaims the count via the destructor. With ASan/LSan attached, ANY leak
// (count > 0 OR live > 0 at exit) fails the test even before the count oracle.
const char* kExc =
    "class Exc: Exception public static n: int := 0; "
    "  public constructor() base(\"e\"); Exc.n := Exc.n + 1; end constructor; "
    "  destructor Exc.n := Exc.n - 1; end destructor; "
    "end Exc; ";

}  // namespace

// ---------------------------------------------------------------------------
// AXIS 1 (BUG REPRO): faulted task + FAST path. A synchronous-throw async
// method is already-done (error_obj set) the moment `Task.FireAndForget`
// receives it. Pre-fix the FAST path's `bz_task_free` reclaims the task
// struct + error_msg but NOT the exception object — the 40-byte Bezel
// exception leaks. Pre-fix return = -999 (ASan LSan); post-fix return = 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_task_fireandforget_fast_path_disposes_fault_3030,
           synchronous_throw_fast_path_disposes_fault) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kExc) +
        "class C "
        "  public static async method Boom(): Task "
        "    throw new Exc(); "                   // synchronous — no await
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(Boom()); "        // FAST path: already done
        "    await Task.Delay(60); "              // let any stragglers settle
        "    return Exc.n; "                      // expect 0: exactly-once
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// AXIS 2 (CONTROL): non-faulted task + FAST path. A successful
// `Task.FromResult<int>(42)` reaches the FAST path with error_obj == null.
// The FAST path's pre-fix code was correct for this cell (nothing to
// dispose); the fix's `if (error_obj && !fault_borrowed)` guard must remain
// a no-op here. Pinning this guards against a regression where the fix
// e.g. accidentally tries to dipose a null pointer.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_task_fireandforget_fast_path_disposes_fault_3030,
           non_faulted_fast_path_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kExc) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(Task.FromResult<int>(42)); "  // FAST, no fault
        "    await Task.Delay(30); "
        "    return Exc.n; "                      // expect 0: no exception created
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// AXIS 3 (CONTROL): faulted Task<T> + FAST path. `Task.FromException<int>(X)`
// constructs an already-faulted task carrying the exception object. Same
// FAST path; same required disposal. Confirms the shape `Task<int>` carries
// the same `error_obj` ownership as `Task` (the spec text is unconditional
// on the family). Pre-fix return = -999 (ASan LSan); post-fix return = 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_task_fireandforget_fast_path_disposes_fault_3030,
           task_int_from_exception_fast_path_disposes_fault) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kExc) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget( "
        "      Task.FromException<int>(new Exc())); "  // FAST, faulted
        "    await Task.Delay(30); "
        "    return Exc.n; "                      // expect 0: exactly-once
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// AXIS 4 (CONTROL): faulted task + SLOW path. Mirror the #1601 family
// through the explicit `Task.FireAndForget` semantics: a task that is
// STILL RUNNING at the moment `Task.FireAndForget` takes it (slow path)
// faults via the worker pool and the slow-path continuation must dispose
// its error_obj. This is the SAME path #1601 fixed; restating it here
// confirms the fix does not regress the slow path when FireAndForget is
// the abandonment idiom (rather than the `using var` scope-end idiom).
// Pre-fix (both before #1601 and after #3030 — slow path was already
// correct) return = 0; this is a regression-guard, not a leak-flip.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_task_fireandforget_fast_path_disposes_fault_3030,
           slow_path_still_running_at_faf_then_faults_later) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kExc) +
        "class C "
        "  public static async method SlowBoom(): Task "
        "    await Task.Delay(30); "
        "    throw new Exc(); "                   // still running at FAF
        "  end SlowBoom; "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(SlowBoom()); "    // SLOW path: not yet done
        "    await Task.Delay(120); "             // let it settle + dispose
        "    return Exc.n; "                      // expect 0: exactly-once
        "  end Main; "
        "end C;"),
        0);
}
