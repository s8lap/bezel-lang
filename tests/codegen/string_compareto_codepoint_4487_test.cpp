// Codegen — fix #4487 (stdlib §9.6 + spec §4.6, v0.306): ordinal string
// comparison walked the raw byte buffer using the CODE-POINT length as a byte
// count — the #535 defect class — so non-ASCII operands compared wrongly, and
// two DIFFERENT strings compared EQUAL. Sibling of #4483 on the ordering axis.
//
// THE CONTRACT. Spec §4.6 (v0.306): "`s < t`, `s > t`, `s <= t`, `s >= t`
// perform ordinal lexicographic comparison of the CODE-POINT sequence", and
// §4.6 makes the per-string 1/2/4-byte storage width invisible to the
// language, so no comparison result may depend on it. Stdlib §9.6 (v0.306):
// "All comparison and equality methods in this version are ordinal (code-
// point-based), including the `ignoreCase` overloads — case-insensitive
// comparison compares each code point's simple Unicode case-folded form, with
// no locale or culture-specific tailoring." Stdlib §2.8: `a.CompareTo(b)`
// returns "a negative number if a < b, zero if equal, a positive number if
// a > b" — the SIGN is specified, the magnitude is not, so every row below
// asserts a sign and never a magnitude.
//
// Those two sentences settle the ordering: it is by code-point VALUE. The
// three members and two operators this file covers — `CompareTo(other)`,
// `CompareTo(other, ignoreCase)`, and `<`/`<=`/`>`/`>=` — are therefore ONE
// relation, and the rows named `..._one_relation_..` assert that they agree.
//
// DEFECT 1 — the 2-arg form. `CompareTo(other, ignoreCase)` lowered to
// `bz_string_compare(loadStringData(self), loadStringLen(self), ...)`: the RAW
// BUFFER with the CODE-POINT count as a byte length. For a width-2 string that
// reads only the first `len` of the `len*width` data bytes. Measured on HEAD
// (4d9d186a) before the fix — each returned 0, i.e. EQUAL:
//
//     "日本".CompareTo("日x", true)   -> 0   (both read as bytes E5 65)
//     "š".CompareTo("a", true)        -> 0   (U+0161 vs 'a', byte 0x61 each)
//     "š".CompareTo("ɡ", true)        -> 0   (U+0161 vs U+0261, byte 0x61 each)
//
// Unlike #4483 there is NO UB here: that helper's `tolower` calls were on
// `(unsigned char)` values and so in-domain. Wrong values only.
//
// DEFECT 2 — the 1-arg form, and with it the ordering OPERATORS. The 1-arg
// `CompareTo` and `<`/`>` share `emitStringOrdCmp`, which took a
// `memcmp(dataL, dataR, minLen * width)` fast path whenever the two operands
// had the SAME storage width. At width 2/4 that orders by the LITTLE-ENDIAN
// storage bytes — low byte first — which is not code-point order. Measured on
// the same HEAD:
//
//     "日本".CompareTo("日x")  -> NEGATIVE. U+672C > U+0078, so it must be > 0.
//     "Āa" < "ĀĀ"              -> false  0x61 < 0x100, so it must be true.
//
// MAGNITUDE CAVEAT. Only the SIGN of the first is a reproducible measurement,
// and the sign is the whole defect. glibc's `memcmp` is free to return the
// byte difference or merely the sign, and the two measurements taken of this
// program DISAGREE: -76 on the box it was first reproduced on (which is the
// ANALYTIC difference 0x2C - 0x78, and is also the value issue #4487 quotes)
// and -1 on a reviewer's rebuild. Negative-where-§4.6-requires-positive holds
// on both. No row below asserts a magnitude — stdlib §2.8 specifies only the
// sign — and that is deliberate, not incidental.
//
// #1480's mixed-width tests kept passing because a width MISMATCH took the
// per-code-point slow path, which was correct; the same-width path was the
// broken one, and every same-width row in the suite differed at an ASCII
// position, where byte order and code-point order agree.
//
// THE FIX routes all three through one runtime helper, `bz_str_compare_cp`
// (bezel_runtime_strings.cpp), which decodes each operand at its OWN width via
// `bz_str_cp` — the width-aware reader the rest of the `bz_str_*` family uses
// — and folds `ignoreCase` through `bz_char_to_lower`, the SAME shared helper
// #4420 introduced and #4483 reused. `bz_string_compare` is deleted rather
// than left beside it: a second "ordinal string comparison" that disagrees
// with the first is the drift that caused #2590.
//
// ABOVE-ASCII FOLDING is unchanged and deliberately NOT widened here:
// `bz_char_to_lower` is the simple case fold within ASCII and identity above
// it, so two code points above ASCII compare equal under `ignoreCase` iff they
// ARE the same code point. #4481 owns widening that, exactly as #4483 recorded.
// The rows above that boundary (U+0161 vs U+0261) assert the identity answer.
//
// MOVED CALLERS. `bz_string_compare` had seven other call sites, every one of
// them asking this same question, and all seven moved. Two are covered by rows
// here because the move CHANGES THEIR ANSWER for non-ASCII input:
//
//   * SortedSet<string>'s sorted-snapshot comparator (LlvmBackend_Calls2.cpp,
//     #2550). Stdlib §4.18 defines its order as the element type's own
//     `CompareTo`, so it must be this relation. Measured pre-fix: a set given
//     "日本", "日x", "日a" enumerated them in INSERTION order, because the
//     comparator answered 0 for all three pairs.
//   * The §9.8 wire-name dispatch in LlvmBackend_Serialization.cpp (six sites:
//     record, class and enum deserialization, the choice discriminator AND its
//     payload field names, and the polymorphic discriminator). All six are
//     EQUALITY tests (`cmp == 0`) with `ignoreCase` 0 — §9.8: "Names are
//     compared ordinally" — and `bz_binder_read_string` decodes the wire's
//     UTF-8 into a width-aware string, so both operands really can be width
//     2/4. Measured pre-fix: an enum with `serialized "日本"` and `serialized
//     "日x"` round-tripped the SECOND constant back as the FIRST.
//
// WHAT DID NOT MOVE — TWO byte-wise ordinal string comparators survive this
// commit, and neither ever called `bz_string_compare`, so neither is reachable
// from this change. Both are pre-existing and out of scope; they are named
// here because after this commit they DISAGREE with `CompareTo`, and a reader
// who finds that disagreement must be able to see it was known:
//
//   * `Array<string>.Sort()` / `.Sorted()` build the codegen thunk
//     `bz$strcmp` (LlvmBackend_Calls.cpp:~4903 and ~5178), which loads each
//     element's data pointer and calls plain `strcmp` — byte-wise AND
//     NUL-terminated. Measured on the same three strings, and IDENTICAL on
//     this commit's parent and on this commit (the file is untouched by it):
//
//         Array<string>.Sort()   ->  日本,日a,日x   (unchanged, still wrong)
//         SortedSet<string>      ->  日a,日x,日本   (fixed here)
//         "日a".CompareTo("日x") ->  negative       (fixed here)
//
//     So before this commit all three were wrong together; after it one is
//     right and `bz$strcmp` is still wrong, which makes the disagreement newly
//     OBSERVABLE. That is a real consequence of fixing one of them, and the
//     sibling issue tracks `bz$strcmp`.
//   * `SortedDictionary<string,V>`'s key order runs on `bz_sort_kv_str`
//     (strcmp over the width-1 data buffer). Its non-ASCII behaviour is worse
//     than a wrong order — distinct keys collapse — and #4493 tracks it.
//
// Nothing here changes either one.
//
// Incidental, reported not asserted: no row below asserts a leak count. The
// disposal gaps in this area belong to #4482, and coupling a value oracle to a
// leak count would make this test fail for something it does not diagnose.
//
// No C++ line comment appears inside any Bezel source literal below: one would
// comment out the rest of the program and the row would pass for the wrong
// reason.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotOut — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Every row returns a small SIGN code, never a comparison result: runAot's
// exitCode is WEXITSTATUS-masked to 8 bits, so a NEGATIVE result could not
// survive the channel at all. stdlib §2.8 specifies the sign
// and not the magnitude, so the sign is also the whole of what may be asserted.
//
//   1 = negative   2 = zero   3 = positive
constexpr int kNeg = 1, kZero = 2, kPos = 3;

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// `body` computes `var r: int := <comparison>;`.
std::string signOf(const std::string& body) {
    return "module M; class C public static method Main(): int "
           + body +
           "  return (if r < 0 then 1 else (if r = 0 then 2 else 3)); "
           "end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// The 2-arg form, `CompareTo(other, ignoreCase)` — the rows that FAIL pre-fix.
// Each pair differs only in code points whose STORAGE BYTES the byte-over-
// code-point-length walk never reached, so each answered 0 (EQUAL) pre-fix.
// ---------------------------------------------------------------------------

// U+672C (本) vs U+0078 (x) at position 1, both operands width 2. The walk
// compared 2 BYTES (`E5 65` on each side — the shared U+65E5) and then tied on
// length. 本 > x, so the answer is positive. Pre-fix: 0.
BEZEL_TEST(string_compareto_codepoint_4487, ic_cjk_low_byte_collision_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"日本\".CompareTo(\"日x\", ignoreCase := true); ")), kPos);
}

// U+0161 (š, width 2) vs 'a' (U+0061, width 1). One byte was compared on each
// side and both were 0x61. U+0161 > U+0061, so positive. Pre-fix: 0.
BEZEL_TEST(string_compareto_codepoint_4487, ic_latin_ext_vs_ascii_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"š\".CompareTo(\"a\", ignoreCase := true); ")), kPos);
}

// U+0161 (š) vs U+0261 (ɡ) — both ABOVE U+00FF and sharing low byte 0x61, so
// this row isolates the byte walk from any ASCII interaction. Above ASCII the
// simple fold is identity (#4420; #4481 owns widening it), so the two fold to
// themselves and U+0161 < U+0261 — negative. Pre-fix: 0.
BEZEL_TEST(string_compareto_codepoint_4487, ic_two_above_ascii_sharing_low_byte_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"š\".CompareTo(\"ɡ\", ignoreCase := true); ")), kNeg);
}

// U+1F600 (😀, width 4) vs U+0100 (Ā, width 2): the widest storage width, and
// the shared low byte is NUL. U+1F600 > U+0100 — positive.
BEZEL_TEST(string_compareto_codepoint_4487, ic_astral_vs_bmp_low_byte_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"\U0001f600\".CompareTo(\"Ā\", ignoreCase := true); ")), kPos);
}

// All four collisions in one program, so a partial fix that repairs one
// storage width but not another shows as a specific value rather than a bare
// red. Each correctly-unequal pair contributes its bit; 15 = all correct,
// 0 = the pre-fix behaviour (every pair wrongly equal).
BEZEL_TEST(string_compareto_codepoint_4487, ic_collision_rows_together_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int := \"日本\".CompareTo(\"日x\", ignoreCase := true); "
        "  var b: int := \"š\".CompareTo(\"a\", ignoreCase := true); "
        "  var c: int := \"š\".CompareTo(\"ɡ\", ignoreCase := true); "
        "  var d: int := \"\U0001f600\".CompareTo(\"Ā\", ignoreCase := true); "
        "  return (if a > 0 then 1 else 0) + (if b > 0 then 2 else 0) + "
        "         (if c < 0 then 4 else 0) + (if d > 0 then 8 else 0); "
        "end Main; end C;"), 15);
}

// ---------------------------------------------------------------------------
// The 2-arg form — rows that answer the same on BOTH sides. They pin that the
// fix changed nothing it should not have.
// ---------------------------------------------------------------------------

// Equal non-ASCII strings compare equal. Pre-fix this also answered 0, because
// both operands were read identically — the reassuring probe passes on the
// broken code, which is exactly why the defect survived.
BEZEL_TEST(string_compareto_codepoint_4487, ic_equal_non_ascii_is_zero_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"日本\".CompareTo(\"日本\", ignoreCase := true); ")), kZero);
}

// Equal prefix, different length: the tiebreak is on length in CODE POINTS.
// The issue records this one as "happens to be right" pre-fix.
BEZEL_TEST(string_compareto_codepoint_4487, ic_equal_prefix_shorter_is_less_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"日本\".CompareTo(\"日本z\", ignoreCase := true); ")), kNeg);
}

// §9.6: the fold makes case-differing ASCII equal.
BEZEL_TEST(string_compareto_codepoint_4487, ic_ascii_case_folds_equal_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"ABC\".CompareTo(\"abc\", ignoreCase := true); ")), kZero);
}

// §9.6: folding does not collapse genuinely different letters — 'c' folds to
// 'c' and precedes 'd'. Both directions.
BEZEL_TEST(string_compareto_codepoint_4487, ic_ascii_orders_on_folded_code_points_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"ABC\".CompareTo(\"abd\", ignoreCase := true); ")), kNeg);
}

BEZEL_TEST(string_compareto_codepoint_4487, ic_ascii_orders_on_folded_code_points_reverse_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"abd\".CompareTo(\"ABC\", ignoreCase := true); ")), kPos);
}

// §9.6 with ignoreCase := false: plain ordinal, 'A' (65) < 'a' (97).
BEZEL_TEST(string_compareto_codepoint_4487, ic_false_is_plain_ordinal_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"ABC\".CompareTo(\"abc\", ignoreCase := false); ")), kNeg);
}

// ---------------------------------------------------------------------------
// The 1-arg form, `CompareTo(other)` — `emitStringOrdCmp`'s same-width path.
// ---------------------------------------------------------------------------

// The issue's headline 1-arg shape. Pre-fix the answer was NEGATIVE, ordered
// by the THIRD DATA BYTE under little-endian width-2 storage (0x2C against
// 0x78). By code point it is U+672C against U+0078, so it must be POSITIVE.
// The sign is what this row asserts and what reproduces; see the MAGNITUDE
// CAVEAT in the header for why no magnitude is asserted anywhere.
BEZEL_TEST(string_compareto_codepoint_4487, ord_cjk_little_endian_byte_order_spec_4_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"日本\".CompareTo(\"日x\"); ")), kPos);
}

// Both operands width 2 and differing at position 1: 'a' (U+0061) against
// U+0100 (Ā). Storage bytes `61 00` against `00 01` order the wrong way round;
// by code point 0x61 < 0x100, so the answer is negative. Pre-fix: positive.
BEZEL_TEST(string_compareto_codepoint_4487, ord_same_width_ascii_vs_bmp_spec_4_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"Āa\".CompareTo(\"ĀĀ\"); ")), kNeg);
}

// Antisymmetry on the same pair: swapping the operands flips the sign.
BEZEL_TEST(string_compareto_codepoint_4487, ord_same_width_antisymmetric_spec_2_8) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"ĀĀ\".CompareTo(\"Āa\"); ")), kPos);
}

// Width 1 against width 4 at the same position: 'z' (U+007A) vs U+10000.
// A MIXED-width pair, which took the correct per-code-point path pre-fix too —
// here to pin that the path #1480 added still answers as it did.
BEZEL_TEST(string_compareto_codepoint_4487, ord_mixed_width_unchanged_spec_4_6) {
    BEZEL_EXPECT_EQ(run(signOf(
        "  var r: int := \"z\".CompareTo(\"\U00010000\"); ")), kNeg);
}

// ASCII controls for the 1-arg form: equal, less, greater.
BEZEL_TEST(string_compareto_codepoint_4487, ord_ascii_equal_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf("  var r: int := \"abc\".CompareTo(\"abc\"); ")), kZero);
}

BEZEL_TEST(string_compareto_codepoint_4487, ord_ascii_less_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf("  var r: int := \"abc\".CompareTo(\"abd\"); ")), kNeg);
}

BEZEL_TEST(string_compareto_codepoint_4487, ord_ascii_greater_spec_9_6) {
    BEZEL_EXPECT_EQ(run(signOf("  var r: int := \"abd\".CompareTo(\"abc\"); ")), kPos);
}

// ASCII CONTROL characters, the low end of the code space, where the ordering
// is ordinal by code point and nothing about the fix may move it: U+0001 <
// U+0002, U+001F < ' ' (U+0020), and a tab (U+0009) below a newline (U+000A).
// All operands are width 1, so this is exactly the memcmp fast path the fix
// replaced — the row proves that replacing it left the width-1 answers alone.
BEZEL_TEST(string_compareto_codepoint_4487, ord_ascii_control_chars_unchanged_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int := \"A\\u0001\".CompareTo(\"A\\u0002\"); "
        "  var b: int := \"\\u001F\".CompareTo(\" \"); "
        "  var c: int := \"x\\tz\".CompareTo(\"x\\nz\"); "
        "  var d: int := \"A\\u0001\".CompareTo(\"A\\u0001\"); "
        "  return (if a < 0 then 1 else 0) + (if b < 0 then 2 else 0) + "
        "         (if c < 0 then 4 else 0) + (if d = 0 then 8 else 0); "
        "end Main; end C;"), 15);
}

// TRANSITIVITY across three code points that the byte walk collapsed onto one
// equivalence class: U+0061 ('a') < U+0078 ('x') < U+672C (本), each behind the
// shared U+65E5 prefix. Pre-fix all three comparisons answered 0, so the
// relation was not even a strict order on this set; post-fix the three
// pairwise answers are consistent and the transitive one follows.
BEZEL_TEST(string_compareto_codepoint_4487, ord_transitivity_over_code_points_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var p: string := \"日a\"; var q: string := \"日x\"; var s: string := \"日本\"; "
        "  var pq: int := p.CompareTo(q); "
        "  var qs: int := q.CompareTo(s); "
        "  var ps: int := p.CompareTo(s); "
        "  var sp: int := s.CompareTo(p); "
        "  return (if pq < 0 then 1 else 0) + (if qs < 0 then 2 else 0) + "
        "         (if ps < 0 then 4 else 0) + (if sp > 0 then 8 else 0); "
        "end Main; end C;"), 15);
}

// ---------------------------------------------------------------------------
// The §4.6 ordering OPERATORS. They share `emitStringOrdCmp` with the 1-arg
// member, so the same defect reached `<`/`<=`/`>`/`>=` — and spec §4.6 gives
// them the same relation, so they must agree with `CompareTo`.
// ---------------------------------------------------------------------------

// Pre-fix `false`: the little-endian bytes ordered "ĀĀ" below "Āa".
BEZEL_TEST(string_compareto_codepoint_4487, op_lt_same_width_ascii_vs_bmp_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string := \"Āa\"; var b: string := \"ĀĀ\"; "
        "  return (if a < b then 1 else 0); "
        "end Main; end C;"), 1);
}

// Pre-fix `false`: the two compared byte-equal over the code-point length.
BEZEL_TEST(string_compareto_codepoint_4487, op_lt_cjk_shared_prefix_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string := \"日x\"; var b: string := \"日本\"; "
        "  return (if a < b then 1 else 0); "
        "end Main; end C;"), 1);
}

// ASCII control: `<` on width-1 operands is unchanged.
BEZEL_TEST(string_compareto_codepoint_4487, op_lt_ascii_unchanged_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string := \"abc\"; var b: string := \"abd\"; "
        "  return (if a < b then 1 else 0) + (if b > a then 2 else 0) + "
        "         (if a <= a then 4 else 0) + (if a >= a then 8 else 0); "
        "end Main; end C;"), 15);
}

// ONE RELATION: `s.CompareTo(t) < 0` and `s < t` must agree, on a pair where
// both answered the WRONG way pre-fix (so the row is not vacuous — it would
// have passed pre-fix on agreement alone, which is why it also asserts the
// direction). Spec §4.6 and stdlib §9.6 define the two as the same ordinal
// comparison, and after this fix they are one runtime call.
BEZEL_TEST(string_compareto_codepoint_4487, op_and_member_are_one_relation_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := \"日x\"; var t: string := \"日本\"; "
        "  var m: bool := s.CompareTo(t) < 0; "
        "  var o: bool := s < t; "
        "  return (if m = o then 1 else 0) + (if m then 2 else 0); "
        "end Main; end C;"), 3);
}

// ONE RELATION across the arities: with `ignoreCase := false` the 2-arg form
// is the 1-arg form. Asserted on the same non-ASCII pair, where both were
// wrong pre-fix in DIFFERENT ways (ZERO from the 2-arg byte walk, NEGATIVE
// from the 1-arg memcmp) — so pre-fix they disagreed outright.
BEZEL_TEST(string_compareto_codepoint_4487, arities_agree_when_not_folding_spec_9_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string := \"日本\"; var t: string := \"日x\"; "
        "  var one: int := s.CompareTo(t); "
        "  var two: int := s.CompareTo(t, ignoreCase := false); "
        "  return (if (one > 0) = (two > 0) then 1 else 0) + (if one > 0 then 2 else 0); "
        "end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// The MOVED CALLERS. Both rows answered wrongly pre-fix, so each is evidence
// that its caller really is the same defect and really did move.
// ---------------------------------------------------------------------------

// stdlib §4.18: `SortedSet<T>` iterates in ascending order, and that order is
// the element type's own `CompareTo` — for `string`, §9.6's ordinal code-point
// relation. The three elements share the U+65E5 prefix and differ at position
// 1 by U+0061 < U+0078 < U+672C. Measured pre-fix: `日本,日x,日a,` — the
// INSERTION order, because the snapshot comparator answered 0 for every pair.
BEZEL_TEST(string_compareto_codepoint_4487, sortedset_string_non_ascii_ascending_spec_4_18) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; class C public static method Main(): int "
            "  using var s := new SortedSet<string>(); "
            "  s.Add(\"日本\"); s.Add(\"日x\"); s.Add(\"日a\"); "
            "  using var a := new Array<string>(); "
            "  using var t := a.Concat(s).ToArray(); "
            "  var acc: string := \"\"; "
            "  for var x in t do acc := acc + x + \",\"; end; "
            "  Console.WriteLine($\"ss={acc}\"); "
            "  return 0; "
            "end Main; end C;"),
        "ss=日a,日x,日本,");
}

// The same set in ASCII: a control that the ordering the fix installs is the
// one this path already produced for ASCII elements.
BEZEL_TEST(string_compareto_codepoint_4487, sortedset_string_ascii_unchanged_spec_4_18) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; class C public static method Main(): int "
            "  using var s := new SortedSet<string>(); "
            "  s.Add(\"pear\"); s.Add(\"apple\"); s.Add(\"fig\"); "
            "  using var a := new Array<string>(); "
            "  using var t := a.Concat(s).ToArray(); "
            "  var acc: string := \"\"; "
            "  for var x in t do acc := acc + x + \",\"; end; "
            "  Console.WriteLine($\"ss={acc}\"); "
            "  return 0; "
            "end Main; end C;"),
        "ss=apple,fig,pear,");
}

// Spec §9.8 / stdlib §2.15: an enum serializes by wire NAME, and the names are
// compared ORDINALLY on the read side. `serialized "日本"` and `serialized
// "日x"` are two distinct names that share their leading storage bytes, so the
// byte walk matched the wrong one: measured pre-fix, serializing `E.Beta` and
// deserializing returned `E.Alpha`.
BEZEL_TEST(string_compareto_codepoint_4487, enum_deserialize_non_ascii_wire_name_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "enum E: ISerializable, IDeserializable "
        "  Alpha serialized \"日本\", Beta serialized \"日x\"; end E; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  E.Beta.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := E.Deserialize(d); "
        "  return (if v = E.Beta then 1 else 0); "
        "end Main; end C;"), 1);
}

// The same round-trip with ASCII wire names: a control that the five moved
// §9.8 dispatch sites still match the names every existing test writes.
BEZEL_TEST(string_compareto_codepoint_4487, enum_deserialize_ascii_wire_name_unchanged_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "enum E: ISerializable, IDeserializable "
        "  Alpha serialized \"alpha\", Beta serialized \"beta\"; end E; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  E.Beta.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var v := E.Deserialize(d); "
        "  return (if v = E.Beta then 1 else 0); "
        "end Main; end C;"), 1);
}
