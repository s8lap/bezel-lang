// Regression tests for #3377 — codegen LEAK introduced by the #2261 narrowing
// of `freshNewReceiverTempEligible`. §28.3 / §29.1 / §30.1 (v0.282).
//
// THE BUG. #2261's predicate narrowing correctly keeps the inline reclaim of
// `disposeFreshNewReceiverTemp` OFF for the deferred-activation result heads
// (`OwningTask` + the four `IOwning*` iterable-interface heads) — the
// iterator/task frame's settlement may still read `this`, so a same-expression
// inline dispose is a use-after-free (the §28.3 defect #2261 pins). But the
// narrowing leaves the fresh receiver's OWNERSHIP lost: the iterator/task
// frame's Dispose body (LlvmBackend_Iterators.cpp `emitDisposeBody`) disposes
// owned parameters, owned locals, closure captures and the yielded value but
// does NOT dispose its `this` capture. The fresh receiver is therefore left
// OWNED BY NOTHING — the §28.3 *Unbound owned temporaries* rule ("every
// unbound owned temporary is disposed exactly once") is violated in the LEAK
// direction.
//
// THE FIX. At the fresh-receiver call site (LlvmBackend_Calls2.cpp
// `lowerCallDispatch`), when `registerFreshNewReceiverTempUnwind` returns null
// BECAUSE of #2261's narrowing (i.e. the result head is one of the five
// deferred-activation heads), the companion helper
// `disposeFreshNewReceiverTempDeferred` pushes the receiver onto the CURRENT
// `disposeStack_` scope with `unwindOnly = false`. The entry fires on both
// normal exit and unwind; reverse-order scope-end iteration fires the source
// frame's `Dispose()` FIRST (which may read `this`), then the receiver — so
// `this` is still live for the frame's settlement, and the receiver is then
// reclaimed exactly once.
//
// THE SOUNDNESS GATE (c3 pins it). Pushing onto a LEXICAL scope is correct
// only when the frame provably dies in that scope. For `return new Src()
// .Items();` it does not, and the ungated first cut of this fix turned the
// leak into a `heap-use-after-free ... freed by ... bz$M.P$Make` — #2261's
// defect, back again. So the helper fires only when the call's result is
// bound, in the same statement, to a SCOPE-BOUND consumer: a `using var`
// initializer or a for-in iterable (`deferredRecvScopeBoundInit_` +
// `exprReceiverChainContains`). Both are scope-bound by the LANGUAGE — §28.3
// rejects `return move en;` out of a `using var` — so the frame cannot
// outlive the scope the receiver is pushed onto. Every other shape keeps the
// pre-fix leak, the sound direction; #3390 tracks threading ownership into
// the frame itself, which is what the escaping shapes need.
//
// THE ORACLE. `runAotChecked` — AOT compile + native exec under generated-code
// AddressSanitizer *with LeakSanitizer ON*. Two independent observables per
// case, which together make the test non-vacuous:
//
//   (1) PROGRAM-VISIBLE: `Src.live` is a ctor(+1)/destructor(-1) counter and
//       `Src.made` a construction count. The program prints
//       "live=<L> made=<M> sum=<S>" to stdout BEFORE returning. A leaked
//       receiver never runs its destructor, so pre-fix the d-cases print
//       `live=4` and post-fix `live=0`. Because the harness captures
//       `stdoutText` even when the run is later failed by an ASan/LSan report,
//       this assertion DISCRIMINATES a leak (`live=4`) from a compile failure
//       (empty stdout) — a distinction the `ran`-only form cannot make, and
//       the exact trap that made an earlier draft of this file vacuous
//       (all six cases "failed" pre-fix only because `IOwningEnumerable<int>`
//       is rejected outright: the owning-enumeration interfaces require a
//       CLASS element type, stdlib §4.13).
//   (2) SANITIZER: `ran` is true only for an ASan/LSan-clean run. Pre-fix,
//       LSan reports "Direct leak of 64 byte(s) in 4 object(s)".
//
// MEASURED (this file's exact sources, `scripts/agent_check.sh --probe --asan`):
//   d1 for-in                 pre-fix live=4 + LSan 4 leaks   post-fix live=0 clean
//   d2 GetOwningEnumerator    pre-fix live=4 + LSan 4 leaks   post-fix live=0 clean
//   d3 IOwningEnumerator      pre-fix live=4 + LSan 4 leaks   post-fix live=0 clean
//   d4 OwningTask (§30.1)     pre-fix live=4 + LSan 4 leaks   post-fix live=0 clean
//   c1 OwningArray control    pre-fix live=0 clean            post-fix live=0 clean
//   c2 bound-receiver control pre-fix live=0 clean            post-fix live=0 clean
//   c3 escaping frame         pre-fix live=1 leak-only        post-fix live=1 leak-only
//                             (UNGATED fix: heap-use-after-free — the gate's reason)
//
// EACH d-CASE LOOPS FOUR TIMES on purpose. A single-shot call would be
// disposed at the end of the ENCLOSING function scope, which is after the
// print — indistinguishable at the `live` observable from a leak that LSan
// happens to miss. Looping puts the receiver's disposal inside the loop-body
// scope, so a correct implementation must reclaim per-iteration and `live`
// reads 0 at print time. It also pins the entry-alloca reuse question: the
// receiver slot is ONE `entryAllocaNullPtr` alloca reused by every iteration,
// so an entry pushed onto a scope OUTSIDE the loop would dispose only the
// LAST receiver and leak the other three (`live=3`).
//
// PAIRING WITH #2261. The d-cases mirror `fresh_receiver_deferred_iterator
// _dispose_2261_test.cpp` d1/d2/d3/d4 (same `Src`/`Client`/`Res` shapes) with
// the live-counter instrumentation added and the body wrapped in a loop; that
// file's `runAotCheckedLenient` (LSan OFF) documented this leak as the known
// follow-up. THIS file re-enables LSan for the same four shapes, which is the
// acceptance criterion #3377 states.
//
// THE CONTROLS pin the fix's NARROW reach. c1: an element-owning collection
// result (`OwningArray<Res>`, #2205's headline) still takes the pre-existing
// INLINE reclaim — the new helper must NOT fire, because `OwningArray` is
// `isOwningCollectionHead` and neither `isTaskFamilyHead` nor
// `isIterableInterfaceHead`. c2: a BOUND receiver (`using var s := new Src();
// s.Items()`, the #2440 / §29.1 path) — `isFreshOwnedClassProducer` requires
// an UNBOUND `new`, so neither the inline reclaim nor the new helper fires
// and the using-var stays the sole owner. Both are clean pre- AND post-fix;
// a regression in either would show up as a double-free (ASan) rather than a
// leak, which is why they run under the same LSan-on oracle.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

struct Outcome {
    bool ran = false;       // ASan/LSan-clean AND the program executed
    std::string out;        // captured stdout, trailing whitespace trimmed
};

// `stdoutText` is populated by the harness BEFORE the ASan/LSan verdict is
// applied, so a leaking run still hands back what the program printed. That
// is what lets the `out` assertion separate "leaked" from "did not compile".
Outcome run(const std::string& src) {
    auto r = runAotChecked(src);
    Outcome o;
    o.ran = r.ran;
    o.out = r.stdoutText;
    while (!o.out.empty() &&
           (o.out.back() == '\n' || o.out.back() == '\r' || o.out.back() == ' '))
        o.out.pop_back();
    return o;
}

}  // namespace

// ============================================================================
// d1 — §29.1 / §28.3: for-in over `new Src().Items()`, four times. The fresh
// `Src` is transferred into the iterator frame's `this` capture and must stay
// live across every MoveNext; at the end of each loop iteration the frame is
// disposed and the receiver must be reclaimed with it. Pre-fix: four receivers
// leak (live=4, LSan 64 bytes in 4 objects). Post-fix: live=0, clean.
// Yields are `N` and `N*2` = 5 + 10 = 15 per iteration, so sum = 60.
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_activation_dispose_3377,
           for_in_fresh_receiver_disposed_at_frame_settlement_29_1) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Src "
        "  public static live: int := 0; "
        "  public static made: int := 0; "
        "  public N: int := 5; "
        "  public constructor() Src.live := Src.live + 1; "
        "    Src.made := Src.made + 1; end constructor; "
        "  destructor Src.live := Src.live - 1; end destructor; "
        "  public method Items(): owned IOwningEnumerable<Res> "
        "    yield return new Res(this.N); "
        "    yield return new Res(this.N * 2); "
        "  end Items; "
        "end Src; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 4 do "
        "      for var r in new Src().Items() do sum := sum + r.Id; end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"live=\" + Src.live.ToString() + "
        "      \" made=\" + Src.made.ToString() + "
        "      \" sum=\" + sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    Outcome o = run(src);
    // Program-visible: every receiver's destructor ran before the print.
    // Pre-fix this reads "live=4 made=4 sum=60"; a compile failure reads "".
    BEZEL_EXPECT_EQ(o.out, std::string("live=0 made=4 sum=60"));
    // Sanitizer-visible: no leak, no UAF, no double-free.
    BEZEL_EXPECT_TRUE(o.ran);
}

// ============================================================================
// d2 — §29.1 / §28.3: the manual `GetOwningEnumerator()` desugaring of d1.
// The fresh `Src` reaches the enumerator through TWO frames (the
// IOwningEnumerable produced by `Items()`, then the enumerator cloned from
// it), so this pins that the receiver's ownership survives the extra hop and
// is still reclaimed at the using-var's scope end. Yields `N` and `N+1`
// = 7 + 8 = 15 per iteration, sum = 60.
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_activation_dispose_3377,
           manual_enumerator_fresh_receiver_disposed_29_1) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Src "
        "  public static live: int := 0; "
        "  public static made: int := 0; "
        "  public N: int := 7; "
        "  public constructor() Src.live := Src.live + 1; "
        "    Src.made := Src.made + 1; end constructor; "
        "  destructor Src.live := Src.live - 1; end destructor; "
        "  public method Items(): owned IOwningEnumerable<Res> "
        "    yield return new Res(this.N); "
        "    yield return new Res(this.N + 1); "
        "  end Items; "
        "end Src; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 4 do "
        "      using var en := new Src().Items().GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        using var r := en.TakeCurrent(); "
        "        sum := sum + r.Id; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"live=\" + Src.live.ToString() + "
        "      \" made=\" + Src.made.ToString() + "
        "      \" sum=\" + sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    Outcome o = run(src);
    BEZEL_EXPECT_EQ(o.out, std::string("live=0 made=4 sum=60"));
    BEZEL_EXPECT_TRUE(o.ran);
}

// ============================================================================
// d3 — §29.1 / §28.3: `Items()` returns the `IOwningEnumerator<Res>` head
// DIRECTLY (one fewer frame than d2). This pins the `IOwningEnumerator` head
// specifically, not just `IOwningEnumerable`. One yield of `N` = 11 per
// iteration, sum = 44.
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_activation_dispose_3377,
           owning_enumerator_fresh_receiver_disposed_29_1) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Src "
        "  public static live: int := 0; "
        "  public static made: int := 0; "
        "  public N: int := 11; "
        "  public constructor() Src.live := Src.live + 1; "
        "    Src.made := Src.made + 1; end constructor; "
        "  destructor Src.live := Src.live - 1; end destructor; "
        "  public method Items(): owned IOwningEnumerator<Res> "
        "    yield return new Res(this.N); "
        "  end Items; "
        "end Src; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 4 do "
        "      using var en := new Src().Items(); "
        "      while en.MoveNext() do "
        "        using var r := en.TakeCurrent(); "
        "        sum := sum + r.Id; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"live=\" + Src.live.ToString() + "
        "      \" made=\" + Src.made.ToString() + "
        "      \" sum=\" + sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    Outcome o = run(src);
    BEZEL_EXPECT_EQ(o.out, std::string("live=0 made=4 sum=44"));
    BEZEL_EXPECT_TRUE(o.ran);
}

// ============================================================================
// d4 — §30.1 / §28.3: the async-task analog, pinning the `OwningTask` head.
// `Fetch(): owned OwningTask<Res>` is an `async` method whose state machine
// carries the receiver's borrow across `await Task.Delay(1)`, so the receiver
// must survive to settlement (`t.Wait()`) and be reclaimed at the using-var's
// scope end. `sum` counts completed iterations, = 4.
//
// The body deliberately does NOT read the result. #3391 settled why the
// original spelling `var r := t.Wait(); if r <> null then ...` was not the
// check it looked like: §30.3 declares `Wait()` with no return type, which
// §4.7 makes unit, so `r` was a `()` and every member access on it lowered
// to nothing (that is the #2961 "assignment RHS failed to lower" store-drop
// the old comment here pointed at). The binding is gone; `sum` counts
// iterations, and `live`/`made` carry this test's real signal. Reading the
// value would be `t.Result` (§30.3's escape hatch) — but a `.Result` read
// inside a loop leaks the result today (#3420), so this test
// stays on the iteration count.
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_activation_dispose_3377,
           owning_task_fresh_receiver_disposed_at_settlement_30_1) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Client "
        "  public static live: int := 0; "
        "  public static made: int := 0; "
        "  public N: int := 13; "
        "  public constructor() Client.live := Client.live + 1; "
        "    Client.made := Client.made + 1; end constructor; "
        "  destructor Client.live := Client.live - 1; end destructor; "
        "  public async method Fetch(): owned OwningTask<Res> "
        "    await Task.Delay(1); "
        "    return new Res(this.N); "
        "  end Fetch; "
        "end Client; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 4 do "
        "      using var t := new Client().Fetch(); "
        "      t.Wait(); "
        "      sum := sum + 1; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"live=\" + Client.live.ToString() + "
        "      \" made=\" + Client.made.ToString() + "
        "      \" sum=\" + sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    Outcome o = run(src);
    BEZEL_EXPECT_EQ(o.out, std::string("live=0 made=4 sum=4"));
    BEZEL_EXPECT_TRUE(o.ran);
}

// ============================================================================
// c1 — NARROW-FIX CONTROL (#2205 / §28.3). An element-owning collection result
// (`owned OwningArray<Res>`) is NOT a deferred-activation head: the produced
// array does not carry the receiver's borrow, so `freshNewReceiverTempEligible`
// still admits it and the pre-existing INLINE reclaim disposes the receiver at
// the end of the full expression. The new helper must not fire here — if it
// did, the receiver would be disposed twice (inline, then again at scope end)
// and ASan would report a double-free. Clean pre-fix AND post-fix.
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_activation_dispose_3377,
           owning_collection_receiver_still_inline_disposed_2205_control) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Mk "
        "  public static live: int := 0; "
        "  public static made: int := 0; "
        "  public constructor() Mk.live := Mk.live + 1; "
        "    Mk.made := Mk.made + 1; end constructor; "
        "  destructor Mk.live := Mk.live - 1; end destructor; "
        "  public method Make(): owned OwningArray<Res> "
        "    var a := new OwningArray<Res>(); "
        "    a.Add(new Res(1)); "
        "    a.Add(new Res(2)); "
        "    return a; "
        "  end Make; "
        "end Mk; "
        "class P "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 4 do "
        "      do _ := new Mk().Make(); end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"live=\" + Mk.live.ToString() + "
        "      \" made=\" + Mk.made.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    Outcome o = run(src);
    BEZEL_EXPECT_EQ(o.out, std::string("live=0 made=4"));
    BEZEL_EXPECT_TRUE(o.ran);
}

// ============================================================================
// c2 — NARROW-FIX CONTROL (#2440 / §29.1). A BOUND receiver: `using var s :=
// new Src();` then `s.Items()`. `isFreshOwnedClassProducer` requires an
// UNBOUND `new` expression, so neither the inline reclaim nor the new helper
// fires; the using-var is the sole owner and the iterator frame merely borrows
// it. If the new helper wrongly admitted a named local, this case would
// double-free. Clean pre-fix AND post-fix; one yield of N = 5, sum = 20.
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_activation_dispose_3377,
           bound_receiver_helper_unaffected_no_double_dispose) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Src "
        "  public static live: int := 0; "
        "  public static made: int := 0; "
        "  public N: int := 5; "
        "  public constructor() Src.live := Src.live + 1; "
        "    Src.made := Src.made + 1; end constructor; "
        "  destructor Src.live := Src.live - 1; end destructor; "
        "  public method Items(): owned IOwningEnumerable<Res> "
        "    yield return new Res(this.N); "
        "  end Items; "
        "end Src; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 4 do "
        "      using var s := new Src(); "
        "      for var r in s.Items() do sum := sum + r.Id; end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"live=\" + Src.live.ToString() + "
        "      \" made=\" + Src.made.ToString() + "
        "      \" sum=\" + sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    Outcome o = run(src);
    BEZEL_EXPECT_EQ(o.out, std::string("live=0 made=4 sum=20"));
    BEZEL_EXPECT_TRUE(o.ran);
}

// ============================================================================
// c3 — THE SOUNDNESS-GATE CONTROL. §28.3 / §29.1.
//
// The fix pushes the receiver onto a LEXICAL scope, which is only correct when
// the produced frame provably dies in that scope. Here it does NOT: `Make`
// RETURNS the iterator frame, so the frame outlives `Make`'s body scope and
// `Main` drives it afterwards.
//
// The first cut of this fix had no gate and MEASURED, on exactly this source:
//
//   ==561817==ERROR: AddressSanitizer: heap-use-after-free
//   READ of size 4 ... in main
//   freed by thread T0 here:  #1 ... in bz$M.P$Make
//
// i.e. it traded #2261's bounded leak straight back for the use-after-free
// #2261 exists to remove. `deferredRecvScopeBoundInit_` +
// `exprReceiverChainContains` gate the push to a `using var` initializer or a
// for-in iterable — both scope-bound by the LANGUAGE (§28.3 rejects
// `return move en;` out of a `using var`) — so this shape takes no push and
// keeps the pre-fix LEAK, which is the sound direction.
//
// ORACLE: `runAotCheckedLenient` — ASan ON, LeakSanitizer OFF. The load-bearing
// assertion is that ASan does NOT fire (no UAF) and the yields read a live
// `this` (`sum` = 5 + 10 = 15). LSan is off because the leak this case still
// has is the KNOWN, DELIBERATE remainder tracked by #3390.
//
// ⚠ #3390 WILL CHANGE THIS TEST. When ownership is threaded into the frame
// itself, this case stops leaking: `live=1` below becomes `live=0` and the
// oracle should move to `runAotChecked`. Update it there — do not delete it;
// the no-UAF assertion is what stops the gate from being removed carelessly.
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_activation_dispose_3377,
           escaping_frame_receiver_not_freed_in_callee_28_3) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Src "
        "  public static live: int := 0; "
        "  public N: int := 5; "
        "  public constructor() Src.live := Src.live + 1; end constructor; "
        "  destructor Src.live := Src.live - 1; end destructor; "
        "  public method Items(): owned IOwningEnumerable<Res> "
        "    yield return new Res(this.N); "
        "    yield return new Res(this.N * 2); "
        "  end Items; "
        "end Src; "
        "class P "
        "  public static method Make(): owned IOwningEnumerable<Res> "
        "    return new Src().Items(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var r in Make() do sum := sum + r.Id; end; "
        "    Console.WriteLine(\"live=\" + Src.live.ToString() + "
        "      \" sum=\" + sum.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;";
    auto r = runAotCheckedLenient(src);
    std::string out = r.stdoutText;
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    // `sum=15` proves both yields read a LIVE `this.N` — an ungated push frees
    // the receiver inside `Make` and this read is a use-after-free.
    // `live=1` is the known remaining leak (#3390), pinned deliberately.
    BEZEL_EXPECT_EQ(out, std::string("live=1 sum=15"));
    // No ASan fire: no use-after-free, no double-free. (LSan is off here.)
    BEZEL_EXPECT_TRUE(r.ran);
}
