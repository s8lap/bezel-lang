// Codegen/ownership — issue #1888 (spec §30.5 "Result lifetime and task
// disposal", v0.194; §29.2 owned iteration-variable auto-drop):
// draining an `OwningArray<OwningTask<T>>` and awaiting each drained task
// DOUBLE-DISPOSED the task → SEGV/UAF.
//
// The spec's canonical Pattern B drain-and-await idiom (spec §30.5):
//   for var task in taskArray.Drain() do
//     results.Add(await task);     // "task discharged by the await; no
//   end;                           //  further obligation"
// The Pattern B `await` is owned-consuming w.r.t. its operand, and §30.5
// explicitly lists "an iteration variable from draining an
// `OwningArray<OwningTask<T>>`" as an admitted owned operand — so `task` is
// discharged exactly once by the await and must NOT be auto-dropped again.
//
// Pre-fix, two coupled defects produced a double-dispose:
//   1. The ownership pass emitted a spurious §29.2/§28.3 auto-drop must-use
//      WARNING on `task` (the seeded-iter-var-not-in-endState fallback in
//      Ownership.cpp did not recognize the Pattern B await as the discharge),
//      and set `ownedIterVarAutoDropAtBodyEnd` so codegen disposed `task` at
//      loop-body end IN ADDITION to the await.
//   2. That body-end disposal used the class-vtable `dynamicDispose`
//      (emitDisposeAndReclaim) — but an `OwningTask<T>` is a runtime task
//      struct with NO Bezel vtable, so it read a bogus vtable slot: the program
//      SEGV'd (`SEGV on unknown address 0x10 in bz$async$Main$Resume`).
//
// The fix (Ownership.cpp / Ownership_Checks.cpp / LlvmBackend_Stmts.cpp):
//   - The ownership pass records a Pattern B await-consumed drained iter var
//     (awaitDischargedIterVars_) and SUPPRESSES the spurious must-use warning
//     (the await IS the discharge — "no further obligation"), matching the
//     warning-free plain-local `var t := M(); await t;` await.
//   - Codegen's body-end iter-var disposal is now TASK-AWARE: for a task-typed
//     iter var it registers a `d.task` `bz_task_fire_and_forget` disposal (the
//     same by-type disposal a plain-`var` task local gets — a bound task's
//     frame is never freed by the await, LlvmBackend_Async.cpp lowerAwait),
//     carrying the per-T owned-result disposer for `OwningTask<T:class>`, rather
//     than the class-vtable dynamicDispose. The task is thus disposed EXACTLY
//     once, whether the body awaits it (result taken; frame fire-and-forgotten)
//     or drops it un-awaited (result reclaimed by the disposer; frame freed).
//
// Verification uses the `Cell` static-counter idiom (ctor +1 / dtor -1): net 0
// after the drained results are disposed ⇒ each Cell disposed exactly once.
// Under runAotChecked (generated-code ASan) the pre-fix double-dispose SEGV
// flips r.ran to false; a leak leaves Cell.n > 0. // per §30.5, §29.2 (v0.194)

#include "support/aot_support.hpp"        // runAotChecked — AOT harness + ASan
#include "support/ownership_support.hpp"  // parseAndCheckOwnership / hasAutoDropWarning
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::parseAndCheckOwnership;
using bezel::test::hasAutoDropWarning;

namespace {
// Cell: static counter, ctor +1 / dtor -1 — a leak / double-free detector
// (net 0 ⇒ each constructed instance was disposed exactly once).
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// A producer that mints a fresh caller-owned OwningTask<Cell> (synchronously
// completing — no inner await; the drained task is already done at the await).
const char* kMaker =
    "  public static async method Make(): OwningTask<Cell> "
    "    return new Cell(); "
    "  end Make; ";

// A producer that GENUINELY SUSPENDS before producing (await Task.Delay), so
// the drained task is NOT yet done at the `await task` — the canonical Pattern B
// case. The iter-var slot must be spilled/restored across the body's await
// suspend (LlvmBackend_Stmts.cpp lowerOwnedSeqDrainFor frame-backing, #1888) or
// at body-end it holds a dangling task pointer and the disposal derefs a freed
// task (pthread_mutex ESRCH / SEGV in bz$async$Main$Resume) — a crash NO
// synchronous-producer test exercises.
const char* kMakerDelayed =
    "  public static async method Make(): OwningTask<Cell> "
    "    await Task.Delay(1); "
    "    return new Cell(); "
    "  end Make; ";
}  // namespace

// §30.5 canonical drain-and-await — the reported #1888 repro. Drain an
// OwningArray<OwningTask<Cell>> and await each drained task into a results
// OwningArray<Cell>. Pre-fix: SEGV in bz$async$Main$Resume (double-dispose of
// the drained task via the class-vtable auto-drop). Post-fix: each task is
// discharged exactly once by the await + body-end task disposal, and the two
// awaited Cells land in `results`, alive. // per §30.5
BEZEL_TEST(drain_owning_task_await_1888, drain_and_await_produces_all_results) {
    auto r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C " + kMaker +
        "  public static async method Main(): Task<int> "
        "    using var tasks := new OwningArray<OwningTask<Cell>>(); "
        "    tasks.Add(Make()); "
        "    tasks.Add(Make()); "
        "    using var results := new OwningArray<Cell>(); "
        "    for var task in tasks.Drain() do "
        "      results.Add(await task); "     // Pattern B await = the discharge
        "    end; "
        "    return results.Count; "          // both results present ⇒ 2
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);        // ASan/LSan-clean: no double-dispose SEGV
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // both drained tasks awaited into results
}

// §30.5 / §29.2 — exactly-once disposal. After the drained results go out of
// scope, every Cell (and every drained task frame) has been disposed exactly
// once, so the static counter is net 0. Pre-fix this program did not even reach
// here (it SEGV'd); a double-free would flip r.ran to false, a leak would leave
// Cell.n > 0. // per §30.5, §29.2
BEZEL_TEST(drain_owning_task_await_1888, drained_task_disposed_exactly_once) {
    auto r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C " + kMaker +
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var tasks := new OwningArray<OwningTask<Cell>>(); "
        "      tasks.Add(Make()); "
        "      tasks.Add(Make()); "
        "      using var results := new OwningArray<Cell>(); "
        "      for var task in tasks.Drain() do "
        "        results.Add(await task); "
        "      end; "
        "    end; "                       // results + tasks disposed here
        "    return Cell.n; "             // net 0 ⇒ each disposed exactly once
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);        // ASan/LSan-clean: no double-dispose SEGV
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // Cell.n == 0
}

// §30.7 / §29.2 / §30.5 — the PRIMARY Pattern B case: a producer that GENUINELY
// SUSPENDS (await Task.Delay) before producing, so the drained task is not done
// at the `await task`. The iter-var slot is live across the body's await suspend
// and must round-trip the async frame (lowerOwnedSeqDrainFor frame-backing,
// #1888); without it, at loop-body end the slot holds a dangling (freed) task
// pointer and the disposal derefs it — a `pthread_mutex` ESRCH abort / SEGV in
// bz$async$*$Resume that the synchronously-completing producer tests above do
// NOT catch. Post-fix: the two awaited Cells land in `results`, alive. // per §30.7
BEZEL_TEST(drain_owning_task_await_1888, suspending_producer_drain_and_await_produces_all_results) {
    auto r = runAotChecked(
        std::string("module M; import System.Threading; ") + kCell +
        "class C " + kMakerDelayed +
        "  public static async method Main(): Task<int> "
        "    using var tasks := new OwningArray<OwningTask<Cell>>(); "
        "    tasks.Add(Make()); "
        "    tasks.Add(Make()); "
        "    using var results := new OwningArray<Cell>(); "
        "    for var task in tasks.Drain() do "
        "      results.Add(await task); "
        "    end; "
        "    return results.Count; "          // both suspending tasks awaited ⇒ 2
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);        // no ESRCH/SEGV on the dangling-slot deref
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // both drained suspending tasks awaited
}

// §30.7 / §29.2 — exactly-once disposal for the suspending case. After the
// drained results go out of scope every Cell (and every drained suspending task
// frame) has been disposed exactly once ⇒ Cell.n net 0. Pre-fix this crashed
// (dangling-slot deref) before reaching the return. // per §30.7, §29.2
BEZEL_TEST(drain_owning_task_await_1888, suspending_producer_drained_task_disposed_exactly_once) {
    auto r = runAotChecked(
        std::string("module M; import System.Threading; ") + kCell +
        "class C " + kMakerDelayed +
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var tasks := new OwningArray<OwningTask<Cell>>(); "
        "      tasks.Add(Make()); "
        "      tasks.Add(Make()); "
        "      tasks.Add(Make()); "
        "      using var results := new OwningArray<Cell>(); "
        "      for var task in tasks.Drain() do "
        "        results.Add(await task); "
        "      end; "
        "    end; "                       // results + tasks disposed here
        "    return Cell.n; "             // net 0 ⇒ each disposed exactly once
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);        // ASan/LSan-clean: no dangling-slot deref
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // Cell.n == 0
}

// §30.5 — the Pattern B await of a drained OwningTask iter var IS the discharge
// ("no further obligation"), so NO spurious §29.2/§28.3 auto-drop must-use
// warning is emitted on `task` — the drain-and-await idiom "compiles clean",
// matching the warning-free plain-local `var t := M(); await t;`. Pre-fix the
// ownership pass warned here. // per §30.5
BEZEL_TEST(drain_owning_task_await_1888, drain_and_await_emits_no_autodrop_warning) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kCell +
        "class C " + kMaker +
        "  public static async method Main(): Task "
        "    using var tasks := new OwningArray<OwningTask<Cell>>(); "
        "    tasks.Add(Make()); "
        "    using var results := new OwningArray<Cell>(); "
        "    for var task in tasks.Drain() do "
        "      results.Add(await task); "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "task"));
}

// §29.2 — the suppression is SCOPED to the await-discharge: a drained
// OwningTask iter var that the body does NOT await (drops un-discharged) still
// gets the ordinary §29.2/§28.3 auto-drop must-use warning. Guards against
// over-suppressing the lint. (The task is still disposed exactly once at
// body-end — the task-aware disposal + per-T result disposer — but the user is
// still told the value was implicitly dropped.) // per §29.2
BEZEL_TEST(drain_owning_task_await_1888, drain_without_await_still_warns) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kCell +
        "class C " + kMaker +
        "  public static async method Main(): Task "
        "    using var tasks := new OwningArray<OwningTask<Cell>>(); "
        "    tasks.Add(Make()); "
        "    for var task in tasks.Drain() do "
        "      Console.WriteLine(\"drop\"); "   // task never awaited/discharged
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "task"));
}
