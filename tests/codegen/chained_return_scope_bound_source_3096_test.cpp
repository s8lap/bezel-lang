// Codegen — fix #3096: §30.5 Pattern A **kind 2** with a scope-bound (named)
// source never reached the chained slot at all, so the borrowed result ended up
// with TWO owners.
//
// Spec (revision v0.252):
//   §30.5 "Result lifetime and task disposal", *Pattern A result slots are of
//   three kinds*, kind 2 — **`return await` over an owned Pattern A source —
//   the chained slot**:
//     "The result is a *borrow through the source task*: ownership of the
//      **source task** transfers into the outer task's state (a **chained**
//      slot), and the outer destructor disposes the chained source task, never
//      the borrowed result directly."
//   Same kind, *Chaining from a scope-bound source* —
//     "When the source is a `using var`/`await using var` local — which §28.3
//      forbids moving — no move occurs: the source's slot already lives in the
//      enclosing state-machine frame, and the chained-slot arrangement **defers
//      its disposal** from the frame's scope end to the outer task's teardown; a
//      disposal-*timing* dispensation … On both paths the source task is
//      disposed exactly once — by the outer task's destructor, transitively."
//   Same kind, soundness paragraph — the move-out half names "an `owned`
//     parameter or a plain owned local — kind 2 above; a `using var` source is
//     not moved, only disposal-deferred".
//   §30.5 kind 1 (the arm that fired instead) — "A **fresh owned value** (a
//     `return` of an owned expression): the task **owns the result** and its
//     destructor disposes it directly."
//
// THE DEFECT. `asyncChainedReturn_` was set only when the awaited operand was a
// `CallExpr` (LlvmBackend_Stmts.cpp), so for a NAMED source
// `bz_task_set_chained` was never called — the outer task took no ownership of
// the source at all. With `chainedReturn` false the
// `else if (v && asyncPatternAResultDisposer_)` arm fired instead and attached
// the **kind-1** per-`T` result disposer to the outer task, so the borrowed
// result was owned by BOTH the source task (through its own scope-end disposal,
// which runs first) and the outer task. That is a use-after-free followed by a
// double free, not the single missing deferral the issue first reported.
//
// OBSERVED PRE-FIX (bezelc --sanitize=address, HEAD 663a68e):
//     ERROR: AddressSanitizer: heap-use-after-free … READ of size 4
//       #0 bz$async$Main$Resume
//     freed by thread T0 here: bz$elemDispose   <- the #1503 per-T disposer
//       ... bz$M.C$ChainAsync                   <- the CALLEE's scope-end
// on the `using var`, `await using var` and plain-owned-local sources below.
//
// THE FIX, in the three parts the shape needs — doing only one of them would
// convert the use-after-free into a leak on some paths and leave the double free
// on others:
//   (1) widen the chained-return detection past the anonymous `CallExpr` to an
//       OWNED named source (`chainedReturnOwnedTaskSource`);
//   (2) suppress that source's own scope-end disposal on the chained return edge
//       — the "defers its disposal" dispensation, done by nulling the local's
//       null-guarded slot immediately before `emitAllDisposals`, so every OTHER
//       exit of the same body keeps disposing it normally;
//   (3) do not attach the kind-1 result disposer on the chained path — which
//       (1) achieves on its own, the two arms being mutually exclusive.
//
// THE OWNS-THE-FRAME PREDICATE. Part (1) must ask the SAME question the `d.task`
// registration asks, or it moves an obligation that was never registered. That
// is now literally one function, `taskLocalOwnsFrame` (LlvmBackend_Setup.cpp),
// called by all three readers: the two registration sites
// (registerUsingDisposal's task arm, lowerStmt's LocalDecl arm) and
// chainedReturnOwnedTaskSource. It is a correction: this fix first shipped
// reading `ld->isUsing` alone, which is NOT the registration test — a §28.10
// BORROW PIN (`using var t2 := task;`, or a task-typed field read) sets
// `usingVarSourceIsBorrow`, and registerUsingDisposal returns before registering
// anything. Chaining such a pin handed the outer task's teardown a `bz_task_t*`
// the body never owned: a double free of the frame plus a second cascade through
// the #1503 result disposer (measured: heap-use-after-free on the local-alias
// spelling, a HANG — the documented corrupted-continuation symptom — on the
// field-read one).
//
// SCOPE. Adjacent shapes NOT fixed here, each filed separately:
// #3125 — an `owned` PARAMETER source (kind 2's move-out half, which SEGVs
// because the §30.1 auto-drop disposes a `Task` through class-vtable dispatch);
// and #3126 — every kind-3 BORROWED source, which still attaches the kind-1
// disposer §30.5 kind 3 says must dispose nothing. #3126 covers a plain `var`
// alias, a plain borrow parameter, an owned-field read, a static task field AND
// (added after the pin finding above) both §28.10 borrow-pin spellings. #3129 —
// a `return await` routed through a `finally`, which reaches neither the chained
// branch nor the kind-1 arm, so the outer task records neither kind; the
// `finallyStack_.empty()` gate on the detection above is deliberately untouched.
//
// The two pin programs are reproduced verbatim below. They are NOT live cases
// here, and the reason is recorded rather than hidden: they cannot be green
// until #3126 lands, because with chaining correctly declined they fall back to
// #3126's kind-1 double-owner and still abort under ASan. What IS established
// mechanically is that they are byte-for-byte what they were before this fix
// existed: compiled at the base commit 663a68e and at this commit, both
// programs produce executables with IDENTICAL ASan BuildIds
// (b14 536138ace919d98e94a29aa0096542eb7b2148af,
//  b15 e03b47d0a1b41d912c77f6cbe3c3111a2c776ca5) and identical failure stacks.
// They are listed as #3126's acceptance criteria; whoever fixes it should make
// them live tests here, and should extend `taskLocalOwnsFrame` /
// `chainedReturnOwnedTaskSource` rather than write a third copy of the
// predicate.
//
//   // #3126 acceptance case A — the §28.10 local-alias pin.
//   public static async method ChainAsync(): Task<Cell>
//     using var task := C.P(42);
//     using var t2 := task;      // pin: usingVarSourceIsBorrow, NO d.task
//     return await t2;
//   end ChainAsync;
//
//   // #3126 acceptance case B — the §28.10 field-read pin.
//   public static async method ChainAsync(h: Holder): Task<Cell>
//     using var t := h.f;        // storage read -> pin -> NO d.task
//     return await t;
//   end ChainAsync;
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

// `chainBody` is the whole body of `ChainAsync` — the §30.5 kind-2 producer
// under test. `P` is the Pattern A source producer (kind 1: its non-chained
// `return <owned>` makes ITS task the result's owner).
std::string program(const std::string& chainBody, const std::string& mainBody) {
    return std::string("module M; ") + kCell +
        "class C "
        "  public static async method P(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end P; "
        "  public static async method Boom(): Task<Cell> "
        "    throw new InvalidOperationException(\"boom\"); end Boom; "
        "  public static async method ChainAsync(): Task<Cell> " + chainBody +
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> " + mainBody +
        "  end Main; "
        "end C;";
}

// The §30.5 kind-2 bodies under test — one per admitted OWNED named source.
const char* kUsingVarSource =
    "  using var task := C.P(42); "
    "  return await task; ";

const char* kAwaitUsingVarSource =
    "  await using var task := C.P(42); "
    "  return await task; ";

// §28.3 v0.190 SB-01: a plain `var` whose initializer is a task-family MINT call
// OWNS the frame (it is the local for which lowerStmt registers the scope-end
// `d.task`), so it is kind 2's "plain owned local".
const char* kPlainOwnedLocalSource =
    "  var task := C.P(42); "
    "  return await task; ";

// A §28.10 borrow PIN declared alongside the real owner, with the CHAINED
// return taking the OWNER. `taskLocalOwnsFrame` must say yes to `task` and no to
// `t2`: the owner's disposal is deferred to the outer teardown, and the pin —
// which registers no disposal at all — must neither be chained nor disturb that.
// (Respelled at v0.350: the pin's source is the borrowing alias `view`; the
// unmarked `using var t2 := task;` over the owned `task` is the §28.10
// *Adopting a named owned binding* compile error, issues #7686/#7614. A
// borrowing alias is the spec's borrow-state source, which still pins.)
const char* kPinBesideOwnedSource =
    "  using var task := C.P(42); "
    "  var view := task; "
    "  using var t2 := view; "
    "  return await task; ";

// CONTROL — the anonymous `CallExpr` source, the only shape the pre-fix gate
// recognised. It must keep working byte-for-byte.
const char* kCallExprSourceControl =
    "  return await C.P(42); ";

// The caller: reads the chained result, then drops the outer task. §30.5 kind 2
// requires the source task — and, transitively, the result — to be disposed
// exactly once, by the outer task's destructor.
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

// The outer task is bound but NEVER awaited. The chained source must still be
// reclaimed transitively at the outer task's teardown — the direction that would
// LEAK if part (2) suppressed the source's disposal without part (1) chaining it
// (LSan is the oracle for this one).
const char* kNeverAwaitedDrop =
    "  do "
    "    using var t := C.ChainAsync(); "
    "  end; "
    "  if Cell.n <> 0 then return 92; end; "
    "  if Cell.made <> 1 then return 93; end; "
    "  return 0; ";

// TWO exits from the same body, only one of them chained. The dispensation is
// per-EDGE: on the non-chained `return` the `using var` must keep its ordinary
// scope-end disposal, or the source task leaks.
const std::string kBranchedProgram =
    "module M; " + std::string(kCell) +
    "class C "
    "  public static async method P(k: int): Task<Cell> "
    "    return new Cell(k); "
    "  end P; "
    "  public static async method ChainAsync(useChain: bool): Task<Cell> "
    "    using var task := C.P(42); "
    "    if useChain then "
    "      return await task; "
    "    end; "
    "    return new Cell(7); "
    "  end ChainAsync; "
    "  public static async method Main(): Task<int> "
    "    do "
    "      using var t := C.ChainAsync(false); "
    "      var c := await t; "
    "      if c.v <> 7 then return 91; end; "
    "    end; "
    "    if Cell.n <> 0 then return 92; end; "
    "    if Cell.made <> 2 then return 93; end; "
    "    do "
    "      using var t2 := C.ChainAsync(true); "
    "      var c2 := await t2; "
    "      if c2.v <> 42 then return 94; end; "
    "    end; "
    "    if Cell.n <> 0 then return 95; end; "
    "    if Cell.made <> 3 then return 96; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// The FAULT edge: the await re-raises, so the chained return is never reached
// and the `using var` must take its ordinary UNWIND disposal. This is what the
// per-edge placement of part (2) buys — a body-wide suppression would leak here.
const std::string kFaultEdgeProgram =
    "module M; " + std::string(kCell) +
    "class C "
    "  public static async method Boom(): Task<Cell> "
    "    throw new InvalidOperationException(\"boom\"); end Boom; "
    "  public static async method ChainAsync(): Task<Cell> "
    "    using var task := C.Boom(); "
    "    return await task; "
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

// CONTROL — a VALUE-typed result. `patternAResultDisposerFor` is null for
// `Task<int>`, so no borrow exists, nothing is chained, and the source keeps its
// ordinary scope-end disposal. Pins that the widened gate did not reach past the
// class/collection row.
const char* kValueResultProgram =
    "module M; "
    "class C "
    "  public static async method P(k: int): Task<int> return k; end P; "
    "  public static async method ChainAsync(): Task<int> "
    "    using var task := C.P(42); "
    "    return await task; "
    "  end ChainAsync; "
    "  public static async method Main(): Task<int> "
    "    using var t := C.ChainAsync(); "
    "    var n := await t; "
    "    if n <> 42 then return 91; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

} // namespace

// The issue's own repro: a `using var` source. Pre-fix a heap-use-after-free.
BEZEL_TEST(chained_return_scope_bound_3096, using_var_source_chains_and_defers_30_5) {
    const std::string src = program(kUsingVarSource, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// §30.5 names `await using var` in the same clause as `using var`.
BEZEL_TEST(chained_return_scope_bound_3096, await_using_var_source_chains_30_5) {
    const std::string src = program(kAwaitUsingVarSource, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Kind 2's "plain owned local" — the §28.3 v0.190 SB-01 mint.
BEZEL_TEST(chained_return_scope_bound_3096, plain_owned_local_source_chains_30_5) {
    const std::string src = program(kPlainOwnedLocalSource, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// The transitive reclaim with the outer task never awaited (the leak direction).
BEZEL_TEST(chained_return_scope_bound_3096, unawaited_outer_reclaims_source_transitively_30_5) {
    const std::string src = program(kUsingVarSource, kNeverAwaitedDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// The dispensation is per-EDGE: the non-chained `return` still disposes the
// `using var`.
BEZEL_TEST(chained_return_scope_bound_3096, non_chained_exit_still_disposes_source_28_10) {
    BEZEL_EXPECT_EQ(runPlain(kBranchedProgram), 0);
    const int c = runChecked(kBranchedProgram);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// The fault edge never reaches the chained return, so the source takes its
// ordinary unwind disposal.
BEZEL_TEST(chained_return_scope_bound_3096, faulted_source_unwind_disposal_unchanged_30_9) {
    BEZEL_EXPECT_EQ(runPlain(kFaultEdgeProgram), 0);
    const int c = runChecked(kFaultEdgeProgram);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// A borrow pin in the same body as the chained owner: the predicate must
// separate them.
BEZEL_TEST(chained_return_scope_bound_3096, pin_beside_owned_source_28_10) {
    const std::string src = program(kPinBesideOwnedSource, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL: the anonymous CallExpr source, correct before and after.
BEZEL_TEST(chained_return_scope_bound_3096, call_expr_source_unchanged_30_5) {
    const std::string src = program(kCallExprSourceControl, kAwaitThenDrop);
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL: a value-typed result is outside the widened gate.
BEZEL_TEST(chained_return_scope_bound_3096, value_typed_result_not_chained_30_5) {
    BEZEL_EXPECT_EQ(runPlain(kValueResultProgram), 0);
    const int c = runChecked(kValueResultProgram);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
