// fix #6500 (§23.4 + §15.6, v0.338; stdlib §4.8): a `Range` index on a
// dictionary whose key type is not a `Range<T>` is not an index access at all,
// and is rejected here.
//
// THE SPEC. stdlib §4.8/§4.9/§4.17 declare exactly ONE indexer on each
// dictionary — `public property [key: K]: V` — and spec §23.4 admits an index
// only when it fits a DECLARED indexer's parameter-type list ("Indexers may be
// overloaded by parameter type list"). A `Range<int>` fits `[key: K]` only where
// `K` IS a `Range<T>`: stdlib §9.8's `Range<T>` is a compiler-intrinsic record
// carrying §8.3's auto-hashable body-record surface, so `Dictionary<Range<int>,
// V>` is a legal key type and `d[1..5]` on it is a key lookup (#6207). Where `K`
// is `int` / `string` / `Guid` / a record, no indexer is applicable.
// §15.6's range SLICING — the alternative reading of `d[a..b]` — is "supported
// on `Array<T>`", with `string` the one other admitted receiver (stdlib §1); a
// dictionary is not among them, and §15.6 says so for the two receivers nearest
// it (`OwningArray<T>`: "Slicing is not admitted on `OwningArray<T>`"; inline
// arrays: "Slicing is not supported on inline arrays in this version").
//
// WHAT THE PRE-FIX COMPILER DID — and why none of it is a warrant to keep the
// index. The `isDict` branch of the IndexExpr typecheck never consulted the
// index's TYPE and typed every `d[<anything>]` as `V`, so each key
// representation failed differently and NONE of them was the range-as-key
// behaviour the issue assumed for the int-keyed form:
//
//   * `Dictionary<string, V>` — codegen's string-key encoder GEP'd and loaded a
//     `bz.string` header out of the `%bz.range` struct:
//       "Load operand must be a pointer.
//        %str.data = load ptr, %bz.range { i64 0, i64 1, i64 1, i8 1 }, align 8"
//     and `bezelc` exits 1 with "IR verification failed (malformed IR emitted)".
//     That is the issue's repro.
//
//   * `Dictionary<int, V>` — SILENTLY WRONG, which is the more expensive half and
//     the half the issue measured as "behaving correctly". The int-key encoder
//     took the range struct's word 0 — `Start` — as the key, so `d[0..1]` reads
//     `d[0]`. MEASURED on the pre-fix base, exit 0, no diagnostic, no throw:
//         var d: Dictionary<int, string> := [0: "AA", 7: "BB"];
//         d[0..1]  ->  [AA]      (not a two-entry slice, and not a miss)
//         d[7..9]  ->  [BB]
//     A user who writes `d[0..1]` meaning a slice silently gets one entry.
//     `int_set2` below is the same shape through the indexer SET.
//
// A clean diagnostic is the outcome the spec asks for and is strictly better
// than both: it replaces a compiler malformation and a silent wrong answer with
// one actionable message. The rows below are all on the `runAot` oracle.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& mainBody, const std::string& decls = "") {
    return "module M;\n" + decls +
           "class C\n  public static method Main(): int\n" + mainBody +
           "  end Main;\nend C;\n";
}

// The rejection oracle, in the shape #3892 asks for: `r.ran == false` ALONE is
// satisfied by a crash, a signal, a timeout and an ASan report as well as by a
// genuine rejection, and on this issue's own repro the pre-fix outcome was a
// genuine compile-time rejection carrying the IR-verification text — so the
// location predicate alone would have been GREEN on the pre-fix compiler. What
// discriminates is the WORDING: pre-fix, this message does not exist anywhere in
// the output, under any outcome. Both halves are asserted.
bool rejectedWith(const JitResult& r, const char* msg) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, msg);
}

// stdout of a program that must compile, run and exit 0. `ran == false` reads
// back as a marker no expectation equals, so a rejection FAILS an accept row
// rather than passing it.
std::string out(const std::string& mainBody, const std::string& decls = "") {
    auto r = runAot(prog(mainBody, decls));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// ── The issue's own repro, verbatim ────────────────────────────────────────

BEZEL_TEST(dict_range_index_6500,
           issue_repro_string_keyed_dict_range_index_spec_23_4_stdlib_4_8) {
    auto r = runAot(prog("    var d: Dictionary<string, string> := "
                         "[\"aa\": \"bb\"];\n"
                         "    var s := d[0..1];\n"
                         "    return s.Length;\n"));
    // Pre-fix: "bezelc: codegen error: IR verification failed (malformed IR
    // emitted; see report above)" — a rejection too, but not this one.
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `Dictionary<K, V>`"));
}

// The int-keyed form the issue measured as "fine and behaving correctly". It is
// not: it compiled and RAN, and read `d[0]` for `d[0..1]`. This row is the one
// that makes the fix load-bearing for the silent-wrong half — it is satisfied by
// nothing on the pre-fix compiler, which produced `ran == true` here.
BEZEL_TEST(dict_range_index_6500,
           int_keyed_range_index_was_silently_reading_the_start_spec_23_4) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := "
                         "[0: \"AA\", 7: \"BB\"];\n"
                         "    Console.WriteLine(d[0..1]);\n"
                         "    Console.WriteLine(d[7..9]);\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `Dictionary<K, V>`"));
}

// A `Range` VALUE rather than a range literal (`var r: Range<int> := 0..1;
// d[r]`). A fix keyed on the `RangeExpr` SYNTAX would leave this spelling to
// codegen, where it took the same malformed string-key path.
BEZEL_TEST(dict_range_index_6500,
           range_value_index_rejects_spec_23_4_spec_16_4) {
    auto r = runAot(prog("    var d: Dictionary<string, string> := "
                         "[\"aa\": \"bb\"];\n"
                         "    var r: Range<int> := 0..1;\n"
                         "    var s := d[r];\n"
                         "    return s.Length;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `Dictionary<K, V>`"));
}

// The indexer SET — `d[0..1] := v` — is a SEPARATE codegen call site
// (`lowerCollectionIndexerSet`, not `lowerCollectionIndexerGet`) and pre-fix
// produced the same malformed `%str.data` load. A get-only fix would leave it.
BEZEL_TEST(dict_range_index_6500,
           range_index_on_the_indexer_set_rejects_spec_23_4) {
    auto r = runAot(prog("    var d: Dictionary<string, string> := "
                         "[\"aa\": \"bb\"];\n"
                         "    d[0..1] := \"zz\";\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `Dictionary<K, V>`"));
}

// stdlib §4.9's OWNING sibling: same declared indexer `[key: K]: V`, same
// pre-fix malformed load. Enumerated by the `isDict` PREDICATE (which covers
// `Dictionary` / `OwningDictionary` / `SortedDictionary`), not by name — so the
// two rows below are covered by the same code as the two above.
BEZEL_TEST(dict_range_index_6500,
           owning_dictionary_range_index_rejects_stdlib_4_9) {
    auto r = runAot(prog("    using var d := new OwningDictionary<string, "
                         "Box>();\n"
                         "    d.Add(\"aa\", new Box(\"bb\"));\n"
                         "    var s := d[0..1];\n"
                         "    return s.V.Length;\n",
                         "class Box\n  public V: string;\n"
                         "  public constructor(v: string) V := v; end "
                         "constructor;\nend Box;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `OwningDictionary<K, V>`"));
}

BEZEL_TEST(dict_range_index_6500,
           sorted_dictionary_range_index_rejects_stdlib_4_17) {
    auto r = runAot(prog("    var d := new SortedDictionary<string, string>();\n"
                         "    d.Add(\"aa\", \"bb\");\n"
                         "    var s := d[0..1];\n"
                         "    return s.Length;\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `SortedDictionary<K, V>`"));
}

// A record key: `Point` declares an `Equals` and a `GetHashCode` (§9.2), so the
// hash machinery is real here and the range index has no more meaning than on an
// `int` key. Pins that the check reads the key TYPE rather than matching a list
// of builtin key names.
BEZEL_TEST(dict_range_index_6500,
           record_keyed_dict_range_index_rejects_spec_9_2_stdlib_4_8) {
    auto r = runAot(prog("    var d := new Dictionary<Point, string>();\n"
                         "    d.Add(new Point(1, 5), \"bb\");\n"
                         "    var s := d[0..1];\n"
                         "    return s.Length;\n",
                         "record Point(X: int, Y: int);\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "range index `[a..b]` is not admitted on `Dictionary<K, V>`"));
}

// ── ACCEPT controls: the boundary the check must not cross ──────────────────

// stdlib §9.8 + spec §8.3, v0.338: `Range<T>` is a compiler-intrinsic record
// carrying §8.3's auto-hashable body-record surface, so it IS an admissible
// `Dictionary` key and `d[1..5]` on `Dictionary<Range<int>, V>` is an ordinary
// §8.3 key lookup — stdlib §9.8's hash, §4.8's `[key: K]` indexer. This is the
// one shape the rejection must NOT take. (#6207 owns that surface's own rows;
// this one exists because the fix's EXEMPTION is the key type, so a regression
// in that exemption shows here.)
//
// Three keys sharing a `Start` and differing in the other storage words
// (§16.3's `IsEndInclusive`, §16.4's `Step`), read back through a literal AND a
// `Range` VARIABLE, so the row is not satisfiable by an encoder that consults
// only the first storage word — the very reading `Dictionary<int, V>` took
// before this fix. `7 + 80 + 900 = 987`.
BEZEL_TEST(dict_range_index_6500,
           range_keyed_dict_range_index_still_looks_the_key_up_spec_8_3_stdlib_9_8) {
    BEZEL_EXPECT_EQ(
        std::string("987\n987\n"),
        out("    var d := new Dictionary<Range<int>, int>();\n"
            "    d[1..5] := 7;\n"
            "    d[1..<5] := 8;\n"
            "    d[1..5 step 2] := 9;\n"
            "    Console.WriteLine((d[1..5] + d[1..<5] * 10 + d[1..5 step 2] "
            "* 100).ToString());\n"
            "    var e: Range<int> := 1..5;\n"
            "    var f: Range<int> := 1..<5;\n"
            "    var g: Range<int> := 1..5 step 2;\n"
            "    Console.WriteLine((d[e] + d[f] * 10 + d[g] * 100)"
            ".ToString());\n"
            "    return 0;\n"));
}

// A BARE TYPE PARAMETER key — `Dictionary<T, V>` in a generic body — is
// undecidable at the DECLARATION: at `T := Range<int>` the index below is a
// §8.3 key lookup and at `T := int` it is inadmissible, and spec §14.2 rule 2
// reads the body's own comparison at each instantiation. So the check must fail
// OPEN there.
//
// Instantiated at `Range<int>` and READ, over three keys that share a `Start`
// and differ in the other storage words (§16.3's `IsEndInclusive`, §16.4's
// `Step`), so the row is not satisfiable by an encoder that consults only the
// range's first word — the very reading `Dictionary<int, V>` took before this
// fix. `7 + 80 + 900 = 987`.
BEZEL_TEST(dict_range_index_6500,
           bare_type_parameter_key_body_is_left_alone_spec_14_2) {
    BEZEL_EXPECT_EQ(
        std::string("987\n"),
        out("    Console.WriteLine(Helper.F<Range<int>>().ToString());\n"
            "    return 0;\n",
            "class Helper\n"
            "  public static method F<T>(): int\n"
            "    var d := new Dictionary<T, int>();\n"
            "    d[1..5] := 7;\n"
            "    d[1..<5] := 8;\n"
            "    d[1..5 step 2] := 9;\n"
            "    return d[1..5] + d[1..<5] * 10 + d[1..5 step 2] * 100;\n"
            "  end F;\n"
            "end Helper;\n"));
}

// The ordinary `int` key index — the shape adjacent to every rejected row above.
// `d[0]` must still read `d`'s entry for key `0`, so a fix that rejected on
// "the index looks like a range" rather than on "the index IS a range and the
// key is not" fails here.
BEZEL_TEST(dict_range_index_6500,
           control_scalar_key_index_is_unchanged_spec_23_4_stdlib_4_8) {
    BEZEL_EXPECT_EQ(
        std::string("AA\nBB\n2\n"),
        out("    var d: Dictionary<int, string> := [0: \"AA\", 7: \"BB\"];\n"
            "    Console.WriteLine(d[0]);\n"
            "    Console.WriteLine(d[7]);\n"
            "    Console.WriteLine(d.Count.ToString());\n"
            "    return 0;\n"));
}

// §15.6's ADMITTED range slice — `Array<T>` — must still slice and produce a
// fresh owned `Array<T>` (`.Count == 2` for `0..1`). Pins that the rejection is
// on the DICTIONARY, not on the index form.
BEZEL_TEST(dict_range_index_6500,
           control_array_range_slice_is_unchanged_spec_15_6) {
    BEZEL_EXPECT_EQ(
        std::string("2\n"),
        out("    var a: Array<int> := [10, 20, 30];\n"
            "    var s := a[0..1];\n"
            "    Console.WriteLine(s.Count.ToString());\n"
            "    return 0;\n"));
}

// stdlib §1's other admitted range receiver — `s[r]` is the substring (§3.4 /
// stdlib §4.6). Same index spelling as the rejected rows, admitted receiver.
BEZEL_TEST(dict_range_index_6500,
           control_string_range_slice_is_unchanged_stdlib_1_spec_3_4) {
    BEZEL_EXPECT_EQ(
        std::string("bc\n"),
        out("    var s := \"abcd\";\n"
            "    var t := s[1..2];\n"
            "    Console.WriteLine(t);\n"
            "    return 0;\n"));
}