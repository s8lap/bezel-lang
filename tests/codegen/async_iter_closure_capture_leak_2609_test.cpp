// Codegen / leak — a `func`-typed (closure) parameter passed to an ASYNC
// ITERATOR is captured into the iterator's state-machine FRAME, which must be
// able to invoke it across every MoveNextAsync. Nothing ever dropped that
// reference: the caller conservatively treats a closure passed as a call
// argument as escaping (it suppresses its own scope-end release, transferring
// responsibility to the callee — LlvmBackend_Setup.cpp ClosureEscapeWalker),
// the async frame neither retained nor released it, and lowerCall's
// ARGUMENT-PASS release was scoped to SYNC-iterator callees only. The cell (40
// bytes) + its env block (8 bytes) therefore leaked on every async-iterator
// call. Issue #2609 — the async sibling of #1864 (the sync-iterator fix,
// LlvmBackend_Iterators.cpp).
//
// SPEC (v0.236). §27.5 "Closures" — a closure's captured state is heap-
// allocated; §28.9 "Runtime-managed types" — the reference-counted runtime
// values (a closure cell among them) whose count the compiler maintains, with
// captured values themselves being borrows; §29.7 "Disposal of partially-
// consumed iterators" and §30.7 "Async iterators" — the async iterator frame's
// teardown is its Dispose(), which is where whatever the frame holds is
// dropped. These are the same four sections #1864 cited for the sync path.
//
// THE ISSUE'S OWN FIX DIRECTION WAS UNSOUND — recorded here because the shape
// is a trap. #2609 proposed simply mirroring the sync `releaseClosureCaptures`
// loop into the async `emitFreeAndReturn`. Doing ONLY that is a DOUBLE-FREE, on
// the simplest possible repro (`single_await_for_closure_arg_is_leak_free`
// below), and it was demonstrated mechanically before this fix was written:
//
//     AddressSanitizer: attempting double-free ... in bz_closure_release
//       #2 bz$asynciter$Gen$Dispose   <- the CLONE's Dispose
//       ... freed by thread T0 here:
//       #2 bz$asynciter$Gen$Dispose   <- the SOURCE's Dispose
//
// The reason is a structural difference from the sync path. The sync iterator
// emits TWO Dispose bodies (`disposeFn` for the source, `cloneDisposeFn` for a
// per-loop enumerator) and passes `releaseClosures=false` for the clone, so only
// the source drops the frame's +1. The ASYNC iterator emits ONE vtable and ONE
// Dispose, shared by the source frame and by every `GetAsyncEnumerator` clone —
// and that clone is a raw `memcpy` of the source frame, so it ALIASES the very
// same closure-cell pointer without taking a count. A release in the shared
// Dispose therefore fires once per frame over one cell.
//
// THE FIX (three paired sites, LlvmBackend_Async.cpp + LlvmBackend_Calls2.cpp):
//   retain 1-of-2  construction entry  — the SOURCE frame takes its own +1
//   retain 2-of-2  GetAsyncEnumerator  — each CLONE takes its own +1
//   release        emitFreeAndReturn   — each frame drops exactly its own +1
//   call site      lowerCall           — the async iterator joins the iterator
//                                        ARGUMENT-PASS release, dropping the
//                                        producer's transient +1
// The first three are rc-NEUTRAL by construction (one retain per frame that can
// reach Dispose, one release in Dispose); they exist so the fourth — which is
// what actually closes the leak — cannot free a cell the frame still holds.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan: a leaked closure
// cell AND a double-free/UAF both flip `r.ran` to false, so every test below is
// simultaneously a leak assertion and an exactly-once assertion. A trailing
// statement after the consumption loop is REQUIRED for the without-fix leak to
// be LSan-visible (LSan is a reachability detector; without a trailer the dead
// register still holding the closure pointer keeps the cell "reachable") — it is
// part of every repro here, exactly as in the #1864 sync test.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required to
// build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The closure argument is deliberately NON-capturing (`lambda() => 7`). A
// lambda that captures a local also allocates a §27.5 shared cell for the
// captured variable, and THAT cell leaks on the sync path identically (verified:
// the same 4-byte residual appears for `for var x in SyncGen(lambda() => k)`),
// i.e. it is a distinct, pre-existing defect filed separately. Keeping the
// capture list empty isolates #2609's closure cell + env block.
const char* kAsyncGen =
    "class C "
    "  public static async method Gen(make: () -> int): IAsyncEnumerable<int> "
    "    yield return make(); "
    "    yield return make(); "
    "  end Gen; ";

}  // namespace

// ── The #2609 repro ────────────────────────────────────────────────────────
// One `await for` over an async iterator taking a `func` parameter. Two frames
// reach Dispose (the source returned by `Gen(...)` and the clone
// `GetAsyncEnumerator` hands the loop), so this single test pins BOTH halves:
// without the retains it double-frees, without the call-site release it leaks
// 48 bytes. Sum 7 + 7 = 14.
BEZEL_TEST(async_iter_closure_capture_leak_2609,
           single_await_for_closure_arg_is_leak_free) {
    auto r = runAotChecked(
        std::string("module M; ") + kAsyncGen +
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen(lambda() => 7) do "
        "      sum := sum + x; "
        "    end; "
        "    Console.WriteLine(\"sum=\" + sum.ToString()); "  // trailer: makes the leak LSan-visible
        "    return sum; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);      // false on a leak OR a double-free
    BEZEL_EXPECT_EQ(r.exitCode, 14);
}

// ── §29.11-style re-iteration: THREE frames over ONE cell ──────────────────
// A bound `IAsyncEnumerable<T>` source driven by two successive `await for`
// loops spawns two clones; with the source that is three frames whose shared
// Dispose body each touches the same closure cell. Exactly-once holds only
// because each clone takes its own +1 at GetAsyncEnumerator. This is the test
// that fails loudest (ASan double-free) if the clone-side retain is dropped.
// 7 + 7 + 7 + 7 = 28.
BEZEL_TEST(async_iter_closure_capture_leak_2609,
           reiterated_bound_source_releases_closure_exactly_once) {
    auto r = runAotChecked(
        std::string("module M; ") + kAsyncGen +
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    using var src := Gen(lambda() => 7); "
        "    await for var x in src do sum := sum + x; end; "
        "    await for var y in src do sum := sum + y; end; "
        "    Console.WriteLine(\"sum=\" + sum.ToString()); "
        "    return sum; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 28);
}

// ── §29.8/§30.1 owned-parameter disposal is undisturbed ────────────────────
// An `owned` class parameter and a `func` parameter captured into the SAME
// frame. §29.8's auto-dispose sweep and #2609's release loop must remain
// disjoint sets — an `owned` parameter has a Named class type, never
// Kind::Function. The static live-object counter asserts the Cell is disposed
// EXACTLY once (live == 0 at exit; a double-dispose would trip ASan first, an
// omitted dispose leaves live == 1), while LSan simultaneously asserts the
// closure cell is gone. Result 5 + 7 + 7 == 19, plus live (0).
BEZEL_TEST(async_iter_closure_capture_leak_2609,
           owned_param_beside_closure_param_disposed_exactly_once) {
    auto r = runAotChecked(
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public V: int := 0; "
        "  public constructor(v: int) V := v; Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static async method Gen(owned c: Cell, make: () -> int): IAsyncEnumerable<int> "
        "    yield return c.V + make(); "
        "    yield return make(); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen(new Cell(5), lambda() => 7) do "
        "      sum := sum + x; "
        "    end; "
        "    Console.WriteLine(\"live=\" + Cell.live.ToString()); "
        "    return sum + Cell.live; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 19);   // 19 + live(0)
}

// ── Routing rule 2 (#442): an async-iterator body with NO yield ────────────
// `Emitter::emitMethod` routes `async` + an `IAsyncEnumerable<T>` return type to
// emitAsyncIteratorMethod EVEN WITH NO YIELD, so that frame carries the retain
// too. The call-site predicate must match that routing exactly — keying it on
// `containsYield` alone would skip the release here and leak (or, symmetrically,
// keying it on the return type alone would release for a plain async callee
// whose frame never retained: a UAF). The loop yields nothing; result is 1.
BEZEL_TEST(async_iter_closure_capture_leak_2609,
           yieldless_async_iterator_body_still_releases_closure) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static async method Gen(make: () -> int): IAsyncEnumerable<int> "
        "    Console.WriteLine(\"gen=\" + make().ToString()); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 1; "
        "    await for var x in Gen(lambda() => 7) do sum := sum + x; end; "
        "    Console.WriteLine(\"sum=\" + sum.ToString()); "
        "    return sum; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── Instance async iterator: the `this` capture must be skipped ────────────
// `frame.captures[0]` is `this`, which is not a ParameterSymbol; both retain
// loops and the release loop must skip it (a `dynamic_cast` to ParameterSymbol
// returning null), and the call site must offset past the receiver argument.
// Getting either wrong retains/releases a class instance pointer as if it were a
// closure cell. 3 + 7 + 7 == 17.
BEZEL_TEST(async_iter_closure_capture_leak_2609,
           instance_async_iterator_skips_this_capture) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public Base: int := 3; "
        "  public async method Gen(make: () -> int): IAsyncEnumerable<int> "
        "    yield return Base + make(); "
        "    yield return make(); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    using var o := new C(); "
        "    await for var x in o.Gen(lambda() => 7) do sum := sum + x; end; "
        "    Console.WriteLine(\"sum=\" + sum.ToString()); "
        "    return sum; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 17);
}
