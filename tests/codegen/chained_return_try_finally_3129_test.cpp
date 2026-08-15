// fix #3129 — §30.5 Pattern A: a `return await <source>` exiting through a
// `try/finally` no longer chains the source task onto the outer task — and the
// kind-1 disposer attach lives only in the no-finally branch, so neither kind
// 1 nor kind 2 was reaching the §30.5 slot. Now both decisions are reached on
// every exit, hoisted into a single predicate-with-several-readers helper that
// fires before emitAsyncComplete on both branches.
//
// Spec authority (v0.252, §30.5):
//   - Pattern A *Result lifetime and task disposal* — "Pattern A result slots
//     are of three kinds" — every Pattern A body lands in exactly one of the
//     three on every body exit. Kind 2 (`return await` over an owned Pattern A
//     source) attaches the source to the outer task's chained slot;
//     "On both paths the source task is disposed exactly once — by the outer
//     task's destructor, transitively." Kind 1 attaches the per-`T` disposer.
//   - The "summary of kind 2" is unconditional — there is no fourth kind in
//     which the outer task records nothing.
//   - *Chaining from a scope-bound source* — "When the source is a `using
//     var` … the chained-slot arrangement defers its disposal from the
//     frame's scope end to the outer task's teardown … a disposal-*timing*
//     dispensation of the same kind as the await-consumes-operand rule."
//
// THE DEFECT (pre-fix, LlvmBackend_Stmts.cpp, the async ReturnStmt handler).
// Two gated-by-finallyStack_.empty() decisions combined:
//   1. The chained-return detection was inside the
//      `if (rs->value && smKind_ == SuspendKind::Await && smStateStruct_ &&
//      asyncResultTy_->isPointerTy() && finallyStack_.empty())` arm, so
//      `asyncChainedReturn_` stayed false on the routed-through-finally edge.
//   2. The kind-1 disposer attached only at the
//      `else if (v && asyncPatternAResultDisposer_)` arm of the same
//      no-finally branch.
// A `return await <source>` (or `return <fresh owned>`) routed through a
// `finally` therefore reached neither — the outer task's result slot recorded
// nothing, §30.5's fourth and unsound kind. A subsequent awaiter then
// borrowed the cell through a freed source task (the using-var scope-end
// disposal freed the result first, before the awaiter got it) — heap-use-
// after-free + double free through the kind-1 disposer.
//
// THE FIX (minimum-surface, the same shape as #3096's predicate hoisting):
//   - Drop `&& finallyStack_.empty()` from the chained-return detection; the
//     detection runs unconditionally. `asyncChainedReturn_` is now true even
//     when an active finally carries the return.
//   - On the routed-through-finally edge, record the chained flag onto the
//     active FinallyFrame (`fr.chainedAsyncReturn = chainedReturn`) and, for
//     a scope-bound owned source, null the source's local slot BEFORE
//     emitDisposalsInRange — the same per-edge null-the-slot deferral
//     `#3096` applied to the direct branch, so the body's scope-end
//     disposal skips the source task (the `task.faf` null guard's reason
//     for being).
//   - Hoist the chained/disposer decision into a single helper,
//     `Emitter::emitAsyncPatternAResultDecision(bool)`, called from the
//     no-finally branch AND from lowerTry's mode-5 dispatch (both the
//     outerFr and the terminal paths). The helper emits the same calls,
//     with the same race-safety ordering, on both exits.
//
// ORACLES. Every test runs on the plain AOT build AND on the ASan/LSan build
// (#2633: neither dominates), and observes reclaim through the live-object
// counter folded into the exit code, so a leak and a premature free show as
// distinct exit codes rather than a silent pass.

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

// `Cell.n` is the live count, `Cell.made` the construction count. Exactly-
// once disposal of `Cell` is then observable as a VALUE.
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public static made: int := 0; "
    "  public v: int; "
    "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; "
    "    Cell.made := Cell.made + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

// The §30.5-kind-2 body under test — `return await <source>` routed through a
// `try/finally`. Pre-fix: heap-use-after-free.
const char* kKind2ChainedThroughFinally =
    "  try "
    "    using var task := C.P(42); "
    "    return await task; "
    "  finally "
    "  end; ";

// Control: the same chain WITHOUT a finally — must still chain and dispose
// exactly once (the #3096 regression already covers this; included here so a
// fix that breaks the no-finally edge fails this lane's tests too).
const char* kKind2ChainedNoFinally =
    "  using var task := C.P(42); "
    "  return await task; ";

// §30.5 kind-1: a `return <fresh owned>` routed through a `try/finally` —
// the kind-1 per-`T` disposer used to be attached only by the no-finally
// branch, so the routed edge recorded no disposer and the cell leaked.
const char* kKind1FreshThroughFinally =
    "  try "
    "    return new Cell(7); "
    "  finally "
    "  end; ";

// Nested-finally exhaustiveness: the chained flag must propagate through
// every finally along the path. Two finallys + the routed `return await` —
// three finallys if the outer dispatch is reached through both, the simplest
// case is two nested finallys.
const char* kNestedFinally =
    "  try "
    "    try "
    "      using var task := C.P(42); "
    "      return await task; "
    "    finally "
    "    end; "
    "  finally "
    "  end; ";

// The standard AwaitThenDrop caller for the chained case: binds the outer
// task, awaits the chain, releases the task. §30.5 kind 2 requires the
// source task — and, transitively, the result — to be disposed exactly once,
// by the outer task's destructor.
const char* kAwaitThenDrop =
    "  do "
    "    using var t := C.ChainAsync(); "
    "    var c := await t; "
    "    if c.v <> 42 then return 91; end; "
    "    if Cell.n <> 1 then return 92; end; "
    "  end; "
    "  if Cell.n <> 0 then return 93; end; "
    "  if Cell.made <> 1 then return 94; end; "
    "  return 0; ";

// Caller for the kind-1 case: read result v=7, dispose exactly once.
const char* kKind1Caller =
    "  do "
    "    using var t := C.FreshAsync(); "
    "    var c := await t; "
    "    if c.v <> 7 then return 81; end; "
    "    if Cell.n <> 1 then return 82; end; "
    "  end; "
    "  if Cell.n <> 0 then return 83; end; "
    "  if Cell.made <> 1 then return 84; end; "
    "  return 0; ";

// The outer task is bound but NEVER awaited. The chained source must still
// be reclaimed transitively at the outer task's teardown — the direction
// that would LEAK if the new fr.chainedAsyncReturn flag were ever dropped
// between the ReturnStmt site and lowerTry's mode=5 dispatch (LSan is the
// oracle here).
const char* kNeverAwaitedDrop =
    "  do "
    "    using var t := C.ChainAsync(); "
    "  end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// The fault edge: the await re-raises BEFORE the chained return fires, so
// the using var must take its ordinary UNWIND disposal — exactly as it
// would without a finally. This pins that the per-edge disposal deferral
// from #3096 hasn't drifted.
const std::string kFaultEdge =
    "module M; " + std::string(kCell) +
    "class C "
    "  public static async method Boom(): Task<Cell> "
    "    throw new InvalidOperationException(\"boom\"); end Boom; "
    "  public static async method ChainAsync(): Task<Cell> "
    "    try "
    "      using var task := C.Boom(); "
    "      return await task; "
    "    finally "
    "    end; "
    "  end ChainAsync; "
    "  public static async method Main(): Task<int> "
    "    var caught := 0; "
    "    try "
    "      using var t := C.ChainAsync(); "
    "      var c := await t; "
    "      if c.v <> 0 then return 90; end; "
    "    catch var e: InvalidOperationException => caught := 1; "
    "    end; "
    "    if caught <> 1 then return 91; end; "
    "    if Cell.n <> 0 then return 92; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

std::string programKind2(const std::string& chainBody, const std::string& mainBody) {
    return std::string("module M; ") + kCell +
        "class C "
        "  public static async method P(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end P; "
        "  public static async method ChainAsync(): Task<Cell> " + chainBody +
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> " + mainBody +
        "  end Main; "
        "end C;";
}

std::string programKind1(const std::string& mainBody) {
    return std::string("module M; ") + kCell +
        "class C "
        "  public static async method FreshAsync(): Task<Cell> " +
        std::string(kKind1FreshThroughFinally) +
        "  end FreshAsync; "
        "  public static async method Main(): Task<int> " + mainBody +
        "  end Main; "
        "end C;";
}

}  // namespace

// ============================================================================
// The §30.5 kind-2 chained return routed through a `try/finally`.
// ============================================================================
BEZEL_TEST(chained_return_try_finally_3129,
           kind_2_chained_return_through_finally_30_5) {
    const std::string src = programKind2(kKind2ChainedThroughFinally, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// ============================================================================
// CONTROL — the no-finally kind-2 chain (#3096's covered shape). Must keep
// working byte-for-byte after the helper hoist. ============================================================================
BEZEL_TEST(chained_return_try_finally_3129,
           kind_2_chained_return_no_finally_unchanged_30_5) {
    const std::string src = programKind2(kKind2ChainedNoFinally, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// ============================================================================
// §30.5 kind-1: `return new Cell(...)` through `try/finally` — the disposer
// attach was also gated on finallyStack_.empty() pre-fix. Now reaches the
// outer task through lowerTry's mode-5 dispatch via the same helper. ============================================================================
BEZEL_TEST(chained_return_try_finally_3129,
           kind_1_disposer_attached_through_finally_30_5) {
    const std::string src = programKind1(kKind1Caller);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// ============================================================================
// Nested finallys: chained flag must propagate through every finally along
// the routed path. LSan is the oracle — a single lost propagation leaks
// the source task and Cell.n = 1 at scope exit. ============================================================================
BEZEL_TEST(chained_return_try_finally_3129,
           kind_2_chained_return_through_nested_finally_30_5) {
    const std::string src = programKind2(kNestedFinally, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// ============================================================================
// Transitively reclaim the chained source even when the outer task is bound
// but never awaited — the kind-2 deferral must hand ownership to the outer
// task's destructor, not just to an awaiting continuation. ============================================================================
BEZEL_TEST(chained_return_try_finally_3129,
           unawaited_outer_reclaims_source_through_finally_30_5) {
    const std::string src = programKind2(kKind2ChainedThroughFinally, kNeverAwaitedDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// ============================================================================
// Fault edge — the await re-raises BEFORE the chained return, so the using
// var must take its ordinary UNWIND disposal. Per-edge placement preserved. ============================================================================
BEZEL_TEST(chained_return_try_finally_3129,
           faulted_source_unwind_disposal_through_finally_30_9) {
    BEZEL_EXPECT_EQ(runPlain(kFaultEdge), 0);
    const int c = runChecked(kFaultEdge);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
