// fix #853 / stdlib §4.1–§4.3: collection-interface members (Count / Add / Clear
// / Contains / Remove / IndexOf / Insert) invoked THROUGH the interface on a
// built-in collection.
//
// Before the fix, a member call through a collection interface fell through to a
// stub: `IReadOnlyCollection<int>.Count` returned 0 regardless of element count,
// and `ICollection<int>.Add` was a silent no-op (the #853 miscompile). Direct
// calls on the concrete collection were fine.
//
// The fix has two parts, split by soundness (stdlib §4.1–§4.3):
//
//   * SOUND BRIDGE — the array-backed *list* interfaces IReadOnlyList<T> and
//     IList<T> are implemented ONLY by the array-backed Array<T> / OwningArray<T>,
//     so an interface-typed receiver carries the same bz_array_t* as the concrete
//     collection and Count/Add/Clear/Contains/Remove/IndexOf/Insert lower through
//     the identical bz_array_* runtime — the same bridge the indexer get/set
//     already use (#401). These now compute the correct value.
//
//   * CLEAN DIAGNOSTIC — the *non-list* collection interfaces IReadOnlyCollection
//     <T> / ICollection<T> / IOwningCollection<T> (and the owning list interface
//     IOwningList<T> for mutation) are genuinely polymorphic: they are implemented
//     by collections with INCOMPATIBLE runtime representations (Array → bz_array_t*
//     count@8; LinkedList → bz_llist_t* count@16; Dictionary/Set → bz_dict_t*),
//     the interface-typed receiver erases which backing it holds, the structs
//     share no kind tag, and there is no collection itable. No single runtime call
//     is sound, so these are diagnosed at compile time instead of miscompiled. A
//     real fix needs a runtime foundation (kind-tag header or per-backing itable),
//     which is deferred (#853 stays open). This test pins the diagnostic so the
//     silent miscompile cannot return.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// AOT-compile+run a program and return its `main` exit code (or -999 if it
// did not run: compile/link reject or program crash/timeout).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// True when the program is REJECTED at compile time (a diagnostic fired) and it
// did NOT run. (A spurious IR-verification failure would also set hasErrors, so
// asserting `!ran` alone is weaker; we require the explicit diagnostic.)
bool rejects(const std::string& src) {
    auto r = runAot(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

}  // namespace

// ---------------------------------------------------------------------------
// SOUND BRIDGE — array-backed list interfaces (IReadOnlyList<T> / IList<T>).
// ---------------------------------------------------------------------------

// §4.1 / §4.3 (#853): `IReadOnlyList<int>.Count` through the interface returns the
// real element count. The #853 repro shape, on the array-backed list interface.
// Load-bearing: before the fix the interface `.Count` returned 0.
BEZEL_TEST(collection_interface_853, ireadonlylist_count_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method GetCount(c: IReadOnlyList<int>): int => c.Count; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    return GetCount(a); "                          // 3
        "  end Main; end C;"), 3);
}

// §4.1 / §4.3 (#853): an OwningArray viewed as IReadOnlyList<T> — the shared
// read-only view of an OWNING array-backed collection (§4.3) — reads Count
// through the interface. The owner disposes elements at scope end.
BEZEL_TEST(collection_interface_853, ireadonlylist_over_owned_count_4_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public V: int; "
        "  public constructor(v: int) V := v; end constructor; end R; "
        "class C "
        "  public static method GetCount(c: IReadOnlyList<R>): int => c.Count; "
        "  public static method Main(): int "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(move (new R(1))); a.Add(move (new R(2))); "
        "    return GetCount(a); "                          // 2
        "  end Main; end C;"), 2);
}

// §4.1 (#853): `IList<int>.Add` through the interface APPENDS to the same backing
// (concrete `.Count` afterwards observes the added elements). Load-bearing:
// before the fix the interface Add was a silent no-op (Count unchanged).
BEZEL_TEST(collection_interface_853, ilist_add_appends_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method AddTwo(c: IList<int>): () "
        "    c.Add(50); c.Add(60); end AddTwo; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); "
        "    AddTwo(a); "
        "    return a.Count; "                              // 1 + 2 = 3
        "  end Main; end C;"), 3);
}

// §4.1 (#853): `IList<int>.Clear` through the interface empties the same backing
// (concrete `.Count` afterwards is 0). Load-bearing: was a silent no-op.
BEZEL_TEST(collection_interface_853, ilist_clear_empties_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method ClearIt(c: IList<int>): () c.Clear(); end ClearIt; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    ClearIt(a); "
        "    return a.Count; "                              // 0
        "  end Main; end C;"), 0);
}

// §4.1 / §25.2 (#853): `IList<int>.Contains` through the interface scans the
// backing with the element's equality. true for a present element, false for an
// absent one. Encoded: present(7) → +1, absent(8) → +0.
BEZEL_TEST(collection_interface_853, ilist_contains_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Has(c: IList<int>, x: int): bool => c.Contains(x); "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(5); a.Add(7); a.Add(9); "
        "    var r := 0; "
        "    if Has(a, 7) then r := r + 1; end; "
        "    if Has(a, 8) then r := r + 10; end; "
        "    return r; "                                    // 1 (has 7, not 8)
        "  end Main; end C;"), 1);
}

// §4.1 (#853): `IList<int>.Remove` / `.Insert` through the interface reach the
// array runtime. Insert(0,100) → [100,1,2]; Remove(100) → true and Count drops
// back to 2. Encoded: 1000 (removed) + final Count(2) = 1002.
//
// fix #7015 (language spec §27.6, v0.340; stdlib §4.1) FLIPPED this row's
// `IndexOf` call out: `IList<T>` declares the settable indexer and `Insert`
// and inherits `ICollection<T>`'s members (`Count`, `IsReadOnly`, `Add`,
// `Clear`, `Contains`, `Remove`) plus `GetEnumerator` — it declares NO
// `IndexOf` (stdlib §4.1). §27.6 resolves a member "on the value exactly as
// the call `v.Scaled()` is", and the BARE form `il.IndexOf` was already
// rejected as a name the head does not declare (fix #6997), so the call form
// had to agree. The rejection is pinned by
// `builtin_interface_undeclared_member_call_7015.ilist_call_indexof_is_
// rejected_spec_4_1`; the concrete `Array<T>.IndexOf` (stdlib §4.4) is
// unaffected. The row's bridge coverage (Insert and Remove through the
// interface reach the array runtime) is unchanged.
BEZEL_TEST(collection_interface_853, ilist_remove_insert_4_1) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Mutate(c: IList<int>): int "
        "    c.Insert(0, 100); "
        "    var removed := c.Remove(100); "
        "    var r := 0; "
        "    if removed then r := r + 1000; end; "
        "    return r; "
        "  end Mutate; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); "
        "    Console.WriteLine(Mutate(a) + a.Count); return 0; "  // 1000 + 2 = 1002
        "  end Main; end C;"), "1002");
}

// §4.1 / §3 (#853): a string-element IList<string>.Add through the interface
// stores the pointer slot; concrete `.Count` observes the additions. (Exercises
// the per-element string retain bookkeeping on the interface Add path.)
BEZEL_TEST(collection_interface_853, ilist_string_add_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method AddIt(c: IList<string>): () "
        "    c.Add(\"alpha\"); c.Add(\"beta\"); end AddIt; "
        "  public static method Main(): int "
        "    using var a := new Array<string>(); "
        "    a.Add(\"seed\"); "
        "    AddIt(a); "
        "    return a.Count; "                              // 3
        "  end Main; end C;"), 3);
}

// §4.1 (#853): Array<string> viewed as IReadOnlyList<string> reads Count through
// the interface for a second (non-int) element type.
BEZEL_TEST(collection_interface_853, ireadonlylist_string_count_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method GetCount(c: IReadOnlyList<string>): int => c.Count; "
        "  public static method Main(): int "
        "    using var a := new Array<string>(); "
        "    a.Add(\"x\"); a.Add(\"y\"); "
        "    return GetCount(a); "                          // 2
        "  end Main; end C;"), 2);
}

// ---------------------------------------------------------------------------
// CLEAN DIAGNOSTIC — polymorphic collection interfaces (IReadOnlyCollection<T>
// / ICollection<T>). No single runtime backing → rejected, not miscompiled.
// ---------------------------------------------------------------------------

// §4.1–§4.3 (#853): `IReadOnlyCollection<int>.Count` through the interface is the
// literal issue repro. It is genuinely polymorphic (Array / LinkedList /
// Dictionary all conform with incompatible backings), so it is REJECTED with a
// clean diagnostic rather than silently returning 0.
BEZEL_TEST(collection_interface_853, ireadonlycollection_count_rejected_4_1) {
    BEZEL_EXPECT_EQ(rejects(
        "module M; class C "
        "  public static method GetCount(c: IReadOnlyCollection<int>): int => c.Count; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    return GetCount(a); "
        "  end Main; end C;"), true);
}

// §4.1–§4.3 (#853): `ICollection<int>.Add` through the interface — polymorphic,
// so REJECTED rather than a silent no-op.
BEZEL_TEST(collection_interface_853, icollection_add_rejected_4_1) {
    BEZEL_EXPECT_EQ(rejects(
        "module M; class C "
        "  public static method AddIt(c: ICollection<int>): () c.Add(99); end AddIt; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(1); "
        "    AddIt(a); "
        "    return a.Count; "
        "  end Main; end C;"), true);
}

// §4.1–§4.3 (#853): `ICollection<int>.Clear` through the interface — REJECTED.
BEZEL_TEST(collection_interface_853, icollection_clear_rejected_4_1) {
    BEZEL_EXPECT_EQ(rejects(
        "module M; class C "
        "  public static method ClearIt(c: ICollection<int>): () c.Clear(); end ClearIt; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(1); a.Add(2); "
        "    ClearIt(a); "
        "    return a.Count; "
        "  end Main; end C;"), true);
}

// §4.1–§4.3 (#853): `ICollection<int>.Contains` through the interface — REJECTED.
// (Returns bool: the diagnostic placeholder must be bool-typed so no spurious IR
// failure masks the diagnostic.)
BEZEL_TEST(collection_interface_853, icollection_contains_rejected_4_1) {
    BEZEL_EXPECT_EQ(rejects(
        "module M; class C "
        "  public static method Has(c: ICollection<int>, x: int): bool => c.Contains(x); "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(5); "
        "    var r := 0; "
        "    if Has(a, 5) then r := 1; end; "
        "    return r; "
        "  end Main; end C;"), true);
}

// ---------------------------------------------------------------------------
// NO REGRESSION — direct concrete calls and the for-in / indexer interface
// paths (already array-backed by #401) are unchanged.
// ---------------------------------------------------------------------------

// §3 / §4.4: a direct concrete `Array<int>.Count` / `.Add` still works (the
// concrete-typed path is untouched).
BEZEL_TEST(collection_interface_853, direct_concrete_count_unchanged_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  return a.Count; "                                // 4
        "end Main; end C;"), 4);
}

// §4.3 / §29.2 / §4.1: a for-in loop and an indexer over an IReadOnlyList<int>
// view still iterate / index the backing array (the #401 paths are not affected
// by the #853 diagnostic, which is scoped to method/Count calls).
BEZEL_TEST(collection_interface_853, forin_and_indexer_over_ireadonlylist_unchanged_4_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method First(c: IReadOnlyList<int>): int => c[0]; "
        "  public static method SumIt(c: IReadOnlyList<int>): int "
        "    var s := 0; "
        "    for var x in c do s := s + x; end; "
        "    return s; "
        "  end SumIt; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(10); a.Add(20); a.Add(30); "
        "    return First(a) + SumIt(a); "                  // 10 + 60 = 70
        "  end Main; end C;"), 70);
}
