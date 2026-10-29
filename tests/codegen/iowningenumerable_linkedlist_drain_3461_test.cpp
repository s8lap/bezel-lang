// Codegen / heap-use-after-free — fix #3461 (§29.1 / §29.2 / §29.11; stdlib
// §4.7 / §4.13, v0.283): a FIELD-BACKED `IOwningEnumerable<T>` stored view over
// an `OwningLinkedList<T>` double-disposed every element, because the
// LinkedList leaf of the enumerable-wrap grid had no draining variant to divert
// to and kept vtable slot 3 as the BORROWING `Current`.
//
// THIS IS #3385 AT THE THIRD FAMILY. #3385 established the rule and fixed the
// forward array leaf; #3405 taught the reverse (stack) leaf to ask the same
// question through `owningDrainWrapForHead`. Both leaves are `bz_array_t*`-
// backed. `LinkedList<T>` / `OwningLinkedList<T>` are node-backed
// (`bz_llist_t*`) and reach their wrap through a THIRD leaf,
// `wrapLinkedListAsIEnumerable`, which neither fix touched — and for which no
// draining enumerator existed at all. #3385, #3354 and #3364 are all closed and
// no open issue named the LinkedList family in this context; it fell between
// them.
//
// THE AUTHORITY. §29.2 decides the iteration protocol by the SOURCE's static
// type: a source typed `IOwningEnumerable<T>` desugars via
// `IOwningEnumerator<T>` (destructive), the loop body seeing
// `var x := __enumerator.TakeCurrent()`. §29.1's *`Current` semantics on
// borrowing and owning enumerators* states what that slot does: `TakeCurrent()`
// "transfers ownership of the most-recently-yielded value to the caller,
// leaving the enumerator's internal slot empty". stdlib §4.5 states the
// consequence for the source collection — "elements leave the collection as
// they are yielded, and partial iteration leaves the remainder in place" — and
// stdlib §4.13 states it for a re-read: "each `GetOwningEnumerator()` call on a
// live view yields a fresh enumerator over the collection's *current* remainder
// — §4.5's partial-iteration contract means a re-enumeration continues draining
// what remains, never replaying what already left". §28.6 admits the stored-view
// face itself, and its worked example spells out the caller's permission on that
// borrow: "the caller may read/iterate/mutate/drain it but not dispose it".
//
// MEASURED ON 0c5ecd1 (the parent of this commit), `owned _l:
// OwningLinkedList<Item>` holding 5, 10, 20 read through
// `public property Owning: IOwningEnumerable<Item> => _l;`:
//
//     order=51020 rem=3
//     AddressSanitizer: heap-use-after-free ... in bz$elemDispose
//         #1 bz_llist_free
//         #2 bz$M.C$DisposeCore
//
// `rem=3` and a use-after-free TOGETHER: the source is neither correctly
// drained nor correctly left alone. The loop discharged each element per
// §28.3/§29.2 while the list still held it, and the field's holder then ran
// `bz_llist_free` with the owning list's element disposer over the very same
// nodes. That is the over-release direction — the disposal-correctness failure
// the owning-head contract exists to prevent.
//
// THE FIX. `wrapLinkedListAsIEnumerable` asks the SAME `owningDrainWrapForHead`
// the two array leaves ask; the head test is not re-spelled for it (#3419
// point 1). `owningDrainWrapForHead` grew a third `DrainBacking` key, `List`,
// which selects the new `wrapLinkedListAsIOwningEnumerator` — the node-backed
// analogue of `wrapArrayAsIOwningEnumerator`, field for field, differing only in
// the drain primitive: `bz_llist_drain_next`, the head-popping primitive stdlib
// §4.7's own `Drain()` already uses. The container is untouched: the wrap
// borrows the list exactly as the borrowing one does, the field's holder still
// frees the (now empty) list, and only ELEMENT ownership moves — to the place
// §28.3/§29.2 already require to discharge it.
//
// ORDER IS UNCHANGED AND ASSERTED ANYWAY. `bz_llist_drain_next` pops the HEAD,
// so a drain walks head-to-tail — the same order the borrowing node walk
// produces, and the order stdlib §4.6's literal convention fixes ("textual
// order is head-to-tail: `using var l: LinkedList<int> := [10, 20, 30];`
// produces a list whose `First` is `10` and `Last` is `30`"). Every row below
// asserts order AND drain together, because the change must alter WHETHER the
// source drains without altering the order in which it yields.
//
// COVERAGE.
//   (A) The UAF pin. Full iteration of the field-backed stored view, receiver
//       disposed after the loop, under ASan/LSan. Pre-fix: heap-use-after-free
//       in the field's element-disposer pass. Post-fix: clean, and the source
//       is observably EMPTY.
//   (B) Partial drain. `break` after one element — stdlib §4.5's "partial
//       iteration leaves the remainder in place": the two undrained elements
//       stay in the field's list and are disposed there exactly once. Pre-fix
//       the one already-discharged element was disposed a second time by that
//       same pass.
//   (C) Re-enumeration. Two sequential loops over the same stored view: the
//       first drains, the second yields nothing (stdlib §4.13). Pre-fix the
//       second replayed all three.
//   (D) The BORROWING-head twin — the verified negative. The same field at an
//       `IEnumerable<Item>` head must stay NON-destructive and keep head-to-tail
//       order. Identical on both sides of the fix; it is the assertion that the
//       new divert is keyed on the HEAD and nothing else.
//   (E) Two families in one program — `OwningLinkedList` and `OwningArray`,
//       both at owning heads. The array leaf's drain must be unaffected, and
//       the two must not share machinery: the frames have different layouts
//       ("ownllenum.<elem>" is 3 fields over a list, "ownarrenum.<elem>" 3 over
//       an array) and a shared cache key would hand one family the other's
//       vtable.
//   (F) The ARGUMENT-coercion face over the same stored view — the anti-drift
//       row. The same leaf serves the property read, the argument coercion and
//       the RETURN edge; one of them silently disagreeing is #3419's pattern
//       and is exactly how this family stayed broken through three fixes.
//       ⚠ Deliberately `runAot`, not `runAotChecked`: the wrapper FRAME minted
//       for a stored-view read in argument position is not claimed by anyone
//       (24 B, LSan-visible). That is a pre-existing, family-independent gap —
//       measured identical for `OwningArray` on this same build — filed
//       separately, not this fix's to close.
//   (G) The LOCAL-DECL face (`LlvmBackend_Setup.cpp`'s coercion), added while
//       addressing the review. A FOURTH face this fix repairs that the original
//       commit did not claim: `using var e: IOwningEnumerable<Item> := l;` over
//       an `OwningLinkedList<Item>` local. Measured 3/3 on `origin/main`
//       (`0c5ecd1`): `order=12 left=2` then rc 139; 3/3 here: `order=12 left=0`
//       rc 0, and ASan/LSan-clean. `left` is the assertion that matters —
//       `left=2` is the source refusing to drain while the loop discharged both
//       elements, which is the double dispose the SIGSEGV then delivers.
//
// ORACLE. `runAotChecked` compiles the generated program with AddressSanitizer
// and LeakSanitizer, so `r.ran` is simultaneously a no-UAF and a no-leak
// assertion; the exit code carries the ownership observable.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The §28.6 stored-view face over an owning-LINKED-LIST field, exposed at both
// enumerable heads, with a window onto the field's residual count.
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
    "  private owned _l: OwningLinkedList<Item>; "
    "  public constructor() "
    "    _l := new OwningLinkedList<Item>(); "
    "    _l.AddLast(new Item(5)); "
    "    _l.AddLast(new Item(10)); "
    "    _l.AddLast(new Item(20)); "
    "  end constructor; "
    "  public property Owning: IOwningEnumerable<Item> => _l; "
    "  public property Borrowing: IEnumerable<Item> => _l; "
    "  public property Remaining: int => _l.Count; "
    "end C; ";

}  // namespace

// ---------------------------------------------------------------------------
// (A) Full drain of the field-backed stored view — the #3461 use-after-free.
//   per §29.1 (TakeCurrent), §29.2 (owning desugaring), §28.6 (stored view),
//   stdlib §4.5 / §4.13 (elements leave the collection as they are yielded)
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_linkedlist_drain_3461,
           field_backed_view_drains_source_no_double_dispose_29_1) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var order := 0; "
        "    for var x in c.Owning do "
        "      using var y := owned x; "
        "      order := order * 100 + y.V; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return c.Remaining; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on 0c5ecd1: ASan heap-use-after-free in bz$elemDispose <-
    // bz_llist_free <- C$DisposeCore — the field's disposer pass reached the
    // elements the loop had already discharged.
    BEZEL_EXPECT_TRUE(r.ran);
    // Head-to-tail: 5, then 10, then 20 (stdlib §4.6's literal convention;
    // bz_llist_drain_next pops the head).
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=51020"));
    // Remaining == 0: every element LEFT the list at its yield. Pre-fix this
    // was 3 — the "rem=3" half of the issue, independent of the crash.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (B) Partial drain — `break` after the first element.
//   per stdlib §4.5 ("partial iteration leaves the remainder in place"),
//   §28.2 (dispose exactly once), §28.3 (owned iteration variable)
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_linkedlist_drain_3461,
           partial_drain_leaves_remainder_owned_by_the_field_4_5) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var total := 0; "
        "    for var x in c.Owning do "
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
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix: the one discharged element was still in the field's list, so the
    // holder's disposer pass faulted on it. Post-fix the two undrained elements
    // are disposed exactly once, by the field's holder — LSan (via r.ran) is
    // the no-leak half of that claim, and no ASan report is the no-double-free
    // half.
    BEZEL_EXPECT_TRUE(r.ran);
    // Remaining == 2 (the head, 5, left), total == 5.
    BEZEL_EXPECT_EQ(r.exitCode, 205);
}

// ---------------------------------------------------------------------------
// (C) Re-enumeration continues draining what remains.
//   per stdlib §4.13 ("a re-enumeration continues draining what remains, never
//   replaying what already left"), §29.11
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_linkedlist_drain_3461,
           re_enumeration_yields_nothing_after_a_full_drain_4_13) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var total := 0; "
        "    for var x in c.Owning do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "    end for; "
        "    for var z in c.Owning do "
        "      using var w := owned z; "
        "      total := total + w.V; "
        "    end for; "
        "    return c.Remaining * 100 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // total == 35 (5+10+20 once), Remaining == 0. Pre-fix the second loop
    // replayed all three elements (total 70) on top of the fault.
    BEZEL_EXPECT_EQ(r.exitCode, 35);
}

// ---------------------------------------------------------------------------
// (D) The BORROWING-head twin — the verified negative for the head key.
//   per stdlib §4.6 (head-to-tail), §29.1 (`Current` is non-destructive),
//   §28.6
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_linkedlist_drain_3461,
           borrowing_head_stays_non_destructive_and_head_to_tail_29_1) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var order := 0; "
        "    for var x in c.Borrowing do "
        "      order := order * 100 + x.V; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return c.Remaining; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=51020"));
    // §29.1's borrowing `Current` lends; all three elements are still there.
    // This row is IDENTICAL on both sides of the fix by design — it is the
    // guard that the divert reads the target HEAD and nothing else.
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// (E) Both families at owning heads in ONE program — the machinery-key row.
//   per stdlib §4.5 / §4.7 / §4.13, §29.1
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_linkedlist_drain_3461,
           linked_list_and_array_owning_views_coexist_4_13) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class D "
        "  private owned _l: OwningLinkedList<Item>; "
        "  private owned _a: OwningArray<Item>; "
        "  public constructor() "
        "    _l := new OwningLinkedList<Item>(); "
        "    _l.AddLast(new Item(1)); "
        "    _l.AddLast(new Item(2)); "
        "    _a := new OwningArray<Item>(); "
        "    _a.Add(new Item(3)); "
        "    _a.Add(new Item(4)); "
        "  end constructor; "
        "  public property ListView: IOwningEnumerable<Item> => _l; "
        "  public property ArrView: IOwningEnumerable<Item> => _a; "
        "  public property Left: int => _l.Count * 10 + _a.Count; "
        "end D; "
        "class Main "
        "  public static method Run(): int "
        "    using var d := new D(); "
        "    var lo := 0; "
        "    for var x in d.ListView do "
        "      using var y := owned x; "
        "      lo := lo * 10 + y.V; "
        "    end for; "
        "    var ao := 0; "
        "    for var p in d.ArrView do "
        "      using var q := owned p; "
        "      ao := ao * 10 + q.V; "
        "    end for; "
        "    Console.WriteLine($\"list={lo} arr={ao} left={d.Left}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // list 1,2 head-to-tail; array 3,4 front-to-back; both drained.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "list=12 arr=34 left=0"));
}

// ---------------------------------------------------------------------------
// (F) The ARGUMENT-coercion face over the same stored view — anti-drift.
//   per §29.2, stdlib §4.5 / §4.13
//
// ⚠ `runAot`, deliberately: the wrapper FRAME minted for a stored-view read in
// ARGUMENT position is claimed by nobody (24 B). Measured identical for the
// `OwningArray` family on this same build, so it is a pre-existing
// family-independent mint-vs-claim gap and is filed separately. The observable
// here is the drain, which is what this fix changes.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_linkedlist_drain_3461,
           argument_face_over_the_stored_view_drains_too_29_2) {
    const std::string src =
        std::string(kPrelude) +
        "class Main "
        "  public static method Walk(e: IOwningEnumerable<Item>): int "
        "    var o := 0; "
        "    for var x in e do "
        "      using var y := owned x; "
        "      o := o * 100 + y.V; "
        "    end for; "
        "    return o; "
        "  end Walk; "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var o := Walk(c.Owning); "
        "    Console.WriteLine($\"o={o} rem={c.Remaining}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on 0c5ecd1 this face read the same borrowing wrap: `rem=3` and
    // the same double dispose at the receiver's scope end.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "o=51020 rem=0"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (G) The LOCAL-DECL coercion face — the fourth face, added on review.
//   per §29.2 (the owning desugaring), stdlib §4.5 / §4.13, §28.3
//
// This is `LlvmBackend_Setup.cpp`'s local-decl arm, which routes a borrowed
// LinkedList source through the same `wrapLinkedListAsIEnumerable` leaf. It was
// repaired by the same one-line divert and was not claimed by the original
// commit — the issue named only the stored-view property face.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_linkedlist_drain_3461,
           local_decl_face_over_a_linked_list_drains_too_29_2) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Main "
        "  public static method Run(): int "
        "    using var l := new OwningLinkedList<Item>(); "
        "    l.AddLast(new Item(1)); "
        "    l.AddLast(new Item(2)); "
        "    var order := 0; "
        "    var lv := l; "  // borrowing alias (spec v0.350 §28.10)
        "    using var e: IOwningEnumerable<Item> := lv; "
        "    for var x in e do "
        "      using var y := owned x; "
        "      order := order * 10 + y.V; "
        "    end for; "
        "    Console.WriteLine($\"order={order} left={l.Count}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on 0c5ecd1: `order=12 left=2` reaches stdout, then SIGSEGV in the
    // local's own scope-end disposal pass over elements the loop discharged.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=12 left=0"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "left=2"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
