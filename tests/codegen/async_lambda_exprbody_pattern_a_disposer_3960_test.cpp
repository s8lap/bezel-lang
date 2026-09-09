// Codegen / memory-unsafety — issue #3960 (§30.5 "Result lifetime and task
// disposal", kind 1, v0.295): `emitAsyncLambda`'s EXPRESSION-body completion
// branch never called the §30.5 kind-1/2/3 decision at all, so a fresh
// CLASS-typed value produced by an expression-bodied async lambda
// (`async lambda() => new Widget()`) attached NO result disposer to the
// outer task, while the byte-for-byte equivalent BLOCK-bodied lambda
// (`async lambda() => do return new Widget(); end`) attached one correctly.
//
// Spec (revision v0.295), §30.5 "Producer-side return-type rules":
//   "`Task<T>` for **class** T: ... Pattern A result slots are of three
//    kinds ... 1. A fresh owned value (a `return` of an owned expression):
//    the task **owns the result** and its destructor disposes it
//    directly."
// §30.6 "Async lambda expressions" (the expression body is an implicit
// return of that value — the same kind-1 rule must apply to it).
//
// THE DEFECT (root cause, differing from the issue's own hypothesis). The
// issue speculated the two body forms derive `declaredTaskTy` inconsistently
// (LlvmBackend_Exprs2.cpp's #3789 fallback). MEASURED here with an explicit
// call-site annotation (`var g: () -> Task<Widget> := async lambda() => ...`)
// so BOTH body forms derive the exact SAME non-null `declaredTaskTy` /
// `asyncPatternAResultDisposer_` — the disposer attach STILL differs. The
// real defect is structural: `emitAsyncLambda`'s expression-body branch
// (LlvmBackend_Async.cpp, the `if (lam.exprBody)` arm) called
// `emitAsyncComplete` directly and never called
// `emitAsyncPatternAResultDecision` (or its detection helper) at all — the
// call the block-body's `lowerReturnStmt` async arm always makes. The fix
// adds that call (via the shared `detectAsyncPatternAKind` /
// `deferChainedAsyncReturnSource` helpers, extracted from lowerReturnStmt's
// three pre-existing, already-identical copies of this logic so a third
// call site does not become a third drifting copy — LANE_RULES §6).
//
// MEASURED (BEZEL_DUMP_IR=1, `() -> Task<Widget3960>` annotation on both
// forms, HEAD 2e25915 pre-fix):
//   block body:  1 call to `bz_task_set_result_disposer`
//   expr  body:  0 calls
// After the fix: both emit exactly 1.
//
// AMPLIFIED RUNTIME PROOF (LANE_RULES §"loop-amplify every leak row" — a
// one-shot LSan row can pass on both sides because the allocation is still
// stack-reachable at exit): 300 iterations of "create the outer task via an
// unannotated `using var`, drop it without ever awaiting it" per body form.
// Pre-fix measured (this repo, scratch .bzl, pre-fix bezelc rebuilt from a
// reverted tree): expr body leaked all 300 Widget3960 instances (disposal
// counter `Widget3960.n` == 300 at exit, observed via the exit-code oracle
// as 300 mod 256 = 44); block body leaked 0. Post-fix: both 0.
//
// ORACLES. Every row runs on BOTH the plain and the ASan/LSan build (#2633:
// neither dominates), and proves reclaim POSITIVELY through a live-object
// counter folded into the exit code, so a leak and a premature free are
// each observable as a distinct exit code rather than as a silent pass.

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

// A disposal-counted class: `n` is the live count. §30.5 kind 1's "the task
// owns the result and its destructor disposes it directly" is then
// observable as a VALUE rather than only as an ASan verdict.
const char* kWidget =
    "class Widget3960 "
    "  public static n: int := 0; "
    "  public constructor() Widget3960.n := Widget3960.n + 1; end constructor; "
    "  destructor Widget3960.n := Widget3960.n - 1; end destructor; "
    "end Widget3960; ";

// AMPLIFICATION COUNT — per LANE_RULES, 200-500 iterations; 200 chosen so a
// fully-leaked run's exit code (200 mod 256 = 200) stays unambiguous and
// distinct from 0 (clean) without wrapping into a small, easy-to-confuse
// residue.
constexpr int kIters = 200;

// The kind-1 core repro: an explicit `() -> Task<Widget3960>` call-site
// annotation on BOTH body forms (isolating this from the separate, also-real
// declaredTaskTy-derivation gap the issue's own text names for the
// UNANNOTATED shape) — the outer task is bound via `using var` and DROPPED
// WITHOUT EVER BEING AWAITED, so the only path that can reclaim the fresh
// Widget3960 is the kind-1 result disposer attached to the outer task by
// `emitAsyncLambda`'s own completion. `%s` is substituted with the lambda
// body form under test.
std::string program(const std::string& lambdaBody) {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters) + " do "
        "      do "
        "        var g: () -> Task<Widget3960> := " + lambdaBody + "; "
        "        using var t := g(); "
        "      end; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

const char* kExprBody = "async lambda() => new Widget3960()";
const char* kBlockBody =
    "async lambda() => do "
    "          return new Widget3960(); "
    "        end";

// §30.5 kind-2 CHAINED control, expr-bodied — a NEW code path this fix adds
// (detectAsyncPatternAKind's CallExpr arm) for an expression-bodied async
// lambda whose whole body IS `await <anonymous producer>`. Must chain the
// producer's task into the outer task's slot exactly like the block-bodied
// twin (chained_return_scope_bound_source_3096_test.cpp's CallExpr control)
// — neither a leak (chaining skipped) nor a double-free (kind-1 disposer
// wrongly ALSO attached; the two arms are mutually exclusive by construction
// in emitAsyncPatternAResultDecision).
std::string kind2ChainedProgram(bool neverAwaited) {
    // issue #4119: the `neverAwaited` arm originally checked liveness only
    // AFTER the `do...end` scope closed, so a premature (pre-#3960-fix) free
    // and a correctly-deferred (post-fix) free both land on `n == 0` there -
    // the row could not discriminate. Mirroring the awaited sibling below,
    // `ChainAsync()`'s whole chain (ChainAsync -> f() -> await C3960.P()) has
    // no genuine suspension - C3960.P() never awaits anything, so per the
    // synchronous-prefix rule (spec §30.1, v0.295: "An exception thrown in
    // the synchronous prefix of an async method (before any await) faults
    // the returned task, not the immediate caller") the whole chain runs to
    // completion synchronously before `C3960.ChainAsync()` even returns.
    // `outer` is therefore already a COMPLETED task holding the chained
    // Widget3960 the moment it is bound - added intermediate check confirms
    // the widget is still alive at that point, before `outer`'s own
    // disposal at `end;`.
    std::string mainBody = neverAwaited
        ? "  do "
          "    using var outer := C3960.ChainAsync(); "
          "    if Widget3960.n <> 1 then return 94; end; "
          "  end; "
          "  if Widget3960.n <> 0 then return 92; end; "
          "  return 0; "
        : "  do "
          "    using var outer := C3960.ChainAsync(); "
          "    var c := await outer; "
          "    if Widget3960.n <> 1 then return 91; end; "
          "  end; "
          "  if Widget3960.n <> 0 then return 93; end; "
          "  return 0; ";
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method P(): Task<Widget3960> "
        "    return new Widget3960(); "
        "  end P; "
        "  public static async method ChainAsync(): Task<Widget3960> "
        "    var f := async lambda() => await C3960.P(); "
        "    return await f(); "
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> " + mainBody +
        "  end Main; "
        "end C3960;";
}

} // namespace

// The issue's own repro, made minimal and reproducible: an unannotated
// expression-bodied async lambda producing a fresh class value, amplified.
// Pre-fix this leaked ALL `kIters` Widget3960 instances (measured 200/200 —
// see the file banner for the 300-iteration ad-hoc measurement, 44 = 300 mod
// 256, that first found this).
BEZEL_TEST(async_lambda_exprbody_pattern_a_3960, exprbody_fresh_class_value_disposed_30_5) {
    const std::string src = program(kExprBody);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL — the block-bodied twin. Correct before and after; pins that the
// fix does not disturb the already-working form.
BEZEL_TEST(async_lambda_exprbody_pattern_a_3960, blockbody_fresh_class_value_disposed_30_5) {
    const std::string src = program(kBlockBody);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// §30.5 kind 2, expr-bodied, awaited: the chained source is disposed exactly
// once, transitively, by the outer task.
BEZEL_TEST(async_lambda_exprbody_pattern_a_3960, exprbody_chained_callexpr_source_awaited_30_5) {
    const std::string src = kind2ChainedProgram(/*neverAwaited=*/false);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// §30.5 kind 2, expr-bodied, outer task NEVER awaited — the leak direction:
// the chained source must still be reclaimed transitively at the outer
// task's teardown.
BEZEL_TEST(async_lambda_exprbody_pattern_a_3960, exprbody_chained_callexpr_source_unawaited_30_5) {
    const std::string src = kind2ChainedProgram(/*neverAwaited=*/true);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
