// Regression tests for issue #8263 — a `SortedSet<Guid>` and a
// `SortedDictionary<Guid, V>` iterated in INSERTION ORDER instead of in
// ascending `Guid` order.
//
// WHAT THE DOCUMENTS REQUIRE (stdlib v0.355).
//   * §2.6 declares
//         public record Guid: IComparable<Guid>, IEquatable<Guid>
//     with "CompareTo = lexicographic over the 16 canonical big-endian bytes
//     (stated explicitly — .NET's field-group order is a known trap)".
//   * §4.18 declares `SortedSet<T> where T: IComparable<T>` — "A
//     comparison-ordered set: membership by `CompareTo`-equality, iteration in
//     ascending order."
//   * §4.17 declares `SortedDictionary<K, V> where K: IComparable<K>` — "A
//     comparison-ordered associative collection: iteration visits entries in
//     ascending key order."
// So `T = Guid` satisfies the bound, and BOTH containers must iterate in
// §2.6's order.
//
// THE DEFECT. The compiler's sorted-collection comparator builder
// (`buildSortedKeyComparator`, src/codegen/LlvmBackend_Calls3.cpp) had no
// `Guid` branch: a `Guid` is stored INLINE as a 16-byte `{i64,i64}` struct
// (§2.6), so it matched none of that builder's scalar arms (integer / float)
// and it fell through to the "unsupported key type" arm. EVERY consumer of
// that comparator then silently degraded:
//   * `SortedSet<Guid>` / `SortedDictionary<Guid, V>` iteration fell back to
//     the UNSORTED insertion-order snapshot — §4.18's "iteration in ascending
//     order" and §4.17's "iteration visits entries in ascending key order"
//     both unmet, with NO diagnostic;
//   * `.First` / `.Last` / `.FirstKey` / `.LastKey` were REFUSED with a
//     diagnostic claiming `Guid` does not implement `IComparable<Guid>` —
//     which §2.6 says it DOES.
// (The sibling defect for `Array<Guid>.Sort()` / `Sorted()` was fixed
// separately by #8237; this is the same missing recognition on the sorted
// COLLECTIONS' side.)
//
// The rows below pin the ORDER, not merely the compilation:
//   * `order_is_over_the_canonical_bytes_not_a_word` uses a pair whose
//     canonical order and whose LOW-WORD order are OPPOSITE — a word-wise (or
//     i32-wise) comparator answers the reverse of §2.6.
//   * `bytes_compare_unsigned` pins `memcmp`'s unsigned byte comparison
//     (0x80 above 0x7f — the same rule the conformance corpus pins for
//     `Guid.Min`/`Max` in tests/conformance/rules/cases/l5_0062.bzl).
//   * `sorted_order_agrees_with_the_compare_to_member` pins that the order the
//     collection imposes IS the order `Guid.CompareTo` reports.
//
// A REFUSED program yields the harness's "<did-not-run>" sentinel, which no
// expected string matches — so a refusal is observed as a FAILURE of the
// positive rows.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
#include <string>

using bezel::test::runAotChecked;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out — the value a REFUSED program produces here,
// and the reason every positive row below fails before the fix.
std::string outChecked(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// `module M; class C public static method Main(): int
// var i := 0; while i < 100 do <body> i += 1; end; return 0; end Main; end C;`
//
// The 100× loop is the leak-amplification idiom (CLAUDE.md: "a leak repro
// that runs once can hide under the sanitizer's noise, so loop it 50–500×");
// it also makes the ORDERING assertion 100 observations rather than one.
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int "
           "var i := 0; while i < 100 do " + body + " i += 1; end;"
           " return 0; end Main; end C;";
}

// `s` repeated `n` times — the expected stdout of the looped program.
std::string rep(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

// The program's stdout, one entry per line, each terminated by a newline.
std::string lines(std::initializer_list<std::string> parts) {
    std::string s;
    for (const auto& p : parts) {
        s += p;
        s += "\n";
    }
    return s;
}

// The three canonical §2.6 renderings used by most rows, in ASCENDING order.
const char* kG1 = "00000001-0000-0000-0000-000000000000";
const char* kG2 = "11111111-1111-1111-1111-111111111111";
const char* kG3 = "22222222-2222-2222-2222-222222222222";

}  // namespace

// #8263 / stdlib §4.18 + §2.6: the issue's own repro, with FIXED Guid values
// (a random `NewGuid()` cannot pin an order). The set is filled in DESCENDING
// order, so the ascending iteration is a different sequence from the
// insertion order and the row fails before the fix.
BEZEL_TEST(sorted_guid_order_8263,
           issue_repro_sortedset_guid_iterates_in_ascending_guid_order) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var s := new SortedSet<Guid>(); "
            "s.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\")); "
            "s.Add(Guid.Parse(\"11111111-1111-1111-1111-111111111111\")); "
            "s.Add(Guid.Parse(\"00000001-0000-0000-0000-000000000000\")); "
            "for var g in s do Console.WriteLine(g); end;")),
        rep(lines({"00000001-0000-0000-0000-000000000000",
                   "11111111-1111-1111-1111-111111111111",
                   "22222222-2222-2222-2222-222222222222"}),
            100));
}

// #8263 / stdlib §4.17 + §2.6: the SortedDictionary TWIN of the row above.
// §4.17's promise is about the KEYS' order ("iteration visits entries in
// ascending key order"), so the row reads `kv.Key`.
BEZEL_TEST(sorted_guid_order_8263,
           sorteddictionary_guid_twin_iterates_in_ascending_key_order) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var d := new SortedDictionary<Guid, int>(); "
            "d.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\"), 2); "
            "d.Add(Guid.Parse(\"11111111-1111-1111-1111-111111111111\"), 1); "
            "d.Add(Guid.Parse(\"00000001-0000-0000-0000-000000000000\"), 0); "
            "for var kv in d do Console.WriteLine(kv.Key); end;")),
        rep(lines({"00000001-0000-0000-0000-000000000000",
                   "11111111-1111-1111-1111-111111111111",
                   "22222222-2222-2222-2222-222222222222"}),
            100));
}

// §2.6's order is over the 16 CANONICAL BYTES, and this pair separates it
// from every WORD-wise order:
//
//   A = 01000000-0000-0000-0000-000000000000  (byte 0 = 0x01, rest 0)
//   B = 00000000-0000-0000-0000-000000000001  (byte 15 = 0x01, rest 0)
//
// Canonically, A > B (byte 0 decides: 0x01 > 0x00), so the ascending order
// is B, A. But A's LOW WORD (bytes 0..7) is 1 and B's is 0 — so a comparator
// that read the low word (or the low i32) would answer A, B: the REVERSE.
BEZEL_TEST(sorted_guid_order_8263,
           order_is_over_the_canonical_bytes_not_a_word) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var s := new SortedSet<Guid>(); "
            "s.Add(Guid.Parse(\"01000000-0000-0000-0000-000000000000\")); "
            "s.Add(Guid.Parse(\"00000000-0000-0000-0000-000000000001\")); "
            "for var g in s do Console.WriteLine(g); end;")),
        rep(lines({"00000000-0000-0000-0000-000000000001",
                   "01000000-0000-0000-0000-000000000000"}),
            100));
}

// The canonical bytes are compared as UNSIGNED (the rule the conformance
// corpus pins for `Guid.Min`/`Max`, l5_0062): 0x80 sorts ABOVE 0x7f. A
// signed comparison of the first byte would put 0x80 first.
BEZEL_TEST(sorted_guid_order_8263, bytes_compare_unsigned) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var s := new SortedSet<Guid>(); "
            "s.Add(Guid.Parse(\"80000000-0000-0000-0000-000000000000\")); "
            "s.Add(Guid.Parse(\"7fffffff-ffff-ffff-ffff-ffffffffffff\")); "
            "for var g in s do Console.WriteLine(g); end;")),
        rep(lines({"7fffffff-ffff-ffff-ffff-ffffffffffff",
                   "80000000-0000-0000-0000-000000000000"}),
            100));
}

// The order the collection imposes IS the order `Guid.CompareTo` reports
// (§2.6: "ordering is CompareTo only"). `bz_guid_compare` is the very
// comparison `Guid.CompareTo` dispatches to, so the two cannot drift; this
// row pins it.
BEZEL_TEST(sorted_guid_order_8263,
           sorted_order_agrees_with_the_compare_to_member) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var s := new SortedSet<Guid>(); "
            "s.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\")); "
            "s.Add(Guid.Parse(\"11111111-1111-1111-1111-111111111111\")); "
            "s.Add(Guid.Parse(\"00000001-0000-0000-0000-000000000000\")); "
            "var first := s.First; var last := s.Last; "
            "Console.WriteLine(first.CompareTo(last));")),
        rep("-1\n", 100));
}

// §4.18's `.First` / `.Last` are the LEAST and GREATEST elements. Before the
// fix both were REFUSED with "`.First` on `SortedSet<T>` (stdlib §4.18)
// requires the element type to implement `IComparable<T>`; the element type
// does not" — a FALSE statement about `Guid`, which §2.6 declares
// `IComparable<Guid>`.
BEZEL_TEST(sorted_guid_order_8263, first_and_last_are_the_extremes) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var s := new SortedSet<Guid>(); "
            "s.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\")); "
            "s.Add(Guid.Parse(\"11111111-1111-1111-1111-111111111111\")); "
            "s.Add(Guid.Parse(\"00000001-0000-0000-0000-000000000000\")); "
            "Console.WriteLine(s.First); Console.WriteLine(s.Last);")),
        rep(lines({"00000001-0000-0000-0000-000000000000",
                   "22222222-2222-2222-2222-222222222222"}),
            100));
}

// §4.17's `.FirstKey` / `.LastKey` are the LEAST and GREATEST keys — the
// SortedDictionary twin of the row above, and REFUSED before the fix with the
// same false diagnostic.
BEZEL_TEST(sorted_guid_order_8263, firstkey_and_lastkey_are_the_extremes) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var d := new SortedDictionary<Guid, int>(); "
            "d.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\"), 2); "
            "d.Add(Guid.Parse(\"11111111-1111-1111-1111-111111111111\"), 1); "
            "d.Add(Guid.Parse(\"00000001-0000-0000-0000-000000000000\"), 0); "
            "Console.WriteLine(d.FirstKey); Console.WriteLine(d.LastKey);")),
        rep(lines({"00000001-0000-0000-0000-000000000000",
                   "22222222-2222-2222-2222-222222222222"}),
            100));
}

// §4.18's MEMBERSHIP is "`CompareTo`-equality" — "adding an equal element
// leaves the set unmodified". This row pins that the fix did not disturb the
// membership half: `Contains` finds a present element, `Remove` takes it out
// and reports the removal, and the count follows.
BEZEL_TEST(sorted_guid_order_8263,
           contains_and_remove_keep_compare_to_identity) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var s := new SortedSet<Guid>(); "
            "s.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\")); "
            "s.Add(Guid.Parse(\"11111111-1111-1111-1111-111111111111\")); "
            "Console.WriteLine(s.Contains(Guid.Parse(\"11111111-1111-1111-1111-111111111111\"))); "
            "Console.WriteLine(s.Remove(Guid.Parse(\"11111111-1111-1111-1111-111111111111\"))); "
            "Console.WriteLine(s.Count); "
            "Console.WriteLine(s.Contains(Guid.Parse(\"11111111-1111-1111-1111-111111111111\"))); "
            "Console.WriteLine(s.Remove(Guid.Parse(\"11111111-1111-1111-1111-111111111111\")));")),
        rep("True\nTrue\n1\nFalse\nFalse\n", 100));
}

// The §5 LINQ materializer is a DIFFERENT consumer of the same comparator
// (`sortedSetSnapshotToSortedArray`, src/codegen/LlvmBackend_Calls2.cpp —
// the sorted snapshot the §5 operator chain reads). It sorts the generic
// key table's snapshot with `bz_sort_keys_gen` + this comparator, so before
// the fix it too yielded the insertion order.
BEZEL_TEST(sorted_guid_order_8263,
           linq_snapshot_of_a_sortedset_guid_is_ascending) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var s := new SortedSet<Guid>(); "
            "s.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\")); "
            "s.Add(Guid.Parse(\"11111111-1111-1111-1111-111111111111\")); "
            "s.Add(Guid.Parse(\"00000001-0000-0000-0000-000000000000\")); "
            "var arr := s.ToArray(); "
            "for var g in arr do Console.WriteLine(g); end;")),
        rep(lines({"00000001-0000-0000-0000-000000000000",
                   "11111111-1111-1111-1111-111111111111",
                   "22222222-2222-2222-2222-222222222222"}),
            100));
}

// CONTROL — an `int` element type is unaffected by the fix: `SortedSet<int>`
// still iterates in ascending order.
BEZEL_TEST(sorted_guid_order_8263, control_sortedset_int_is_unchanged) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "using var s := new SortedSet<int>(); "
            "s.Add(3); s.Add(1); s.Add(2); "
            "for var x in s do Console.WriteLine(x); end;")),
        rep(lines({"1", "2", "3"}), 100));
}
