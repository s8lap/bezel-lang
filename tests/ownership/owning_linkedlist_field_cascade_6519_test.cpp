// Ownership / leak — fix #6519 (§28.11 / stdlib §4.7, v0.338):
// an `owned IEnumerable<T>` FIELD store did not cascade to an
// `OwningLinkedList<T>`'s class elements, and its `LinkedList<string>` +1s.
// The elements leaked.
//
// THE DEFECT, AND WHY IT IS STRUCTURAL RATHER THAN A CASCADE GAP.
// #6439 fixed the same shape for the ARRAY-BACKED owning families
// (`OwningArray<T>` / `OwningStack<T>` / `OwningQueue<T>`, all sharing
// `bz_array_t`) by routing the owning wrap's slot-1 free through
// `bz_array_free_owning_wrap(arr, fallback, consult_header)`, which reads
// `bz_array_t::elem_disposer` — the construction-time header tag #2608
// introduced. `OwningLinkedList<T>` is node-backed: its `bz_llist_t` carried
// NO disposer field at all, so the header-consult mechanism could not apply
// without adding one. This fix adds it and wires the twin free.
//
// The leak itself: a field declared `private owned _seq: IEnumerable<Item>`
// stores an `OwningLinkedList<Item>`. Because `IEnumerable<T>` is an
// iterator-format head (§29.7), the store materialises an OWNING ENUMERATOR
// WRAPPER (`bz$llenumown$ptr$Dispose`) holding the list pointer, and the
// holder's teardown calls that wrapper's slot-1 Dispose. That Dispose
// emitted `bz_llist_free(self->list, NULL)` — a hardcoded NULL, with no
// element-type-derived fallback either. The list was freed; the `Item`s were
// stranded. MEASURED on base `2dbbb7e9`: `32 byte(s) leaked in 2
// allocation(s)`, both frames `bz$M.Holder$ctor`, with the property NEVER
// read — a store-time cascade gap, not a read-time one. Amplified to 300x300:
// `1440000 byte(s) leaked in 90000 allocation(s)`.
//
// A SECOND LEAK AT THE SAME FREE SITE, found by the same matrix and fixed by
// the same change. `LinkedList<string>` takes a +1 per element on every
// insert (`llRetainElem`, #2686 / #2692 / #2693) and pairs it with a release
// at disposal — but this wrap passed a bare NULL, so the +1 was never
// released. MEASURED amplified: `3297600 byte(s) leaked in 180000
// allocation(s)`, against a CLEAN `using var a := new LinkedList<string>()`
// baseline (row (d) pins that baseline, so the leak cannot be attributed to
// the retain). The fix's `fallback` argument is the element-type-derived
// disposer (`ownedWrapElementDisposer`), exactly as on the array twins; for a
// `string` element that is `stringElementReleaseThunk`.
//
// WHY THE HEAD IS THE DISCRIMINATOR, and why this is not a header-always
// rule. Consulting the header unconditionally is an OVER-DISPOSE, measured
// rather than assumed, and the owning head is REACHABLE at this very wrap —
// not a theoretical one. `owningDrainWrapForHead` only intercepts the head
// inside `wrapLinkedListAsIEnumerable`; a callee-owned list returned at an
// `IOwningEnumerable<T>` head routes to `wrapOwnedLinkedListAsIEnumerable`
// instead, and that wrap does NOT drain. MEASURED: the IR of
// `static method Build(): IOwningEnumerable<Item>` returning a populated
// `OwningLinkedList<Item>` contains `bz$llenumown$ptr$Dispose` and ZERO
// `bz_llist_drain_next` calls. There the loop has discharged each element
// while the list still names the pointer, so consulting the header would
// double-dispose. Rows (f)-(g) are the NEGATIVE CONTROL that makes the fix
// falsifiable: they are the owning-head shapes a header-always rule breaks,
// pinned so a reintroduced over-dispose fails here BY NAME instead of
// surfacing as an unexplained sanitizer report in some other lane's test.
//
// THE CACHE KEY. `bz$llenumown$<elemName>$` is keyed on the lowered element
// type, and `string`, a class and `IGrouping` all lower to `ptr` — so the
// disposer (#3450's reason) and now the head (#6439's reason) are both part
// of the key, each because it changes the Dispose BODY. The head segment
// marks the NON-cascading (owning) head, which `IOwningEnumerable<T>`'s
// `where T: class` (stdlib §4.13) makes reachable only for a class element
// and therefore only under `ptr`, so every other name — including the `i32`
// rows pinned by `tests/lit/enumerator_vtable_cache_internal_linkage_3477.bzl`
// — keeps its pre-fix spelling.
//
// ORACLE. Every row is on `runAotChecked`, which always runs the program
// under ASan/LSan. `leaksAtLeast` is NOT used: it is a leak oracle, not an
// ASan oracle, and a row pinning it can go green while the sanitizer is not
// running at all. Each row asserts `r.ran`, the exact `exitCode`, AND — via
// `hasSanitizerReport` — the absence of a sanitizer headline, so a leak or a
// double-free cannot hide behind a coincidentally-correct exit code, and a
// wrong printed answer cannot hide behind a fixed leak. `wasSkipped` returns
// early ONLY on a toolchain SKIP (an absent ASan link), never on a leak or a
// crash; the skip surface is a real assertion, not an absorbing guard.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic. This is
// the toolchain-only condition; it does not absorb a leak or a crash.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// True if `text` carries an ASan / LSan report headline. runAotChecked folds
// the subprocess's combined output into stdoutText, so this is where the
// sanitizer's own verdict arrives — the marker list is wider than
// support/aot_support.hpp's because this fix's risk direction is over-dispose,
// so a use-after-free / double-free headline must fail a row by name and not
// merely perturb an exit code. Mirrors the same helper inlined at
// tests/codegen/arrow_body_nullable_string_plus1_uaf_2262_test.cpp (the
// harness's own copy is in `aot_detail`).
bool hasSanitizerReport(const std::string& text) {
    static const char* markers[] = {
        "ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
        "SUMMARY: AddressSanitizer", "detected memory leaks",
        "heap-use-after-free", "double-free", "use-after-free",
    };
    for (const char* m : markers)
        if (text.find(m) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) THE REPRO. An `owned IEnumerable<Item>` field, stored and never read.
//     Base: `32 byte(s) leaked in 2 allocation(s)`.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_linkedlist_field_cascade_6519,
           field_store_no_read_no_leak_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _seq: IEnumerable<Item>; "
        "  public constructor() "
        "    var a := new OwningLinkedList<Item>(); "
        "    a.Add(new Item(3)); "
        "    a.Add(new Item(4)); "
        "    _seq := a; "
        "  end constructor; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (b) THE AMPLIFIED ROW. A single leak of 32 bytes can hide under the
//     sanitizer's noise, so the repro is looped 300x300. Base:
//     `1440000 byte(s) leaked in 90000 allocation(s)`. The PRINTED counter is
//     asserted, not only the exit code: exit codes truncate to 8 bits and
//     cannot distinguish 90000 iterations from any other count.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_linkedlist_field_cascade_6519,
           field_read_amplified_300x300_no_leak_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _seq: IEnumerable<Item>; "
        "  public constructor() "
        "    var a := new OwningLinkedList<Item>(); "
        "    for var i in 1..300 do a.Add(new Item(i)); end for; "
        "    _seq := a; "
        "  end constructor; "
        "  public property Seq: IEnumerable<Item> get => do return _seq; end; "
        "  end Seq; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 1..300 do "
        "      using var h := new Holder(); "
        "      for var x in h.Seq do total := total + x.V; end for; "
        "    end for; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 300 iterations x sum(1..300) = 300 * 45150 = 13545000.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=13545000") != std::string::npos);
}

// ---------------------------------------------------------------------------
// (c) THE SECOND LEAK AT THE SAME FREE SITE. A `LinkedList<string>` behind
//     the same `owned IEnumerable<string>` field. The insert side retains a
//     +1 per element (#2686); this wrap never released it. Base (amplified):
//     `3297600 byte(s) leaked in 180000 allocation(s)`.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_linkedlist_field_cascade_6519,
           string_element_released_28_11) {
    const std::string src =
        "module M; "
        "class Holder "
        "  private owned _seq: IEnumerable<string>; "
        "  public constructor() "
        "    var a := new LinkedList<string>(); "
        "    for var i in 1..300 do a.AddLast($\"s{i}\"); end for; "
        "    _seq := a; "
        "  end constructor; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var j in 1..300 do "
        "      using var h := new Holder(); "
        "      n := n + 1; "
        "    end for; "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=300") != std::string::npos);
}

// ---------------------------------------------------------------------------
// (d) THE BASELINE row (c) is measured against. The identical
//     `LinkedList<string>` shape with the local bound directly (no interface
//     widening, so no owning wrap) must be leak-free BEFORE the fix too —
//     otherwise row (c) would be attributing a pre-existing leak to this
//     fix. A plain value element is the same control for the class shape.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_linkedlist_field_cascade_6519,
           string_local_baseline_already_clean_28_11) {
    const std::string src =
        "module M; "
        "class P "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var j in 1..300 do "
        "      using var a := new LinkedList<string>(); "
        "      for var i in 1..300 do a.AddLast($\"s{i}\"); end for; "
        "      n := n + 1; "
        "    end for; "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=300") != std::string::npos);
}

// ---------------------------------------------------------------------------
// (e) THE RETURN FACE. A callee-owned `OwningLinkedList<Item>` returned at an
//     `IEnumerable<Item>` head — the OTHER caller of this wrap (row (a) is
//     the field-store face). Pinned separately because a fix that closes only
//     the field face leaves this one leaking, and because it is the face
//     `owningDrainWrapForHead` does NOT intercept at the owning head (rows
//     (f)-(g)). MEASURED on base: `32 byte(s) leaked in 2 allocation(s)`.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_linkedlist_field_cascade_6519,
           callee_owned_return_cascades_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class G "
        "  public static method Make(): IEnumerable<Item> "
        "    var a := new OwningLinkedList<Item>(); "
        "    a.AddLast(new Item(3)); "
        "    a.AddLast(new Item(4)); "
        "    return a; "
        "  end Make; "
        "end G; "
        "class P "
        "  public static method Main(): int "
        "    using var e := G.Make(); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (e2) THE SECOND `bz_llist_alloc` SITE — the collection-literal one, which
//      the array family's #2635 pass had to chase down separately (a stamp at
//      only one construction site is an invariant nothing enforces).
//
//      Its stamp is INFERRED, not measured, and this row says so rather than
//      implying coverage it does not have. MEASURED: a literal cannot target
//      `IEnumerable<Item>` at all (§15.4 admits only a concrete collection
//      target — "a collection literal `[...]` cannot target the interface
//      type `IEnumerable`"), and every route from a literal-constructed
//      `OwningLinkedList<Item>` to this owning wrap is blocked upstream —
//      binding the literal to an `OwningLinkedList<Item>` local and widening
//      it is rejected ("a borrow cannot be stored into an `owned` field"),
//      and returning it at a widened head leaks the LIST HEADER itself on
//      BASE too (152 B / 5 allocs, a separate pre-existing defect this fix
//      neither causes nor claims). So no Bezel program reaches the literal
//      site AND this wrap today.
//
//      What the row DOES pin is the real thing: the literal site builds and
//      disposes an owning list correctly, with the header stamped (the setter
//      is present in the object file). A future change that drops the second
//      stamp cannot be caught here today; it is caught by the invariant the
//      stamp exists to maintain, which is why it is written rather than
//      omitted.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_linkedlist_field_cascade_6519,
           literal_construction_site_disposes_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class P "
        "  public static method Main(): int "
        "    using var l: OwningLinkedList<Item> := [new Item(3), new Item(4)]; "
        "    return l.Count; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    // The printed/returned Count is what proves the literal site built a
    // 2-element list rather than an empty one — a body of `=> true` or a bare
    // exit code could not see a stamp that silently broke the count.
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// ===========================================================================
// NEGATIVE CONTROL — the over-dispose direction.
//
// The `IOwningEnumerable<T>` head is REACHABLE at this wrap (measured: a
// callee-owned list returned at that head mints `bz$llenumown$ptr$Dispose`,
// with zero `bz_llist_drain_next` calls in the IR). This wrap does not
// drain, so the loop has already discharged each element while the list
// still names the pointer, and consulting the header there is a
// heap-use-after-free. These two rows are pinned so a header-always rule —
// the obvious "simpler" variant of this fix — fails HERE, by name, rather
// than surfacing later as an unexplained sanitizer report.
// ===========================================================================

// ---------------------------------------------------------------------------
// (f) RETURN face: a callee-owned `OwningLinkedList<Item>` returned at an
//     `IOwningEnumerable<Item>` head and iterated. Expected sum 3+4 = 7.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_linkedlist_field_cascade_6519,
           owning_head_callee_owned_return_no_double_dispose_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class G "
        "  public static method Make(): IOwningEnumerable<Item> "
        "    var a := new OwningLinkedList<Item>(); "
        "    a.Add(new Item(3)); "
        "    a.Add(new Item(4)); "
        "    return a; "
        "  end Make; "
        "end G; "
        "class P "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var x in G.Make() do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "    end for; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 7; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=7") != std::string::npos);
}

// ---------------------------------------------------------------------------
// (g) Stored-view face: the same owning head reached through a property
//     over an owned `OwningLinkedList<Item>` field, iterated with an `owned`
//     loop variable. The array twin of this shape is #6439's own row (e); it
//     is kept here so this file's negative control does not depend on another
//     file's row staying in place. Expected sum 3+4 = 7.
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_linkedlist_field_cascade_6519,
           owning_head_stored_view_no_double_dispose_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Holder "
        "  private owned _a: OwningLinkedList<Item>; "
        "  public constructor() "
        "    _a := new OwningLinkedList<Item>(); "
        "    _a.Add(new Item(3)); "
        "    _a.Add(new Item(4)); "
        "  end constructor; "
        "  public property Items: IOwningEnumerable<Item> "
        "    get => do return _a; end; "
        "  end Items; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    var total := 0; "
        "    using var h := new Holder(); "
        "    for var x in h.Items do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "    end for; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 7; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=7") != std::string::npos);
}

// ---------------------------------------------------------------------------
// (h) §28.11's ORDERING ROW. `Clear()` disposes an owning collection's
//     elements in its own Dispose order — reverse storage order for the
//     ordered families, which for a linked list is by DESCENDING insertion
//     sequence, not by list position (`bz_llist_free`'s `seq` sort, #869).
//     This row builds a list whose insertion order and position order
//     DISAGREE (AddFirst interleaved with AddLast), so a disposer that ran
//     head-to-tail would dispose a different element per node than the
//     reverse-insertion rule fixes. It is green before and after the fix —
//     it is the control that keeps a future reordering of the dispose walk
//     from being blamed on this change, and it reads the data (the printed
//     per-element order), so a walk that disposes the right COUNT in the
//     wrong ORDER still fails here.
//
//     How the order is observed: each `Item`'s destructor appends its V to a
//     class static accumulator as a decimal digit, so the printed number IS
//     the disposal order. `Item`'s destructor is the class's own slot-1
//     cleanup, which is what the stamped element disposer dispatches to
//     through the vtable. `Ord` starts at 0 and each disposal does
//     `Ord * 10 + V`, so 3-then-2-then-1 reads 321, while a positional
//     head-to-tail walk would read 213 and a positional tail-to-head walk 132.
//     (A string accumulator was tried first and is NOT usable here: the class
//     static string field plus `Log := Log + …` form does not parse, so the
//     numeric accumulator is the measured-working spelling.)
// ---------------------------------------------------------------------------
BEZEL_TEST(owning_linkedlist_field_cascade_6519,
           reverse_insertion_disposal_order_28_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor Ord := Ord * 10 + V; end destructor; "
        "  public static Ord: int := 0; "
        "end Item; "
        "class Holder "
        "  private owned _seq: IEnumerable<Item>; "
        "  public constructor() "
        "    var a := new OwningLinkedList<Item>(); "
        "    a.AddLast(new Item(1)); "   // seq 1, at the TAIL
        "    a.AddFirst(new Item(2)); "  // seq 2, at the HEAD
        "    a.AddLast(new Item(3)); "   // seq 3, at the TAIL
        "    _seq := a; "
        "  end constructor; "
        "end Holder; "
        "class P "
        "  public static method Main(): int "
        "    do "
        "      using var h := new Holder(); "
        "    end; "
        "    Console.WriteLine($\"ord={Item.Ord}\"); "
        "    return 0; "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasSanitizerReport(r.stdoutText));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // §28.11's reverse-INSERTION order: last-inserted first → 3, 2, 1 → 321.
    // (Position order would be 213 head-to-tail, or 132 tail-first.)
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ord=321") != std::string::npos);
}