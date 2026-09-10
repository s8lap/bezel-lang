// Codegen / memory-unsafety — issue #4189 (§30.5 "Disposing an unconsumed
// `OwningTask<T>`" / "Pattern A result slots are of three kinds", v0.298):
// pre-existing, confirmed independent of both #4159's fix and #4162's fix
// (reproduces identically with both already applied — see those issues'
// own commits, 8439b2a and 1abb29d, and #4162's own test file, which found
// this exact defect while building an "outer is later awaited" accept-side
// control and filed it here rather than folding it in, per this project's
// "no while-we're-at-it expansions" discipline).
//
// ROOT CAUSE. `using var` binding an inferred `OwningTask<T:class>` local
// whose PRODUCER body is itself a §30.5 Pattern A kind-2 chain (`async
// lambda() => await <Task<T:class> source>`) double-frees when the bound
// local is later AWAITED via the Pattern B "bound OwningTask" idiom
// (`using var w := await outer;`).
//
// `bz_task_take_result` (`src/runtime/bezel_runtime_concurrency.cpp`) is
// called by the AWAIT'S own Pattern-B owned-consuming handling to extract
// the value out of the AWAITED task and mark it consumed — but pre-fix it
// only nulled THAT task's own `result` field. A genuine kind-2 chain
// completion stores the SAME pointer VALUE in its own, separate `result`
// buffer at EVERY hop of the chain (a borrow-alias, not an independently
// owned copy — see #4162's own commit body), and the chain's true origin
// (here, `C3960.P()`'s own task) ALWAYS carries its own, unconditionally
// attached, kind-1 `result_disposer` (§30.5 "Pattern A ... the task always
// owns its result directly"). Nulling only the awaited task's own slot
// leaves that upstream disposer fully armed on a still-populated `result`
// field it does not know has already been handed to the caller. When
// `outer`'s own `using var` scope-end disposal later walks the chain
// (`bz_task_free`'s recursive `chained_source` reclaim), that upstream
// disposer fires a SECOND time on the exact object `w`'s own, correct,
// Pattern-B-consumed disposal already freed.
//
// FIX (`bz_task_take_result`): after nulling the awaited task's own
// `result` (as it always did), additionally walk `chained_source`
// TRANSITIVELY (a chain may be more than one hop deep — Row B below pins
// this) and null each upstream hop's own `result` the same way, without
// touching `chained_source` itself (still needed by `bz_task_free` to
// recurse for memory reclaim). That is exactly what makes
// `bz_task_free`'s pre-existing `task->result == null` guard skip an
// upstream hop's `result_disposer` once the chain is eventually freed,
// mirroring the protection it already gave the awaited task's own slot.
//
// Spec (v0.298), §30.5 "Disposing an unconsumed OwningTask<T>": disposal
// obligations attach to the produced result and must fire "exactly once,
// whichever path the program actually takes" — here, the chain's own
// intermediate hops must yield disposal responsibility entirely to
// whichever consumer actually extracts and takes ownership of the value.
//
// MEASURED (this repo, HEAD with #4159 + #4162 already applied, pre-this-
// fix, isolated worktree, bezelc --version confirmed matching, amplified
// 200x):
//   - Plain oracle: SIGSEGV (WIFSIGNALED) on the first iteration.
//   - ASan/LSan: heap-use-after-free in bz$elemDispose <- bz_task_free
//     (the chained-source recursion's own disposer firing again), freed by
//     `w`'s own Pattern-B-consumed scope-end disposal (the first,
//     legitimate free), previously allocated by C3960.P()'s own task.
// Post-fix: both oracles clean, disposal-counting exit code returns to 0
// every iteration.
//
// ORACLES: plain (SIGSEGV pre-fix -> WIFSIGNALED -> `ran=false`) AND
// ASan/LSan (heap-use-after-free pre-fix -> `ran=false`) — neither
// dominates (#2633).

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

// 200 per LANE_RULES amplification guidance — a one-shot leak/double-free
// row can pass on both sides (the allocation is still stack-reachable, or
// the corruption unobserved, at exit).
constexpr int kIters = 200;

// Row A — the issue's own primary repro, verbatim: a one-hop chain (`f`'s
// body is `await C3960.P()`), `outer := f()` bound via `using var`, then
// AWAITED (not merely dropped) via the Pattern B "bound OwningTask" idiom.
std::string oneHopAwaitedProgram() {
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
        "        using var w := await outer; "
        "      end; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

// Row B — a TWO-HOP chain (`g`'s body chains onto `C3960.P()`; `f`'s body
// chains onto `g()`), so the chain's true origin (P()'s own task, which
// carries the only unconditionally-attached disposer) is two
// `chained_source` hops away from `outer`. A fix that only walks ONE hop
// (e.g. nulling `chained_source->result` but not transitively continuing
// into `chained_source->chained_source`) would still double-free HERE even
// though Row A is clean — this row exists specifically to pin the
// transitive walk.
std::string twoHopAwaitedProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method P(): Task<Widget3960> "
        "    return new Widget3960(); "
        "  end P; "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters) + " do "
        "      do "
        "        var g := async lambda() => await C3960.P(); "
        "        var f := async lambda() => await g(); "
        "        using var outer := f(); "
        "        using var w := await outer; "
        "      end; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

// Row C — accept-side control: the SAME "bind, then await via Pattern B"
// idiom, but the producer body is a genuine kind-1 fresh value (`new
// Widget3960()`, no further `await`) — `chained_source` is never set at
// all, so this exercises `bz_task_take_result`'s pre-existing, unchanged
// top-level-only nulling exactly as before. Must stay correctly disposed
// exactly once, unaffected by this fix.
std::string nonChainedAwaitedControlProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters) + " do "
        "      do "
        "        var f := async lambda() => new Widget3960(); "
        "        using var outer := f(); "
        "        using var w := await outer; "
        "      end; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

// Row D — accept-side / no-double-free control: the exact Row A producer
// shape, but `outer` is DROPPED WITHOUT EVER BEING AWAITED (issue #4162's
// own idiom). `bz_task_take_result` is never called on this path at all,
// so this row is orthogonal to this fix — kept here to confirm this fix
// does not disturb #4162's own fix on the identical producer this file
// also awaits in Row A.
std::string oneHopDroppedControlProgram() {
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

// Row E — the issue's own primary repro shape (Row A), outside a loop:
// single-shot correctness (exit code / disposal count), not a
// double-free/leak-magnitude proof.
std::string oneHopAwaitedSingleShotProgram() {
    return std::string("module M; ") + kWidget +
        "class C3960 "
        "  public static async method P(): Task<Widget3960> "
        "    return new Widget3960(); "
        "  end P; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      var f := async lambda() => await C3960.P(); "
        "      using var outer := f(); "
        "      using var w := await outer; "
        "    end; "
        "    return Widget3960.n; "
        "  end Main; "
        "end C3960;";
}

} // namespace

// Row A — the issue's own primary repro. Pre-fix: SIGSEGV / heap-use-after-
// free on the first iteration. Post-fix: disposed exactly once per
// iteration (n returns to 0).
BEZEL_TEST(using_var_awaited_chained_owningtask_double_free_4189,
           one_hop_chain_awaited_disposed_once_fixed_30_5) {
    const std::string src = oneHopAwaitedProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Row B — TWO-HOP chain: pins the TRANSITIVE walk, not just one hop.
BEZEL_TEST(using_var_awaited_chained_owningtask_double_free_4189,
           two_hop_chain_awaited_disposed_once_fixed_30_5) {
    const std::string src = twoHopAwaitedProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Row C — accept-side control: non-chained producer, awaited via Pattern B.
// Must stay clean (unaffected by this fix).
BEZEL_TEST(using_var_awaited_chained_owningtask_double_free_4189,
           non_chained_awaited_control_clean_30_5) {
    const std::string src = nonChainedAwaitedControlProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Row D — no-double-free control: the Row A producer, dropped without
// await (issue #4162's own idiom, unaffected by this fix).
BEZEL_TEST(using_var_awaited_chained_owningtask_double_free_4189,
           one_hop_chain_dropped_control_clean_30_5) {
    const std::string src = oneHopDroppedControlProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// Row E — Row A's exact shape outside a loop (single-shot correctness).
BEZEL_TEST(using_var_awaited_chained_owningtask_double_free_4189,
           one_hop_chain_awaited_single_shot_clean_30_5) {
    const std::string src = oneHopAwaitedSingleShotProgram();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
