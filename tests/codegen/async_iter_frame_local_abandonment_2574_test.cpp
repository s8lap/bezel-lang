// Regression tests for #2574 (spec §29.7 / §30.7, v0.236): the async sibling
// of #2306 — an ASYNC owning iterator's frame teardown leaks plain-owned `var`
// locals on consumer abandonment. §29.7's pass runs "the pending cleanup
// actions active at the point of suspension … in reverse order of
// establishment, exactly as if the body had exited at the suspension point".
// Before the fix the async iterator's Dispose (emitAsyncIteratorMethod /
// LlvmBackend_Async.cpp) had the SAME architectural gap #2306 fixed for the
// sync path: smIterCleanupAtState_'s snapshot at each yield skipped
// `unwindOnly` entries (the YieldStmt handler in LlvmBackend_Stmts.cpp), so a
// plain `var L := new C(...);` later transferred by `yield return L;` on an
// IOwningAsyncEnumerable<T> iterator (#1863) leaked when the consumer
// abandoned mid-suspension.
//
// The fix mirrors #2306: a `collectAsyncSuspendStatesForOwnedLocals` pre-pass
// walks the body in linear order recording (establishedAtState,
// transferredAtState) per local — counting BOTH yields AND awaits (async
// suspend states, §30.7) so the window matches the runtime
// smNextSuspendState_ numbering — and a `disposeOwnedLocals` sweep inside the
// async iterator's emitFreeAndReturn gates each local's slot on the live
// window. Restricted to IOwningAsyncEnumerable<T>.
//
// Spec authority: §29.7 "the pending cleanup actions active at the point of
// suspension … exactly as if the body had exited at the suspension point";
// §30.7 async iteration with owned yields; §29.2 iterator yield ownership.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// `Box` carries static live/dead counters and prints its dispose id so
// LSan-clean and exactly-once semantics are observable from outside ASan.
const char* kBox =
    "class Box "
    "  public static live: int := 0; "
    "  public static dead: int := 0; "
    "  public Id: int := 0; "
    "  public constructor(id: int) Id := id; Box.live := Box.live + 1; end constructor; "
    "  destructor Console.WriteLine(\"dispose \" + Id.ToString()); Box.dead := Box.dead + 1; end destructor; "
    "end Box; ";

// The async iterator body from the issue: `var a := new Box(1);` is a plain-
// owned local that is transferred by `yield return a;` (owning async
// iterable). If the consumer abandons before that yield, `a` must still be
// disposed by the frame teardown (§29.7).
const char* kGen =
    "class Gen "
    "  public static async method Produce(): IOwningAsyncEnumerable<Box> "
    "    var a := new Box(1); "
    "    yield return new Box(2); "
    "    yield return a; "
    "  end Produce; "
    "end Gen; ";

}  // namespace

// §29.7 / #2574 — the brief's exact repro (async form). Yield 1 transfers
// Box(2) to the consumer; the consumer breaks BEFORE the second MoveNextAsync,
// so the body never reaches `yield return a;` — local `a` (Box 1) is still
// owned by the frame at the suspension point. Without the fix the frame-
// teardown visit does not dispose `a` (live=2 dead=1, LSan leak); with the
// fix the state-keyed dispose fires (establishedAtState=1,
// transferredAtState=2 → live window [1, 2)), Box(1) is disposed exactly once.
BEZEL_TEST(async_iter_frame_local_abandonment_2574,
           abandon_after_first_yield_disposes_untransferred_local_29_7) {
    const std::string src = std::string("module M; ") + kBox + kGen +
        "class C "
        "  public static async method Main(): Task<int> "
        "    Box.live := 0; "
        "    Box.dead := 0; "
        "    await for var x in Gen.Produce() do "
        "      using var u := owned x; "
        "      break; "
        "    end; "
        "    return Box.live - Box.dead; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 2"), 1);
}

// §29.7 / #2574 — full consumption control. The consumer drains both yields.
// The body reaches `yield return a;`, which transfers `a`'s ownership to the
// consumer (TakeCurrent). The iterator's Dispose at exhaustion must NOT
// re-dispose `a` (the state-keyed predicate excludes it: state ==
// transferredAtState). Each Box disposed exactly once — no double-free.
BEZEL_TEST(async_iter_frame_local_abandonment_2574,
           full_drain_does_not_redispose_transferred_local_29_7) {
    const std::string src = std::string("module M; ") + kBox + kGen +
        "class C "
        "  public static async method Main(): Task<int> "
        "    Box.live := 0; "
        "    Box.dead := 0; "
        "    await for var x in Gen.Produce() do "
        "      using var u := owned x; "
        "    end; "
        "    return Box.live - Box.dead; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 2"), 1);
}

// §29.7 / §30.7 / #2574 — the await-yield interleave. An `await` between the
// two yields consumes a suspend state (smNextSuspendState_ is pre-incremented
// for BOTH yields and awaits), shifting the second yield's state number. The
// linear scan MUST count awaits so the live window [1, 3) matches the runtime
// state numbering. Consumer breaks after the first yield (state 1); Box(1) is
// live and disposed. Without await-counting the window would be [1, 2) and
// would be incorrect for the await-suspended state 2.
BEZEL_TEST(async_iter_frame_local_abandonment_2574,
           await_between_yields_state_keyed_window_correct_30_7) {
    const std::string src = std::string("module M; ") + kBox +
        "class Gen "
        "  public static async method Produce(): IOwningAsyncEnumerable<Box> "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    await Task.Delay(0); "
        "    yield return a; "
        "  end Produce; "
        "end Gen; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    Box.live := 0; "
        "    Box.dead := 0; "
        "    await for var x in Gen.Produce() do "
        "      using var u := owned x; "
        "      break; "
        "    end; "
        "    return Box.live - Box.dead; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 2"), 1);
}

// §29.7 / #2574 — two locals, each transferred at a different yield. `b` is
// transferred at the first yield (window closed), `a` at the second (still
// live at state 1). On break after the first yield, only `a`'s slot is
// disposed; `b`'s slot is null via consumer TakeCurrent.
BEZEL_TEST(async_iter_frame_local_abandonment_2574,
           two_locals_state_keyed_only_untransferred_at_suspension_29_7) {
    const std::string src = std::string("module M; ") + kBox +
        "class Gen "
        "  public static async method Produce(): IOwningAsyncEnumerable<Box> "
        "    var a := new Box(1); "
        "    var b := new Box(2); "
        "    yield return b; "
        "    yield return a; "
        "  end Produce; "
        "end Gen; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    Box.live := 0; "
        "    Box.dead := 0; "
        "    await for var x in Gen.Produce() do "
        "      using var u := owned x; "
        "      break; "
        "    end; "
        "    return Box.live - Box.dead; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // a (Box 1) was NOT transferred at the first yield → disposed by frame teardown.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 1"), 1);
    // b (Box 2) WAS transferred at the first yield → disposed by consumer's using var.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 2"), 1);
}
