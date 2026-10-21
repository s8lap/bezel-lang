// Spec-derived regression test for the third #6776 fix-forward.
//
// Spec §28.7 (Drop-on-overwrite), v0.340: whole-value reassignment of an owned
// local or field disposes the displaced value "if the previous value is live".
// Spec §28.5 (Owned fields), v0.340: an `owned` field is disposed by its
// owner's destructor. stdlib §4.10 (`Set<T>`) and §4.18 (`SortedSet<T>`),
// v0.340: hash-backed sets; a `SortedSet<T>` element is `T: IComparable<T>`,
// so it never holds a class element. Spec §8.2 (Runtime type identity),
// v0.340: the #6776 fix gave `Array<T>`'s runtime struct a type tag, which
// moved its `data`/`count` words.
//
// THE DEFECT. Four disposal sites keyed the runtime free on the collection's
// HEAD NAME: the drop-on-overwrite pass for collection locals
// (disposeDisplacedCollectionLocal), the owned-collection field store
// (lowerOwnedCollectionFieldStore) and, for `SortedSet` only, the owner's
// scope-end field pass (emitOwnedFieldDisposals) and the shared entry builder
// behind the §28.8 owned-parameter auto-drop (buildCollectionTempDisposal).
// "Set" and "SortedSet" are listed as array-backed names, but a Set whose
// element is generic-dict-eligible (int, string, a record, …) is a
// `bz_dict_t*` at construction, so the displaced table went to
// `bz_array_free`. Before #6776 the two structs shared
// a {ptr, count, capacity} prefix and the misroute only leaked the table's
// order array and keys — MEASURED on 4bf709ff (pre-#6776): the owned-field
// row below printed the right answer and leaked 68 B per displaced
// `Set<int>` (13600 B in 400 allocations over 200 displacements). With the
// tagged layout, `bz_array_free` reads `data` off the table's `capacity` word
// — MEASURED on 78d28fd2 (the base), ASan armed: every non-control row below
// SEGVs in `bz_array_free` (plain build: `free(): invalid pointer`, rc 134).
//
// THE FIX routes a dict-backed `Set` / `SortedSet` through `bz_dict_free` at
// all four sites, exactly as the scope-end disposal of a `using var` Set
// local already does (a string element's +1 is released first, as a KEY).
//
// Every row runs under the checked oracle (`runAotChecked`: sanitizers armed,
// exit code 0 required) and compares exact stdout, 100 iterations per program,
// so a crash, a wrong count, a leak of the displaced table or of a string
// element, or a double free fails the row rather than passing silently. The
// two control rows pin the array-backed sets (a class-element `Set<T>` and an
// `OwningSet<T>`), whose free must stay `bz_array_free`.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

std::string rep(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

// `module M; <prelude> class C public static method Main(): int
// var i := 0; while i < 100 do <body> i += 1; end; return 0; end Main; end C;`
std::string prog(const std::string& body, const std::string& prelude = "") {
    return "module M; " + prelude +
           " class C public static method Main(): int "
           " var i := 0; while i < 100 do " + body + " i += 1; end;"
           " return 0; end Main; end C;";
}

const char* kItem =
    " class Item public N: int; public constructor(n: int) N := n; end constructor; end Item; ";

// A class holding an `owned` set field, reset through a method (the issue's
// field-reassignment shape) and read through `Count()`.
std::string boxOf(const std::string& ty, const std::string& add) {
    return " class Box private owned _s: " + ty + ";"
           " public constructor() _s := new " + ty + "(); " + add + " end constructor;"
           " public method Reset() _s := new " + ty + "(); " + add + " end Reset;"
           " public method Count(): int => _s.Count;"
           " end Box; ";
}

}  // namespace

// ---------------------------------------------------------------------------
// §28.7 — reassigning a same-module dict-backed set LOCAL disposes the
// displaced table through bz_dict_free (the issue's local repro).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           local_set_int_reassign_frees_displaced_table_28_7_4_10) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("var l := new Set<int>(); l.Add(1);"
                        " l := new Set<int>(); l.Add(2); l.Add(3);"
                        " Console.WriteLine($\"{l.Count} {l.Contains(1)}\");")),
        rep("2 False\n", 100));
}

BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           local_sortedset_int_reassign_frees_displaced_table_28_7_4_18) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("var l := new SortedSet<int>(); l.Add(1);"
                        " l := new SortedSet<int>(); l.Add(3); l.Add(2);"
                        " Console.WriteLine($\"{l.Count} {l.First} {l.Contains(1)}\");")),
        rep("2 2 False\n", 100));
}

// A string element is held as a KEY with a +1: the displaced table's keys must
// be released before the free (a leak fails the checked oracle).
BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           local_set_string_reassign_releases_keys_and_frees_table_28_7_4_10) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("var l := new Set<string>(); l.Add(\"a\" + \"x\");"
                        " l := new Set<string>(); l.Add(\"b\" + \"y\"); l.Add(\"c\" + \"z\");"
                        " Console.WriteLine($\"{l.Count} {l.Contains(\"ax\")} {l.Contains(\"by\")}\");")),
        rep("2 False True\n", 100));
}

// ---------------------------------------------------------------------------
// §28.7 / §28.5 — reassigning an `owned` dict-backed set FIELD in a method
// (the issue's field repro).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           owned_field_set_int_reassign_frees_displaced_table_28_7_28_5) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("using var b := new Box(); b.Reset(); b.Reset();"
                        " Console.WriteLine(b.Count());",
                        boxOf("Set<int>", "_s.Add(1);"))),
        rep("1\n", 100));
}

BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           owned_field_sortedset_int_reassign_frees_displaced_table_28_7_4_18) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("using var b := new Box(); b.Reset(); b.Reset();"
                        " Console.WriteLine(b.Count());",
                        boxOf("SortedSet<int>", "_s.Add(1); _s.Add(2);"))),
        rep("2\n", 100));
}

BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           owned_field_set_string_reassign_releases_keys_28_7_4_10) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("using var b := new Box(); b.Reset(); b.Reset();"
                        " Console.WriteLine(b.Count());",
                        boxOf("Set<string>", "_s.Add(\"a\" + \"x\"); _s.Add(\"b\" + \"y\");"))),
        rep("2\n", 100));
}

// ---------------------------------------------------------------------------
// §28.5 / stdlib §4.18 — an `owned SortedSet<T>` field at its OWNER's scope
// end (no reassignment): the destructor-side field pass, which already freed
// a dict-backed `Set<T>` field through bz_dict_free (#6270), must do the same
// for `SortedSet<T>`.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           owned_field_sortedset_int_owner_dispose_frees_table_28_5_4_18) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("using var b := new Box(); Console.WriteLine(b.Count());",
                        boxOf("SortedSet<int>", "_s.Add(1); _s.Add(2);"))),
        rep("2\n", 100));
}

// ---------------------------------------------------------------------------
// Controls — the ARRAY-backed sets keep bz_array_free: a class-element
// `Set<T>` (stdlib §4.10: a class element is not dict-eligible) and an
// `OwningSet<T>` (§28.11: the owner disposes its elements). Correct on the
// base and on the fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           control_local_set_class_element_reassign_stays_array_backed_4_10) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("using var a := new Item(1); using var b := new Item(2);"
                        " var l := new Set<Item>(); l.Add(a);"
                        " l := new Set<Item>(); l.Add(a); l.Add(b);"
                        " Console.WriteLine(l.Count);",
                        kItem)),
        rep("2\n", 100));
}

BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           control_owned_field_owningset_reassign_disposes_elements_28_11) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("using var b := new Box(); b.Reset(); b.Reset();"
                        " Console.WriteLine(b.Count());",
                        std::string(kItem) +
                            boxOf("OwningSet<Item>", "_s.Add(new Item(1)); _s.Add(new Item(2));"))),
        rep("2\n", 100));
}

// ---------------------------------------------------------------------------
// Site 4 — the §28.8 owned-PARAMETER auto-drop. `registerUnwindOwnedParam`
// builds the callee's scope-end entry through `buildCollectionTempDisposal`,
// whose set-family test named `Set` and `OwningSet` only; a `SortedSet<T>`
// (hash-backed for every dict-eligible `T`, stdlib §4.18) fell to the
// bz_array_free arm. MEASURED on 78d28fd2 (ASan): `Sink(move x)` with
// `owned v: SortedSet<int>` → SEGV in bz_array_free from `Sink`'s scope end;
// `SortedSet<string>` the same (rc 134 at 4bf709ff). The `Set<int>` twin is
// routed by the same builder and is a control.
// ---------------------------------------------------------------------------
namespace {
std::string sinkOf(const std::string& ty, const std::string& probe) {
    return "class H public static method Sink(owned v: " + ty + ")"
           " Console.WriteLine($\"{v.Count} " + probe + "\"); end Sink; end H;";
}
}  // namespace

BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           owned_param_sortedset_int_auto_dispose_frees_table_28_8_4_18) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("var x := new SortedSet<int>(); x.Add(7); x.Add(5); x.Add(6);"
                        " H.Sink(move x);",
                        sinkOf("SortedSet<int>", "{v.Contains(6)} {v.First()}"))),
        rep("3 True 5\n", 100));
}

BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           owned_param_sortedset_string_auto_dispose_releases_keys_28_8_4_18) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("var x := new SortedSet<string>(); x.Add(\"b\" + \"y\");"
                        " x.Add(\"a\" + \"x\"); H.Sink(move x);",
                        sinkOf("SortedSet<string>", "{v.Contains(\"ax\")}"))),
        rep("2 True\n", 100));
}

BEZEL_TEST(codegen_dict_backed_set_drop_on_overwrite_6776,
           control_owned_param_set_int_auto_dispose_frees_table_28_8_4_10) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog("var x := new Set<int>(); x.Add(7); x.Add(5); x.Add(6); x.Add(6);"
                        " H.Sink(move x);",
                        sinkOf("Set<int>", "{v.Contains(6)}"))),
        rep("3 True\n", 100));
}
