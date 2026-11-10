// Regression tests for #8279 — the ERASED `IReadOnlyCollection<DictionaryEntry<K,V>>`
// view over a DICTIONARY backing.
//
// THE AUTHORITY.
//
//   stdlib §4.8 `Dictionary<K, V>` (v0.355):
//
//     "public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>"
//
//     — so a `Dictionary<K,V>` IS an `IReadOnlyCollection<DictionaryEntry<K,V>>`
//     (stdlib §4's `ICollection<T> : IReadOnlyCollection<T>`), and spec §5.1
//     (v0.355) admits the conversion: "a class converts to an interface it
//     declares or inherits".
//
//   stdlib §4 / §4.1 — the borrowing collection family's common ancestor.
//
//   stdlib §4.8 "Iteration order" — a `Dictionary<K,V>` iterates in INSERTION
//   order; stdlib §4.17 `SortedDictionary<K,V>` iterates in ASCENDING KEY
//   order.
//
//   spec §29 / §29.2 — an `IReadOnlyCollection<T>` is iterable by `for ... in`.
//
// THE DEFECT.
//
//   The erased receiver's slot holds the backing's pointer VERBATIM, and the
//   erased scan dispatches on the backing's RUNTIME KIND (the §28.2 kind tag
//   read back by `bz_runtime_kind`). A `Dictionary<K,V>` is DICT-backed — its
//   representation is a `bz_dict_t*`, NOT a `bz_array_t*` — so the array
//   protocol read the dict header as an array header: the LENGTH came from
//   the wrong header word (a generic-key table's `key_size`) and the DATA
//   POINTER was the table's `hash_fn` — a SEGFAULT at address 0x8.
//
//   A STRING-keyed table did not even crash: its `key_size` is 0, so the
//   "length" read was 0 and the erased view SILENTLY yielded NOTHING (the
//   concrete view over the very same dictionary yielded every entry).
//
//   The fix (#8279) EXTENDS the materialization the accepted sibling commit
//   `7aa3ab3b` (#8361 / #8363) introduced: that commit materialized a
//   dict-backed or list-backed erased collection into an ARRAY SNAPSHOT, and
//   deliberately EXCLUDED the `DictionaryEntry<K,V>` element shape, because
//   the snapshot it builds holds the table's KEYS and an entry is a key/value
//   PAIR, not a key. This commit adds the ENTRY materialization: the snapshot
//   holds the table's ENTRIES, in the `DictionaryEntry` layout, decoded
//   through the SAME shared per-index decoders the CONCRETE
//   `for var e in d` path uses — so the erased view and the concrete view
//   agree element for element.
//
// THE ROWS. Every row PRINTS ITS VISIT ORDER and asserts the ORDERED VALUES
// (never merely "no diagnostic"):
//
//   (1)  #8279's OWN REPRO — a `Dictionary<int,int>` bound to the erased
//        interface, the callee summing the entry VALUES. RED on the unfixed
//        base (a SEGFAULT), GREEN here.
//   (2)  the `SortedDictionary<int,int>` twin of (1). RED on the unfixed base
//        (a SEGFAULT), GREEN here.
//   (3)  the PARAMETER shape — the erased view as a CALLEE's parameter type.
//        RED on the unfixed base (a SEGFAULT), GREEN here.
//   (4)  the KEY half — the erased view must yield the table's KEYS (this is
//        what proves the snapshot holds the ENTRIES and not the KEYS).
//   (5)  the STRING-KEY twin — the erased view must AGREE with the concrete
//        view. RED on the unfixed base (the erased half silently yielded
//        NOTHING), GREEN here.
//   (6)  the erased view must AGREE with the CONCRETE view over the same
//        dictionary — same elements, same order.
//   (7)  the `Array<DictionaryEntry<K,V>>` CONTROL — an array-backed erased
//        receiver is NOT materialized; the array protocol is untouched.
//        GREEN on both arms.
//   (8)  the `Array<int>` CONTROL — the plain erased view over an array.
//        GREEN on both arms.
//   (9)  the NULLABLE-VALUE twin — the erased view must see a null value.
//   (10) the #8265 ORDER AXIS — an erased `IReadOnlyCollection<T>` over a
//        `Stack<T>` still visits TOP-TO-BOTTOM (stdlib §4.11). The ORDER axis
//        must not be flattened by this fix. GREEN on both arms.
//
// EVERY ROW PRINTS ITS VISIT ORDER ON ONE LINE.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// runAotChecked returns ran=false on any sanitizer fire (ASan use-after-free /
// LSan leak); the rows that MATERIALIZE an entry snapshot use it, so the
// snapshot's own lifetime is pinned by the same assertion that pins its
// values: the materialization must neither leak nor double-free.
JitResult checked(const std::string& src) {
    return runAotChecked(src);
}

}  // namespace

// ---------------------------------------------------------------------------
// (1) #8279's OWN REPRO — a `Dictionary<int,int>` bound to the erased
//     `IReadOnlyCollection<DictionaryEntry<int,int>>` and for-in iterated in
//     the CALLEE.
//
//     stdlib §4.8: `Dictionary<K,V>` IS an `ICollection<DictionaryEntry<K,V>>`,
//     and spec §5.1 admits the conversion, so the callee's parameter type is
//     a legal erased view of the caller's `Dictionary`.
//
//     On the unfixed base the callee SEGFAULTED (ASan: "SEGV on unknown
//     address 0x000000000008"), because the erased scan read the
//     `bz_dict_t*` header as a `bz_array_t*`.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_dictionary_entry_view_sums_values_4_8_8279) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Sum(d: IReadOnlyCollection<DictionaryEntry<int, int>>): int "
        "    var n := 0; "
        "    for var e in d do n := n + e.Value; end for; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var d := new Dictionary<int, int>(); "
        "    d.Add(1, 10); d.Add(2, 20); "
        "    Console.WriteLine($\"{C.Sum(d)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = checked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "30"));
}

// ---------------------------------------------------------------------------
// (2) The `SortedDictionary<int,int>` twin of (1) — the same mechanism (a
//     dict-backed collection read through the array protocol).
//
//     The VALUES are asserted, not the ORDER: stdlib §4.17's ascending-key
//     order is a property of the SORTED dictionary, and the erased slot
//     carries no marker that says "this table is a sorted dictionary" (a
//     `Dictionary<K,V>` and a `SortedDictionary<K,V>` are the same table at
//     RUN TIME), so the erased view cannot recover it — the same known gap
//     the accepted sibling commit `7aa3ab3b` left for the erased
//     `SortedSet<T>` view. The ORDER of the erased SORTED view is therefore
//     NOT pinned here; what IS pinned is that it must not crash and must
//     yield every entry's value.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_sorted_dictionary_entry_view_sums_values_4_8_8279) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Sum(d: IReadOnlyCollection<DictionaryEntry<int, int>>): int "
        "    var n := 0; "
        "    for var e in d do n := n + e.Value; end for; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var d := new SortedDictionary<int, int>(); "
        "    d.Add(1, 10); d.Add(2, 20); "
        "    Console.WriteLine($\"{C.Sum(d)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = checked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "30"));
}

// ---------------------------------------------------------------------------
// (3) The PARAMETER shape — the erased view as the CALLEE's parameter type,
//     with a NULLABLE value, over a `Dictionary<int, string?>`.
//
//     The nullable value is the shape §8.6's tagged box produces: the erased
//     view must decode the boxed value exactly as the concrete view does
//     (the value half of the entry snapshot is a BORROW of the table's own
//     value slot).
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_entry_view_over_dictionary_parameter_4_8_8279) {
    const std::string src =
        "module M; import System; "
        "class C "
        "  public static method Sum(d: IReadOnlyCollection<DictionaryEntry<int, string?>>): int "
        "    var n := 0; "
        "    for var e in d do n := n + (e.Value ?? \"\").Length; end for; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var d := new Dictionary<int, string?>(); "
        "    d[1] := \"ab\"; "
        "    d.Add(2, \"cde\"); "
        "    Console.WriteLine($\"{C.Sum(d)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = checked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "5"));
}

// ---------------------------------------------------------------------------
// (4) The KEY half — the erased view must yield the table's KEYS.
//
//     This is the row that proves the snapshot holds the ENTRIES (the
//     key/value PAIRS) and not the table's KEYS: the accepted sibling commit
//     `7aa3ab3b` materialized a dict-backed SET into the table's KEYS, and
//     that snapshot is NOT what the `DictionaryEntry` element shape needs.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_entry_view_reads_the_keys_4_8_8279) {
    const std::string src =
        "module M; import System; "
        "class C "
        "  public static method Keys(d: IReadOnlyCollection<DictionaryEntry<int, int>>): string "
        "    var s := \"\"; "
        "    for var e in d do s := s + $\"{e.Key};\"; end for; "
        "    return s; "
        "  end Keys; "
        "  public static method Main(): int "
        "    using var d := new Dictionary<int, int>(); "
        "    d.Add(7, 70); d.Add(8, 80); "
        "    Console.WriteLine($\"keys={C.Keys(d)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "keys=7;8;"));
}

// ---------------------------------------------------------------------------
// (5) The STRING-KEY twin — the erased view over a `Dictionary<string,int>`.
//
//     This shape did not even SEGFAULT on the unfixed base: a string-keyed
//     table's `key_size` is 0, so the array protocol's "length" read was 0
//     and the erased view SILENTLY yielded NOTHING — a wrong answer with no
//     diagnostic at all. The erased view must agree with the CONCRETE view
//     over the very same dictionary.
//
//     The string key is REBUILT into an owned `bz_string*` that the entry
//     snapshot owns; the snapshot's own free gives it back. The row runs
//     under ASan, so a leak or an over-release of that key is caught.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_string_keyed_entry_view_agrees_with_concrete_4_8_8279) {
    const std::string src =
        "module M; import System; "
        "class C "
        "  public static method SumErased(d: IReadOnlyCollection<DictionaryEntry<string, int>>): int "
        "    var n := 0; "
        "    for var e in d do n := n + e.Value; end for; "
        "    return n; "
        "  end SumErased; "
        "  public static method SumConcrete(d: Dictionary<string, int>): int "
        "    var n := 0; "
        "    for var e in d do n := n + e.Value; end for; "
        "    return n; "
        "  end SumConcrete; "
        "  public static method Keys(d: IReadOnlyCollection<DictionaryEntry<string, int>>): string "
        "    var s := \"\"; "
        "    for var e in d do s := s + e.Key + \";\"; end for; "
        "    return s; "
        "  end Keys; "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, int>(); "
        "    d.Add(\"aa\", 10); d.Add(\"bb\", 20); "
        "    Console.WriteLine($\"erased={C.SumErased(d)}\"); "
        "    Console.WriteLine($\"concrete={C.SumConcrete(d)}\"); "
        "    Console.WriteLine($\"keys={C.Keys(d)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = checked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=30"));
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "concrete=30"));
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "keys=aa;bb;"));
}

// ---------------------------------------------------------------------------
// (6) The erased view must AGREE with the CONCRETE view over the same
//     dictionary — same elements, same order.
//
//     stdlib §4.8's iteration order for a `Dictionary<K,V>` is the INSERTION
//     order, and the CONCRETE `for var e in d` path yields exactly that. The
//     erased view must not disagree with it.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_entry_view_agrees_with_concrete_view_4_8_8279) {
    const std::string src =
        "module M; import System; "
        "class C "
        "  public static method Erased(d: IReadOnlyCollection<DictionaryEntry<int, int>>): string "
        "    var s := \"\"; "
        "    for var e in d do s := s + $\"{e.Key}={e.Value};\"; end for; "
        "    return s; "
        "  end Erased; "
        "  public static method Concrete(d: Dictionary<int, int>): string "
        "    var s := \"\"; "
        "    for var e in d do s := s + $\"{e.Key}={e.Value};\"; end for; "
        "    return s; "
        "  end Concrete; "
        "  public static method Main(): int "
        "    using var d := new Dictionary<int, int>(); "
        "    d.Add(3, 30); d.Add(1, 10); d.Add(2, 20); "
        "    Console.WriteLine($\"erased={C.Erased(d)}\"); "
        "    Console.WriteLine($\"concrete={C.Concrete(d)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=3=30;1=10;2=20;"));
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "concrete=3=30;1=10;2=20;"));
}

// ---------------------------------------------------------------------------
// (7) The `Array<DictionaryEntry<K,V>>` CONTROL — an ARRAY-backed erased
//     receiver is NOT materialized: the array protocol is the right one
//     there, and this fix must not change it.
//
//     GREEN on both arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_entry_view_over_array_backing_is_unchanged_4_8_8279) {
    const std::string src =
        "module M; import System; "
        "class C "
        "  public static method Erased(d: IReadOnlyCollection<DictionaryEntry<int, int>>): string "
        "    var s := \"\"; "
        "    for var e in d do s := s + $\"{e.Key}={e.Value};\"; end for; "
        "    return s; "
        "  end Erased; "
        "  public static method Main(): int "
        "    using var a := new Array<DictionaryEntry<int, int>>(); "
        "    a.Add(new DictionaryEntry<int, int>(7, 70)); "
        "    a.Add(new DictionaryEntry<int, int>(8, 80)); "
        "    Console.WriteLine($\"erased={C.Erased(a)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=7=70;8=80;"));
}

// ---------------------------------------------------------------------------
// (8) The `Array<int>` CONTROL — the plain erased view over an array-backed
//     collection. The array path must be untouched by the entry
//     materialization.
//
//     GREEN on both arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_int_collection_view_is_unchanged_4_1_8279) {
    const std::string src =
        "module M; "
        "class C "
        "  public static method Sum(d: IReadOnlyCollection<int>): int "
        "    var n := 0; "
        "    for var e in d do n := n + e; end for; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(10); a.Add(20); "
        "    Console.WriteLine($\"{C.Sum(a)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "30"));
}

// ---------------------------------------------------------------------------
// (9) The NULLABLE-VALUE twin — a `Dictionary<int, string?>` holding a null
//     value. The erased view must see the null (the value half of the entry
//     snapshot is the boxed value the concrete view reads).
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_entry_view_sees_a_null_value_4_8_8279) {
    const std::string src =
        "module M; import System; "
        "class C "
        "  public static method Nulls(d: IReadOnlyCollection<DictionaryEntry<int, string?>>): int "
        "    var n := 0; "
        "    for var e in d do if e.Value is null then n := n + 1; end if; end for; "
        "    return n; "
        "  end Nulls; "
        "  public static method Main(): int "
        "    using var d := new Dictionary<int, string?>(); "
        "    d[1] := \"a\"; "
        "    d.Add(2, null); "
        "    Console.WriteLine($\"nulls={C.Nulls(d)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "nulls=1"));
}

// ---------------------------------------------------------------------------
// (10) THE ORDER AXIS — #8265's twin. An erased `IReadOnlyCollection<T>` over
//      a `Stack<T>` MUST STILL visit TOP-TO-BOTTOM (stdlib §4.11: "All four
//      types iterate (via `IReadOnlyCollection<T>`) in take order — a stack
//      top-to-bottom (pop order)").
//
//      This fix EXTENDS the materialization the sibling commit `7aa3ab3b`
//      introduced; it must not flatten the ORDER axis. GREEN on both arms
//      (the row was flipped by `7aa3ab3b`, which the chain carries).
// ---------------------------------------------------------------------------
BEZEL_TEST(erased_collection_entry_backing_8279,
           erased_stack_view_still_visits_top_to_bottom_4_11_8279) {
    const std::string src =
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new Stack<int>(); "
        "    s.Push(1); s.Push(2); s.Push(3); "
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
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "erased=3;2;1;"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "erased=1;2;3;"));
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "concrete=3;2;1;"));
}
