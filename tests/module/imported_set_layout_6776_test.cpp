// Cross-module rows for the #6776 fix-forward: an IMPORTED builtin collection
// must lower through the same runtime arm as a local one (spec §2.5, v0.340:
// an imported declaration has the same meaning in the importer as in its home
// module).
//
// ── THE BUG ────────────────────────────────────────────────────────────────
//
// ModuleLoader rebuilds an imported builtin TypeRef as Named +
// `declaration == nullptr` + `astNode == nullptr`, with the head name only in
// `importedName` (which `TypeRef::headName()` returns). Many codegen
// head-name predicates (`isSetName`, `arrIsOwning`, `isStrBld`, ...) were
// gated on `tt->astNode && tt->astNode->name && ...` before calling
// `headName()`, so an imported receiver failed the predicate and fell to a
// different arm. For the dict-backed `Set<T>` / `SortedSet<T>` (stdlib §4.10,
// §4.18) the fallback is the `bz_array_*` arm. Before #6776 that arm read
// `count` at offset 8 of the `bz_dict_t`, which gave a plausible Count and
// wrong iteration / Contains. #6776 moved `bz_array_t.count` to offset 24
// (the new `{vtable, type_desc}` header), so the same misread now returns 4
// and the program aborts or segfaults. The fix removes the astNode gate, so
// the predicate keys on `headName()`. The `!declaration` / `Kind::Named`
// guards stay, so an imported USER type (declaration set, importedName empty)
// can never match.
//
// ── ORACLE ─────────────────────────────────────────────────────────────────
//
// `runAotModules`: the two-module AOT path (`compileObject` for the library,
// `compileFile` with a `Dependency` for the importer), the only path that
// produces the astNode-less imported TypeRef. A same-module row cannot reach
// the shape. Every row asserts the exact stdout. The leak rows use
// `runAotModulesChecked` (generated-code ASan + LSan, exitcode=99 on a leak)
// and loop 50x so that a per-iteration leak cannot hide.
//
// Fail-without (src/ reverted to e335c351, test binary rebuilt): every row
// below fails. The figures are in the commit body.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

// The library exports producers whose RETURN TYPES are builtin collections.
// None of them is an exported user type here, so the importer's
// `lookupNamed` fails for each one and each comes back astNode-less.
constexpr const char* kLib6776 =
    "module Lib6776; "
    "public class Item6776 "
    "  public N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "end Item6776; "
    "public class M6776 "
    "  public static method St(): owned Set<int> "
    "    var v := new Set<int>(); v.Add(5); v.Add(6); v.Add(7); v.Add(6); "
    "    return v; "
    "  end St; "
    "  public static method SS(): owned SortedSet<int> "
    "    var v := new SortedSet<int>(); v.Add(7); v.Add(5); v.Add(6); "
    "    return v; "
    "  end SS; "
    "  public static method Sk(): owned Stack<int> "
    "    var v := new Stack<int>(); v.Push(5); v.Push(6); v.Push(7); "
    "    return v; "
    "  end Sk; "
    "  public static method SD(): owned SortedDictionary<int, int> "
    "    var v := new SortedDictionary<int, int>(); "
    "    v[7] := 70; v[5] := 50; v[6] := 60; "
    "    return v; "
    "  end SD; "
    "  public static method SB(): owned StringBuilder "
    "    var v := new StringBuilder(); v.Append(\"ab\"); v.Append(\"cde\"); "
    "    return v; "
    "  end SB; "
    "  public static method A(): owned Array<int> "
    "    var v := new Array<int>(); v.Add(5); v.Add(6); v.Add(7); "
    "    return v; "
    "  end A; "
    "  public static method OS(): owned OwningSet<Item6776> "
    "    var v := new OwningSet<Item6776>(); "
    "    v.Add(new Item6776(5)); v.Add(new Item6776(6)); "
    "    return v; "
    "  end OS; "
    "  public static method OA(): owned OwningArray<Item6776> "
    "    var v := new OwningArray<Item6776>(); "
    "    v.Add(new Item6776(5)); v.Add(new Item6776(6)); "
    "    return v; "
    "  end OA; "
    "  public static method OSk(): owned OwningStack<Item6776> "
    "    var v := new OwningStack<Item6776>(); "
    "    v.Push(new Item6776(5)); v.Push(new Item6776(6)); "
    "    return v; "
    "  end OSk; "
    "  public static method OQ(): owned OwningQueue<Item6776> "
    "    var v := new OwningQueue<Item6776>(); "
    "    v.Enqueue(new Item6776(5)); v.Enqueue(new Item6776(6)); "
    "    return v; "
    "  end OQ; "
    "  public static method OLL(): owned OwningLinkedList<Item6776> "
    "    var v := new OwningLinkedList<Item6776>(); "
    "    _ := v.AddLast(new Item6776(5)); _ := v.AddLast(new Item6776(6)); "
    "    return v; "
    "  end OLL; "
    "  public static method OD(): owned OwningDictionary<int, Item6776> "
    "    var v := new OwningDictionary<int, Item6776>(); "
    "    v.Add(5, new Item6776(5)); v.Add(6, new Item6776(6)); "
    "    return v; "
    "  end OD; "
    "end M6776;";

// The importer. `body` is the statements under test.
std::string app6776(const std::string& body) {
    return "module App6776; "
           "import Lib6776; "
           "class C6776 public static method Main(): int "
           "  " + body + " "
           "  return 0; "
           "end Main; end C6776;";
}

test::JitResult run6776(const std::string& body) {
    return test::runAotModules(
        {{"Lib6776", kLib6776}, {"App6776", app6776(body)}}, "App6776");
}

}  // namespace

// ===========================================================================
// Set<T> / SortedSet<T>: the dict-backed family (stdlib §4.10, §4.18).
// ===========================================================================

// stdlib §4.10: `Set<T>.Count` is the number of distinct elements. Four Adds,
// one duplicate, so 3. At e335c351 it prints 4 (`bz_array_len` reading the
// dict at the moved offset) and then aborts (rc 134).
BEZEL_TEST(imported_set_layout_6776, imported_set_count_spec_4_10) {
    auto r = run6776("using var x := M6776.St(); Console.WriteLine(x.Count);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// stdlib §4.18: `SortedSet<T>.Count`.
BEZEL_TEST(imported_set_layout_6776, imported_sorted_set_count_spec_4_18) {
    auto r = run6776("using var x := M6776.SS(); Console.WriteLine(x.Count);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// stdlib §4.10: `for` over an imported Set visits every element once. The sum
// is checked because §4.10 states no iteration order for `Set<T>`. At
// e335c351 this segfaults (rc 139). Before #6776 it summed 0.
BEZEL_TEST(imported_set_layout_6776, imported_set_foreach_visits_each_element_spec_4_10) {
    auto r = run6776(
        "using var x := M6776.St(); var t := 0; "
        "for var e in x do t += e; end; Console.WriteLine(t);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("18\n"));
}

// stdlib §4.18: a SortedSet enumerates in ascending order. The library
// inserts 7, 5, 6.
BEZEL_TEST(imported_set_layout_6776, imported_sorted_set_foreach_ascending_spec_4_18) {
    auto r = run6776(
        "using var x := M6776.SS(); var t := \"\"; "
        "for var e in x do t := t + $\"{e},\"; end; Console.WriteLine(t);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5,6,7,\n"));
}

// stdlib §4.10: `Contains` and `Add` on an imported Set. Before #6776,
// `Contains(6)` returned False. At e335c351 it segfaults.
BEZEL_TEST(imported_set_layout_6776, imported_set_contains_and_add_spec_4_10) {
    auto r = run6776(
        "using var x := M6776.St(); "
        "Console.WriteLine($\"{x.Contains(6)} {x.Contains(9)}\"); "
        "x.Add(9); x.Add(9); Console.WriteLine(x.Count);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True False\n4\n"));
}

// stdlib §4.18: `SortedSet<T>.First` / `.Last` are the least and greatest
// elements. Before the fix both read 0.
BEZEL_TEST(imported_set_layout_6776, imported_sorted_set_first_last_spec_4_18) {
    auto r = run6776(
        "using var x := M6776.SS(); Console.WriteLine($\"{x.First} {x.Last}\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5 7\n"));
}

// stdlib §4.10 + §5: a LINQ terminal and `UnionWith` with an imported Set as
// the source (the lazy-enumerator and set-algebra arms).
BEZEL_TEST(imported_set_layout_6776, imported_set_linq_sum_and_union_spec_4_10) {
    auto r = run6776(
        "using var x := M6776.St(); Console.WriteLine(x.Sum()); "
        "using var y := new Set<int>(); y.Add(1); y.UnionWith(x); "
        "Console.WriteLine(y.Count);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("18\n4\n"));
}

// stdlib §4.10: an explicit `Dispose()` of an imported Set frees the dict, not
// a `bz_array_t`. At e335c351 this aborts (rc 134) before printing.
BEZEL_TEST(imported_set_layout_6776, imported_set_explicit_dispose_spec_4_10) {
    auto r = run6776(
        "var x := M6776.St(); x.Dispose(); Console.WriteLine(\"d\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("d\n"));
}

// ===========================================================================
// Other heads behind the same astNode gate.
// ===========================================================================

// stdlib §4.11 "Iteration order": a stack enumerates top-to-bottom (pop
// order), and `TryPeek` reads the top. The imported Stack used to scan bottom-
// up (5,6,7) and peeked the bottom (5). That was wrong before #6776 too.
BEZEL_TEST(imported_set_layout_6776, imported_stack_foreach_and_trypeek_top_first_spec_4_11) {
    auto r = run6776(
        "using var x := M6776.Sk(); var t := \"\"; "
        "for var e in x do t := t + $\"{e},\"; end; Console.WriteLine(t); "
        "var p: int? := null; _ := x.TryPeek(out p); Console.WriteLine(p ?? -1);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7,6,5,\n7\n"));
}

// stdlib §4.17: a SortedDictionary enumerates in ascending key order, and
// `FirstKey` / `LastKey` are the extremes. The imported form iterated in hash
// order (7,5,6) and read 0 0.
BEZEL_TEST(imported_set_layout_6776, imported_sorted_dictionary_order_and_first_last_key_spec_4_17) {
    auto r = run6776(
        "using var x := M6776.SD(); var t := \"\"; "
        "for var e in x do t := t + $\"{e.Key},\"; end; Console.WriteLine(t); "
        "Console.WriteLine($\"{x.FirstKey} {x.LastKey}\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5,6,7,\n5 7\n"));
}

// stdlib §3.1 `StringBuilder.Append`: on an imported StringBuilder this was
// REJECTED ("no dispatch arm") because the `isStrBld` predicate was gated.
BEZEL_TEST(imported_set_layout_6776, imported_string_builder_append_spec_3_1) {
    auto r = run6776(
        "using var x := M6776.SB(); x.Append(\"fg\"); "
        "Console.WriteLine(x.Length); Console.WriteLine(x);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\nabcdefg\n"));
}

// stdlib §4.4: `Array<T>.IsReadOnly` is false and `Capacity >= Count`. On the
// imported receiver both statements lowered to nothing (#3079 warning), so
// stdout was empty.
BEZEL_TEST(imported_set_layout_6776, imported_array_is_read_only_and_capacity_spec_4_4) {
    auto r = run6776(
        "using var x := M6776.A(); Console.WriteLine(x.IsReadOnly); "
        "Console.WriteLine(x.Capacity >= 3);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nTrue\n"));
}

// ===========================================================================
// Owning-family element disposal (spec §28.11, stdlib §4.5/§4.7/§4.9/§4.10/
// §4.11). `collIsOwning` / `arrIsOwning` / `dictIsOwning` were gated, so an
// imported Owning* collection was disposed as borrowing and its elements
// leaked (pre-#6776 too). Under LSan that is exitcode 99.
// ===========================================================================

BEZEL_TEST(imported_set_layout_6776, imported_owning_collections_dispose_their_elements_checked_spec_28_11) {
    auto r = test::runAotModulesChecked(
        {{"Lib6776", kLib6776},
         {"App6776", app6776(
             "var i := 0; "
             "while i < 50 do "
             "  using var a := M6776.OA(); a.Add(new Item6776(9)); "
             "  using var s := M6776.OS(); s.Add(new Item6776(9)); "
             "  using var k := M6776.OSk(); k.Push(new Item6776(9)); "
             "  using var q := M6776.OQ(); q.Enqueue(new Item6776(9)); "
             "  using var l := M6776.OLL(); _ := l.AddLast(new Item6776(9)); "
             "  using var d := M6776.OD(); d.Add(9, new Item6776(9)); "
             "  i += 1; "
             "end; "
             "Console.WriteLine(\"done\");")}},
        "App6776");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// The same, with explicit `Dispose()` (the `emitExplicitDisposeChain` arms).
BEZEL_TEST(imported_set_layout_6776, imported_owning_collections_explicit_dispose_checked_spec_28_11) {
    auto r = test::runAotModulesChecked(
        {{"Lib6776", kLib6776},
         {"App6776", app6776(
             "var i := 0; "
             "while i < 50 do "
             "  var a := M6776.OA(); a.Dispose(); "
             "  var s := M6776.OS(); s.Dispose(); "
             "  var k := M6776.OSk(); k.Dispose(); "
             "  var q := M6776.OQ(); q.Dispose(); "
             "  var l := M6776.OLL(); l.Dispose(); "
             "  var d := M6776.OD(); d.Dispose(); "
             "  i += 1; "
             "end; "
             "Console.WriteLine(\"done\");")}},
        "App6776");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

}  // namespace bezel::module
