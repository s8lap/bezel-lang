// Codegen / memory-unsafety — issue #4162 (§30.5 "Disposing an unconsumed
// `OwningTask<T>`" / "Pattern A result slots are of three kinds", v0.298):
// pre-existing, confirmed UNRELATED to #4118 (byte-for-byte identical crash
// before and after that fix). `using var` binding an unawaited, CLOSURE-
// INVOKED chained `Task<T>`/`OwningTask<T>` directly (rather than through an
// intermediate `async method` wrapper) double-frees inside a loop.
//
// ROOT CAUSE. Two INDEPENDENT codegen sites can each legitimately attach a
// disposal obligation to the SAME task, with neither aware of the other:
//   1. `LlvmBackend_Disposal.cpp`'s `attachOwningTaskResultDisposer`
//      (invoked from the `using var` LOCAL registration path) attaches a
//      per-T `result_disposer` PURELY from the bound local's declared
//      `OwningTask<T:class>` type — it never inspects the bound
//      expression's PRODUCER.
//   2. The producer itself — here, `var f := async lambda() => await
//      C3960.P();`, an unannotated async lambda whose body is ITSELF
//      `await <Pattern A source>` — independently sets `chained_source`
//      via `bz_task_set_chained` in its own §30.5 kind-2 completion
//      (`emitAsyncPatternAResultDecision`), transferring disposal
//      responsibility for the BORROWED result to the chain (ultimately
//      `C3960.P()`'s own task, which owns and disposes it directly).
//
// `bz_task_free` (pre-fix) ran BOTH unconditionally: `outer`'s own
// `result_disposer` disposed the widget once (from `outer`'s own result
// buffer, which for a genuine kind-2 completion holds the exact SAME
// pointer value as the chain target's result — a borrow-alias, not an
// independently owned copy), then the `chained_source` recursion disposed
// the identical object again via `C3960.P()`'s own kind-1 disposer: a
// double free.
//
// This shape is NOT exercised by the existing `chained_return_*` /
// `async_lambda_exprbody_pattern_a_3960` test families, all of which route
// an unawaited chained task through an intermediate `async method` before
// disposing it via `using var` — that intermediate method's OWN completion
// takes the chained branch (never attaching its own `result_disposer`;
// `emitAsyncPatternAResultDecision`'s two branches are `if`/`else if`), so
// the conflict this issue measures never arises there. This repro instead
// disposes the closure-produced task DIRECTLY, so the `using var`
// registration's disposer and the closure's own chain collide on the same
// task.
//
// FIX (`bz_task_free`, `src/runtime/bezel_runtime_concurrency.cpp`): before
// invoking a task's own `result_disposer`, additionally check whether
// `chained_source` is set AND `chained_source->result` is STILL POPULATED
// — i.e. the chain still genuinely owns a live result. Only in that case is
// the disposer suppressed (the chain's own recursive `bz_task_free` will
// reach the real owner and dispose it there). Checking the chained
// source's own `result` field (not merely whether `chained_source` is
// set) is what distinguishes a genuine kind-2 BORROW-ALIAS (this issue)
// from #4159's ORPHANED Pattern-B extraction (where `chained_source` is
// set but already-drained/null — there, this task's own `result_disposer`
// is the ONLY thing that will ever dispose the value, and must still
// fire). Reading `chained_source->result` here is race-safe: by the time
// `bz_task_free` runs on a task, its `chained_source`'s own producer has
// already fully settled — the chain is always set before this task's own
// `bz_task_complete` runs — extending the "no other thread still touches
// the struct" invariant `bz_task_free` already relies on for its own
// fields.
//
// MEASURED (this repo, HEAD fbfd7a1 + #4118's + #4159's fixes, pre-#4162-fix):
//   - Plain oracle: SIGSEGV (WIFSIGNALED), identical on `2aa5b1d` (pre-#4118)
//     and post-#4118/#4159 — confirming this is unrelated to either.
//   - ASan/LSan: "heap-use-after-free" in `bz$elemDispose <- bz_task_free
//     <- bz$async$M.C3960$Main$Resume <- main`, freed by `main ->
//     bz$elemDispose` (the FIRST, erroneous free), previously allocated by
//     `bz$async$M.C3960$P$Resume <- bz$M.C3960$P <- lam$0 <- main`.
// Post-fix: both oracles clean, and (per the disposal-counting static
// field, returned as the exit code) the widget is disposed EXACTLY ONCE
// per iteration — this fix produces "freed exactly once" on every path,
// never a leak: the guard only ever SUPPRESSES a disposer invocation when
// the chain's own recursion will reach the SAME object's real owner.
//
// ORACLES: plain (SIGSEGV pre-fix -> WIFSIGNALED -> `ran=false`) AND
// ASan/LSan (heap-use-after-free pre-fix -> `ran=false`) — neither
// dominates (#2633).
//
// RELATED, NOT FIXED HERE: writing this file's own "outer is later
// AWAITED" accept-side control surfaced a THIRD, separate, still-open
// defect in the same family — filed as #4189 (confirmed independent of
// this fix). See the note beside `routedThroughAsyncMethodControlProgram`
// below for why that control was removed rather than shipped failing.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// Plain (non-instrumented) AOT run. A SIGSEGV is WIFSIGNALED -> ran=false -> -999.
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan+LSan run: a heap-use-after-free / double-free / leak surfaces as -999.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

// Disposal-counted class: `n` is the live count. Returning it as the exit
// code makes "freed exactly once" (n==0), "leaked" (n>0), and "double freed
// / corrupted" (crash, or n<0 surfaced via a nonzero unsigned-wrap exit)
// each independently observable rather than only "did not crash".
const char* kWidget =
    "class Widget3960 "
    "  public static n: int := 0; "
    "  public constructor() Widget3960.n := Widget3960.n + 1; end constructor; "
    "  destructor Widget3960.n := Widget3960.n - 1; end destructor; "
    "end Widget3960; ";

constexpr int kIters = 200;

// The issue's own primary repro: `f` is a named local holding a §30.5 kind-2
// chained async lambda (`await C3960.P()`); `outer := f()` is disposed via
// `using var` DIRECTLY (no intermediate async method), never awaited,
// amplified 200x.
std::string directClosureChainProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method P(): Task<Widget3960> "
        "    return new Widget3960(); "
        "  end P; "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters) + " do "
        "      do "
        "        var f := async lambda() => await C3960.P(); "
        "        using var outer := f(); "
        "      end; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

// Accept-side control: the SAME direct-closure-`using var` idiom, but the
// closure's own body is a genuine kind-1 fresh value (`new Widget3960()`,
// no further await) — `chained_source` is never set at all here, so the
// `using var` registration's disposer is the sole, unconflicted owner.
// Must stay correctly disposed exactly once (unaffected by this fix).
std::string directClosureNonChainedControlProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters) + " do "
        "      do "
        "        var f := async lambda() => new Widget3960(); "
        "        using var outer := f(); "
        "      end; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

// NOTE: an "outer is AWAITED before scope end" accept-side control
// (`using var outer := f(); using var w := await outer;`) was attempted
// here and DELIBERATELY REMOVED — it does not accept. It reproduces a
// SEPARATE, pre-existing, still-open defect (filed as #4189, confirmed
// independent of both this fix and #4159's: identical crash with and
// without this file's `bz_task_free` change): `bz_task_take_result` only
// nulls the AWAITED task's own `result` buffer, never a transitively
// chained source's, so the chain's own (always-set) disposer fires again
// later and double-frees the same object `w`'s own correct disposal
// already freed. Out of scope for #4162 — see #4189.

// Accept-side control: the same defect-triggering shape, but routed through
// an intermediate `async method` (the idiom every pre-existing
// `chained_return_*` / `async_lambda_exprbody_pattern_a_3960` test already
// exercises, per this file's own banner) — must remain correct, pinning
// that this fix does not disturb the already-working idiom.
std::string routedThroughAsyncMethodControlProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method P(): Task<Widget3960> "
        "    return new Widget3960(); "
        "  end P; "
        "  public static async method ChainAsync(): Task<Widget3960> "
        "    var f := async lambda() => await C3960.P(); "
        "    return await f(); "
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters) + " do "
        "      do "
        "        using var outer := C3960.ChainAsync(); "
        "      end; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

// Accept-side control: the defect shape outside a loop — single-shot
// correctness, not a leak/double-free-magnitude proof.
std::string singleShotOutsideLoopProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method P(): Task<Widget3960> "
        "    return new Widget3960(); "
        "  end P; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      var f := async lambda() => await C3960.P(); "
        "      using var outer := f(); "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

} // namespace

// The issue's own primary repro — pre-fix SIGSEGVs (plain) / heap-use-after-
// frees (ASan) on the first iteration; post-fix, disposed exactly once per
// iteration (n returns to 0).
BEZEL_TEST(using_var_direct_closure_chain_double_free_4162,
           direct_closure_chain_disposed_once_fixed_30_5) {
    const std::string src = directClosureChainProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Control — a non-chained direct closure `using var`. Unaffected.
BEZEL_TEST(using_var_direct_closure_chain_double_free_4162,
           direct_closure_non_chained_control_clean_30_5) {
    const std::string src = directClosureNonChainedControlProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Control — routed through an intermediate async method (the pre-existing,
// already-tested idiom). Unaffected.
BEZEL_TEST(using_var_direct_closure_chain_double_free_4162,
           routed_through_async_method_control_clean_30_5) {
    const std::string src = routedThroughAsyncMethodControlProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Control — the same shape outside a loop (single-shot correctness).
BEZEL_TEST(using_var_direct_closure_chain_double_free_4162,
           single_shot_outside_loop_clean_30_5) {
    const std::string src = singleShotOutsideLoopProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
