// Codegen / heap-use-after-free — fix #3385 (§29.1 / §29.11, v0.282):
// iterating a FIELD-BACKED `IOwningEnumerable<T>` stored view double-disposed
// every element, because vtable slot 3 of the wrap was the BORROWING `Current`
// instead of §29.1's `TakeCurrent`.
//
// THE DEFECT. §28.6 admits the stored-view face — a bare read of an `owned`
// field lends, and "may be returned through a non-`owned` return type as a
// borrow of the field ... uniformly so for a class-typed owned field and for
// an owning-collection field (`owned _items: OwningArray<T>`)"; its own worked
// example spells out what the caller may do with that borrow: "the caller may
// read/iterate/mutate/drain it but not dispose it". §29.2 then decides the
// iteration protocol by the SOURCE's static type: "a source typed
// `IEnumerable<T>` desugars via `IEnumerator<T>` (non-destructive); a source
// typed `IOwningEnumerable<T>` desugars via `IOwningEnumerator<T>`
// (destructive)", the loop body seeing `var x := __enumerator.TakeCurrent()`.
// §29.1's *`Current` semantics on borrowing and owning enumerators* states the
// transfer the owning slot performs: `TakeCurrent()` "transfers ownership of
// the most-recently-yielded value to the caller, leaving the enumerator's
// internal slot empty" — and stdlib §4.5/§4.13 state the consequence for the
// source collection: "elements leave the collection as they are yielded", so a
// re-enumeration "continues draining what remains, never replaying what
// already left".
//
// #3355's interim fix (`5f184fd`) widened the wrap gate to accept the
// `IOwningEnumerable` head — eliminating a SIGSEGV — but reused the BORROWING
// wrap helpers, whose slot 3 returns the element at the current index without
// transferring it out of the collection. Its own commit body records the gap
// as "re-iterating the same IOwningEnumerable reads the same items twice". On
// a FIELD-owned source it is strictly worse than a double READ: the loop's
// owned iteration variable is discharged per §28.3, and the field's holder
// then runs `bz_array_free` with the owning array's element disposer over the
// very same, still-present slots — a heap-use-after-free (ASan:
// `bz$elemDispose` <- `bz_array_free` <- `C$DisposeCore`).
//
// THE FIX. `wrapArrayAsIEnumerable` routes an `IOwningEnumerable<T>` target to
// the existing draining authority `wrapArrayAsIOwningEnumerator` (#2218) —
// MoveNext pops through the same `bz_array_drain_next` the `for x in
// coll.Drain()` loop uses, TakeCurrent moves the pending element out and nulls
// the slot per §29.1, and Dispose disposes an advanced-but-untaken pending.
// No second classification predicate is minted. That helper's slot 4 was
// `return self;` (a no-op default for a slot an `IOwningEnumerator<T>`
// receiver never dispatches); it now mints a fresh enumerator over the
// collection's current remainder, which §29.2's desugaring requires (it calls
// slot 4 and then disposes BOTH the source view and the enumerator) and which
// stdlib §4.13's drain-view contract independently requires.
//
// The container is untouched by the fix: the wrap borrows the array exactly as
// before, and the field's holder still frees the (now empty) array. Only
// ELEMENT ownership moves, and it moves to the place §28.3/§29.2 already
// require to discharge it.
//
// COVERAGE.
//   (A) The UAF pin. Field-backed expression-bodied stored view, full
//       iteration, receiver disposed after the loop. Pre-fix: ASan
//       heap-use-after-free in the field's element-disposer pass. Post-fix:
//       runs clean and the source collection is observably EMPTY (the
//       §29.11 / stdlib §4.5 transfer), encoded as Remaining*100 + total.
//   (B) Partial drain. `break` after one element: stdlib §4.5's
//       "partial iteration leaves the remainder in place" — the two undrained
//       elements stay in the field's array and are disposed there exactly
//       once (pre-fix the one already-discharged element was disposed a
//       second time by that same pass → UAF).
//   (C) Re-enumeration. Two sequential loops over the same stored view: the
//       first drains, the second yields nothing — stdlib §4.13's "a
//       re-enumeration continues draining what remains, never replaying what
//       already left". Pre-fix the second loop replayed both elements (and
//       faulted), which is the single-consumption violation #3355's commit
//       body named.
//
//   (D) The BOUND spelling — `var e := c.Items; for var x in e do ... end;`.
//       This is the SEAM between this fix and #3392, and it is pinned here
//       because it is the one shape where the two meet.
//
// ⚠ THE TWO FIXES ARE MUTUALLY NECESSARY — this is why (D) matters.
// #3392 (three commits earlier on this branch) widened
// `registerIterableSourceDisposal`'s computed-getter arm so a local bound to
// the stored view CLAIMS the freshly minted wrapper frame. On its own that
// widening would be a DOUBLE FREE on this path: before this commit,
// `wrapArrayAsIOwningEnumerator`'s vtable slot 4 was `return self;`, so the
// source view V and the loop's enumerator C were THE SAME POINTER — #3392's
// decl-slot claim and the loop's own enumerator disposal would both have
// slot-1'd it.
// This commit's slot-4 change (mint a fresh child clone per §29.2's
// desugaring and stdlib §4.13) is what makes V and C distinct, and therefore
// what makes #3392's claim safe. The ownership split is:
//   V — claimed by #3392 at the local's scope end; its MoveNext is never
//       dispatched (`IOwningEnumerable<T>` declares only
//       `GetOwningEnumerator()`), so its `pending` is null for its whole
//       lifetime and its Dispose is `free(self)` alone.
//   C — registered by the loop, disposed at loop end; `TakeCurrent` nulled
//       its `pending` at each yield, so its Dispose is also `free(self)`.
//   elements — transit C's `pending` for the span of one
//       MoveNext→TakeCurrent pair and land on the loop variable, which
//       §28.3/§29.2 require to discharge them.
//   the array — never freed by either frame; the field's holder frees it,
//       now empty.
// An earlier revision of this note claimed the bound spelling "still LEAKS
// the wrapper FRAME … tracked as #3392". That was measured against main
// WITHOUT #3392 and is FALSE on this branch — #3392 closes it three commits
// back. Corrected per [[fix-must-update-existing-tests]]: a shipped record a
// sibling commit falsified is the same defect class as a stale pinned test.
//
// ORACLE. `runAotChecked` compiles the generated program with AddressSanitizer
// and LeakSanitizer enabled, so `r.ran` is simultaneously a no-UAF and a
// no-leak assertion; the exit code carries the ownership observable.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// The shared prelude: an owning collection field, the §28.6 stored-view face
// at `IOwningEnumerable<Item>`, and a window onto the field's residual count.
// (Never put a C++ `//` comment inside these literals: it is lexed as Bezel
// source and comments out the rest of the program. Bezel's own comment forms
// are `//`, `/* ... */` and `///` -- spec §1.5, which is exhaustive.)
const char* const kPrelude =
    "module M; "
    "class Item "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item; "
    "class C "
    "  private owned _items: OwningArray<Item>; "
    "  public constructor() "
    "    _items := new OwningArray<Item>(); "
    "    _items.Add(new Item(5)); "
    "    _items.Add(new Item(10)); "
    "    _items.Add(new Item(20)); "
    "  end constructor; "
    "  public property Items: IOwningEnumerable<Item> => _items; "
    "  public property Remaining: int => _items.Count; "
    "end C; ";

}  // namespace

// ---------------------------------------------------------------------------
// (A) Full drain of a field-backed stored view — the #3385 use-after-free.
//   per §29.1 (TakeCurrent), §29.2 (owning desugaring), §28.6 (stored view),
//   stdlib §4.5 / §4.13 (elements leave the collection as they are yielded)
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_stored_view_drain_3385,
           field_backed_view_drains_source_no_double_dispose_29_1) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var total := 0; "
        "    for var x in c.Items do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "    end for; "
        "    return c.Remaining * 100 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    // Pre-fix: ASan heap-use-after-free in bz$elemDispose <- bz_array_free
    // <- C$DisposeCore — the field's disposer pass reached the elements the
    // loop had already discharged.
    BEZEL_EXPECT_TRUE(r.ran);
    // Remaining == 0: every element LEFT the collection at its yield
    // (§29.1 TakeCurrent nulls the pending slot; stdlib §4.5). total == 35.
    BEZEL_EXPECT_EQ(r.exitCode, 35);
}

// ---------------------------------------------------------------------------
// (B) Partial drain — `break` after the first element.
//   per stdlib §4.5 ("partial iteration leaves the remainder in place"),
//   §28.2 (dispose exactly once), §28.3 (owned iteration variable)
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_stored_view_drain_3385,
           partial_drain_leaves_remainder_owned_by_the_field_4_5) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var total := 0; "
        "    for var x in c.Items do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "      break; "
        "    end for; "
        "    return c.Remaining * 100 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    // Pre-fix: the one discharged element was still in the field's array, so
    // the holder's disposer pass faulted on it (UAF). Post-fix the two
    // undrained elements are disposed exactly once, by the field's holder —
    // LSan (via r.ran) is the no-leak half of that claim.
    BEZEL_EXPECT_TRUE(r.ran);
    // Remaining == 2, total == 5.
    BEZEL_EXPECT_EQ(r.exitCode, 205);
}

// ---------------------------------------------------------------------------
// (C) Re-enumeration continues the drain rather than replaying it.
//   per §29.11 (multiple enumeration), stdlib §4.13 ("a re-enumeration
//   continues draining what remains, never replaying what already left")
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_stored_view_drain_3385,
           second_enumeration_yields_nothing_29_11) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var first := 0; "
        "    for var x in c.Items do "
        "      using var y := owned x; "
        "      first := first + y.V; "
        "    end for; "
        "    var second := 0; "
        "    for var x2 in c.Items do "
        "      using var y2 := owned x2; "
        "      second := second + y2.V; "
        "    end for; "
        "    return first * 4 + second; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    // Pre-fix the borrowing slot 3 left every slot populated, so the second
    // loop replayed all three elements (the single-consumption violation
    // #3355's commit body recorded) and then double-disposed them.
    BEZEL_EXPECT_TRUE(r.ran);
    // first == 35 (5+10+20), second == 0.
    BEZEL_EXPECT_EQ(r.exitCode, 140);
}

// ---------------------------------------------------------------------------
// (D) BOUND-and-iterated — the #3385 × #3392 seam.
//   per §29.1 (TakeCurrent), §29.2 (the desugaring's source handling for a
//   named local), §28.6 (stored view), stdlib §4.5 / §4.13
//
// Distinct from (A) in exactly one respect: the stored view is bound to a
// LOCAL first, then iterated. That routes disposal through a different owner
// than the direct-read spelling — §29.2 pins a named-local source with a
// borrow rather than assuming its disposal obligation, so the loop does NOT
// dispose V; #3392's decl-slot claim does, at `e`'s scope end.
//
// Neither commit's own tests reach this: #3385's (A)-(C) deliberately use the
// direct-read spelling to stay on the element axis, and #3392's cases read the
// property without ever iterating. The case was previously DELETED from
// `block_bodied_ienumerable_property_bound_disposal_2769_test.cpp:193`
// because "the IOwningEnumerable wrap path SEGVs in BOTH the direct and bound
// spellings" — this pins that it no longer does.
//
// Observable is identical to (A) — `Remaining * 100 + total` == 35 — because
// binding the view changes WHO frees the frames, not where the elements go.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_stored_view_drain_3385,
           bound_stored_view_drains_once_frames_freed_by_two_owners_29_2) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var total := 0; "
        "    var e := c.Items; "
        "    for var x in e do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "    end for; "
        "    return c.Remaining * 100 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    // `ran` is simultaneously the no-UAF and the no-LEAK assertion (ASan+LSan).
    // Without #3392 this leaks V's 24-byte frame; without THIS commit's slot-4
    // clone, V and C are the same pointer and #3392's claim double-frees it.
    BEZEL_EXPECT_TRUE(r.ran);
    // Remaining == 0 (every element left the collection), total == 35.
    BEZEL_EXPECT_EQ(r.exitCode, 35);
}
