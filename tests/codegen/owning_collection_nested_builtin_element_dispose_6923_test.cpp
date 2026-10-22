// fix #6923 (spec §28.11 *The element slot policy*, v0.340; stdlib §4.5 /
// §4.9 / §4.14): an OWNING collection's `Dispose()` / `Clear()` / `RemoveAt` /
// indexer-set "disposes every live element" (§28.11's owned-slot column). For
// an element whose type is itself a built-in with heap storage and no user
// TypeDecl, the per-element disposer the owning collection is handed comes
// from `elementPolicyFor` (LlvmBackend_ElementPolicy.cpp). Three gaps there
// leaked the nested element, MEASURED on c0a1928b with generated-code ASan/LSan
// (100 iterations, two nested elements per iteration):
//
//   * `Set<T>` / `SortedSet<T>` matched no arm (excluded from the borrowing
//     list for their dynamic backing): the whole nested table leaked —
//     `OwningArray<Set<int>>` 85,600 B in 1,200 allocations (the issue's
//     repro), `Set<string>` 109,000 B, the array-backed `Set<Item>` 41,600 B;
//   * a borrowing collection with `string` elements co-owns a +1 per element
//     (§28.11's shared-reference column, §28.9), but the nested free passed a
//     NULL element disposer — `OwningArray<Array<string>>` leaked 21,000 B of
//     strings (Stack / Queue / LinkedList / Dictionary<K, string> the same);
//   * `OwningBuffer<C>` owns its class slots (stdlib §4.14) but was freed with
//     a NULL element disposer (9,600 B for `OwningBuffer<Item>`);
//   * the vtable-less built-in classes (`Mutex`, `StringBuilder`, ...) took the
//     Value row (no disposer): 17,600 B / 118,400 B.
//
// Every row runs on `runAotChecked` (generated-code ASan + LSan; a leak or a
// double free flips `ran` to false), loops 100x, and asserts the program's
// stdout — at least one row per shape READS the nested element's contents, so
// a fix that freed the element early (use-after-free) or released a string it
// does not own (over-release abort) fails too.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotChecked;

namespace {

std::string repeatLine(const std::string& line, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += line + "\n";
    return s;
}

void expectClean(const JitResult& r, const std::string& expectedStdout) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "  diag: " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

// The issue's two factories plus the element class, shared by every row.
constexpr const char* kLib =
    "public class Item\n"
    "  public N: int;\n"
    "  public constructor(n: int) N := n; end constructor;\n"
    "end Item;\n"
    "public class M\n"
    "  public static method MkSet(): owned Set<int>\n"
    "    var v := new Set<int>(); v.Add(5); v.Add(6); v.Add(7);\n"
    "    return v;\n"
    "  end MkSet;\n"
    "  public static method MkStrSet(): owned Set<string>\n"
    "    var v := new Set<string>(); v.Add(\"a\" + \"x\"); v.Add(\"b\" + \"y\");\n"
    "    return v;\n"
    "  end MkStrSet;\n"
    "end M;\n";

std::string program(const std::string& body) {
    return std::string("module Loc;\nimport System.Threading;\n") + kLib +
           "class C\n"
           "  public static method Main(): int\n"
           "    var i := 0;\n"
           "    while i < 100 do\n"
           "      " + body + "\n"
           "      i += 1;\n"
           "    end;\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// Row 1 — the issue's repro verbatim (same-module): OwningArray<Set<int>>.
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           owning_array_of_set_int_issue_repro_spec_28_11) {
    expectClean(runAotChecked(program(
        "using var outer := new OwningArray<Set<int>>(); "
        "outer.Add(M.MkSet()); outer.Add(M.MkSet()); "
        "Console.WriteLine(outer.Count);")),
        repeatLine("2", 100));
}

// Row 2 — the OwningDictionary value slot, and a READ of the nested table.
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           owning_dictionary_of_set_int_reads_then_disposes_spec_28_11) {
    expectClean(runAotChecked(program(
        "using var outer := new OwningDictionary<int, Set<int>>(); "
        "outer.Add(1, M.MkSet()); outer.Add(2, M.MkSet()); "
        "Console.WriteLine($\"{outer.Count} {outer[2].Contains(6)}\");")),
        repeatLine("2 True", 100));
}

// Row 3 — SortedSet<int> (always hash-backed) and Set<string> (string keys
// released before the table is freed); both read.
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           sorted_set_and_string_set_elements_spec_28_11) {
    expectClean(runAotChecked(program(
        "using var a := new OwningArray<SortedSet<int>>(); "
        "var s := new SortedSet<int>(); s.Add(7); s.Add(5); a.Add(move s); "
        "using var b := new OwningArray<Set<string>>(); "
        "b.Add(M.MkStrSet()); b.Add(M.MkStrSet()); "
        "Console.WriteLine($\"{a[0].Count} {b[1].Contains(\"by\")}\");")),
        repeatLine("2 True", 100));
}

// Row 4 — the array-backed Set<Item> (class element: a borrowing set holds
// borrows, so only the set's own storage is freed, never the Items).
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           array_backed_class_set_frees_storage_not_elements_spec_28_11) {
    expectClean(runAotChecked(program(
        "using var it := new Item(4); "
        "using var outer := new OwningArray<Set<Item>>(); "
        "var s := new Set<Item>(); s.Add(it); outer.Add(move s); "
        "Console.WriteLine($\"{outer[0].Count} {it.N}\");")),
        repeatLine("1 4", 100));
}

// Row 5 — string-element borrowing collections nested in an owning slot: each
// co-owns a +1 per string (§28.11 shared-reference column), released when the
// nested collection is freed. Reads every element kind back first.
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           string_element_borrowing_collections_release_their_strings_spec_28_11) {
    expectClean(runAotChecked(program(
        "using var oa := new OwningArray<Array<string>>(); "
        "var a := new Array<string>(); a.Add(\"a\" + \"x\"); a.Add(\"b\" + \"y\"); "
        "oa.Add(move a); "
        "using var od := new OwningDictionary<int, Dictionary<string, string>>(); "
        "var d := new Dictionary<string, string>(); d[\"k\" + \"1\"] := \"v\" + \"1\"; "
        "od.Add(1, move d); "
        "using var os := new OwningArray<Stack<string>>(); "
        "var k := new Stack<string>(); k.Push(\"s\" + \"1\"); os.Add(move k); "
        "using var oq := new OwningArray<Queue<string>>(); "
        "var q := new Queue<string>(); q.Enqueue(\"q\" + \"1\"); oq.Add(move q); "
        "using var ol := new OwningArray<LinkedList<string>>(); "
        "var l := new LinkedList<string>(); _ := l.AddLast(\"l\" + \"1\"); ol.Add(move l); "
        "Console.WriteLine($\"{oa[0][1]} {od[1][\"k1\"]} {os[0].Peek()} {oq[0].Peek()} {ol[0].Count}\");")),
        repeatLine("by v1 s1 q1 1", 100));
}

// Row 6 — OwningBuffer<Item> nested in an owning slot owns its class slots.
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           owning_buffer_of_class_disposes_its_slots_spec_4_14) {
    expectClean(runAotChecked(program(
        "using var outer := new OwningArray<OwningBuffer<Item>>(); "
        "var b := new OwningBuffer<Item>(2); b[0] := new Item(3); b[1] := new Item(4); "
        "outer.Add(move b); "
        "Console.WriteLine($\"{outer[0].Length} {outer[0][1]!.N}\");")),
        repeatLine("2 4", 100));
}

// Row 7 — vtable-less built-in class elements, released as their `using var`
// auto-drop releases them.
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           vtableless_builtin_class_elements_mutex_stringbuilder_spec_28_11) {
    expectClean(runAotChecked(program(
        "using var om := new OwningArray<Mutex>(); "
        "om.Add(new Mutex()); om.Add(new Mutex()); "
        "using var ob := new OwningDictionary<int, StringBuilder>(); "
        "var sb := new StringBuilder(); sb.Append(\"ab\"); sb.Append(\"cd\"); "
        "ob.Add(1, move sb); "
        "Console.WriteLine($\"{om.Count} {ob[1].ToString()}\");")),
        repeatLine("2 abcd", 100));
}

// Row 8 — the other owned-slot rows of §28.11: Clear(), RemoveAt and the
// indexer-set (which disposes the displaced value) each dispose a nested set.
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           clear_removeat_and_indexer_set_dispose_nested_sets_spec_28_11) {
    expectClean(runAotChecked(program(
        "using var outer := new OwningArray<Set<string>>(); "
        "outer.Add(M.MkStrSet()); outer.Add(M.MkStrSet()); outer.Add(M.MkStrSet()); "
        "outer.RemoveAt(0); "
        "outer[0] := M.MkStrSet(); "
        "var n := outer.Count; "
        "outer.Clear(); "
        "outer.Add(M.MkStrSet()); "
        "Console.WriteLine($\"{n} {outer.Count} {outer[0].Contains(\"ax\")}\");")),
        repeatLine("2 1 True", 100));
}

// Row 9 — CONTROL: a BORROWING Array<Set<int>> holds borrows; the sets are
// owned (and disposed) by their own locals, so the borrowing outer must not
// free them (a double free here flips `ran`).
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           control_borrowing_array_of_sets_does_not_free_them_spec_28_11) {
    expectClean(runAotChecked(program(
        "using var s := M.MkSet(); "
        "using var outer := new Array<Set<int>>(); "
        "outer.Add(s); outer.Add(s); "
        "Console.WriteLine($\"{outer.Count} {s.Contains(7)}\");")),
        repeatLine("2 True", 100));
}

// Row 10 — the issue's cross-module twin: the nested sets are minted by an
// IMPORTED factory (`LibX.M.MkSet`), nested and disposed in the importer.
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           cross_module_factory_sets_nested_in_owning_array_spec_28_11) {
    std::vector<ModuleSource> mods;
    mods.push_back({"LibX", std::string("module LibX;\n") + kLib});
    mods.push_back({"App",
        "module App;\nimport LibX;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      using var outer := new OwningDictionary<int, Set<string>>();\n"
        "      outer.Add(1, M.MkStrSet()); outer.Add(2, M.MkStrSet());\n"
        "      using var o2 := new OwningArray<Set<int>>();\n"
        "      o2.Add(M.MkSet());\n"
        "      Console.WriteLine($\"{outer.Count} {outer[1].Contains(\"ax\")} {o2[0].Count}\");\n"
        "      i += 1;\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n"});
    expectClean(bezel::test::runAotModulesChecked(std::move(mods), "App"),
                repeatLine("2 True 3", 100));
}

// Row 11 — the same Set/SortedSet gap at the other per-collection free thunk:
// a §30.5 Pattern A `Task<Set<T>>` result is freed at the task's teardown by
// `patternAResultCollectionDisposer`, whose kind dispatch sent Set/SortedSet to
// bz_array_free. MEASURED on c0a1928b: an ASan SEGV inside free for
// `Task<Set<int>>`, `Task<Set<string>>` and `Task<SortedSet<int>>` (the
// array-backed `Task<Set<Item>>` was clean and stays clean).
BEZEL_TEST(owning_collection_nested_builtin_element_dispose_6923,
           task_pattern_a_set_results_are_freed_as_tables_spec_30_5) {
    const std::string src = std::string("module Loc;\n") + kLib +
        "class C\n"
        "  public static async method A(): Task<Set<int>>\n"
        "    var v := new Set<int>(); v.Add(5); v.Add(6); return v;\n"
        "  end A;\n"
        "  public static async method B(): Task<Set<string>>\n"
        "    var v := new Set<string>(); v.Add(\"a\" + \"x\"); return v;\n"
        "  end B;\n"
        "  public static async method D(): Task<SortedSet<int>>\n"
        "    var v := new SortedSet<int>(); v.Add(7); v.Add(5); v.Add(6); return v;\n"
        "  end D;\n"
        "  public static async method E(): Task<Set<Item>>\n"
        "    return new Set<Item>();\n"
        "  end E;\n"
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      using var a := C.A(); a.Wait();\n"
        "      using var b := C.B(); b.Wait();\n"
        "      using var d := C.D(); d.Wait();\n"
        "      using var e := C.E(); e.Wait();\n"
        "      Console.WriteLine($\"{a.Result.Count} {b.Result.Contains(\"ax\")} "
        "{d.Result.Count} {e.Result.Count}\");\n"
        "      i += 1;\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    expectClean(runAotChecked(src), repeatLine("2 True 3 0", 100));
}
