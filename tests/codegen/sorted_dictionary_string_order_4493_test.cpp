// Codegen — fix #4493 (stdlib §4.17 + §9.6, v0.338; spec §4.6).
//
// THE CONTRACT. Stdlib §4.17 (v0.338): "Ordering is defined by `K`'s
// `IComparable<K>.CompareTo`" and iteration "visits entries in ascending key
// order". Stdlib §9.6 (v0.338): "All comparison and equality methods in this
// version are ordinal (code-point-based)". So a SortedDictionary<string,V>
// must enumerate, and answer FirstKey/LastKey, by CODE-POINT order, the same
// relation SortedSet<string> already uses (#4487).
//
// THE DEFECT. The sorted-snapshot comparator `bz_kv_idx_cmp_str`
// (bezel_runtime_collections.cpp) and the `bz_dict_min_key_str` /
// `bz_dict_max_key_str` scans used byte-wise `strcmp` over the raw key
// buffers. A width>1 key's storage bytes are little-endian code units, so the
// order followed the LOW byte first, and a zero byte ended the comparison
// early. Measured on origin/main e1ba6b44 (issue #4493 triage):
//   `é`, `Ā`, `€`, `日` inserted into SortedDictionary<string,int> enumerated
//   as `Ā,€,日,é` (SortedSet<string> gives `é,Ā,€,日`).
//
// ORACLE. Every row asserts stdout (the observable is printed, never an exit
// code, since the exit status is 8-bit). Every row runs under runAotChecked,
// so generated-code ASan/LSan is active and a memory error fails the row, and
// BEZEL_EXPECT_TRUE(r.ran) keeps a non-run from passing as an empty string.
//
// FAIL-WITHOUT (MEASURED). The fixed runtime file was copied aside,
// `git checkout origin/main -- src/runtime/bezel_runtime_collections.cpp` was
// run, the codegen binary rebuilt and this filter run: 2 rows FAIL (the
// enumeration row and the astral/BMP enumeration row), 5 pass. Each row says
// whether it passes on origin/main.
//
// Keys are chosen so storage width differs across the set (width 1 for é,
// width 2 for Ā/€/日/U+FF5E, width 4 for U+1F600), so the mixed-width compare
// is what is exercised. Byte order and code-point order disagree for them.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

std::string wrap(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "end Main; end C;";
}

// runAotChecked's stdout with the single trailing newline removed. `ran` is
// reported to the caller, which asserts it with BEZEL_EXPECT_TRUE, so a
// program that did not run fails the row rather than comparing a sentinel.
std::string checkedOut(const std::string& program, bool& ran) {
    auto r = runAotChecked(program);
    ran = r.ran;
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// U+1F600 (GRINNING FACE), a 4-byte UTF-8 sequence.
const char* kEmoji = "\xF0\x9F\x98\x80";
// U+FF5E (FULLWIDTH TILDE), a 3-byte UTF-8 sequence. In UTF-16 this unit
// (0xFF5E) sorts ABOVE the surrogate lead 0xD83D of U+1F600; in code-point
// order U+FF5E sorts BELOW U+1F600. The row below pins the code-point answer.
const char* kFullwidthTilde = "\xEF\xBD\x9E";

}  // namespace

// ---------------------------------------------------------------------------
// Enumeration order for the non-ASCII set from the issue (stdlib §4.17 / §9.6).
// Insertion order is deliberately NOT code-point order.
// Post-fix: "é,Ā,€,日,". FAILS on origin/main (fail-without run, MEASURED).
// The issue's triage QUOTES the pre-fix enumeration as "Ā,€,日,é,".
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dictionary_string_order_4493,
           sorted_dictionary_enumeration_codepoint_order_spec_4_17_stdlib_9_6) {
    bool ran = false;
    std::string out = checkedOut(wrap(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"日\"] := 1; d[\"é\"] := 2; d[\"€\"] := 3; d[\"Ā\"] := 4; "
        "  var acc: string := \"\"; "
        "  for var e in d do acc := acc + e.Key + \",\"; end; "
        "  Console.WriteLine($\"sd={acc}\"); "
        "  return 0; "), ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(out, "sd=é,Ā,€,日,");
}

// ---------------------------------------------------------------------------
// The same set through SortedSet<string> (stdlib §4.18, #4487): the two
// sorted collections must agree on one relation. Post-fix: "ss=é,Ā,€,日,".
// PASSES on origin/main too (MEASURED): SortedSet<string> already uses
// bz_str_compare_cp (#4487), so this row is the control the issue names.
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dictionary_string_order_4493,
           sortedset_agrees_with_sorteddictionary_spec_4_18_stdlib_9_6) {
    bool ran = false;
    std::string out = checkedOut(wrap(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"日\"); s.Add(\"é\"); s.Add(\"€\"); s.Add(\"Ā\"); "
        "  var acc: string := \"\"; "
        "  for var x in s do acc := acc + x + \",\"; end; "
        "  Console.WriteLine($\"ss={acc}\"); "
        "  return 0; "), ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(out, "ss=é,Ā,€,日,");
}

// ---------------------------------------------------------------------------
// Lookups on the same non-ASCII keys AFTER the order change (stdlib §4.17:
// point operations use CompareTo-equality). A comparator that ordered
// inconsistently with the stored snapshot would miss these. PASSES on
// origin/main too (MEASURED): point lookups use bz_key_eq, already code-point,
// so this row guards the lookup/order pairing, not the order itself.
// Post-fix: "lookup=1234 rm=1 gone=0 n=3".
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dictionary_string_order_4493,
           sorted_dictionary_lookup_after_order_change_spec_4_17) {
    bool ran = false;
    std::string out = checkedOut(wrap(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"日\"] := 4; d[\"é\"] := 1; d[\"€\"] := 3; d[\"Ā\"] := 2; "
        "  var a := d[\"é\"]; var b := d[\"Ā\"]; "
        "  var c := d[\"€\"]; var e := d[\"日\"]; "
        "  var r := (if d.Remove(\"€\") then 1 else 0); "
        "  var g := (if d.ContainsKey(\"€\") then 1 else 0); "
        "  Console.WriteLine($\"lookup={a}{b}{c}{e} rm={r} gone={g} n={d.Count}\"); "
        "  return 0; "), ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(out, "lookup=1234 rm=1 gone=0 n=3");
}

// ---------------------------------------------------------------------------
// Surrogate-range key vs a BMP key above it (stdlib §4.17 / §9.6). Inserted as
// U+1F600, U+FF5E, "a". Code-point order is a < U+FF5E < U+1F600.
// A UTF-16 code-unit order would put U+1F600 (lead 0xD83D) below U+FF5E.
// HYPOTHESIS for the byte-order cause: little-endian width-4 storage puts a
// zero low byte first for U+1F600, so strcmp ranks it first. FAILS on
// origin/main (fail-without run, MEASURED); the exact pre-fix string was not
// captured. Post-fix: "sd=a,～,😀, first=a last=😀".
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dictionary_string_order_4493,
           sorted_dictionary_astral_vs_bmp_codepoint_order_spec_4_6_stdlib_9_6) {
    bool ran = false;
    std::string out = checkedOut(wrap(
        std::string("  using var d := new SortedDictionary<string,int>(); "
        "  d[\"") + kEmoji + "\"] := 1; d[\"" + kFullwidthTilde +
        "\"] := 2; d[\"a\"] := 3; "
        "  var acc: string := \"\"; "
        "  for var e in d do acc := acc + e.Key + \",\"; end; "
        "  var f := d.FirstKey; var l := d.LastKey; "
        "  Console.WriteLine($\"sd={acc} first={f} last={l}\"); "
        "  return 0; "), ran);
    BEZEL_EXPECT_TRUE(ran);
    std::string expect = std::string("sd=a,") + kFullwidthTilde + "," + kEmoji +
                         ", first=a last=" + kEmoji;
    BEZEL_EXPECT_EQ(out, expect);
}

// ---------------------------------------------------------------------------
// The same astral/BMP pair through SortedSet<string> (stdlib §4.18): must give
// the identical order. Post-fix: "ss=a,～,😀,". PASSES on origin/main too
// (MEASURED): SortedSet<string> already orders by bz_str_compare_cp (#4487).
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dictionary_string_order_4493,
           sortedset_astral_vs_bmp_agrees_spec_4_18_stdlib_9_6) {
    bool ran = false;
    std::string out = checkedOut(wrap(
        std::string("  using var s := new SortedSet<string>(); "
        "  s.Add(\"") + kEmoji + "\"); s.Add(\"" + kFullwidthTilde +
        "\"); s.Add(\"a\"); "
        "  var acc: string := \"\"; "
        "  for var x in s do acc := acc + x + \",\"; end; "
        "  Console.WriteLine($\"ss={acc}\"); "
        "  return 0; "), ran);
    BEZEL_EXPECT_TRUE(ran);
    std::string expect = std::string("ss=a,") + kFullwidthTilde + "," + kEmoji + ",";
    BEZEL_EXPECT_EQ(out, expect);
}

// ---------------------------------------------------------------------------
// Lookups of the astral and BMP keys after the change (stdlib §4.17 point
// operations). Post-fix: "found=12 n=2 rm=1". Guards that a key whose storage
// bytes contain zero high bytes is still found. PASSES on origin/main too
// (MEASURED): point lookup is bz_key_eq (code-point), so this guards the pairing.
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dictionary_string_order_4493,
           sorted_dictionary_astral_lookup_after_order_change_spec_4_17) {
    bool ran = false;
    std::string out = checkedOut(wrap(
        std::string("  using var d := new SortedDictionary<string,int>(); "
        "  d[\"") + kEmoji + "\"] := 1; d[\"" + kFullwidthTilde +
        "\"] := 2; d[\"a\"] := 3; "
        "  var x := d[\"" + kEmoji + "\"]; var y := d[\"" + kFullwidthTilde + "\"]; "
        "  var r := (if d.Remove(\"a\") then 1 else 0); "
        "  Console.WriteLine($\"found={x}{y} n={d.Count} rm={r}\"); "
        "  return 0; "), ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(out, "found=12 n=2 rm=1");
}

// ---------------------------------------------------------------------------
// ASCII control (stdlib §4.17). Byte order and code-point order agree for
// width-1 keys, so this row passes on origin/main as well; it guards that the
// fix did not disturb the ASCII path. Expected "a,b,c," before and after.
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dictionary_string_order_4493,
           sorted_dictionary_ascii_control_spec_4_17_stdlib_9_6) {
    bool ran = false;
    std::string out = checkedOut(wrap(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"c\"] := 3; d[\"a\"] := 1; d[\"b\"] := 2; "
        "  var acc: string := \"\"; "
        "  for var e in d do acc := acc + e.Key + \",\"; end; "
        "  Console.WriteLine($\"sd={acc}\"); "
        "  return 0; "), ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(out, "sd=a,b,c,");
}
