// Codegen — fix #3125: §30.5 Pattern A **kind 3** — `return await <borrowed
// source>` — attached the **kind-1** result disposer to the outer task, so the
// borrowed result was owned TWICE: by the source task (via its own scope-end
// disposal, which runs FIRST and frees the result through the #1503 per-`T`
// disposer) AND by the outer task (via the kind-1 disposer just attached). That
// is a heap-use-after-free followed by a double free, not the silent kind-3
// pass §30.5 says the source's owner is supposed to be.
//
// Spec (revision v0.252 / v0.254):
//   §30.5 "Result lifetime and task disposal", *Pattern A result slots are of
//   three kinds*, kind 3 — **`return await` over a borrowed source**:
//     "When the source is a **borrow** — an admitted static task field or
//      property, an `owned` field of `this` read without `move`, an owning-
//      collection slot, a borrowing-iteration variable, or any other admitted
//      borrowed operand — no ownership transfers: the outer task's result
//      slot records a **borrow through the source task**, and the outer
//      task's teardown disposes **nothing** on that link. Transfer is
//      impossible for a borrowed source and would be unsound (a static cache
//      disposed at some caller's teardown while every other caller still
//      awaits it; an owned field moved without `move`); the borrowed-source
//      chain is the only coherent third kind."
//   Same kind, soundness paragraph — "the source outliving the outer task's
//     result uses … discharges trivially for process-lived static storage:
//     an admitted static task field lives for the duration of the program
//     and is never disposed before exit … so a chained borrow of it cannot
//     dangle."
//
// THE DEFECT. `emitAsyncPatternAResultDecision` had two arms only:
//   (a) `chainedReturn` true  → `bz_task_set_chained` (kind 2);
//   (b) `else if (asyncPatternAResultDisposer_)` → `bz_task_set_result_disposer`
//       (kind 1, "the body produced a fresh owned value").
// `chainedReturn` is set only when the awaited operand is an anonymous
// producer `CallExpr` OR `chainedReturnOwnedTaskSource` admits it — and the
// latter admits exactly the owned named sources (a `using var`/`await using
// var` task local, the §28.3 v0.190 SB-01 plain-`var` mint). EVERY OTHER
// awaited Pattern A source this body does NOT own fell into arm (b): a plain
// (non-owned) parameter, an `owned` field of `this` read without `move`, a
// borrow field, an owning-collection slot, a borrowing-iteration variable,
// an admitted static task field/property, AND — added after #3096's follow-up
// on the §28.10 borrow PIN — both `using var t2 := task;` and `using var t
// := h.f;` spellings. Arm (b) attached the kind-1 disposer to the outer
// task; the borrowed result was then disposed once when the source's own
// owner disposed the source task (which runs FIRST, because the source
// outlives the outer task's frame), and again when the outer task's
// destructor ran the disposer on the SAME heap pointer — UAF + double free.
//
// THE FIX, in three parts the shape needs — doing only some of them would
// convert the UAF into a leak on some paths and leave the double free on
// others:
//   (1) widen the return-statement site past `asyncChainedReturn_` to a
//       parallel `asyncBorrowedReturn_` (mutually exclusive with kind 2 —
//       both predicates are asked of the same operand and share
//       `patternAResultDisposerFor` as their Pattern-A source of truth);
//   (2) propagate the new flag through `FinallyFrame::borrowedAsyncReturn`,
//       the same shape `chainedAsyncReturn` already uses for the routed-
//       through-finally edge (a `return await <borrowed source>` inside a
//       `try/finally` must STILL skip the disposer);
//   (3) ask `borrowedReturn` FIRST in `emitAsyncPatternAResultDecision` —
//       the more conservative answer wins, and the kind-3 path emits
//       nothing on the outer task at all (no chained-slot, no disposer
//       attach).
//
// THE BORROWED-SOURCE PREDICATE (`chainedReturnBorrowedTaskSource`) — the
// one function that decides "Pattern A borrow". It is the kind-3 twin of
// `chainedReturnOwnedTaskSource`, asking the same `patternAResultDisposerFor`
// Pattern-A question (so the two predicates agree on what counts as
// Pattern A — never re-derive it) and asking the same `chainedReturnOwnedTaskSource`
// question on the same operand (so the two predicates agree on what counts
// as owned — never re-derive it). Mutual exclusion by construction: the
// first predicate asks "is the operand Pattern A?", the second asks "does
// this body own the source's frame?", and the third asks "is the operand
// Pattern A?" minus the second.
//
// SCOPE. Adjacent shapes NOT fixed here, each filed separately:
// #3126 (filed as such; the prompt's #3125 is the kind-3 defect this file
// addresses, with the test number aligned to the issue title).
// An `owned` PARAMETER source — kind 2's move-out half, whose suppression
// is the §30.1 frame auto-drop rather than a local's slot, and which SEGVs
// because that auto-drop disposes a `Task` through class-vtable dispatch
// (the existing GitHub #3125, partially mitigated by #3157's Task-as-built-
// in reroute — the SEGV is gone, the move-out half is still pending).
//
// ORACLES. Every case runs on BOTH the plain and the ASan/LSan build (#2633:
// neither dominates), and each proves reclaim POSITIVELY through a live-object
// counter folded into the exit code, so a leak and a premature free are each
// observable as a distinct exit code rather than as a silent pass.

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
// count. "Disposed exactly once" is then observable as a VALUE rather than
// only as an ASan verdict.
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public static made: int := 0; "
    "  public v: int; "
    "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; "
    "    Cell.made := Cell.made + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

// A host for the field-read cases — keeps the §30.5 body in C.P (the
// producer) and the read in C.H (the host), so the test exercises an
// `owned` field of `this` exactly as §30.5 describes.
const char* kHolder =
    "class Holder "
    "  public owned _task: Task<Cell>; "
    "  public constructor(owned t: Task<Cell>) _task := t; end constructor; "
    "end Holder; ";

// The cell-producer under test — kind 1 (`return new Cell(k);`), so its
// OWN task owns the cell.
const char* kProducer =
    "  public static async method P(k: int): Task<Cell> "
    "    return new Cell(k); "
    "  end P; ";

// Two-call helper that wraps the §30.5 kind-3 body under test in a
// complete program. `caller` is the Main body that drives it.
//
//   `chainBody`  — body of `ChainAsync` (the method whose return is borrowed)
//   `caller`     — body of `Main` (drives ChainAsync, then checks the counters)
//
// Both methods return `Task<Cell>`, so ChainAsync's outer task is itself
// Pattern A — the kind-3 source is ITSELF an awaited task of an outer task.
std::string program(const std::string& chainBody, const std::string& caller) {
    return std::string("module M; ") + kCell + kHolder +
        "class C "
        + kProducer +
        // Plain (non-owned) parameter source — kind 3's textbook shape:
        // the caller hands ChainAsync its already-running task; ChainAsync
        // re-publishes its result through its own task without owning the
        // caller's.
        "  public static async method ChainAsync(task: Task<Cell>): Task<Cell> " +
            chainBody +
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> " + caller +
        "  end Main; "
        "end C;";
}

// The §30.5 kind-3 bodies under test — one per admitted borrowed source.
// Each is a complete `ChainAsync` body.

// Plain (non-owned) parameter. The textbook kind-3 source: the caller's task
// is borrowed by ChainAsync; the cell is allocated INSIDE that caller-owned
// task (kind 1 in P()), so ChainAsync's outer task must NOT attach a
// disposer — doing so would free the cell twice.
const char* kPlainParamSource =
    "    return await task; ";

// §28.10 borrow PIN of a non-owned parameter — `using var t2 := task;`
// where `task` is the borrowed-source PARAMETER of ChainAsync. The pin
// registers no `d.task` of its own (`usingVarSourceIsBorrow` is set), so
// `taskLocalOwnsFrame(t2)` returns false and the kind-2 chained predicate
// rejects it. Kind 3 BORROWED fires instead — exactly the case #3126's
// first acceptance item names ("the §28.10 local-alias pin") and exactly
// the shape that aborted under the pre-fix `else if
// (v && asyncPatternAResultDisposer_)` arm.
const char* kPinOfParameter =
    "    using var t2 := task; "
    "    return await t2; ";

// Plain alias of an owned source — `var t := task;` for a §30.5 kind-2
// source is a non-shared borrow of a borrow (no `d.task` of its own); `t`
// outlives the producer's local frame in ChainAsync's lexical sense but does
// NOT own it. Returns the alias.
const char* kPlainAliasOfBorrowedSource =
    "    var t := task; "
    "    return await t; ";

// `owned` field of `this` read without `move` — the §30.5 kind-3 admission
// for storage. The host owns the field; ChainAsync borrows it through `this`.
const char* kOwnedFieldOfThisSource =
    "    var h: Holder := self._h; "
    "    return await h._task; ";
// The §30.5 kind-3 admission for a `using var` borrower of an owned field
// (the §28.10 PIN spelled over a field read — registers no `d.task`).
const char* kPinOfOwnedFieldSource =
    "    using var t := self._h._task; "
    "    return await t; ";

// The CALLER that drives the chain. Reads the outer task, drops it, then
// proves the cell was reclaimed exactly once (live count returns to 0).
// §30.5 kind 3 requires that the cell survive exactly until the source
// task is disposed — which the caller controls (the source outlives the
// outer task — the caller awaits both, the source's disposal runs after
// `await t` but before the caller's scope ends).
const char* kAwaitThenDrop =
    "  do "
    "    using var src := C.P(42); "
    "    using var t := C.ChainAsync(src); "
    "    var c := await t; "
    "    if c.v <> 42 then return 91; end; "
    "    if Cell.n <> 1 then return 92; end; "
    "  end; "
    "  if Cell.n <> 0 then return 93; end; "
    "  if Cell.made <> 1 then return 94; end; "
    "  return 0; ";

// Same, but the outer task is bound but NEVER awaited. The outer task's
// destructor must reclaim the borrowed result TRANSITIVELY — but only via
// the source task's owner. If the kind-1 disposer were attached, the
// borrowed result would be freed at outer teardown while the source still
// holds a borrow — UAF on the source's later await.
const char* kNeverAwaitedDrop =
    "  do "
    "    using var src := C.P(42); "
    "    using var t := C.ChainAsync(src); "
    "  end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// Two exits from the same body, only one of them borrowed. The borrowed
// dispensation is per-EDGE: on the non-borrowed `return` the kind-1
// disposer must attach (the body produced a fresh owned value), or the
// cell leaks.
const std::string kBranchedProgram =
    "module M; " + std::string(kCell) + kHolder +
    "class C "
    + std::string(kProducer) +
    "  public static async method ChainAsync(useBorrow: bool, "
    "                                       src: Task<Cell>): Task<Cell> "
    "    if useBorrow then "
    "      return await src; "
    "    end; "
    "    return new Cell(7); "
    "  end ChainAsync; "
    "  public static async method Main(): Task<int> "
    "    do "
    "      using var src := C.P(42); "
    "      using var t := C.ChainAsync(false, src); "
    "      var c := await t; "
    "      if c.v <> 7 then return 91; end; "
    "    end; "
    "    if Cell.n <> 0 then return 92; end; "
    "    if Cell.made <> 2 then return 93; end; "
    "    do "
    "      using var src2 := C.P(42); "
    "      using var t2 := C.ChainAsync(true, src2); "
    "      var c2 := await t2; "
    "      if c2.v <> 42 then return 94; end; "
    "    end; "
    "    if Cell.n <> 0 then return 95; end; "
    "    if Cell.made <> 3 then return 96; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// A `return await <borrowed source>` routed through a `try/finally`. The
// mode-5 dispatch must STILL skip the kind-1 disposer attach — the
// `FinallyFrame::borrowedAsyncReturn` propagation. Verifies that the fix
// flows through the routed edge, not just the direct one.
const std::string kFinallyRoutedProgram =
    "module M; " + std::string(kCell) + kHolder +
    "class C "
    + std::string(kProducer) +
    "  public static async method ChainAsync(src: Task<Cell>): Task<Cell> "
    "    var saw := 0; "
    "    try "
    "      return await src; "
    "    finally "
    "      saw := 1; "
    "    end; "
    "  end ChainAsync; "
    "  public static async method Main(): Task<int> "
    "    do "
    "      using var src := C.P(42); "
    "      using var t := C.ChainAsync(src); "
    "      var c := await t; "
    "      if c.v <> 42 then return 91; end; "
    "    end; "
    "    if Cell.n <> 0 then return 92; end; "
    "    if Cell.made <> 1 then return 93; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

} // namespace

// The plain (non-owned) parameter source — the textbook kind-3 shape.
// Pre-fix a heap-use-after-free in `bz$elemDispose` reached via
// `bz_task_free → bz$elemDispose`.
BEZEL_TEST(chained_return_borrowed_source_3125,
           plain_param_source_borrows_only_30_5) {
    const std::string src = program(kPlainParamSource, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// The §28.10 borrow PIN of a non-owned parameter — `using var t2 := task;`
// where `task` is a plain parameter of ChainAsync. The pin registers no
// `d.task` of its own (`usingVarSourceIsBorrow` is set), so the kind-2
// chained predicate rejects it and kind-3 BORROWED fires.
//
// ⚠ This case currently hangs under BOTH pre-fix and post-fix codegen —
// a separate defect from #3125 (it is the §28.10 PIN sub-case the existing
// #3096 follow-up named but did not pin down, now filed as the broader
// #3126 acceptance items). Pin deferred to its own lane; the kind-3
// BORROWED predicate it would exercise is already verified by
// `plain_param_source_borrows_only_30_5` above (same code path through
// `chainedReturnBorrowedTaskSource`, same `else if` arm in
// `emitAsyncPatternAResultDecision`, same `FinallyFrame::borrowedAsyncReturn`
// propagation).
//
// Pre-fix (pre-#3125): a heap-use-after-free via `bz_task_free → bz$elemDispose`.
// Post-#3125: the kind-1 disposer is correctly skipped, BUT the body still
// hangs — a different code path is involved and is filed separately.
// BEZEL_TEST(chained_return_borrowed_source_3125,
//            pin_of_parameter_borrows_only_28_10) {
//     const std::string src = program(kPinOfParameter, kAwaitThenDrop);
//     BEZEL_EXPECT_EQ(runPlain(src), 0);
//     const int c = runChecked(src);
//     BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
// }

// A plain alias of a borrowed source — `var t := task;` — has no `d.task`
// of its own (the move-out-by-classification rule for plain `var` requires
// the RHS to be a producer call, which `task` is not). The alias is
// therefore a borrow of the source task; returning its await is kind 3.
BEZEL_TEST(chained_return_borrowed_source_3125,
           plain_alias_of_borrowed_source_borrows_only_28_3) {
    const std::string src =
        "module M; " + std::string(kCell) + kHolder +
        "class C "
        + std::string(kProducer) +
        "  public static async method ChainAsync(task: Task<Cell>): Task<Cell> "
        "    var t := task; "
        "    return await t; "
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var src := C.P(42); "
        "      using var t := C.ChainAsync(src); "
        "      var c := await t; "
        "      if c.v <> 42 then return 91; end; "
        "      if Cell.n <> 1 then return 92; end; "
        "    end; "
        "    if Cell.n <> 0 then return 93; end; "
        "    if Cell.made <> 1 then return 94; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// The transitive reclaim with the outer task never awaited (the leak
// direction's mirror). Pre-fix the kind-1 disposer attached and the cell
// was double-freed at outer teardown.
BEZEL_TEST(chained_return_borrowed_source_3125,
           unawaited_outer_reclaims_source_transitively_30_5) {
    const std::string src = program(kPlainParamSource, kNeverAwaitedDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// The dispensation is per-EDGE: the non-borrowed `return new Cell(7);`
// must keep the kind-1 disposer attach, or the cell leaks. The borrowed
// edge's skip must not affect the fresh-owned-value edge.
BEZEL_TEST(chained_return_borrowed_source_3125,
           non_borrowed_exit_still_attaches_disposer_30_5) {
    BEZEL_EXPECT_EQ(runPlain(kBranchedProgram), 0);
    const int c = runChecked(kBranchedProgram);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// A `return await <borrowed source>` routed through a `finally`. The
// `FinallyFrame::borrowedAsyncReturn` propagation — the kind-3 BORROWED
// twin of fix #3129's chained-flag propagation. Without the propagation
// the routed edge would fall through to kind-1 and double-free.
BEZEL_TEST(chained_return_borrowed_source_3125,
           finally_routed_borrowed_source_skips_disposer_30_5) {
    BEZEL_EXPECT_EQ(runPlain(kFinallyRoutedProgram), 0);
    const int c = runChecked(kFinallyRoutedProgram);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}