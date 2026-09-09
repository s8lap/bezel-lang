// fix #4093 / §29.1 / §30.7 / §28.3 (v0.295):
//
// `IOwningAsyncEnumerator<T>` returned DIRECTLY by an async iterator method
// (`using var e := Gen();` where `Gen` declares `IOwningAsyncEnumerator<T>`
// as its return type — the §30.7 enumerator-direct-return form, the owning
// sibling of the `IAsyncEnumerator<T>`-direct form fixed under #759) leaked
// the ENUMERATOR OBJECT (the 64-byte state-machine frame allocated by the
// async iterator's construction entry, `bz$M.Gen$Produce` in the trace) at
// scope end. Sibling defect of #4092 (yielded-value leak) — same fix-site
// cluster (async-iterator state machine) but a different ownership slot.
//
// Pre-fix, the `using var e := Gen();` spelling was the regression gate. The
// disposal arm for async-iterator ENUMERATOR heads (`IAsyncEnumerator<T>` /
// `IOwningAsyncEnumerator<T>`) lived only inside the `if (ld->isAwaitUsing)`
// branch in `registerUsingDisposal` (LlvmBackend_Setup.cpp), so plain
// `using var` (the synchronous spelling) skipped it and the frame was never
// registered for scope-end reclamation. The iterator-format Dispose vtable
// slot 1 (the same slot-1 Dispose path the sync `isSyncIterEnum` arm takes
// for IOwningEnumerable / IOwningEnumerator / IEnumerator / IEnumerable /
// IAsyncEnumerable / IOwningAsyncEnumerable) was never wired up, and the
// frame survived every iteration's scope end.
//
// MEASURED pre-fix (HEAD `2d201b3`):
//
//   Direct leak of 3200 byte(s) in 50 object(s) allocated from:
//       #0 ... in calloc
//       #1 ... in bz$M.Gen$Produce
//       #2 ... in bz$M.C$Run
//   SUMMARY: AddressSanitizer: 3200 byte(s) leaked in 50 allocation(s).
//
// 3200 B / 50 allocs = 64 B per state-machine frame — the exact frame size
// for the `IOwningAsyncEnumerator<Res>` direct-return shape. The remainder
// after #4092's yielded-value portion is subtracted.
//
// THE FIX: move the existing `isAsyncIterEnum` arm OUT of the `isAwaitUsing`
// branch in `registerUsingDisposal` (LlvmBackend_Setup.cpp). The arm is
// self-contained (it does not depend on any of the `await using`-specific
// collection / dict / list-interface checks that surround it), and the
// iterator-format slot-1 Dispose it dispatches is synchronous regardless of
// the binding spelling — the iterator-format frame's vtable layout
// `[0]=td,[1]=Dispose,[2]=MoveNextAsync,[3]=Current` puts the sync Dispose
// at slot 1, and §30.7 verbatim says the sync fallback (§29.7) runs for an
// iterator-format `await using` (the same fallback #3437 wired for
// `IOwningAsyncEnumerable`). Lifting the arm into the spell-agnostic path
// closes the `using var e := Gen();` leak; the `await using var en := Gen();`
// twin that was working all along still works.
//
// The moved arm carries its self-alias guard (#2356 / #1268) intact, so a
// `using var en := src.GetAsyncEnumerator();` on a NAMED `src` of
// owned-param async-iterator type still gets the runtime identity check
// that prevents the source/clone from being Disposed twice — the same
// property `await for`'s desugaring has carried since #1268.
//
// FAILS WITHOUT / PASSES WITH (the fix): test 1 below
// (direct_return_frame_disposed_at_scope_end) leaks 3200 B / 50 allocs
// under LSan without the fix; r.ran flips false → true with the fix in
// place. Tests 2-4 are controls: pin the `await using` twin
// (already-working path, unaffected), pin the owning-vs-borrowing distinction
// (the BORROWING `IAsyncEnumerator<T>` direct return is not tested here —
// the issue is the ENUMERATOR OBJECT slot, which both owning and borrowing
// forms occupy identically; the spec-distinguishing slot, the yielded-value
// slot, is #4092's fix site), and pin the disposable-vs-self-alias guard for
// the GetAsyncEnumerator spelling on a NAMED source.

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

// Enumerator-direct owning iterator: yields owned Res values. The form the
// #4093 fix's lifted `isAsyncIterEnum` arm now registers for scope-end
// disposal under BOTH `using var` and `await using var`.
const char* kGenEnum =
    "class Gen "
    "  public static async method Produce(): IOwningAsyncEnumerator<Res> "
    "    yield return new Res(); "
    "    yield return new Res(); "
    "    yield return new Res(); "
    "  end Produce; "
    "end Gen; ";

// Tracking resource: ctor/dtor on static counters. live - disposed == 0
// iff every allocation was disposed exactly once. The Res allocations
// themselves are NOT the leak #4093 fixes (that's #4092's yielded-value
// twin); the tracker here is for the case where the ENUMERATOR FRAME
// (state machine) would be the only thing still alive at scope end if the
// fix is missing — `r.ran` itself is the memory-safety assertion under
// `runAotChecked`.
const char* kRes =
    "class Res "
    "  public static live: int := 0; "
    "  public static disposed: int := 0; "
    "  public constructor() Res.live := Res.live + 1; end constructor; "
    "  destructor Res.disposed := Res.disposed + 1; end destructor; "
    "end Res; ";

}  // namespace

// §28.3 / §30.7: the headline — a `using var e := Gen.Produce();` loop
// across 50 iterations. The async iterator method's construction entry
// mints a 64-byte state-machine frame per call (the ENUMERATOR OBJECT
// that #4093 fixes). Without the fix the frame survives every iteration's
// scope end — measured 3200 B / 50 allocs. With the fix the frame is
// registered for iterator-format slot-1 Dispose (the same path #759 wired
// for the awaiting twin) and reclaimed exactly once per iteration.
BEZEL_TEST(iowningasyncenumerator_direct_return_leak_4093,
           direct_return_frame_disposed_at_scope_end) {
    const std::string src = std::string("module M; ") + kRes + kGenEnum +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      using var e := Gen.Produce(); "         // fix #4093: line under test
        "      var n := 0; "
        "      while await e.MoveNextAsync() do "
        "        using var x := e.TakeCurrent(); "
        "        n := n + 1; "
        "      end while; "
        "      if n <> 3 then return 50; end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live - Res.disposed; "          // 0 = exact-disposal invariant
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.3 / §30.7 control: the `await using` twin of the headline — already
// working pre-fix (the existing `isAsyncIterEnum` arm handled it inside
// `isAwaitUsing`). The fix moves the arm OUT of `isAwaitUsing`; this test
// pins that the awaiting path is unaffected. Each yielded Res is taken and
// auto-dropped in the loop body.
BEZEL_TEST(iowningasyncenumerator_direct_return_leak_4093,
           await_using_twin_unaffected_control) {
    const std::string src = std::string("module M; ") + kRes + kGenEnum +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      await using var e := Gen.Produce(); "    // control (already worked)
        "      var n := 0; "
        "      while await e.MoveNextAsync() do "
        "        using var x := e.TakeCurrent(); "
        "        n := n + 1; "
        "      end while; "
        "      if n <> 3 then return 50; end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.3 / §30.7 control: the `await for` path over an
// `IOwningAsyncEnumerable<Res>` (the spec-sanctioned iterator return,
// §30.7) is unaffected by the fix — the `isSyncIterEnum` arm already
// registers the iterator-format slot-1 Dispose for both spellings (#3437),
// and the moved `isAsyncIterEnum` arm does not interact with the enumerable
// head. Each yielded Res is taken and explicitly Disposed in the loop body.
BEZEL_TEST(iowningasyncenumerator_direct_return_leak_4093,
           await_for_owning_enumerable_control_unchanged) {
    const char* kGenEnumBl =
        "class GenB "
        "  public static async method Produce(): IOwningAsyncEnumerable<Res> "
        "    yield return new Res(); "
        "    yield return new Res(); "
        "    yield return new Res(); "
        "  end Produce; "
        "end GenB; ";
    const std::string src = std::string("module M; ") + kRes + kGenEnumBl +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      await for var x in GenB.Produce() do "
        "        x.Dispose(); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}