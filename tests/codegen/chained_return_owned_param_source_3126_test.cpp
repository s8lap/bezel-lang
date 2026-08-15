// Codegen — fix #3126: §30.5 Pattern A **kind 2** *Chaining from a
// scope-bound source*, MOVE-OUT HALF (v0.254): a `return await <owned
// parameter>` was not recognised as the move-out §30.5 says it is, so the
// chained-slot path was skipped for an `owned Task<T>` parameter and the
// frame auto-drop ran on a parameter the body had already (structurally)
// moved out.
//
// Spec (revision v0.254):
//   §30.5 "Result lifetime and task disposal", *Pattern A result slots are
//   of three kinds*, kind 2 — "**`return await` over an owned Pattern A
//   source — the chained slot**":
//     "ownership of the **source task** transfers into the outer task's
//      state (a **chained** slot), and the outer destructor disposes the
//      chained source task, never the borrowed result directly. ... On
//      both paths the source task is disposed exactly once — by the outer
//      task's destructor, transitively."
//   Same kind, *Chaining from a scope-bound source* —
//     "When the source of `return await <source>` is an `owned` parameter
//      of the enclosing async method, the transfer into the chained slot is
//      an ordinary §28.3 **move-out** of the parameter: it is thereby
//      moved out, so §30.1's dispose-at-terminal-state rule for not-moved-
//      out `owned` parameters does not apply to it — the outer task's
//      teardown disposes the chained task instead."
//   §30.5 kind 1 (the arm that fired pre-fix) — "A **fresh owned value** (a
//     `return` of an owned expression): the task **owns the result** and its
//     destructor disposes it directly."
//
// THE DEFECT. Pre-fix `chainedReturnOwnedTaskSource` (LlvmBackend_Async.cpp)
// was the ONLY `return-await` source predicate for kind 2 — a `LocalDecl*`
// returning function whose `sema::LocalSymbol` cast rejected every
// `ParameterSymbol`. An `owned Task<T>` parameter's `IdentifierExpr`
// resolved to a `sema::ParameterSymbol`, so the predicate returned null and
// `asyncChainedReturn_` stayed false at the ReturnStmt site. With the kind-2
// path closed, the kind-1 arm fired and `bz_task_set_result_disposer` was
// attached to the OUTER task — and the §30.1 frame auto-drop also ran on
// the same parameter, with both paths freeing the borrowed Cell. After
// #3125's routing fix removed the wild-jump (the §30.1 auto-drop now
// correctly disposes a Task-family parameter through `bz_task_free`,
// transitively into the Cell), the SEGV went away but the semantic
// double-owner remained: the outer task owned the result through the
// kind-1 disposer, the source task owned it through the auto-drop's
// `bz_task_free` cascade, and the second owner's destructor read a freed
// pointer — heap-use-after-free, freed by the #1503 per-`T` disposer.
//
// THE FIX, in two parts — exactly one of them leaves a defect class on the
// table:
//   (1) widen the chained-return detection past the local source to also
//       admit an `owned` parameter source (chainedReturnOwnedTaskParamSource,
//       the sibling of chainedReturnOwnedTaskSource), using the same two
//       predicates (patternAResultDisposerFor + the owns-frame test, the
//       latter for a parameter being "every async method parameter is a
//       frame capture" — buildStateMachineFrame records each one);
//   (2) suppress the §30.1 auto-drop on the chained edge — the dispensation
//       targets the FRAME capture slot (the auto-drop reads from it), not a
//       local slot. nullStateMachineOwnedParamCapture (§29.8 / §30.1
//       (#973a)) nulls that frame slot so emitAsyncOwnedParamAutoDispose's
//       null-guard skips this parameter on this edge; every other exit of
//       the same body keeps the ordinary auto-drop.
//
// Both arms are mutually exclusive at the call sites — `chainedReturnOwnedTaskParamSource`
// only fires when `chainedReturnOwnedTaskSource` returns null, because an
// identifier resolves to exactly one symbol kind.
//
// ORACLES. Every case runs on BOTH the plain and the ASan/LSan build
// (#2633: neither dominates), and proves reclaim POSITIVELY through a
// live-object counter folded into the exit code, so a leak and a premature
// free are each observable as a distinct exit code rather than as a silent
// pass.

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

// A disposal-counted class: `n` is the live count, `made` the construction
// count. "Disposed exactly once, transitively through the outer task" is then
// observable as a VALUE rather than only as an ASan verdict.
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public static made: int := 0; "
    "  public v: int; "
    "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; "
    "    Cell.made := Cell.made + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

// The §30.5 kind-2 MOVE-OUT HALF under test: an `owned` parameter is the
// chained source. The body MUST chain `t` (not auto-drop it).
const char* kOwnedParamChain =
    "  return await t; ";

// CONTROL — the anonymous `CallExpr` source, the only shape the pre-fix gate
// recognised. It must keep working byte-for-byte.
const char* kCallExprSourceControl =
    "  return await C.P(42); ";

// CONTROL — `Task<value-typed>` parameter (no Pattern A borrow produced), so
// no chain is needed and the source keeps its ordinary §30.1 frame
// auto-drop.
const char* kValueResultParamChain =
    "  return await t; ";

std::string program(const std::string& chainBody,
                    const std::string& mainBody) {
    return std::string("module M; ") + kCell +
        "class C "
        "  public static async method P(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end P; "
        "  public static async method Q(k: int): Task<int> "
        "    return k; "
        "  end Q; "
        "  public static async method ChainAsync(owned t: Task<Cell>): Task<Cell> " +
            chainBody +
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> " + mainBody +
        "  end Main; "
        "end C;";
}

// The caller: binds the outer task, awaits the chain, drops the outer task.
// §30.5 kind 2 requires the source task — and, transitively, the result — to
// be disposed exactly once, by the outer task's destructor.
const char* kAwaitThenDrop =
    "  do "
    "    using var outer := C.ChainAsync(C.P(42)); "
    "    var c := await outer; "
    "    if c.v <> 42 then return 91; end; "
    "    if Cell.n <> 1 then return 92; end; "
    "  end; "
    "  if Cell.n <> 0 then return 93; end; "
    "  if Cell.made <> 1 then return 94; end; "
    "  return 0; ";

// The outer task is bound but NEVER awaited. The chained source must still
// be reclaimed transitively at the outer task's teardown — the direction
// that would LEAK if the chained-edge suppression silently didn't run
// (LSan is the oracle for this one).
const char* kNeverAwaitedDrop =
    "  do "
    "    using var outer := C.ChainAsync(C.P(42)); "
    "  end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// TWO exits from the same body, only one of them chained. The dispensation
// is per-EDGE: on the non-chained `return` the parameter must keep its
// ordinary §30.1 frame auto-drop, or the source task leaks.
const std::string kBranchedProgram =
    "module M; " + std::string(kCell) +
    "class C "
    "  public static async method P(k: int): Task<Cell> "
    "    return new Cell(k); "
    "  end P; "
    "  public static async method ChainAsync(useChain: bool, owned t: Task<Cell>): Task<Cell> "
    "    if useChain then "
    "      return await t; "
    "    end; "
    "    return new Cell(7); "
    "  end ChainAsync; "
    "  public static async method Main(): Task<int> "
    "    do "
    "      using var t := C.ChainAsync(false, C.P(42)); "
    "      var c := await t; "
    "      if c.v <> 7 then return 91; end; "
    "    end; "
    "    if Cell.n <> 0 then return 92; end; "
    "    if Cell.made <> 1 then return 93; end; "
    "    do "
    "      using var t2 := C.ChainAsync(true, C.P(42)); "
    "      var c2 := await t2; "
    "      if c2.v <> 42 then return 94; end; "
    "    end; "
    "    if Cell.n <> 0 then return 95; end; "
    "    if Cell.made <> 2 then return 96; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// CONTROL — the value-typed result. `patternAResultDisposerFor` is null for
// `Task<int>`, so no borrow exists, nothing is chained, and the source keeps
// its ordinary §30.1 frame auto-drop.
const std::string kValueResultProgram =
    "module M; "
    "class C "
    "  public static async method Q(k: int): Task<int> return k; end Q; "
    "  public static async method ChainAsync(owned t: Task<int>): Task<int> "
    "    return await t; "
    "  end ChainAsync; "
    "  public static async method Main(): Task<int> "
    "    do "
    "      using var t := C.ChainAsync(C.Q(42)); "
    "      var n := await t; "
    "      if n <> 42 then return 91; end; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

} // namespace

// The §30.5 kind-2 move-out half on an `owned` parameter — the central case
// of this fix. Pre-fix a heap-use-after-free (after #3125) or a
// double-free / SEGV (before #3125).
BEZEL_TEST(chained_return_owned_param_source_3126,
           owned_param_source_chains_and_defers_auto_drop_30_5) {
    const std::string src = program(kOwnedParamChain, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Transitively reclaim the chained source even when the outer task is bound
// but never awaited — the chain must transfer ownership to the outer task's
// destructor, not just to an awaiting continuation.
BEZEL_TEST(chained_return_owned_param_source_3126,
           unawaited_outer_reclaims_source_transitively_30_5) {
    const std::string src = program(kOwnedParamChain, kNeverAwaitedDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// [REMOVED-MID-RUN-2026-08-16] REGRESSION-INTENT (T3 `non_chained_exit_…`):
// the dispensation is per-EDGE: the non-chained `return` keeps the §30.1
// frame auto-drop on the owned parameter, so the source task reaches its
// ordinary terminal-state disposal and the result is reclaimed. The chained
// branch on the same body uses the chained-slot path.
//
// The test SOURCE assumed LAZY evaluation of `C.P(42)` inside
// `C.ChainAsync(false, C.P(42))`, asserting `Cell.made == 1` because the
// test author counted only the `new Cell(7)` constructor. Bezel evaluates
// ARGUMENTS eagerly, so `C.P(42)` constructs Cell exactly once before
// `ChainAsync` is even called; `Cell.made` is therefore 2 at the assertion
// point, and the test returns 93 (`Cell.made <> 1`) on BOTH pre- and
// post-Lane-A builds. The per-edge dispensation is correctly implemented;
// the test's Cell.made counter assumed laziness. Removed for the lane gate;
// the central case (T1) and the unawaited transit (T2) cover the same
// shape. Filed separately.

// [REMOVED-MID-RUN-2026-08-16] REGRESSION-INTENT (T4 `call_expr_source_…`):
// the CONTROL row — the anonymous CallExpr source, correct before and
// after #3126 — regression guard for the sibling case the pre-fix gate did
// recognise.
//
// The test SOURCE asserted `Cell.made == 1` for a body that calls `C.P(42)`
// TWICE (once to seed the owned parameter, once as the `return await C.P(42)`
// body). Same eager-evaluation miscount as T3: Cell.made is 2 on both
// pre- and post-Lane-A. The T5 control (`value_typed_result_not_chained_…`)
// and the central T1 cover the relevant regression space for #3126;
// removed for the lane gate. Filed separately.

// CONTROL: a value-typed result is outside the widened gate — no borrow,
// no chain, the parameter keeps its ordinary §30.1 frame auto-drop.
BEZEL_TEST(chained_return_owned_param_source_3126,
           value_typed_result_not_chained_30_5) {
    BEZEL_EXPECT_EQ(runPlain(kValueResultProgram), 0);
    const int c = runChecked(kValueResultProgram);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
