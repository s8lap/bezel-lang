// Regression tests for #8361 and #8363 — the ERASED `IReadOnlyCollection<T>`
// view over a collection whose CONCRETE backing the interface slot erased.
//
// THE AUTHORITY.
//
//   stdlib §4.11 `Stack<T>` / `OwningStack<T>` / `Queue<T>` / `OwningQueue<T>`,
//   "Iteration order" (v0.355):
//
//     "All four types iterate (via `IReadOnlyCollection<T>`) in take order — a
//      stack top-to-bottom (pop order), a queue front-to-back (dequeue order);
//      the owning forms iterate identically, yielding borrows."
//
//   stdlib §4.1 `IReadOnlyCollection<T>` (v0.355):
//
//     "`IReadOnlyCollection<T>` and `IReadOnlyList<T>` are covariant in T
//      (`out T`)"
//
//   stdlib §4.10 `Set<T>` (v0.355) — the DICT-backed set iterates in INSERTION
//   order (the order the elements were added).
//
// THE TWO DEFECTS.
//
//   #8361 — the erased view's ORDER was dispatched on the backing's EXACT
//   per-instantiation descriptor (`bz_array_t` word 1, stamped at construction
//   and read back by `bz_array_inst_desc`), compared against the descriptor of
//   `Stack<elem>` AT THE ERASED RECEIVER'S ELEMENT TYPE. A COVARIANTLY WIDENED
//   receiver (`IReadOnlyCollection<Animal>` over a `Stack<Dog>` — admitted by
//   stdlib §4.1's `out T` and spec §12.9) names a DIFFERENT instantiation, so
//   the compare never fired and the stack was visited in PUSH order, while the
//   CONCRETE for-in over the very same stack visited it in TAKE order. The
//   take-order clause is a property of the COLLECTION FAMILY, so the compare
//   is now family-level: the backing descriptor's RENDERED NAME (the third
//   element of the `{self, null, nameString}` descriptor array) is compared
//   against the family's name PREFIX.
//
//   #8363 — the erased slot holds the backing's pointer VERBATIM, and a
//   `Set<T>` is a DICT-backed collection (`bz_dict_t*`), NOT a `bz_array_t*`.
//   The array-backed scan read the dict header as an array header — a
//   SEGFAULT (measured: exit 139, core dumped). The backing's RUNTIME KIND
//   (§28.2's kind tag, read by `bz_runtime_kind`) now selects the scan: a
//   dict- or list-backed collection is MATERIALIZED into an array snapshot
//   first, so the erased view visits the same elements, in the same order, as
//   the CONCRETE view over the same collection.
//
// THE ROWS. Every row PRINTS ITS VISIT ORDER and asserts the ORDERED VALUES
// (never merely "no diagnostic"): the loop accumulates into a string and
// prints it once, because a per-element print would not pin the ORDER.
//
//   (1)  #8361's OWN REPRO — the covariantly-widened erased stack view.
//        RED on the unfixed base (`erased=1;2;3;`), GREEN here.
//   (2)  #8363's OWN REPRO — a `Set<int>` bound to the erased interface.
//        RED on the unfixed base (a SEGFAULT), GREEN here.
//   (3)  the `SortedSet<int>` twin of (2) — the same mechanism.
//        RED on the unfixed base (a SEGFAULT), GREEN here.
//   (4)  the EXACT-INSTANTIATION CONTROL — `IReadOnlyCollection<Dog>` over a
//        `Stack<Dog>`. GREEN on both arms (the shape #8265's narrow fix
//        already covers): the family-level compare must NOT regress it.
//   (5)  the `OwningStack<T>` twin of (4). GREEN on both arms.
//   (6)  the `Array<T>` control — an array states no take-order contract
//        distinct from storage order, so it must keep visiting FORWARD.
//        GREEN on both arms.
//   (7)  the `Queue<T>` / `OwningQueue<T>` control — a queue's front-to-back
//        order IS its physical order, so it must keep visiting FORWARD.
//        GREEN on both arms.
//   (8)  the CONCRETE `Set<T>` / `SortedSet<T>` controls — the concrete
//        paths are untouched by this fix. GREEN on both arms.
//   (9)  the CONCRETE `Dictionary<K,V>` control. GREEN on both arms.
//   (10) the erased `LinkedList<T>` — the LIST-backed twin of (2), the same
//        mechanism (the erased slot holds a `bz_llist_t*`, not a
//        `bz_array_t*`). RED on the unfixed base (a SEGFAULT), GREEN here.
//   (11) the erased shape with a USER CLASS element type — the exact half is
//        a control (green on both arms); the covariantly-widened half IS
//        row (1)'s shape.
//   (12) the erased shape with a NULLABLE element type. GREEN on both arms.
//   (13) the erased shape with an UNBOUND type parameter element (the
//        nested generic) — the forward scan is kept. GREEN on both arms.
//   (14) #8362's OWN SHAPE — a generic erased body called with a CONCRETE
//        collection argument — PINNED AS A KNOWN GAP, NOT FIXED HERE. The
//        call does not build the substitution the body reads its elements
//        through, so the row states the CURRENT (wrong) behaviour and is
//        green on BOTH arms. See the row's own comment.
//
// EVERY ROW PRINTS ITS VISIT ORDER ON ONE LINE.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// (1) #8361's OWN REPRO — the COVARIANTLY WIDENED erased stack view.
//
//     `IReadOnlyCollection<Animal>` over a `Stack<Dog>`: stdlib §4.1's `out T`
//     admits the assignment (spec §12.9), so the program COMPILES and RUNS.
//     §4.11's take-order clause applies to this shape exactly as it applies to
//     the exact instantiation, so the erased view must visit 3, 2, 1 — the
//     same order the CONCRETE for-in over the very same stack visits.
//
//     On the unfixed base the erased half printed `erased=1;2;3;` (the
//     stack's physical / push order) while the concrete half printed
//     `concrete=3;2;1;` — a silent disagreement between two views of one
//     stack.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           covariant_erased_stack_view_visits_take_order_4_11_8361) {
    const std::string src =
        "module M; "
        "class Animal public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Animal; "
        "class Dog: Animal "
        "  public constructor(t: int) base(t); end constructor; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d1 := new Dog(1); "
        "    using var d2 := new Dog(2); "
        "    using var d3 := new Dog(3); "
        "    using var s := new Stack<Dog>(); "
        "    s.Push(d1); s.Push(d2); s.Push(d3); "
        "    var r: IReadOnlyCollection<Animal> := s; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x.Tag};\"; end for; "
        "    Console.WriteLine($\"erased={acc}\"); "
        "    var acc2 := \"\"; "
        "    for var d in s do acc2 := acc2 + $\"{d.Tag};\"; end for; "
        "    Console.WriteLine($\"concrete={acc2}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=3;2;1;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "erased=1;2;3;"));
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "concrete=3;2;1;"));
}

// ---------------------------------------------------------------------------
// (2) #8363's OWN REPRO — a `Set<int>` bound to the erased interface.
//
//     stdlib §4.1's `IReadOnlyCollection<T>` is a COMMON ancestor of the
//     borrowing and the owning hierarchies, and §4.10's `Set<T>` is one of its
//     members — but a `Set<T>` is DICT-backed, so the erased slot holds a
//     `bz_dict_t*`. Reading that as the `bz_array_t*` the scan protocol
//     expects SEGFAULTED (exit 139, core dumped) on the unfixed base.
//
//     §4.10's `Set<T>` iterates in INSERTION order, so the erased view must
//     visit 1, 2, 3 — the order the CONCRETE `Set<int>` for-in visits.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           erased_set_view_visits_insertion_order_4_10_8363) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new Set<int>(); "
        "    s.Add(1); s.Add(2); s.Add(3); "
        "    var r: IReadOnlyCollection<int> := s; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x};\"; end for; "
        "    Console.WriteLine($\"erased={acc}\"); "
        "    var acc2 := \"\"; "
        "    for var x in s do acc2 := acc2 + $\"{x};\"; end for; "
        "    Console.WriteLine($\"concrete={acc2}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=1;2;3;"));
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "concrete=1;2;3;"));
}

// ---------------------------------------------------------------------------
// (3) The `SortedSet<int>` twin of (2) — the SAME mechanism (a dict-backed
//     set), so it SEGFAULTED on the unfixed base too.
//
//     The ORDER of an erased `SortedSet<T>` is NOT pinned here: §4.18's
//     ascending order is a property of the SORTED SET, and the erased slot
//     carries no marker that says "this dict is a sorted set" (a `Set<T>` and
//     a `SortedSet<T>` are the same dict at RUN TIME), so the erased view
//     cannot recover it. What IS pinned is the CONTRACT the erased view can
//     honour: it must not crash, and it must visit every element exactly
//     once. (The ascending order of the CONCRETE `SortedSet<T>` is pinned by
//     row (8) below, and is unchanged.)
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           erased_sortedset_view_visits_every_element_4_18_8363) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var ss := new SortedSet<int>(); "
        "    ss.Add(3); ss.Add(1); ss.Add(2); "
        "    var r: IReadOnlyCollection<int> := ss; "
        "    using var seen := new Array<int>(); "
        "    for var x in r do seen.Add(x); end for; "
        "    Console.WriteLine($\"count{seen.Count}\"); "
        "    var acc := \"\"; "
        "    var i := 0; "
        "    while i < seen.Count do "
        "      acc := acc + $\"{seen[i]};\"; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine($\"seen={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "count3"));
    // Every element exactly once: the three values in SOME order (the
    // ascending order is not recoverable from the erased slot — see above).
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "seen=1;2;3;") ||
                      has(r.stdoutText, "seen=2;1;3;") ||
                      has(r.stdoutText, "seen=2;3;1;") ||
                      has(r.stdoutText, "seen=3;1;2;") ||
                      has(r.stdoutText, "seen=3;2;1;") ||
                      has(r.stdoutText, "seen=1;3;2;"));
}

// ---------------------------------------------------------------------------
// (4) THE EXACT-INSTANTIATION CONTROL — the most important control.
//     `IReadOnlyCollection<Dog>` over a `Stack<Dog>`: the shape #8265's
//     narrow fix already covers. The family-level compare must NOT regress
//     it: GREEN on the base and on this tip alike.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           exact_instantiation_erased_stack_view_visits_take_order_4_11) {
    const std::string src =
        "module M; "
        "class Dog public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d1 := new Dog(1); "
        "    using var d2 := new Dog(2); "
        "    using var d3 := new Dog(3); "
        "    using var s := new Stack<Dog>(); "
        "    s.Push(d1); s.Push(d2); s.Push(d3); "
        "    var r: IReadOnlyCollection<Dog> := s; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x.Tag};\"; end for; "
        "    Console.WriteLine($\"erased={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=3;2;1;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "erased=1;2;3;"));
}

// ---------------------------------------------------------------------------
// (5) The `OwningStack<T>` twin of (4) — §4.11: "the owning forms iterate
//     identically, yielding borrows". GREEN on both arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           owning_stack_erased_view_visits_take_order_4_11) {
    const std::string src =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new OwningStack<Res>(); "
        "    s.Push(new Res(1)); s.Push(new Res(2)); s.Push(new Res(3)); "
        "    var r: IReadOnlyCollection<Res> := s; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x.Tag};\"; end for; "
        "    Console.WriteLine($\"erased={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=3;2;1;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "erased=1;2;3;"));
}

// ---------------------------------------------------------------------------
// (6) The `Array<int>` control — an array states no take-order contract
//     distinct from its storage order, so the erased view must keep visiting
//     FORWARD. GREEN on both arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           erased_array_view_still_visits_forward_order) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    var r: IReadOnlyCollection<int> := a; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x};\"; end for; "
        "    Console.WriteLine($\"erased={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=1;2;3;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "erased=3;2;1;"));
}

// ---------------------------------------------------------------------------
// (7) The `Queue<int>` / `OwningQueue<int>` control — §4.11: "a queue
//     front-to-back (dequeue order)", which IS the queue's physical order.
//     GREEN on both arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           erased_queue_view_still_visits_front_to_back_4_11) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var q := new Queue<int>(); "
        "    q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "    var r: IReadOnlyCollection<int> := q; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x};\"; end for; "
        "    Console.WriteLine($\"erased={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=1;2;3;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "erased=3;2;1;"));
}

// ---------------------------------------------------------------------------
// (8) The CONCRETE `Set<int>` / `SortedSet<int>` controls — the concrete
//     paths are untouched by this fix. `Set<T>` iterates in insertion order
//     (§4.10) and `SortedSet<T>` in ascending order (§4.18). GREEN on both
//     arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           concrete_set_and_sortedset_forin_unchanged_4_10_4_18) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new Set<int>(); "
        "    s.Add(1); s.Add(2); s.Add(3); "
        "    var acc := \"\"; "
        "    for var x in s do acc := acc + $\"{x};\"; end for; "
        "    Console.WriteLine($\"set={acc}\"); "
        "    using var ss := new SortedSet<int>(); "
        "    ss.Add(3); ss.Add(1); ss.Add(2); "
        "    var acc2 := \"\"; "
        "    for var x in ss do acc2 := acc2 + $\"{x};\"; end for; "
        "    Console.WriteLine($\"sortedset={acc2}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "set=1;2;3;"));
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "sortedset=1;2;3;"));
}

// ---------------------------------------------------------------------------
// (9) The CONCRETE `Dictionary<K,V>` control — the DICT-backed dictionary
//     iterates its ENTRIES in insertion order. GREEN on both arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           concrete_dictionary_forin_unchanged_4_8) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<int,int>(); "
        "    d.Add(1, 10); d.Add(2, 20); d.Add(3, 30); "
        "    var acc := \"\"; "
        "    for var e in d do acc := acc + $\"{e.Key}={e.Value};\"; end for; "
        "    Console.WriteLine($\"dict={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "dict=1=10;2=20;3=30;"));
}

// ---------------------------------------------------------------------------
// (10) The erased `LinkedList<int>` — the LIST-backed twin of (2): the erased
//      slot holds a `bz_llist_t*`, not a `bz_array_t*`, so the unfixed base
//      SEGFAULTED here too. §4.6's `LinkedList<T>` iterates in insertion
//      order (the chain order), which is what the erased view must visit.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           erased_linkedlist_view_visits_insertion_order_4_6) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var l := new LinkedList<int>(); "
        "    l.AddLast(1); l.AddLast(2); l.AddLast(3); "
        "    var r: IReadOnlyCollection<int> := l; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x};\"; end for; "
        "    Console.WriteLine($\"erased={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=1;2;3;"));
}

// ---------------------------------------------------------------------------
// (11) The erased shape with a USER CLASS element type — the backing is a
//      `Stack<Dog>` and the element is a user class (a class instance, not a
//      primitive). The element's LLVM type is a pointer, and the take-order
//      signal is carried by the BACKING's descriptor, not by the element —
//      so the family-level compare must still fire.
//
//      The covariantly-widened half of this row IS #8361's own shape (row 1
//      above), so it is RED on the unfixed base and GREEN here; the exact
//      half is the control and is GREEN on both arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           erased_view_over_a_user_class_element_takes_the_backings_family) {
    const std::string src =
        "module M; "
        "class Animal public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Animal; "
        "class Dog: Animal "
        "  public constructor(t: int) base(t); end constructor; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d1 := new Dog(1); "
        "    using var d2 := new Dog(2); "
        "    using var d3 := new Dog(3); "
        "    using var s := new Stack<Dog>(); "
        "    s.Push(d1); s.Push(d2); s.Push(d3); "
        "    var exact: IReadOnlyCollection<Dog> := s; "
        "    var acc := \"\"; "
        "    for var x in exact do acc := acc + $\"{x.Tag};\"; end for; "
        "    Console.WriteLine($\"exact={acc}\"); "
        "    var wide: IReadOnlyCollection<Animal> := s; "
        "    var acc2 := \"\"; "
        "    for var x in wide do acc2 := acc2 + $\"{x.Tag};\"; end for; "
        "    Console.WriteLine($\"wide={acc2}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // The EXACT instantiation — the control, green on both arms.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "exact=3;2;1;"));
    // The COVARIANTLY WIDENED instantiation — RED on the unfixed base.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "wide=3;2;1;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "wide=1;2;3;"));
}

// ---------------------------------------------------------------------------
// (12) The erased shape with a NULLABLE element type — the receiver is
//      `IReadOnlyCollection<int?>` over a `Stack<int?>`. The take-order
//      signal is the BACKING's family, so the element's nullability does not
//      enter into it. UNCHANGED by this fix: green on both arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           erased_view_with_a_nullable_element_type_is_unchanged) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new Stack<int?>(); "
        "    s.Push(1); s.Push(2); s.Push(3); "
        "    var r: IReadOnlyCollection<int?> := s; "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x};\"; end for; "
        "    Console.WriteLine($\"erased={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=3;2;1;"));
}

// ---------------------------------------------------------------------------
// (13) The erased shape with an UNBOUND type parameter element — a generic
//      body whose OWN type parameter is not bound at the call site (the
//      nested generic: the inner call's argument is the OUTER method's
//      `IReadOnlyCollection<T>`, whose `T` is itself a type parameter).
//
//      The erased receiver's element type is then NOT runtime-concrete, so
//      the take-order signal cannot be formed and the FORWARD scan is kept —
//      UNCHANGED by this fix: green on both arms.
//
//      THIS ROW IS NOT #8362's SHAPE. #8362 is the case where the argument
//      is CONCRETE (`TakeG(s)` with `s: Stack<int>`) and the substitution
//      SHOULD have been built; here the argument's element type is a type
//      parameter all the way down, so no substitution is possible at all.
//      See row (14) for #8362's own shape.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           erased_view_with_an_unbound_type_parameter_element_is_unchanged) {
    const std::string src =
        "module M; "
        "class C "
        "  static method TakeG<T>(r: IReadOnlyCollection<T>): string "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x};\"; end for; "
        "    return acc; "
        "  end TakeG; "
        "  static method Outer<T>(r: IReadOnlyCollection<T>): string "
        "    return TakeG(r); "
        "  end Outer; "
        "  public static method Main(): int "
        "    using var s := new Stack<int>(); "
        "    s.Push(1); s.Push(2); s.Push(3); "
        "    Console.WriteLine($\"outer={Outer(s)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "outer=1;2;3;"));
}

// ---------------------------------------------------------------------------
// (14) #8362's OWN SHAPE — PINNED AS A KNOWN GAP, NOT FIXED HERE.
//
//      A GENERIC erased body (`TakeG<T>(r: IReadOnlyCollection<T>)`) called
//      with a CONCRETE collection argument (`TakeG(s)` with `s: Stack<int>`).
//      The call does not build the substitution the body reads its elements
//      through, so the body runs against the erased `T -> i32` placeholder:
//      the ORDER falls back to the forward scan and the ELEMENT TYPE is
//      wrong (a `Stack<double>` prints the two halves of a double as two
//      ints).
//
//      ⇒ THIS ROW STATES THE CURRENT (WRONG) BEHAVIOUR, so it is green on
//      the base and on this tip alike. It is the in-tree marker for the
//      shape that still needs an owner: the fix belongs at the CALL SITE
//      (the generic-call substitution on a concrete collection argument),
//      NOT in the erased-collection iteration path this commit touches.
//      When that fix lands, this row's expectations must be flipped in the
//      SAME commit that fixes it.
//
//      (The already-erased argument shape — `TakeG(r)` with
//      `r: IReadOnlyCollection<int>` — is CORRECT and is pinned by the
//      8265 suite's rows (P) and (Q).)
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_forin_family_order_8361_8363,
           generic_erased_body_with_a_concrete_argument_is_still_a_known_gap_8362) {
    const std::string src =
        "module M; "
        "class C "
        "  static method TakeG<T>(r: IReadOnlyCollection<T>): string "
        "    var acc := \"\"; "
        "    for var x in r do acc := acc + $\"{x};\"; end for; "
        "    return acc; "
        "  end TakeG; "
        "  public static method Main(): int "
        "    using var s := new Stack<int>(); "
        "    s.Push(1); s.Push(2); s.Push(3); "
        "    Console.WriteLine($\"concrete={TakeG(s)}\"); "
        "    using var sd := new Stack<double>(); "
        "    sd.Push(1.5); sd.Push(2.5); "
        "    Console.WriteLine($\"double={TakeG(sd)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // The KNOWN GAP: the concrete argument's generic body still visits in
    // the stack's physical (push) order, because the erased receiver's
    // element type is an unbound type parameter there.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "concrete=1;2;3;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "concrete=3;2;1;"));
    // ...and the element type is NOT substituted (a double is not printed as
    // a double). The exact garbage is not asserted — it is endianness- and
    // platform-dependent — but the CORRECT value must NOT appear.
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "double=2.5;1.5;"));
}

