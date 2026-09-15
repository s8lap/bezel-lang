// Codegen / memory-unsafety — issue #4159 (§30.5 "Disposing an unconsumed
// `OwningTask<T>`" / "Producer-side return-type rules", v0.298): fix #4118
// unmasked (did not cause) a pre-existing leak for an unannotated inline
// `async lambda` IIFE — or an equivalent NAMED local — whose own body is a
// genuinely kind-1 fresh owned class value, used as the source of an active
// §30.5 `return await <call>` in an ENCLOSING async body.
//
// ROOT CAUSE. An unannotated async lambda's own inferred function type wraps
// a fresh owned class body into `OwningTask<T>` (Pattern B), not `Task<T>`
// (Pattern A) — ExprTypecheck_Exprs.cpp's #800-Layer-2 wrap
// (`ownedResult` is true whenever the inferred body type is a class, with no
// call-site annotation to override it). `detectAsyncPatternAKind`'s CallExpr
// arm (LlvmBackend_Async.cpp) does not distinguish this from a genuine
// Pattern A `Task<T:class>` source — both set `asyncChainedReturn_`, so the
// ENCLOSING body (`ChainAsync` below) chains onto the inner task via
// `bz_task_set_chained`. `lowerAwait`'s own Pattern B "owned-consuming"
// handling (`isOwnedTask && !freeTaskAfter`) correctly EXTRACTS the widget
// pointer via `bz_task_take_result` before the chain is set — but the inner
// lambda's OWN kind-1 completion never attached a disposer to itself
// (`patternAResultDisposerFor`'s Task-only guard excludes `OwningTask`), so
// once its result buffer is nulled by `bz_task_take_result`, NOTHING will
// ever dispose the extracted widget: chaining onto a now-empty, disposer-
// less task is a no-op, and the enclosing body's own completion took the
// CHAINED branch (not kind-1), so it never attached its own disposer either.
//
// FIX (LlvmBackend_Stmts.cpp's `emitAsyncPatternAResultDecision`, the
// `chainedReturn` arm; new runtime query `bz_task_needs_caller_disposal`,
// `src/runtime/bezel_runtime_concurrency.cpp` / `bezel_runtime.h`): right
// after `bz_task_set_chained`, query whether the just-chained source task
// settled with NEITHER its own `chained_source` NOR its own
// `result_disposer` set. Only in that case (the source was a genuinely
// orphaned Pattern-B kind-1 extraction) does the enclosing body ALSO attach
// its own kind-1 disposer to the value it just extracted. A genuine Pattern
// A chain source (or a Pattern B source that is itself further chained, as
// in #4118's own regression test) always answers "already handled" — the
// query is a RUNTIME fact, not a static Task-vs-OwningTask guess, which is
// what lets this distinguish the two without double-freeing #4118's own
// still-green regression rows (a naive "always attach when chainedReturn
// AND the source's static type is OwningTask" first attempt at this fix
// broke exactly those two rows — see the fix's commit body).
//
// Spec (v0.298), §30.5 "Disposing an unconsumed `OwningTask<T>`": "If a
// `OwningTask<T>` is dropped or disposed before its result has been
// transferred ... disposing the task disposes the result if one was
// produced ... This makes 'fire and forget then drop' safe for owned-result
// tasks." The inner lambda's task here is never explicitly `using var`-bound
// (it is an anonymous chain source), but the SAME obligation applies: its
// produced-but-never-transferred-onward-through-a-real-disposer result must
// still be disposed exactly once.
//
// MEASURED (this file's own probes, HEAD fbfd7a1 + #4118's fix, pre-this-fix):
//   - Plain oracle: exit code 200 (all 200 leaked) for the amplified repro.
//   - ASan/LSan: "Direct leak of 3200 byte(s) in 200 object(s)" via
//     bz$asynclam$0$Resume <- lam$0 <- bz$M.C3960$ChainAsync <- main.
// Post-fix: both oracles clean (exit 0 / no leak report).
//
// ORACLES: plain (leaked count IS the exit code, via a disposal-counting
// static field) AND ASan/LSan — neither dominates (#2633).

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

// 200 per LANE_RULES amplification guidance — a one-shot leak row can pass
// on both sides (the allocation is still stack-reachable at exit).
constexpr int kIters = 200;

// Row A (the issue's own primary repro): an INLINE, unannotated async-lambda
// IIFE nested directly in the operand of ChainAsync's own active
// `return await <call>`, whose body is a bare `new Widget3960()` — a
// genuinely kind-1 fresh value, no further await. `outer` is dropped via
// `using var` WITHOUT ever being awaited, amplified 200x.
std::string inlineIifeProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method ChainAsync(): Task<Widget3960> "
        "    return await (async lambda() => new Widget3960())(); "
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

// Row B (accept-side grid: "a non-IIFE async lambda") — the IDENTICAL kind-1
// bare-value body, but bound to a NAMED local `f` first rather than invoked
// inline. The defect (and the fix) is keyed on RUNTIME task state, not on
// the AST shape of the call operand, so this must be fixed identically.
std::string namedLocalProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method ChainAsync(): Task<Widget3960> "
        "    var f := async lambda() => new Widget3960(); "
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

// Row C (accept-side grid: "an annotated async lambda") — the call-site
// `() -> Task<Widget3960>` annotation makes `haveAnnotation` true in
// lowerLambda, so `declaredTaskTy` is `Task<Widget3960>` (Pattern A) and
// `patternAResultDisposerFor` already attaches a disposer directly (#3960's
// own fix, unaffected by and unrelated to this one — the annotated shape
// never reaches this fix's new runtime query at all, since the inner lambda
// is not chained through Pattern-B extraction in the first place). Control:
// must stay clean before and after this fix.
std::string annotatedControlProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters) + " do "
        "      do "
        "        var g: () -> Task<Widget3960> := async lambda() => new Widget3960(); "
        "        using var t := g(); "
        "      end; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

// Row D (accept-side grid: "using var over a normally-awaited task") — the
// SAME leak-triggering inline-IIFE shape, but ChainAsync's own result is
// AWAITED (not merely dropped) before the caller's `using var` scope ends.
// Must dispose exactly once either way.
std::string awaitedThenDisposedProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method ChainAsync(): Task<Widget3960> "
        "    return await (async lambda() => new Widget3960())(); "
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters) + " do "
        "      do "
        "        using var outer := C3960.ChainAsync(); "
        "        var w := await outer; "
        "        if Widget3960.n <> 1 then return 77; end; "
        "      end; "
        "      if Widget3960.n <> 0 then return 78; end; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

// Row E (accept-side grid: "the same shapes outside a loop") — Row A's exact
// shape, single iteration, no amplification: a plain correctness check
// (exit code / disposal count), not a leak-magnitude proof.
std::string singleShotOutsideLoopProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method ChainAsync(): Task<Widget3960> "
        "    return await (async lambda() => new Widget3960())(); "
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var outer := C3960.ChainAsync(); "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

// Row F (no-double-free control, local twin of
// async_lambda_iife_chain_flag_swap_4118_test.cpp's own
// nested_iife_kind2_inner_swap_fixed_30_5): an INLINE IIFE whose own body is
// ITSELF `await C3960.P()` — a genuine Pattern-A kind-2 chain onto a real,
// non-null, disposer-bearing task. The new runtime query
// (bz_task_needs_caller_disposal) must answer "already handled" here (P()'s
// own task always carries its own kind-1 disposer, always has, unaffected
// by this fix), so ChainAsync must NOT ALSO attach a second disposer —
// otherwise this row double-frees. Kept in THIS file (not just #4118's) so
// a future change to emitAsyncPatternAResultDecision is caught by both
// issues' own regression suites.
std::string noDoubleFreeChainControlProgram() {
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

} // namespace

// Row A — the issue's own primary repro. Pre-fix: exit 200 (all leaked) /
// ASan reports 3200 B in 200 objects. Post-fix: clean.
BEZEL_TEST(async_lambda_iife_owningtask_orphan_disposer_4159,
           inline_iife_kind1_owningtask_orphan_fixed_30_5) {
    const std::string src = inlineIifeProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Row B — identical defect via a NAMED local instead of an inline IIFE.
BEZEL_TEST(async_lambda_iife_owningtask_orphan_disposer_4159,
           named_local_kind1_owningtask_orphan_fixed_30_5) {
    const std::string src = namedLocalProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Row C — accept-side control: an annotated async lambda. Must stay clean.
BEZEL_TEST(async_lambda_iife_owningtask_orphan_disposer_4159,
           annotated_async_lambda_control_unaffected_30_5) {
    const std::string src = annotatedControlProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Row D — accept-side control: awaited (not merely dropped) before disposal.
BEZEL_TEST(async_lambda_iife_owningtask_orphan_disposer_4159,
           using_var_over_normally_awaited_task_clean_30_5) {
    const std::string src = awaitedThenDisposedProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Row E — accept-side control: the same shape outside a loop (single-shot
// correctness, not a leak-magnitude proof).
BEZEL_TEST(async_lambda_iife_owningtask_orphan_disposer_4159,
           single_shot_outside_loop_clean_30_5) {
    const std::string src = singleShotOutsideLoopProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Row F — no-double-free control: a genuine Pattern A chain (inner IIFE body
// itself `await C3960.P()`) must NOT get a second disposer attached.
BEZEL_TEST(async_lambda_iife_owningtask_orphan_disposer_4159,
           no_double_free_genuine_chain_control_30_5) {
    const std::string src = noDoubleFreeChainControlProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
