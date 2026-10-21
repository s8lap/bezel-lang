// Codegen — stdlib §4.18 / §5 (v0.286), issue 3711: the §5 lazy-operator
// lowering of a `SortedSet<T>` source must observe ASCENDING element order.
//
// AUTHORITY (quoted verbatim from docs/ at v0.286).
//
//   stdlib §4.18 `SortedSet<T>`, first line of the section:
//     "A comparison-ordered set: membership by `CompareTo`-equality, iteration
//      in ascending order."
//   and its declaration:
//     "public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>"
//   stdlib line 1467:
//     "public interface IReadOnlyCollection<out T>: IEnumerable<T>"
//   stdlib §5 `System.Collections.Linq`, the `public extension IEnumerable<T>`
//   block, declares `Where(predicate: (T) -> bool)`, `Select<U>(selector)`,
//   `Count(): int`, `First(): T` and `ToArray(): owned Array<T>`.
//
// Together: a `SortedSet<T>` IS an `IEnumerable<T>`, so the whole §5 surface
// applies to it, and §4.18 fixes the order those operators must observe.
//
// THE DEFECT (measured on the parent, `build/bezelc` at 0070402).
//
// `Emitter::lowerToLazyEnumerator` (LlvmBackend_Calls3.cpp) is the ONE place
// that turns a §5 source expression into a `bz_lazy_t*`. Its Set arm admits
// `{Set, OwningSet, SortedSet}` and hands all three to `bz_lazy_from_set`,
// which snapshots the element keys with `bz_dict_keys_gen` and enumerates them
// in the hash table's bucket order. That is right for `Set`/`OwningSet` —
// stdlib §4.10 promises no order at all — and WRONG for `SortedSet<T>`.
//
// Issue 2726 put SortedSet in that arm deliberately (before it, the receiver
// fell through to `bz_lazy_from_array` over a `bz_dict_t*` and the elements
// were bz_entry_t header bytes). That repaired the COUNT and the element
// BYTES; it left the ORDER wrong, and #2726's own regression test asserts a
// SUM — which is order-blind, so nothing observed the residue. Measured on the
// parent for a set inserted 30, 10, 20, 40:
//
//     new LinkedList<int>(s)      ->  30,10,20,40      (§5 lazy path)
//     a.Concat(s).ToArray()       ->  30,10,20,40      (§5 lazy path)
//     for var x in s do … end     ->  10,20,30,40      (§29.2 for-in path)
//
// The two surfaces over the SAME receiver disagreed — the exact property
// #2471 / #2472 established they must not — and the §5 half disagreed with the
// document.
//
// THE FIX. Feed the ONE existing sorted-snapshot authority. The §29.2 for-in
// and explicit-`GetEnumerator()` surfaces already get §4.18 right, through
// `wrapSortedSetAsIEnumerable` (LlvmBackend_Calls2.cpp). Its snapshot half is
// split out as `Emitter::sortedSetSnapshotToSortedArray` and the SortedSet arm
// of `lowerToLazyEnumerator` now calls it, wrapping the ascending array with
// `bz_lazy_from_owned_array` — the same ownership shape `bz_lazy_from_set`
// provided (the lazy owns the fresh array, frees it once at `bz_lazy_dispose`
// with a NULL element disposer, and borrows every element). `Set`/`OwningSet`
// keep `bz_lazy_from_set` verbatim; the two guards admitting the arm are
// unchanged, so no receiver that reached one lowering now reaches a different
// one. Both paths therefore read the same snapshot code and cannot disagree
// again.
//
// WHAT THIS FILE DOES **NOT** FIX (issue 3730 — the sema half of 3711).
//
// A DIRECT `SortedSet<T>` receiver — `s.ToArray()`, `s.Where(…)` — is still
// empty at rc=0, and NOT for a codegen reason: `ExprTypecheck_Calls.cpp`'s
// `isArrLike` (the gate that types the §5 operator surface on a builtin
// collection receiver) lists Array/OwningArray/Stack/OwningStack/Queue/
// OwningQueue/Set/OwningSet/LinkedList/OwningLinkedList and omits `SortedSet`,
// so the call is never TYPED and the local's slot degrades (#2434) before
// codegen has an opinion. That gate is not this file's to move. Every row here
// therefore reaches the lazy lowering the way a SortedSet reachably can today:
// as a §4.6 `LinkedList<T>` source constructor, and as the ARGUMENT of a §5
// operator on another receiver (`Concat`), where the argument is lowered by
// `lowerToLazyEnumerator` directly with no `isArrLike` gate in the way.
//
// AXIS GRID (source shape × element type × what is asserted).
//
//   row                                            asserts        parent
//   ---------------------------------------------  -------------  ----------
//   linkedlist_source_int_ascending                order          30,10,20,40
//   concat_argument_int_ascending                  order          30,10,20,40
//   string_elements_ordinal_ascending              order          pear,…
//   long_and_char_elements_ascending               order+stride   30,10,20
//   where_select_count_first_over_sortedset        §5 surface     30,10,20,40
//   forin_and_linq_agree_on_one_receiver           agreement      differ
//   class_element_linkedlist_source_ascending      order          30,10,20,40
//   class_element_concat_and_forin_agree           order+agree    differ
//   class_element_empty_sortedset                  empty/no-crash empty (ok)
//   class_element_repeated_pipelines_leak_free     ASan/LSan      clean
//   set_receiver_order_unchanged (CONTROL)         no change      unchanged
//   owningset_receiver_order_unchanged (CONTROL)   no change      unchanged
//   repeated_pipelines_are_leak_free               ASan/LSan      clean
//
// THE CLASS-ELEMENT AXIS (added at review). `SortedSet<T>` at a CLASS element
// reaches the sub-arm this commit adds — fix #2773 (B1) carves
// `headName() == "SortedSet"` out of `genericBackingType`'s `isSet` exclusion,
// so a class-element SortedSet is dict-backed and passes the arm's second
// guard — and shipped with no §5 regression coverage: the `OwningSet<Box>`
// control below is class-element but takes the ARRAY-backed path (that is what
// it exists to pin), so it is not evidence for this axis.
// `sorted_set_class_element_2773_test.cpp` covers the §29.2 for-in and
// explicit-`GetEnumerator()` surfaces for a class element and stops there. The
// four `class_element_*` rows close the §5 half; without them the same
// order-blindness that let #2726's SUM assertion mask this defect for four
// revisions would apply to the one element category whose comparator is a
// vtable dispatch rather than a memcmp.
//
// The two CONTROL rows are the §6b guard: fix #2726's own body records that
// the broader Set-family change "regressed 30+ tests", so a fix here must
// prove it moved SortedSet and nothing else. They assert that a `Set<int>` and
// an `OwningSet<C>` receiver produce the SAME order through the §5 path as
// through `for-in` — i.e. that their lowering is byte-for-byte the behaviour
// it was — and they hold on the parent as well as on this build.
//
// No C++ line comment lives inside any Bezel source literal below: one would
// comment out the rest of the program and the row would pass for the wrong
// reason.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return std::string("module M;\n"
                       "import System;\n"
                       "class C\n"
                       "  public static method Main(): int\n") +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

// Walk a LinkedList<int> built from `src` and join its node values. The §4.6
// source constructor routes `src` through lowerToLazyEnumerator, so the node
// chain's order IS the lazy source's order. `ll.FirstNode` is a
// `LinkedListNode<T>?`, so the member reads are force-unwrapped (§7.4).
constexpr const char* kJoinIntLinkedList =
    "    var acc := \"\";\n"
    "    var n := ll.FirstNode;\n"
    "    while n <> null do\n"
    "      acc := acc + $\"{n!.Value},\";\n"
    "      n := n!.Next;\n"
    "    end;\n";

// `Ord: IComparable<Ord>` — a CLASS element with a dispatchable `CompareTo`.
// stdlib §4.18 constrains `SortedSet<T>` with `where T: IComparable<T>` and
// says nothing that excludes a class, and fix #2773 (B1) made the shape
// CONSTRUCTIBLE: `genericBackingType(collTy, 0, isSet=true, …)` carves
// `collTy->headName() == "SortedSet"` out of the `isSet` exclusion that keeps
// `Set`/`OwningSet` array-backed, so a class-element SortedSet is dict-backed
// and therefore reaches the Set arm — and the sub-arm this commit adds — like
// any other element type. Ordering comes from `buildObjectCompareToThunk`
// (fix #3488), which dispatches `CompareTo` through the object vtable.
//
// This is the SAME declaration `tests/codegen/sorted_set_class_element_2773_test.cpp`
// uses, deliberately: that file pins the §29.2 for-in and explicit
// `GetEnumerator()` surfaces for a class element, and this one pins the §5
// lazy surface for it, so the two files are directly comparable.
constexpr const char* kOrdPrelude =
    "module M;\n"
    "import System;\n"
    "class Ord: IComparable<Ord>\n"
    "  public V: int := 0;\n"
    "  public constructor(v: int) V := v; end constructor;\n"
    "  public method CompareTo(other: Ord): int return V - other.V;"
    " end CompareTo;\n"
    "end Ord;\n";

// The four elements every class-element row inserts, deliberately out of
// order (30, 10, 20, 40) so neither insertion order nor allocation order is
// ascending: `new` hands out addresses in increasing order here, so a
// comparator that silently compared POINTERS instead of dispatching
// `CompareTo` would print 30,10,20,40 — the parent's answer — and be caught.
constexpr const char* kOrdElements =
    "    using var s := new SortedSet<Ord>();\n"
    "    using var e1 := new Ord(30); using var e2 := new Ord(10);\n"
    "    using var e3 := new Ord(20); using var e4 := new Ord(40);\n"
    "    s.Add(e1); s.Add(e2); s.Add(e3); s.Add(e4);\n";

std::string ordProg(const std::string& body) {
    return std::string(kOrdPrelude) +
           "class C\n"
           "  public static method Main(): int\n" +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

}  // namespace

// ===========================================================================
// §4.18 — the §4.6 LinkedList<T> source constructor over a SortedSet<int>.
// `new LinkedList<int>(s)` is the shape fix #2726's own regression test uses,
// and the one its SUM assertion could not see the order of. Parent measured
// "30,10,20,40," (bucket order); §4.18 requires "10,20,30,40,".
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           linkedlist_source_int_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new SortedSet<int>();\n"
                       "    s.Add(30); s.Add(10); s.Add(20); s.Add(40);\n"
                       "    using var ll := new LinkedList<int>(s);\n" +
                       std::string(kJoinIntLinkedList) +
                       "    Console.WriteLine($\"ll={acc}\");")),
        "ll=10,20,30,40,");
}

// ===========================================================================
// §5 / §4.18 — a SortedSet as the ARGUMENT of a §5 operator on another
// receiver. `a.Concat(s)` lowers `s` through the SAME lowerToLazyEnumerator
// SortedSet arm, with no sema gate in the way (the receiver is an Array<int>,
// which `isArrLike` does admit). The Array is empty, so the concatenation IS
// the SortedSet's own enumeration order.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           concat_argument_int_ascending_spec_5) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new SortedSet<int>();\n"
                       "    s.Add(30); s.Add(10); s.Add(20); s.Add(40);\n"
                       "    using var a := new Array<int>();\n"
                       "    using var t := a.Concat(s).ToArray();\n"
                       "    var acc := \"\";\n"
                       "    for var x in t do acc := acc + $\"{x},\"; end;\n"
                       "    Console.WriteLine($\"concat={acc}\");")),
        "concat=10,20,30,40,");
}

// ===========================================================================
// §4.18 — STRING elements. The snapshot's string arm is a separate comparator
// (#2550: a SortedSet<string> stores each element as a malloc(8) buffer
// holding the bz_string_struct*, so the sort must dereference before
// comparing). Ordinal ascending: apple < fig < pear. This row is what proves
// the §5 path picked up the string comparator and not just the generic one —
// the generic comparator over raw pointer values would order by ADDRESS, which
// is insertion-dependent, and the inserted order here is deliberately neither
// ascending nor descending.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           string_elements_ordinal_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new SortedSet<string>();\n"
                       "    s.Add(\"pear\"); s.Add(\"apple\"); s.Add(\"fig\");\n"
                       "    using var a := new Array<string>();\n"
                       "    using var t := a.Concat(s).ToArray();\n"
                       "    var acc := \"\";\n"
                       "    for var x in t do acc := acc + x + \",\"; end;\n"
                       "    Console.WriteLine($\"s={acc}\");")),
        "s=apple,fig,pear,");
}

// ===========================================================================
// §4.18 — element STRIDE. `long` is an 8-byte element and `char` a 4-byte one;
// the lazy source and the snapshot array must agree on the pitch or the drain
// reads at the wrong offset. An order assertion catches a stride mismatch far
// more sharply than a count does: a wrong pitch produces neither the ascending
// sequence nor the insertion one.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           long_and_char_elements_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var sl := new SortedSet<long>();\n"
                       "    sl.Add(30L); sl.Add(10L); sl.Add(20L);\n"
                       "    using var al := new Array<long>();\n"
                       "    using var tl := al.Concat(sl).ToArray();\n"
                       "    var accl := \"\";\n"
                       "    for var x in tl do accl := accl + $\"{x},\"; end;\n"
                       "    Console.WriteLine($\"long={accl}\");\n"
                       "    using var sc := new SortedSet<char>();\n"
                       "    sc.Add('c'); sc.Add('a'); sc.Add('b');\n"
                       "    using var ac := new Array<char>();\n"
                       "    using var tc := ac.Concat(sc).ToArray();\n"
                       "    var accc := \"\";\n"
                       "    for var x in tc do accc := accc + $\"{x},\"; end;\n"
                       "    Console.WriteLine($\"char={accc}\");")),
        "long=10,20,30,\nchar=a,b,c,");
}

// ===========================================================================
// §5 — the OPERATOR SURFACE over a SortedSet source: Where, Select, Count()
// and First(). Each is a distinct lazy/terminal consumer of the same source,
// so each re-asks lowerToLazyEnumerator for it. `First()` is the sharpest of
// the four: over an ascending source it is 10 (the §4.18 least element), over
// the parent's bucket order it was 30.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           where_select_count_first_over_sortedset_spec_5) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var s := new SortedSet<int>();\n"
            "    s.Add(30); s.Add(10); s.Add(20); s.Add(40);\n"
            "    using var a := new Array<int>();\n"
            "    using var w := a.Concat(s).Where(lambda(x: int) => x > 15)"
            ".ToArray();\n"
            "    var accw := \"\";\n"
            "    for var x in w do accw := accw + $\"{x},\"; end;\n"
            "    Console.WriteLine($\"where={accw}\");\n"
            "    using var sel := a.Concat(s).Select(lambda(x: int) => x + 1)"
            ".ToArray();\n"
            "    var accs := \"\";\n"
            "    for var x in sel do accs := accs + $\"{x},\"; end;\n"
            "    Console.WriteLine($\"select={accs}\");\n"
            "    var c := a.Concat(s).Count();\n"
            "    Console.WriteLine($\"count={c}\");\n"
            "    var f := a.Concat(s).First();\n"
            "    Console.WriteLine($\"first={f}\");")),
        "where=20,30,40,\nselect=11,21,31,41,\ncount=4\nfirst=10");
}

// ===========================================================================
// #2471 / #2472 — THE AGREEMENT ROW. One receiver, both surfaces, one program:
// the §5 lazy path and the §29.2 for-in path must produce the same string.
// This is the property the defect broke and the reason a second spelling of
// the sorted snapshot would have been the wrong fix — the two lines now read
// the same `sortedSetSnapshotToSortedArray`, so they cannot drift apart again
// without this row going red.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           forin_and_linq_agree_on_one_receiver_spec_29_2) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new SortedSet<int>();\n"
                       "    s.Add(30); s.Add(10); s.Add(20); s.Add(40);\n"
                       "    using var a := new Array<int>();\n"
                       "    using var t := a.Concat(s).ToArray();\n"
                       "    var linq := \"\";\n"
                       "    for var x in t do linq := linq + $\"{x},\"; end;\n"
                       "    var fo := \"\";\n"
                       "    for var x in s do fo := fo + $\"{x},\"; end;\n"
                       "    Console.WriteLine($\"agree={linq = fo}\");\n"
                       "    Console.WriteLine($\"linq={linq}\");")),
        "agree=True\nlinq=10,20,30,40,");
}

// ===========================================================================
// §6b CONTROL — `Set<int>` is UNTOUCHED. stdlib §4.10 promises this family no
// order, and its §5 lowering still goes through `bz_lazy_from_set`. The row
// asserts the two surfaces AGREE with each other (which is all §4.10 can ask)
// rather than pinning a particular bucket order — a hash-order literal would
// be a brittle assertion about the runtime's table, not about this fix.
//
// This row passes on the parent too. That is the point: it is the guard that a
// fix aimed at SortedSet did not move the whole Set family, which is exactly
// how the broader #2726 thesis regressed 30+ tests.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           set_receiver_order_unchanged_control_spec_4_10) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    using var s := new Set<int>();\n"
                       "    s.Add(30); s.Add(10); s.Add(20); s.Add(40);\n"
                       "    using var a := new Array<int>();\n"
                       "    using var t := a.Concat(s).ToArray();\n"
                       "    var linq := \"\";\n"
                       "    for var x in t do linq := linq + $\"{x},\"; end;\n"
                       "    var fo := \"\";\n"
                       "    for var x in s do fo := fo + $\"{x},\"; end;\n"
                       "    Console.WriteLine($\"agree={linq = fo}\");\n"
                       "    Console.WriteLine($\"n={t.Count}\");")),
        "agree=True\nn=4");
}

// ===========================================================================
// §6b CONTROL — `OwningSet<T>` is UNTOUCHED. It is the OTHER head in the arm's
// disjunction, and its class element takes the ARRAY-backed representation
// (#1070 / #2803), so `genericBackingType(…, isSet=true, …)` returns null for
// it and it falls PAST the Set arm entirely to the array wrap below. Both
// facts must survive: the row asserts the elements come back in insertion
// order with the right count, which is what the array path produces.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           owningset_receiver_order_unchanged_control_spec_4_10) {
    BEZEL_EXPECT_EQ(
        runAotOut("module M;\n"
                  "import System;\n"
                  "public class Box\n"
                  "  public V: int := 0;\n"
                  "  public constructor(v: int) V := v; end;\n"
                  "end Box;\n"
                  "class C\n"
                  "  public static method Main(): int\n"
                  "    using var s := new OwningSet<Box>();\n"
                  "    s.Add(new Box(3)); s.Add(new Box(1));\n"
                  "    s.Add(new Box(2));\n"
                  "    using var a := new Array<Box>();\n"
                  "    using var t := a.Concat(s).ToArray();\n"
                  "    var acc := \"\";\n"
                  "    for var x in t do acc := acc + $\"{x.V},\"; end;\n"
                  "    Console.WriteLine($\"os={acc}\");\n"
                  "    return 0;\n"
                  "  end Main;\n"
                  "end C;\n"),
        "os=3,1,2,");
}

// ===========================================================================
// §28.6 / §28.9 — the new lowering must be leak-free and double-free-free.
// `bz_lazy_from_owned_array` OWNS the snapshot (own_arr = 1) and frees it once
// at `bz_lazy_dispose` with a NULL element disposer; a string element slot
// holds the SortedSet's own +1, which the set releases at its own scope end,
// so the lazy must NOT release it.
//
// LOOP-AMPLIFIED to 300 iterations: a one-shot LSan row passes on both sides
// of a per-pipeline leak because the last allocation is still reachable at
// exit. At 300 iterations a one-array-per-pipeline leak is ~300 reports.
// Both element families run in the same program: int (generic key bytes) and
// string (the borrowed-pointer arm, where a spurious release would be a
// double-free rather than a leak).
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           repeated_pipelines_are_leak_free_spec_28_6) {
    auto r = runAotChecked(prog(
        "    var total := 0;\n"
        "    for var i in 0..<300 do\n"
        "      using var s := new SortedSet<int>();\n"
        "      s.Add(30); s.Add(10); s.Add(20); s.Add(40);\n"
        "      using var a := new Array<int>();\n"
        "      using var t := a.Concat(s).ToArray();\n"
        "      total := total + t.Count;\n"
        "      using var ss := new SortedSet<string>();\n"
        "      ss.Add(\"pear\"); ss.Add(\"apple\");\n"
        "      using var a2 := new Array<string>();\n"
        "      using var t2 := a2.Concat(ss).ToArray();\n"
        "      total := total + t2.Count;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=1800\n"));
}

// ===========================================================================
// §4.18 at a CLASS element — the §4.6 LinkedList<T> source constructor.
// `Ord: IComparable<Ord>` is dict-backed (fix #2773 B1), so `new
// LinkedList<Ord>(s)` routes through the SAME sub-arm the int row above
// exercises; the difference is that the ascending order now comes from
// `buildObjectCompareToThunk`'s vtable dispatch of `CompareTo` rather than
// from a memcmp over key bytes. The parent yields the bucket order,
// 30,10,20,40 — which is also the allocation order, so this row would still
// be red on a build that compared the stored POINTERS.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           class_element_linkedlist_source_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(
        runAotOut(ordProg(std::string(kOrdElements) +
                          "    using var ll := new LinkedList<Ord>(s);\n"
                          "    var acc := \"\";\n"
                          "    var n := ll.FirstNode;\n"
                          "    while n <> null do\n"
                          "      acc := acc + $\"{n!.Value.V},\";\n"
                          "      n := n!.Next;\n"
                          "    end;\n"
                          "    Console.WriteLine($\"ll={acc}\");")),
        "ll=10,20,30,40,");
}

// ===========================================================================
// §5 / §29.2 at a CLASS element — the Concat-ARGUMENT shape, plus the
// agreement property in the same program. This is the class-element twin of
// `forin_and_linq_agree_on_one_receiver_spec_29_2`: one receiver, both
// surfaces, and the §5 half must print what the for-in half prints. On the
// parent the two strings differ, so `agree=False`.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           class_element_concat_argument_and_forin_agree_spec_5) {
    BEZEL_EXPECT_EQ(
        runAotOut(ordProg(std::string(kOrdElements) +
                          "    using var arr := new Array<Ord>();\n"
                          "    using var t := arr.Concat(s).ToArray();\n"
                          "    var linq := \"\";\n"
                          "    for var x in t do linq := linq + $\"{x.V},\";"
                          " end;\n"
                          "    var fo := \"\";\n"
                          "    for var x in s do fo := fo + $\"{x.V},\";"
                          " end;\n"
                          "    Console.WriteLine($\"agree={linq = fo}\");\n"
                          "    Console.WriteLine($\"concat={linq}\");")),
        "agree=True\nconcat=10,20,30,40,");
}

// ===========================================================================
// §5 at a CLASS element — the EMPTY set. `bz_dict_keys_gen` returns count 0
// and a keys array the snapshot must still free; the sort runs over zero
// elements and the decode loop never executes. The row exists because an
// empty source is the one input where the snapshot's `free(keysArr)` is the
// only thing it does, and because a fresh `bz_array_alloc` of an empty array
// still has to reach `bz_lazy_dispose` — a wrong answer here is a crash or a
// leak, not a permutation, so no ordering row would catch it. Both consumer
// shapes are exercised.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           class_element_empty_sortedset_yields_empty_spec_5) {
    BEZEL_EXPECT_EQ(
        runAotOut(ordProg("    using var s := new SortedSet<Ord>();\n"
                          "    using var arr := new Array<Ord>();\n"
                          "    using var t := arr.Concat(s).ToArray();\n"
                          "    Console.WriteLine($\"count={t.Count}\");\n"
                          "    using var ll := new LinkedList<Ord>(s);\n"
                          "    Console.WriteLine($\"ll={ll.Count}\");")),
        "count=0\nll=0");
}

// ===========================================================================
// §28.6 / §28.11 at a CLASS element — the borrow-not-release contract.
// A class element is a §28.11 borrow slot: the `using var` locals own the
// four `Ord` instances, the SortedSet borrows them, and the snapshot array
// borrows the same pointers again. `bz_lazy_from_owned_array` must free the
// ARRAY and touch no element — releasing one would be a double-free at the
// locals' scope end, and retaining one would strand it.
//
// The int/string amplification row above cannot substitute for this: neither
// element category is a class pointer, so neither exercises the slot policy
// (`elementPolicyFor`) or a destructor. 250 iterations x 4 owned instances =
// 1000 destructor-eligible objects; a per-pipeline error is ~250 reports.
// `t[0].V` is read inside the loop so the row also value-verifies the least
// element on every iteration: total = 250 * (4 + 10 + 4) = 4500.
// ===========================================================================
BEZEL_TEST(sorted_set_linq_ascending_3711,
           class_element_repeated_pipelines_are_leak_free_spec_28_6) {
    auto r = runAotChecked(ordProg(
        "    var total := 0;\n"
        "    for var i in 0..<250 do\n"
        "      using var s := new SortedSet<Ord>();\n"
        "      using var e1 := new Ord(30); using var e2 := new Ord(10);\n"
        "      using var e3 := new Ord(20); using var e4 := new Ord(40);\n"
        "      s.Add(e1); s.Add(e2); s.Add(e3); s.Add(e4);\n"
        "      using var arr := new Array<Ord>();\n"
        "      using var t := arr.Concat(s).ToArray();\n"
        "      total := total + t.Count + t[0].V;\n"
        "      using var ll := new LinkedList<Ord>(s);\n"
        "      total := total + ll.Count;\n"
        "    end;\n"
        "    Console.WriteLine($\"total={total}\");"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("total=4500\n"));
}
