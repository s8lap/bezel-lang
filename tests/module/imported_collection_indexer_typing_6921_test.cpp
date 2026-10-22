// Cross-module rows for #6921: an indexer read on an IMPORTED builtin
// collection must be typed exactly as a local one (spec §2.5, v0.340: an
// imported declaration has the same meaning in the importer as in its home
// module; stdlib §4.5 / §4.8 / §4.9 define the indexer `[i]: T` / `[k]: V`).
//
// ── THE BUG ────────────────────────────────────────────────────────────────
//
// ModuleLoader rebuilds an imported builtin collection TypeRef (`M.OA()`
// returning `owned OwningArray<Item>`) as Named + `declaration == nullptr` +
// `astNode == nullptr`, with its head only in `importedName`. Sema's IndexExpr
// arm for the builtin collections was gated on `tt->astNode`, so an imported
// receiver skipped it and `x[0]` was left with NO type. Codegen still lowered
// the plain read, so `Console.WriteLine(a[1])` happened to print, but:
//   * a member read on the element (`x[0].N`) lowered to nothing — the whole
//     `Console.WriteLine` vanished, or an interpolation hole printed 0;
//   * `var e := x[1]` got an untyped local, so `e.N` vanished as well;
//   * `var z: int := x[1]` (an `Item` into an `int`) compiled with no
//     diagnostic — a silent acceptance.
//
// ── ORACLE ─────────────────────────────────────────────────────────────────
//
// `runAotModules`: the two-module AOT path, the only one that produces the
// astNode-less imported TypeRef. Every accept row asserts the exact stdout;
// the reject row asserts the §5.1 diagnostic.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

constexpr const char* kLib6921 =
    "module Lib6921; "
    "public record P6921(A: int, B: int); "
    "public class Item6921 "
    "  public N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "end Item6921; "
    "public class M6921 "
    "  public static method OA(): owned OwningArray<Item6921> "
    "    var v := new OwningArray<Item6921>(); "
    "    v.Add(new Item6921(5)); v.Add(new Item6921(6)); v.Add(new Item6921(7)); "
    "    return v; "
    "  end OA; "
    "  public static method OD(): owned OwningDictionary<int, Item6921> "
    "    var v := new OwningDictionary<int, Item6921>(); "
    "    v.Add(5, new Item6921(5)); v.Add(6, new Item6921(6)); "
    "    return v; "
    "  end OD; "
    "  public static method DP(): owned Dictionary<int, P6921> "
    "    var v := new Dictionary<int, P6921>(); "
    "    v[5] := new P6921(5, 50); v[6] := new P6921(6, 60); v[7] := new P6921(7, 70); "
    "    return v; "
    "  end DP; "
    "  public static method AI(): owned Array<int> "
    "    var v := new Array<int>(); v.Add(5); v.Add(6); v.Add(7); "
    "    return v; "
    "  end AI; "
    "  public static method AD(): owned Dictionary<int, Array<int>> "
    "    var v := new Dictionary<int, Array<int>>(); "
    "    var a := new Array<int>(); a.Add(1); a.Add(2); v[1] := a; "
    "    return v; "
    "  end AD; "
    "end M6921;";

std::string app6921(const std::string& body) {
    return "module App6921; "
           "import Lib6921; "
           "class C6921 public static method Main(): int "
           "  " + body + " "
           "  return 0; "
           "end Main; end C6921;";
}

test::JitResult run6921(const std::string& body) {
    return test::runAotModules(
        {{"Lib6921", kLib6921}, {"App6921", app6921(body)}}, "App6921");
}

}  // namespace

// stdlib §4.5: `OwningArray<T>[i]` is the element T. The issue's first repro:
// at c0a1928b the WriteLine is dropped (a #3079 internal warning) and nothing
// prints.
BEZEL_TEST(imported_collection_indexer_typing_6921,
           imported_owning_array_element_member_read_spec_4_5) {
    auto r = run6921(
        "using var x := M6921.OA(); x[0] := new Item6921(9); "
        "Console.WriteLine(x[0].N);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}

// stdlib §4.9: `OwningDictionary<K, V>[k]` is the value V. The issue's second
// repro (nothing printed at c0a1928b).
BEZEL_TEST(imported_collection_indexer_typing_6921,
           imported_owning_dictionary_value_member_read_spec_4_9) {
    auto r = run6921(
        "using var x := M6921.OD(); x[5] := new Item6921(9); "
        "Console.WriteLine(x[5].N);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}

// stdlib §4.8: `Dictionary<K, V>[k]` over a record V. The issue's third
// repro: at c0a1928b the hole printed `0 2`.
BEZEL_TEST(imported_collection_indexer_typing_6921,
           imported_dictionary_record_value_field_in_interpolation_spec_4_8) {
    auto r = run6921(
        "using var x := M6921.DP(); x[5] := new P6921(5, 55); "
        "_ := x.Remove(6); Console.WriteLine($\"{x[5].B} {x.Count}\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("55 2\n"));
}

// stdlib §4.5 + spec §21.1 (an inferred local takes the initializer's type):
// `var e := x[1]` is an `Item6921`, so `e.N` reads 6. At c0a1928b `e` was
// untyped and the WriteLine was dropped.
BEZEL_TEST(imported_collection_indexer_typing_6921,
           imported_owning_array_element_into_inferred_local_spec_4_5) {
    auto r = run6921(
        "using var x := M6921.OA(); var e := x[1]; Console.WriteLine(e.N);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

// spec §5.1: there is no implicit conversion from a class to `int`. The
// element of an imported `OwningArray<Item6921>` is an `Item6921`, so this is
// rejected exactly as it is for a local collection. At c0a1928b it compiled.
BEZEL_TEST(imported_collection_indexer_typing_6921,
           imported_owning_array_element_into_int_rejected_spec_5_1) {
    auto r = run6921(
        "using var x := M6921.OA(); var z: int := x[1]; Console.WriteLine(z);");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(test::diagnosticMessageContains(
        r, "is not implicitly convertible to the target type (§5.1)"));
}

// stdlib §4.4: `Count: int` on an `Array<T>`. The imported receiver shape
// reached the `.Count` typing in sema's member resolution with no astNode and
// was left untyped, so the `ToString()` call on it "produced no value". At
// c0a1928b this did not compile.
BEZEL_TEST(imported_collection_indexer_typing_6921,
           imported_array_count_to_string_spec_4_4) {
    auto r = run6921(
        "using var a := M6921.AI(); Console.WriteLine(a.Count.ToString());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// stdlib §4.8 + §4.4: the indexer element of an imported
// `Dictionary<int, Array<int>>` is an `Array<int>`, whose `Count` is an `int`.
// Typing the element (this fix's IndexExpr arm) is what brings `x[1].Count`
// to the `.Count` arm, so both halves are needed for the row.
BEZEL_TEST(imported_collection_indexer_typing_6921,
           imported_dictionary_array_value_count_to_string_spec_4_8) {
    auto r = run6921(
        "using var x := M6921.AD(); Console.WriteLine(x[1].Count.ToString());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

}  // namespace bezel::module
