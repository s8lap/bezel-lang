// Codegen — issue #2610 / spec §29.1 "Current semantics on borrowing and owning
// enumerators" (v0.236) + §29.2 (for-desugaring) + §29.3 (iterator methods):
//
//   §29.1 (the governing sentence, v0.193/v0.236): "under either strategy the
//   enumerator **owns** the pending value from a successful advance until
//   `TakeCurrent()` transfers it, and a `Dispose()` after an advance whose value
//   was never taken **disposes the pending value**. An advanced-but-untaken
//   element neither leaks nor reaches the consumer."
//
// The sync `emitIteratorMethod` (LlvmBackend_Iterators.cpp) recognized only
// `IOwningEnumerable<T>` as an owning iterator head (`isOwningIter`), so a
// method declared to return `IOwningEnumerator<T>` DIRECTLY (the enumerator-
// direct form — the owning sibling of the `IEnumerator<T>`-direct single-use
// form §29.3/§29.11 support) was misclassified as the BORROWING form:
//   - vtable slot 3 was emitted as a plain non-destructive Current (no null-on-
//     take), while the consumer dispatched `.TakeCurrent()` against the same
//     slot expecting ownership transfer;
//   - the producer's #1120 advance/Dispose machinery (admitted because the
//     `elemIsOwnedClass` exclusion matched only `IOwningEnumerable`) then
//     touched the value the consumer already owned.
// The result was a heap-use-after-free on the TakeCurrent path (ASan:
// "==ERROR: AddressSanitizer: heap-use-after-free" in main; a raw SIGSEGV
// without ASan). The dominant `for var x in src do` path was unaffected
// because it desugars via `IOwningEnumerable<T>` (the recognized head); the
// bug was reachable only via the manual protocol (`using var en := Gen();` +
// `en.TakeCurrent()`) against an `IOwningEnumerator<T>`-direct producer.
//
// The fix mirrors the async `isOwningIter` extension landed under #2591
// (IOwningAsyncEnumerator): recognize `IOwningEnumerator<T>` as an owning
// iterator head AND exclude it from `elemIsOwnedClass` (so the #1120 borrowing-
// form disposal does not race the owning-form TakeCurrent). With the fix the
// enumerator-direct frame behaves identically to `IOwningEnumerable<T>`:
// TakeCurrent nulls the slot on transfer, disposeYieldedVal's owning branch
// covers abandonment, and the exhaustion path defensively nulls the slot.
//
// FAILS WITHOUT / PASSES WITH (the fix): tests 1 and 3 (TakeCurrent paths)
// trigger a heap-use-after-free under ASan without the fix (the producer
// re-touches the value the consumer already owns); r.ran flips false → true /
// exitCode flips 1 → 0. Tests 2 and 4 are controls: 2 pins that abandon-
// without-take was already correct in sync (the #1120 disposeYieldedVal admits
// the pending value) and that the fix did not regress it; 4 pins that the
// dominant `for` desugaring over `IOwningEnumerable<T>` is unaffected.

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
// static int form used by the existing #1120/#1349/#1891 suites).
// live - disposed == 0 iff every allocation was disposed exactly once.
const char* kRes =
    "class Res "
    "  public static live: int := 0; "
    "  public static disposed: int := 0; "
    "  public constructor() Res.live := Res.live + 1; end constructor; "
    "  destructor Res.disposed := Res.disposed + 1; end destructor; "
    "end Res; ";

// Enumerator-direct owning iterator: yields owned Res values. The form the
// fix's IOwningEnumerator recognition arm makes correctly owning.
const char* kGenEnum =
    "class Gen "
    "  public static method Produce(): IOwningEnumerator<Res> "
    "    yield return new Res(); "
    "    yield return new Res(); "
    "  end Produce; "
    "end Gen; ";

// Enumerable owning iterator: the spec-sanctioned IOwningEnumerable<T>
// return (§29.2), exercised by the `for` control below.
const char* kGenEnumBl =
    "class GenB "
    "  public static method Produce(): IOwningEnumerable<Res> "
    "    yield return new Res(); "
    "    yield return new Res(); "
    "  end Produce; "
    "end GenB; ";

}  // namespace

// §29.1 v0.236 / #2610 (the bug): manual protocol — MoveNext twice, TakeCurrent
// BOTH values, each wrapped in a `using var` that disposes it. Pre-fix the
// producer's slot 3 was a non-destructive Current (no null-on-take); the
// producer's #1120 advance-dispose then re-disposed the value the consumer's
// `using var` already owned → heap-use-after-free under ASan (exit 1 / SIGSEGV).
// With the fix slot 3 is the TakeCurrent owning path (nulls on transfer) and
// the #1120 machinery is gated out for the owning form (elemIsOwnedClass
// excludes IOwningEnumerator). live - disposed == 0.
BEZEL_TEST(sync_iter_owning_enumerator_direct_2610,
           takecurrent_full_consumption_exactly_once_no_double_free) {
    const std::string src = std::string("module M; ") + kRes + kGenEnum +
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    do "
        "      using var en := Gen.Produce(); "
        "      en.MoveNext(); "
        "      using var a := en.TakeCurrent(); n := n + 1; "
        "      en.MoveNext(); "
        "      using var b := en.TakeCurrent(); n := n + 1; "
        "    end; "
        "    if n <> 2 then return 50; end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §29.1 control: abandon after ONE MoveNext WITHOUT TakeCurrent. The §29.1
// pending-value rule requires Dispose to dispose the advanced-but-untaken
// value. This was ALREADY correct in the sync path pre-#2610 (the #1120
// disposeYieldedVal admitted the pending value because elemIsOwnedClass was
// true for the misclassified head) and the fix must not regress it: with the
// fix disposeYieldedVal's OWNING branch (isOwningIter) covers this. Pins no
// leak on abandon. live - disposed == 0.
BEZEL_TEST(sync_iter_owning_enumerator_direct_2610,
           abandon_without_take_disposes_pending_value) {
    const std::string src = std::string("module M; ") + kRes + kGenEnum +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var en := Gen.Produce(); "
        "      en.MoveNext(); "
        "    end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §29.1 v0.236 / #2610 (the bug, partial-consumption shape): take the FIRST
// value via TakeCurrent, advance again, then abandon the SECOND without take.
// The second Res is disposed by Dispose's disposeYieldedVal (owning branch);
// the first Res is disposed by its `using var a`. Pre-fix this shared the
// TakeCurrent UAF (slot 3 did not null on take → the second MoveNext's advance
// path re-touched the already-owned first value). With the fix both dispose
// exactly once. live - disposed == 0.
BEZEL_TEST(sync_iter_owning_enumerator_direct_2610,
           take_first_abandon_second_exactly_once) {
    const std::string src = std::string("module M; ") + kRes + kGenEnum +
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var en := Gen.Produce(); "
        "      en.MoveNext(); "
        "      using var a := en.TakeCurrent(); "
        "      en.MoveNext(); "
        "    end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §29.2 control: the dominant `for var x in src do` path over
// IOwningEnumerable<T> was already correct (the desugaring pairs MoveNext with
// TakeCurrent against the recognized IOwningEnumerable head) and must remain
// unaffected by the fix (no regression). Each yielded Res is taken and
// auto-dropped in the loop body. live - disposed == 0.
BEZEL_TEST(sync_iter_owning_enumerator_direct_2610,
           for_in_owning_enumerable_unaffected) {
    const std::string src = std::string("module M; ") + kRes + kGenEnumBl +
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var r in GenB.Produce() do "
        "      r.Dispose(); "
        "      n := n + 1; "
        "    end; "
        "    if n <> 2 then return 50; end; "
        "    return Res.live - Res.disposed; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}
