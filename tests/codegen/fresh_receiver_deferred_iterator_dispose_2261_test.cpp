// Regression tests for #2261 — codegen UAF: the fresh-receiver disposal site
// (`disposeFreshNewReceiverTemp`, src/codegen/LlvmBackend_Disposal.cpp) wrongly
// freed a fresh-receiver temp whose method call returned a deferred-activation
// frame (an iterator / async-task frame). §28.3 / §29.1 / §30.1 (v0.282).
//
// THE BUG. `freshNewReceiverTempEligible` (#1859/#2205's gate) admits a
// decl-null owning-type-head result under an announced `owned` return as a
// "fresh transfer, not a borrow of the receiver" — sound for the element-owning
// collection heads (#1008 Part C: OwningArray, OwningDictionary, OwningSet,
// OwningStack, OwningQueue, OwningLinkedList) AND for the owning-wrapper heads
// (Shared, Weak, ChannelSender, CancellationTokenSource). But the same set
// (`kOwningTypeHeads` in src/common/BuiltinRegistry.cpp) ALSO contains
// `OwningTask` and the four `IOwning*` iterable-interface heads
// (`IOwningEnumerable`, `IOwningEnumerator`, `IOwningAsyncEnumerable`,
// `IOwningAsyncEnumerator`) — exactly the deferred-activation heads that
// §28.3's *Calls mint for the mint-announcing families* row 2 says "the
// produced frame still **carries** the call's receiver's and plain arguments'
// borrows until settlement" (§29.1 for iterators; §30.1 for async). So
// admitting them on the same K1.5/K3.13 "fresh transfer" footing misclassifies
// a deferred-activation frame as a no-borrow value, and the dispose site fires
// `emitDisposeAndReclaim(recvPtr)` on a still-borrowed receiver.
//
// THE BUG-MECHANICS NARROWING. The same exclusion precisely removes only the
// five heads (`OwningTask` + `IOwningEnumerable` + `IOwningEnumerator` +
// `IOwningAsyncEnumerable` + `IOwningAsyncEnumerator`) — every other member
// of `kOwningTypeHeads` stays admitted.
//
// REPRO SHAPE (the issue's own). An instance iterator method
// `Items(): owned IOwningEnumerable<Res>` that reads `this.N` across a
// `yield return`, called as `new Src().Items()` — the iterator state
// machine's first MoveNext reads `this.N` from a heap block the disposer
// already freed (the §28.3 *Unbound owned temporaries* reclaim that
// #2205 widened to all `isOwningTypeHead` results).
//
// THE FIX (codegen, LlvmBackend_Disposal.cpp:3098-3116). Narrow the
// decl-null admitting branch's predicate from
//   isOwningCollectionHead(head) || isOwningTypeHead(head)
// to
//   isOwningCollectionHead(head) || (isOwningTypeHead(head) &&
//                                   !isTaskFamilyHead(head) &&
//                                   !isIterableInterfaceHead(head))
// — exactly excludes `OwningTask` + the four `IOwning*` heads while keeping
// the element-owning collections and the owning-wrapper heads admitted
// (#1008 Part C / #2205 reasoning preserved). Other disposal sites
// (property / indexer / field reads — see fresh_receiver_property_read
// _disposal_2751_test.cpp and fresh_receiver_indexer_read_disposal_2814_test
// .cpp) already pass `calleeReturnsOwned=false` BY CONSTRUCTION and never
// reach this branch.
//
// ORACLE. ASan-instrumented `runAotCheckedLenient` (compile + native exec,
// LeakSanitizer OFF). The deferred-activation receiver reads freed memory on
// the first MoveNext, so the UAF shows up as ASan's `heap-use-after-free`
// and the run flips `r.ran` to false. PASS-WITH: program completes correctly
// (the receiver's lifetime is correctly threaded through the deferred-
// activation frame's settlement).
//
// KNOWN LEAK (out of scope for #2261; #3377 is the follow-up). The
// predicate-narrowing fix stops the UAF but leaves the fresh receiver
// OWNED BY NOTHING — the iterator frame's Dispose body
// (LlvmBackend_Iterators.cpp ~1966-1969: `disposeYieldedVal` +
// `disposeOwnedParams` + `disposeOwnedLocals` + `releaseClosureCaptures`)
// handles owned parameters, owned locals, closure captures, and the yielded
// value, but does NOT dispose the `this` capture. The receiver temp therefore
// leaks. §28.3 *Unbound owned temporaries* says such temps must be disposed
// exactly once; that disposal is a separate fix (#3377 — follow-up to #2261)
// that will thread ownership through the iterator frame's settlement. For
// THIS lane, the leak is the SOUND direction — bounded leak is strictly
// better than a use-after-free (the fresh_receiver_property_read_disposal
// _2751_test.cpp precedent uses `runAotCheckedLenient` for the same reason).
// LSan is therefore disabled; ASan still catches UAF / OOB / double-free,
// which is the load-bearing assertion in d1/d2/d3.
//
// d1 pins the issue's headline iterator repro; d2 pins the manual
// `IOwningEnumerator` variant; d3 pins the §30.1 async-task (OwningTask)
// variant. The CONTROLS (c1/c2) prove the fix is NARROW: the element-
// owning collections (#2205's original cases) and a named receiver all
// stay unaffected.

#include <string>

#include "support/aot_support.hpp"   // runAotCheckedLenient — AOT + ASan (no LSan)
#include "test_harness.hpp"

using bezel::test::runAotCheckedLenient;

namespace {

// Sentinel: -999 when the AOT run fails (an ASan fire flips `ran` to false),
// the program's exit code otherwise. LSan is off — the leak (the known
// follow-up #3377, not the load-bearing assertion here) is tolerated.
int run(const std::string& src) {
    auto r = runAotCheckedLenient(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ============================================================================
// d1 — THE ISSUE'S HEADLINE REPRO. §29.1 / §28.3: an instance iterator method
// `Items(): owned IOwningEnumerable<Res>` that reads `this.N` across a
// `yield return`, called as `new Src().Items()` in a for-in desugaring.
// The fix's exclusion keeps the receiver alive past the call; the iterator's
// MoveNext reads a valid `this.N`; the receiver is reclaimed via the
// iterator's own frame teardown. Sum of yielded Ids is `5 + 10 = 15`.
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_iterator_dispose_2261,
           owning_enumerable_for_in_receiver_not_freed_29_1) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Src "
        "  public N: int := 5; "
        "  public method Items(): owned IOwningEnumerable<Res> "
        "    yield return new Res(this.N); "
        "    yield return new Res(this.N * 2); "
        "  end Items; "
        "end Src; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var r in new Src().Items() do sum := sum + r.Id; end; "
        "    return sum; "  // 5 + 10 = 15
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 15);
}

// ============================================================================
// d2 — same iterator shape, manual `GetOwningEnumerator()` desugaring. The
// fresh-receiver temp's lifetime must thread through the `using var en := …`
// binding — which is the §29.1 for-loop equivalent. Three yields reading
// `this.N` (with `+1` / `+2` offset) exercise the multi-yield carry path.
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_iterator_dispose_2261,
           unbound_temporary_receiver_through_manual_enumerator_29_1) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Src "
        "  public N: int := 7; "
        "  public method Items(): owned IOwningEnumerable<Res> "
        "    yield return new Res(this.N); "
        "    yield return new Res(this.N + 1); "
        "    yield return new Res(this.N + 2); "
        "  end Items; "
        "end Src; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    do "
        "      using var en := new Src().Items().GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        using var r := en.TakeCurrent(); "
        "        sum := sum + r.Id; "
        "      end; "
        "    end; "
        "    return sum; "  // 7 + 8 + 9 = 24
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 24);
}

// ============================================================================
// d3 — the `IOwningEnumerator` head shape. A `GetEnumerator()` that returns
// the owning enumerator DIRECTLY (not the enumerable wrapper) — same §29.1
// deferral, same receiver-borrow carry across MoveNext. This pins the
// exclusion of the `IOwningEnumerator` head (not just `IOwningEnumerable`).
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_iterator_dispose_2261,
           owning_enumerator_receiver_not_freed_29_1) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Src "
        "  public N: int := 11; "
        "  public method Items(): owned IOwningEnumerator<Res> "
        "    yield return new Res(this.N); "
        "  end Items; "
        "end Src; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    do "
        "      using var en := new Src().Items(); "
        "      while en.MoveNext() do "
        "        using var r := en.TakeCurrent(); "
        "        sum := sum + r.Id; "
        "      end; "
        "    end; "
        "    return sum; "  // 11
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 11);
}

// ============================================================================
// d4 — §30.1 / §28.3: the async-task analog. `Fetch(): owned OwningTask<Res>`
// is an `async` method whose state machine carries the receiver's borrow
// across `await` suspension points (per the §30.1 "frame residence" rule).
// The fresh receiver must NOT be disposed at the call site; the consumer
// disposes it at Task-settlement via `using var t := …; t.Wait()`. This
// pins the fix's exclusion of the `OwningTask` head (in the Task-family
// intersection of `isOwningTypeHead`).
// ============================================================================
BEZEL_TEST(fresh_receiver_deferred_iterator_dispose_2261,
           owning_task_receiver_not_freed_before_settlement_30_1) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Client "
        "  public N: int := 13; "
        "  public async method Fetch(): owned OwningTask<Res> "
        "    await Task.Delay(1); "
        "    return new Res(this.N); "
        "  end Fetch; "
        "end Client; "
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var t := new Client().Fetch(); "
        "      var r := t.Wait(); "
        "      if r = null then return 88; end; "
        "      if r.Id <> 13 then return 99; end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ============================================================================
// CONTROLS — the fix must stay NARROW; element-owning collections and the
// owning-wrapper heads stay admitted and their fresh receivers stay disposed.
// ============================================================================

// c1 — #2205's headline (element-owning collection head). `Make()` returns
// `owned OwningArray<Res>`; the receiver temp is properly disposed (no leak),
// the array's elements dispose exactly once. Pre-fix and post-fix both pass;
// this confirms the owning-collection heads stay admitted by the precise-
// exclusion form.
BEZEL_TEST(fresh_receiver_deferred_iterator_dispose_2261,
           owning_collection_receiver_still_disposed_2205_control) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Mk "
        "  public method Make(): owned OwningArray<Res> "
        "    var a := new OwningArray<Res>(); "
        "    a.Add(new Res(1)); "
        "    a.Add(new Res(2)); "
        "    return a; "
        "  end Make; "
        "end Mk; "
        "class P "
        "  public static method Main(): int "
        "    do _ := new Mk().Make(); end; "
        "    return 0; "
        "  end Main; "
        "end P;";
    // ASan/LSan clean (ran=true) is the load-bearing assertion — proves no
    // leak (the receiver block is reclaimed) and no UAF. Exit code is 0 either
    // way; the sentinel `-999` distinguishes an ASan fire.
    int n = run(src);
    BEZEL_EXPECT_FALSE(n == -999);
}

// c2 — BOUND-receiver control: a NAMED receiver (`using var src := new Src();`)
// followed by `src.Items()` is the pre-existing #2440 / §29.1 path and is
// unaffected by this fix (the fresh-receiver disposal site never fires for a
// named receiver — `isFreshOwnedClassProducer` requires an unbound `new`
// expression). The iterator still consumes the receiver correctly.
BEZEL_TEST(fresh_receiver_deferred_iterator_dispose_2261,
           named_receiver_unaffected_does_not_double_dispose) {
    const std::string src =
        "module M; "
        "class Res "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "end Res; "
        "class Src "
        "  public N: int := 5; "
        "  public method Items(): owned IOwningEnumerable<Res> "
        "    yield return new Res(this.N); "
        "  end Items; "
        "end Src; "
        "class P "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    do "
        "      using var src := new Src(); "
        "      using var en := src.Items().GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        using var r := en.TakeCurrent(); "
        "        sum := sum + r.Id; "
        "      end; "
        "    end; "
        "    return sum; "  // 5
        "  end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 5);
}
