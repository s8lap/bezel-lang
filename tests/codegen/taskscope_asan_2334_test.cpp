// fix #2334 (stdlib §6.10 / spec §28.14 v0.235, §28.9): a local captured by a
// lambda passed to TaskScope.Spawn's `scoped` parameter never had its 8-byte
// capture cell freed on ANY exit path, including the ordinary success path —
// collectDefinitelyEscapingLambdas (#694) recognizes only three definite-
// escape forms (direct return, returned var, `this.f` field-store); a lambda
// passed as a call argument is none of them, so the capture correctly lands
// in capturedByEscaping (excluded from the unsound scope-end free) but never
// in the sound envdtor-triggered free either.
//
// taskscope_asan_808_test.cpp's own probes are deliberately CAPTURE-FREE
// (see its file comment and the "orthogonal ... see issue #2334" notes on
// the exception-edge probes) specifically to dodge this gap — that is how it
// stayed unfixed under the ASan gate. This file adds the capturing shapes.
//
// The fix does NOT extend #694's definitelyEscapingLambdas_/envdtor
// machinery (the root-cause note's own suggested direction) — tracing
// bz_task_scope_spawn (bezel_runtime_concurrency.cpp) shows a spawned
// closure's envdtor can run strictly EARLIER than the captured local's own
// `using var` scope end, which would race a later same-scope read or
// disposal of that local through the same cell. Instead, a lambda literal
// passed directly to `scope.Spawn(...)` on a `using var`/`await using var`
// TaskScope receiver is classified non-escaping for cellFreeEligible_
// purposes (collectCellFreeEligible, LlvmBackend_Setup.cpp), reusing the
// existing owner-disposal-deferred machinery so the cell frees at the
// captured local's OWN scope end — governed by lexical program order, not by
// the closure's own drop timing. Soundness rests on §28.14's admission
// having already verified every captured local is declared before `scope`.
//
// EXCEPT on the unwind edge: TaskScope.DisposeAsync's exception-path drain
// is a documented (#1422) NON-BLOCKING zombie drain (still-running children
// are not awaited before DisposeAsync returns), so the quiescence argument
// does not hold there. An adversarial probe against the fix caught a real
// heap-use-after-free on this edge before Disposal::skipCellFreeOnUnwind /
// LastCellEntry::unwindUnsafe / Emitter::unwindUnsafeCellFree_ were added to
// suppress the free specifically on unwind, leaving the ordinary
// conservative leak there instead (never a UAF).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>

#include "ast/Node.hpp"
#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <filesystem>

namespace {

namespace fs = std::filesystem;

// Link+run `source` under ASan; `asanOptions` selects the leak-detection
// posture. Returns 0 on a clean run matching `expectedExitCode`, 1 on any
// sanitizer report (crash, UAF, double-free, or — when detect_leaks=1 — a
// leak) or a value mismatch, -1 if the toolchain is unavailable (SKIP).
int runUnderAsanWithOptions(const std::string& source, int expectedExitCode,
                            const std::string& asanOptions) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsanValue(source, expectedExitCode, "bezel_tscope2334", asanOptions);
}

// Leak-checked: fails on ANY leak, UAF, or double-free (the ordinary "must
// be fully clean" assertion — mirrors taskscope_asan_808_test.cpp).
void expectAsanClean(const std::string& source, int expectedExitCode) {
    int a = runUnderAsanWithOptions(source, expectedExitCode,
                                    "detect_leaks=1:exitcode=99");
    if (a == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(a, 0);
}

// Leak detection OFF: asserts only the ABSENCE of a crash/UAF/double-free
// and the correct VALUE — per CLAUDE.md's own note that LeakSanitizer
// overrides the process exit code, this is how a shape that is EXPECTED to
// leak (the unwind-edge / loop-capture conservative fallbacks below) is
// checked for memory SAFETY without the leak itself masking a real bug.
void expectAsanSafeIgnoringLeaks(const std::string& source,
                                 int expectedExitCode) {
    int a = runUnderAsanWithOptions(source, expectedExitCode,
                                    "detect_leaks=0:exitcode=99");
    if (a == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(a, 0);
}

// Confirms a leak IS reported — proves the conservative-leak fallback below
// is actually armed (not vacuously passing because nothing leaks).
//
// fix #4577: `expectedExitCode` MUST be the program's real value, passed by
// the caller, not hardcoded. `runUnderAsanWithOptions` (family A,
// tests/support/run_under_asan.hpp `runUnderAsanValue`) returns 1 for THREE
// distinct outcomes — a raw crash, a genuine sanitizer report (exitcode=99),
// AND a clean run whose own exit value merely differs from
// `expectedExitCode` — and every program this helper is called on returns
// `Tracked._count == 1`. A hardcoded `expectedExitCode=0` therefore made `1`
// come back unconditionally (the "wrong value" branch fires every time,
// sanitizer report or not), so `BEZEL_EXPECT_EQ(a, 1)` below could never
// fail — CLAUDE.md's "oracles that pass vacuously" class. Passing the real
// expected value collapses the helper to two outcomes (crash or genuine
// sanitizer report vs. a correct clean run), so `1` here is reachable ONLY
// via an actual sanitizer/crash finding.
void expectAsanLeakDetected(const std::string& source, int expectedExitCode) {
    int a = runUnderAsanWithOptions(source, expectedExitCode,
                                    "detect_leaks=1:exitcode=99");
    if (a == -1) return;  // toolchain unavailable: skip
    // runUnderAsanWithOptions returns 1 for ANY sanitizer report, including a
    // leak (exitcode=99) — so `1` here means "a leak (or crash) occurred".
    // Distinguish leak-vs-crash is not needed: expectAsanSafeIgnoringLeaks
    // above already proves this same shape has no crash/UAF, so a nonzero
    // result here is necessarily the conservative leak, not a fresh crash —
    // PROVIDED `expectedExitCode` is the program's real value (see above).
    BEZEL_EXPECT_EQ(a, 1);
}

// Common Tracked class: a static counter incremented once per destructor
// run, so Main()'s return value pins "disposed exactly N times" mechanically
// (a wrong count — too few from a leak masking disposal, too many from a
// double-dispose — fails the exit-code check even under
// expectAsanSafeIgnoringLeaks, which does not otherwise look at leaks).
const char* kTrackedClass =
    "class Tracked "
    "  public static _count: int := 0; "
    "  public method Touch() end Touch; "
    "  destructor Tracked._count := Tracked._count + 1; end; "
    "end Tracked; ";

}  // namespace

// THE HEADLINE: the issue's own repro verbatim — a `using var` local
// captured by a lambda passed to `scope.Spawn(...)`, disposed on the
// ORDINARY success path (no exception, no early return). Before the fix:
// LeakSanitizer reported "Direct leak of 8 byte(s) in 1 object(s)" from
// bz$async$Run$Resume on this exact program, unconditionally, on every run.
BEZEL_TEST(taskscope_asan_2334, normal_path_capture_no_leak) {
    expectAsanClean(
        std::string("module M; import System.Threading; ") + kTrackedClass +
        "class Prog "
        "  public static async method Run(): Task "
        "    using var a := new Tracked(); "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(3); "
        "      a.Touch(); "
        "    end); "
        "    await scope.Join(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Prog.Run(); "
        "    return Tracked._count; "
        "  end Main; "
        "end Prog;",
        1);
}

// Adversarial self-pass (a): the captured local is read AGAIN through the
// same boxed cell AFTER the Spawn call but BEFORE Join — the shape that
// would UAF if the fix instead freed the cell inside the closure's own
// envdtor (the root-cause note's originally suggested direction) rather
// than at the local's own scope end. Must stay fully clean.
BEZEL_TEST(taskscope_asan_2334, read_after_spawn_before_join_no_uaf) {
    expectAsanClean(
        std::string("module M; import System.Threading; ") + kTrackedClass +
        "class Prog "
        "  public static async method Run(): Task "
        "    using var a := new Tracked(); "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(3); "
        "      a.Touch(); "
        "    end); "
        "    a.Touch(); "
        "    await scope.Join(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Prog.Run(); "
        "    return Tracked._count; "
        "  end Main; "
        "end Prog;",
        1);
}

// Adversarial self-pass (c): the SAME local captured by TWO separate Spawn
// calls in the same scope — the classic double-free hazard for arming a
// cell-free. lastLambdaCapturedCells_ is only populated on the FIRST fresh
// boxing (subsequent captures see alreadyBoxed=true), so the cell must be
// freed exactly once regardless of capture count.
BEZEL_TEST(taskscope_asan_2334, double_capture_two_spawns_same_local_freed_once) {
    expectAsanClean(
        std::string("module M; import System.Threading; ") + kTrackedClass +
        "class Prog "
        "  public static async method Run(): Task "
        "    using var a := new Tracked(); "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(1); "
        "      a.Touch(); "
        "    end); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(2); "
        "      a.Touch(); "
        "    end); "
        "    await scope.Join(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Prog.Run(); "
        "    return Tracked._count; "
        "  end Main; "
        "end Prog;",
        1);
}

// A `return` statement (not fall-off-end) after Join still disposes through
// the ordinary try/finally desugaring (spec §28.10) — exercises the fix
// through a different codegen exit-emission point than falling off the
// method body.
BEZEL_TEST(taskscope_asan_2334, early_return_after_join_no_leak) {
    expectAsanClean(
        std::string("module M; import System.Threading; ") + kTrackedClass +
        "class Prog "
        "  public static async method Run(cond: bool): Task "
        "    using var a := new Tracked(); "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(3); "
        "      a.Touch(); "
        "    end); "
        "    if cond then "
        "      await scope.Join(); "
        "      return; "
        "    end; "
        "    await scope.Join(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Prog.Run(true); "
        "    return Tracked._count; "
        "  end Main; "
        "end Prog;",
        1);
}

// THE UNWIND EDGE (the case the adversarial probe caught a real UAF on
// during development of this fix): the scope disposes via DisposeAsync's
// documented (#1422) NON-BLOCKING zombie drain, never reaching an explicit
// Join at runtime — quiescence does not hold on this edge, so the fix must
// NOT free the cell here. Checked in two parts: (1) memory-SAFE (no UAF/
// double-free) with leak detection off, since the expected leak would
// otherwise mask a real bug behind LSan's own exit-code override (CLAUDE.md);
// (2) the conservative leak genuinely IS still present (skipCellFreeOnUnwind
// is actually armed, not vacuously true because nothing needed it).
BEZEL_TEST(taskscope_asan_2334, unwind_edge_conservative_leak_not_uaf) {
    const std::string source =
        std::string("module M; import System.Threading; ") + kTrackedClass +
        "class Prog "
        "  public static method Always(): bool => true; "
        "  public static async method RunException(): Task "
        "    using var a := new Tracked(); "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do "
        "      await Task.Delay(3); "
        "      a.Touch(); "
        "    end); "
        "    if Prog.Always() then "
        "      throw new ArgumentException(\"boom\"); "
        "    end; "
        "    await scope.Join(); "
        "  end RunException; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      await Prog.RunException(); "
        "    catch "
        "      var e: ArgumentException => do end "
        "    end; "
        "    return Tracked._count; "
        "  end Main; "
        "end Prog;";
    // `a`'s destructor still runs exactly once (the object itself is
    // disposed normally by the `using var` unwind path; only the 8-byte
    // CAPTURE-CELL free is what this fix conservatively skips on unwind).
    expectAsanSafeIgnoringLeaks(source, 1);
    expectAsanLeakDetected(source, 1);
}

// Control shape / #694 loop-guard interaction: `t` is declared BEFORE
// `scope` (satisfies §28.14's declared-before admission), but the Spawn call
// recurs across loop iterations, so boxLocalSlot uses GUARDED boxing for
// `t`'s cell (one cell shared across all three spawned closures) — ORIGINALLY
// (see 9ed5152, this file's own creation) exactly the shape
// LlvmBackend_Exprs2.cpp's `wasGuardedLocal`/`guardedOk` check conservatively
// excluded, mirroring #694's own `loopStack_.empty()` guard for the SAME
// reason: a single shared cell cannot be safely freed at any one capturing
// closure's own point without risking a UAF on a sibling iteration's
// still-live closure.
//
// fix #4577: that premise is now STALE, and this row's oracle was hiding it.
// #3498 (b43196a4, "narrow #3498: feed §28.14's call-bounded default to the
// capture-cell walker, and let #969 answer the in-loop question it already
// answers", §27.5/§28.9/§28.14, v0.283) added a `declaredShallower` conjunct
// to `guardedOk` (`git blame` on that line: b43196a48) — reading #969's own
// `declScopeDepth_` predicate, already computed at the capture site — and `t`
// is declared SHALLOWER than the loop that captures it, so `declaredShallower`
// is true here and `guardedOk` now admits the free. That free lands at `t`'s
// OWN scope end, i.e. AFTER `await scope.Join()` has already awaited every
// spawned iteration's closure to completion — which is exactly #3498's GAP-2
// argument (the exclusion was "answering a question its own consumer answers
// better") and is NOT the per-iteration free the original comment worried
// about, so it is sound, not a UAF: measured `detect_leaks=0` clean (no
// crash/UAF/double-free) AND `detect_leaks=1` clean (no leak) at the SAME
// exit code (1) — the row's own `expectAsanLeakDetected(source, 1)` FAILED
// the instant #4577 fixed that helper's vacuous oracle (hardcoded
// `expectedExitCode=0` made it unconditionally return 1, so it could not have
// caught #3498 closing this leak at the time it happened). Re-pointed at what
// is now true, with the single ordinary `expectAsanClean` this file's other
// fully-clean rows already use: `t`'s cell is reclaimed exactly once, with no
// leak and no UAF.
BEZEL_TEST(taskscope_asan_2334, loop_capture_over_shallower_local_now_reclaimed_3498) {
    const std::string source =
        std::string("module M; import System.Threading; ") + kTrackedClass +
        "class Prog "
        "  public static async method RunLoopCapture(): Task "
        "    using var t := new Tracked(); "
        "    await using var scope := new TaskScope(); "
        "    for var i in 0..2 do "
        "      scope.Spawn(async lambda() => do "
        "        await Task.Delay(1); "
        "        t.Touch(); "
        "      end); "
        "    end; "
        "    await scope.Join(); "
        "  end RunLoopCapture; "
        "  public static async method Main(): Task<int> "
        "    await Prog.RunLoopCapture(); "
        "    return Tracked._count; "
        "  end Main; "
        "end Prog;";
    expectAsanClean(source, 1);
}

// Control shape: EventSource<T>.Subscribe (also a `scoped` position, stdlib
// §6.5) capturing state reached from `this` — the `this`-bounded form this
// fix deliberately does NOT touch (see the LlvmBackend_Setup.cpp comment at
// the TaskScope.Spawn special case and issue #2383, filed to widen this
// fix's using-var-bounded argument to other §28.14 register positions). The
// fix's callee match is hardcoded to `Spawn` on a TaskScope receiver, so it
// structurally cannot fire here; this pins that non-interference
// mechanically rather than by inspection.
BEZEL_TEST(taskscope_asan_2334, event_subscribe_this_bounded_capture_unaffected) {
    expectAsanClean(
        "module M; "
        "class Ticker "
        "  private owned readonly _ticked: EventSource<int> := new(); "
        "  public property Ticked: IEvent<int> => _ticked; "
        "  public method Fire() _ticked.Raise(1); end Fire; "
        "end Ticker; "
        "class Observer "
        // Mirrors stdlib §6.5's own TickObserver worked example verbatim in
        // shape: `_sub` is an owned field, so §28.14 form 3 resolves the
        // bounding object to `this` — the handler capturing `this` (via the
        // OnTick call) is admitted regardless of how Ticker was reached.
        "  private owned _sub: Subscription; "
        "  private _seen: int := 0; "
        "  public constructor(t: Ticker) "
        "    _sub := t.Ticked.Subscribe(lambda(n: int) => do OnTick(n); end); "
        "  end constructor; "
        "  private method OnTick(n: int) "
        "    _seen := _seen + n; "
        "  end OnTick; "
        "  public property Seen: int => _seen; "
        "end Observer; "
        "class Prog "
        "  public static method Main(): int "
        "    using var t := new Ticker(); "
        "    using var o := new Observer(t); "
        "    t.Fire(); "
        "    return o.Seen; "
        "  end Main; "
        "end Prog;",
        1);
}
