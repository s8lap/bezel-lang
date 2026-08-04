// Codegen — issue #2591 / spec §29.1 "Current semantics on borrowing and owning
// enumerators" (v0.236) + §30.7 (async iterators):
//
//   §29.1 (the governing sentence, v0.193): "under either strategy the
//   enumerator **owns** the pending value from a successful advance until
//   `TakeCurrent()` transfers it, and a `Dispose()`/`DisposeAsync()` after an
//   advance whose value was never taken **disposes the pending value**. An
//   advanced-but-untaken element neither leaks nor reaches the consumer."
//
// The async owning iterator's Dispose body (emitAsyncIteratorMethod's
// emitFreeAndReturn, LlvmBackend_Async.cpp) had no equivalent of the sync
// iterator's disposeYieldedVal (§29 / #1120, LlvmBackend_Iterators.cpp:1719):
// when a consumer abandoned after a successful MoveNextAsync WITHOUT calling
// TakeCurrent, the yielded owned value left in current_val leaked. The fix
// mirrors the sync pass — dispose a non-null current_val in the async Dispose
// for an owning iterator (isOwningIter) with a reference element type — and
// additionally recognizes IOwningAsyncEnumerator<T> (the enumerator-direct
// return form) as an owning iterator head, so the owning contract (TakeCurrent
// + disposeYieldedVal) applies to it too (pre-#2591 it was misclassified as the
// borrowing form and leaked every yielded value).
//
// The dominant `await for` path always pairs MoveNextAsync with TakeCurrent,
// so it masks the gap; the bug is only reachable via the MANUAL protocol
// (MoveNextAsync + Dispose without TakeCurrent, §29.1's "manual-protocol
// abandonment"). The IOwningAsyncEnumerable manual protocol is currently
// blocked by a separate codegen gap (#2434, lowerLocalDecl), so the regression
// exercises the IOwningAsyncEnumerator-direct form — the owning iterator shape
// the fix's recognition arm makes testable at runtime.
//
// FAILS WITHOUT / PASSES WITH (the fix): test 1 below
// (abandon_without_take_disposes_pending_value) leaks a 16-byte Res under LSan
// without the fix (current_val never disposed); r.ran flips false → true. Tests
// 2-3 are controls pinning exactly-once (no double-free) and the unaffected
// `await for` dominant path.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Tracking resource: ctor/dtor on static counters (the §22.12-admitted mutable
// static int form used by the existing #1891 suite). live - disposed == 0
// iff every allocation was disposed exactly once.
const char* kRes =
    "class Res "
    "  public static live: int := 0; "
    "  public static disposed: int := 0; "
    "  public constructor() Res.live := Res.live + 1; end constructor; "
    "  destructor Res.disposed := Res.disposed + 1; end destructor; "
    "end Res; ";

// Enumerator-direct owning iterator: yields owned Res values. The form the
// fix's IOwningAsyncEnumerator recognition arm makes correctly owning.
const char* kGenEnum =
    "class Gen "
    "  public static async method Produce(): IOwningAsyncEnumerator<Res> "
    "    yield return new Res(); "
    "    yield return new Res(); "
    "  end Produce; "
    "end Gen; ";

// Enumerable owning iterator: the spec-sanctioned IOwningAsyncEnumerable<T>
// return (§30.7), exercised by the `await for` control below.
const char* kGenEnumBl =
    "class GenB "
    "  public static async method Produce(): IOwningAsyncEnumerable<Res> "
    "    yield return new Res(); "
    "    yield return new Res(); "
    "  end Produce; "
    "end GenB; ";

}  // namespace

// §29.1 v0.236 (the bug): manual protocol — advance once (a Res is yielded
// into current_val), then abandon WITHOUT TakeCurrent. The §29.1 pending-value
// rule requires Dispose to dispose the advanced-but-untaken value. Before the
// fix current_val leaked (LSan: 16-byte Res from bz$asynciter$Produce$Resume);
// after, it is disposed exactly once. live - disposed == 0.
BEZEL_TEST(async_iter_owning_dispose_yielded_val_2591,
           abandon_without_take_disposes_pending_value) {
    const std::string src = std::string("module M; ") + kRes + kGenEnum +
        "class C "
        "  public static async method Main(): Task<int> "
        "    do "
        "      await using var en := Gen.Produce(); "
        "      await en.MoveNextAsync(); "
        "    end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §29.1 / §29.2 control: full consumption via TakeCurrent must dispose each
// yielded Res exactly once — NO double-free. TakeCurrent (vtable slot 3) nulls
// current_val when transferring ownership, so the Dispose path's
// disposeYieldedVal sees a null slot and is a no-op here; each Res is disposed
// by its own `using var`. Pins that the fix did not introduce a double-free on
// the take-then-abandon edge. live - disposed == 0.
BEZEL_TEST(async_iter_owning_dispose_yielded_val_2591,
           full_consumption_takecurrent_exactly_once_no_double_free) {
    const std::string src = std::string("module M; ") + kRes + kGenEnum +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var n := 0; "
        "    do "
        "      await using var en := Gen.Produce(); "
        "      await en.MoveNextAsync(); "
        "      using var a := en.TakeCurrent(); n := n + 1; "
        "      await en.MoveNextAsync(); "
        "      using var b := en.TakeCurrent(); n := n + 1; "
        "    end; "
        "    if n <> 2 then return 50; end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §30.7 control: the dominant `await for` path over IOwningAsyncEnumerable<T>
// always pairs MoveNextAsync with TakeCurrent, so it was already correct and
// must remain unaffected by the fix (no regression). Each yielded Res is
// taken and auto-dropped in the loop body. live - disposed == 0.
BEZEL_TEST(async_iter_owning_dispose_yielded_val_2591,
           await_for_owning_path_unaffected) {
    const std::string src = std::string("module M; ") + kRes + kGenEnumBl +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var n := 0; "
        "    await for var r in GenB.Produce() do "
        "      r.Dispose(); "
        "      n := n + 1; "
        "    end; "
        "    if n <> 2 then return 50; end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}
