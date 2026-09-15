// Codegen / memory-unsafety — issue #4118 (§30.5 "Result lifetime and task
// disposal", kind 2, v0.295): `lowerLambda`'s ASYNC branch
// (`LlvmBackend_Exprs2.cpp`, the `if (lam.isAsync)` arm) never saved/reset
// `asyncChainedReturn_` / `asyncBorrowedReturn_` around its nested
// `emitAsyncLambda` call, so an inline `async lambda` IIFE nested directly in
// the operand of an ACTIVE `return await <call>` inherited the enclosing
// return statement's already-set Pattern-A kind flag, misclassified itself,
// and clobbered the flag back to `false` on its own way out — so the
// enclosing return statement's own post-operand-lowering read then saw
// `false` and ALSO misclassified itself. The two §30.5 kind decisions swap.
//
// Spec (v0.295), §30.5 "Producer-side return-type rules", Pattern A kind 2:
//   "The result is a borrow through the source task: ownership of the source
//    task transfers into the outer task's state (a chained slot), and the
//    outer destructor disposes the chained source task, never the borrowed
//    result directly."
//
// THE DEFECT, mechanically: `detectAsyncPatternAKind` (LlvmBackend_Async.cpp)
// EXPLICITLY sets `asyncChainedReturn_ = true` when it detects a genuine
// kind-2 `return await <CallExpr>`, but does NOT explicitly set it `false`
// for a genuine kind-1 body (a plain fresh-value return) — it just leaves the
// flag as whatever it already was. `SmFrameSaveScope` (which DOES wrap the
// nested `emitAsyncLambda` call) does not cover these two flags — per
// `LlvmBackend_impl.hpp`'s own frame-list comment, they are "deliberately
// absent ... saved where they are needed, by name" — so nothing gave the
// nested lambda's own kind-1 detection a clean slate.
//
// REPRO (this file's `program()`): `ChainAsync` is genuinely §30.5 kind-2
// (`return await (L)()` where `L` is an inline async-lambda IIFE — a bare
// CallExpr operand, `detectAsyncPatternAKind` legitimately sets
// `asyncChainedReturn_ = true` for ChainAsync BEFORE lowering `(L)()`).
// Lowering `(L)()` recursively compiles `L`'s own literal via
// `emitAsyncLambda`. `L`'s own body is itself `await C3960.P()` — ALSO
// genuinely kind-2 (chained onto `P()`'s real, non-null task) — so `L`
// correctly re-sets the (inherited, but irrelevant since about to be
// overwritten) flag to `true` for itself, chains correctly onto `P()`'s
// task, and then (correctly, for ITSELF) resets the global flag to `false`
// on its own way out. That `false` then leaks into ChainAsync's own
// post-operand read: ChainAsync misclassifies itself as kind-1 and attaches
// `bz_task_set_result_disposer` DIRECTLY to its own task instead of chaining
// onto `L`'s task.
//
// MEASURED (BEZEL_DUMP_IR=1, HEAD 2aa5b1d pre-fix vs this fix):
//   pre-fix:  ChainAsync$Resume:  bz_task_set_result_disposer(...)   [WRONG — should chain onto L]
//             asynclam$0$Resume (L): bz_task_set_chained(outer, P()_task)  [correct for L itself]
//   post-fix: ChainAsync$Resume:  bz_task_set_chained(outer, L_task)  [correct]
//             asynclam$0$Resume (L): bz_task_set_chained(outer, P()_task)  [unchanged, still correct]
//
// RUNTIME CONSEQUENCE (why this is a memory-safety bug, not just a wrong
// intrinsic): ChainAsync's own `lowerAwait` (for `await (L)()`) ALSO reads
// the clobbered `asyncChainedReturn_` to decide `freeTaskAfter` — since it
// reads `false` (clobbered), it frees `L`'s task IMMEDIATELY after
// extracting the result. `L`'s task is (correctly) chained onto `P()`'s
// task, so freeing it PREMATURELY disposes the widget produced by `P()` —
// while ChainAsync is still holding that SAME pointer as its own
// about-to-be-returned result. ChainAsync's own (wrongly-attached) direct
// disposer then fires a SECOND time on the same, already-disposed pointer
// when the caller's `using var` scope ends: a heap-use-after-free / double
// free, confirmed under ASan (see below) and as a SIGSEGV on the plain
// oracle.
//
// SCOPE NOTE: the ORIGINAL issue's own single-level repro (inner lambda body
// `new Widget3960()`, no further await — a genuine kind-1 inner) also swaps
// (confirmed via BEZEL_DUMP_IR both here and in the issue), but happens to
// be memory-safety-INVISIBLE for that specific shape: ChainAsync's wrongly-
// attached direct disposer still targets the correct final widget pointer,
// and `L`'s own wrongly-attached chained-with-null-source slot is a no-op,
// so the net dispose count is coincidentally right. This file uses the
// `await C3960.P()` inner shape instead (a genuine, non-null, competing
// chain source) specifically because it does NOT coincidentally cancel out
// — see `nested_iife_kind2_inner_swap_fixed_30_5` below for the measured
// crash. (Using the original single-level shape post-fix instead surfaces a
// SEPARATE, pre-existing, already-documented leak — the unannotated-IIFE
// `declaredTaskTy`-null gap #3960's own text predicted — filed as #4159;
// deliberately not asserted here.)
//
// ORACLES: plain (SIGSEGV pre-fix -> WIFSIGNALED -> `ran=false`) AND
// ASan/LSan (heap-use-after-free pre-fix -> `ran=false`) — neither dominates
// (#2633).

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

// Plain (non-instrumented) AOT run. A SIGSEGV is WIFSIGNALED -> ran=false.
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan+LSan run: a heap-use-after-free / double-free surfaces as -999.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent -> case skipped
    return r.ran ? r.exitCode : -999;
}

// Disposal-counted class: `n` is the live count.
const char* kWidget =
    "class Widget3960 "
    "  public static n: int := 0; "
    "  public constructor() Widget3960.n := Widget3960.n + 1; end constructor; "
    "  destructor Widget3960.n := Widget3960.n - 1; end destructor; "
    "end Widget3960; ";

// 200 per LANE_RULES amplification guidance; the underlying defect is a
// deterministic heap-use-after-free (not a probabilistic one), so a single
// iteration already reproduces it, but the loop stays for robustness and to
// match the amplification convention used across this defect class.
constexpr int kIters = 200;

// The Finding-2 repro from issue #4118, made mechanically demonstrable: an
// inline async-lambda IIFE (`L`) nested directly in the operand of
// ChainAsync's own ACTIVE `return await <call>`, where `L`'s own body is
// ITSELF a genuine kind-2 chain onto a real, non-null source task
// (`C3960.P()`'s task) — the shape that turns the flag swap into an
// observable double dispose rather than a coincidental cancellation.
std::string program() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method P(): Task<Widget3960> "
        "    return new Widget3960(); "
        "  end P; "
        "  public static async method ChainAsync(): Task<Widget3960> "
        "    return await (async lambda() => await C3960.P())(); "
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

// CONTROL — must be unaffected. The SAME inner-lambda shape
// (`async lambda() => await C3960.P()`), bound to a NAMED local (`f`) via
// its own, separate `var` DECLARATION statement — NOT an inline IIFE nested
// in the operand of an active `return await <call>`. `lowerLambda`'s async
// branch (the code this fix touches) still runs for `f`'s own literal, but
// with NO enclosing chained-return in flight to have set `asyncChainedReturn_`
// beforehand, so this fix's save(false)/reset(false)/restore(false) around
// the nested `emitAsyncLambda` call is a no-op here — this row must produce
// byte-for-byte the same result before and after #4118's fix. Mirrors the
// already-green `exprbody_chained_callexpr_source_unawaited_30_5` row in
// `async_lambda_exprbody_pattern_a_3960_test.cpp` (verified passing on both
// sides of this fix as part of this fix's own regression sweep).
//
// NOTE: an adjacent-looking shape — disposing `f()`'s own task DIRECTLY via
// `using var` without routing it through an intermediate `async method`
// first — hits a SEPARATE, pre-existing heap-use-after-free confirmed
// present identically before and after this fix (filed as #4162). This
// control deliberately routes through `ChainAsync` (as every existing
// `chained_return_*` / `async_lambda_exprbody_pattern_a_3960` test does) to
// stay clear of that unrelated defect.
std::string controlProgram() {
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

} // namespace

// The fix: pre-fix this SIGSEGVs (plain) / heap-use-after-frees (ASan) on
// the very first iteration. Post-fix: clean, Widget3960.n == 0 (every
// disposal happens exactly once, via the correctly-restored §30.5 chain
// ChainAsync -> L -> P()).
BEZEL_TEST(async_lambda_iife_chain_flag_swap_4118, nested_iife_kind2_inner_swap_fixed_30_5) {
    const std::string src = program();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL — the identical inner-lambda shape used WITHOUT an active
// enclosing chained return. Must be clean before and after the fix.
BEZEL_TEST(async_lambda_iife_chain_flag_swap_4118, nonnested_control_unaffected_30_5) {
    const std::string src = controlProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
