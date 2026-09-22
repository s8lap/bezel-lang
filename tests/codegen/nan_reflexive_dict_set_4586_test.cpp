// fix #4586 / spec §4.3.1 / §8.3 / §8.4, stdlib §9.2, v0.306.
//
// §4.3.1 makes `=` (and therefore `Equals`, §8.4) reflexive for floats: "NaN
// = NaN is true", and `-0.0 = +0.0` is true. The rationale names the reason
// explicitly: "Equality is the operation used by hash tables, sets, and
// `case` patterns; programmers expect a value to be equal to itself, and an
// irreflexive `=` makes `Set<double>` and `Dictionary<double, V>` misbehave
// when NaN is inserted." (Bezel_Design_Rationale.md, the §4.3.1 entry.)
//
// `double`/`float` are GENERIC-DICT-BACKED-ELIGIBLE keys (unlike the
// linear-scan `Array`/`LinkedList`/`Span` family #4573 already fixed), so
// `Set<double>`/`Dictionary<double, V>` route through the hash-table
// backing's per-T trampolines (buildValueEqTrampoline / buildValueHashCode,
// LlvmBackend_Calls3.cpp / emitValueHashCode, LlvmBackend_String.cpp) rather
// than the array-scan funnel #4573 unified. Before this fix:
//   - buildValueEqTrampoline's float arm used raw `FCmpOEQ` (strict IEEE),
//     so a bucket-resident NaN never compared equal to a lookup NaN — a
//     `Contains`/`ContainsKey` miss even when both hash to the same bucket.
//   - emitValueHashCode's float arm hashed the RAW bit pattern truncated to
//     the low 32 bits, so two NaNs with different payloads (or a genuinely
//     adversarial pair, since the low-32-bit truncation happens to leave
//     +0.0/-0.0 hashing alike by accident) could still land in different
//     buckets and never reach the (also broken) eq check at all.
// Both halves are fixed together: the eq trampoline gets the same
// oeq-or-both-NaN reflexive rule as the scalar `=` operator (fix #1165's
// pattern, LlvmBackend_Exprs.cpp / emitValueEquality), and the hash
// trampoline canonicalizes every NaN payload to one bit pattern and every
// zero to +0.0 before hashing, so §8.3's `a.Equals(b) => a.GetHashCode() =
// b.GetHashCode()` invariant holds for the reflexive rule.
//
// `Double.IeeeEquals`/`BitwiseEquals` (stdlib §9.2) are a DIFFERENT code path
// (LlvmBackend_Calls2.cpp) and are deliberately untouched by this fix — they
// stay strict IEEE / bit-exact, which is their whole reason to exist as an
// escape hatch from the reflexive default. A control test below pins that.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// --- Set<double> ---------------------------------------------------------

// Issue #4586's own repro: two NaNs computed the same way (0.0/0.0) plus a
// distinct ordinary value. Pre-fix this was 3 (every NaN treated as unique).
BEZEL_TEST(nan_reflexive_dict_set_4586, set_double_nan_dedup_count_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run(
        "var n1: double := 0.0 / 0.0; var n2: double := 0.0 / 0.0; "
        "using var s := new Set<double>(); "
        "s.Add(n1); s.Add(n2); s.Add(1.0); "
        "return s.Count;"), 2);
}

// The identity-blind oracle: insert a NaN with ONE bit pattern, look up with
// a NaN of a GENUINELY DIFFERENT bit pattern (BitConverter round-trip, stdlib
// §2.12) — so a pass here cannot be explained by the two operands being the
// same object/bits, only by the reflexive rule actually being applied by
// BOTH the hash trampoline (to land the lookup in the right bucket) and the
// equality trampoline (to match once there). BitwiseEquals confirms the two
// payloads really do differ.
BEZEL_TEST(nan_reflexive_dict_set_4586,
           set_double_contains_finds_differently_payloaded_nan_spec_4_3_1_8_3) {
    BEZEL_EXPECT_EQ(run(
        "var n1: double := BitConverter.Int64BitsToDouble(0x7FF8000000000001L); "
        "var n2: double := BitConverter.Int64BitsToDouble(0x7FFF800000000002L); "
        "if Double.BitwiseEquals(n1, n2) then return 97; end; "  // sanity: distinct
        "using var s := new Set<double>(); "
        "s.Add(n1); "
        "return int(s.Contains(n2));"), 1);
}

// -0.0 and +0.0 are equal per §4.3.1 and must collapse to ONE set entry, and
// a lookup with either sign must find it (the hash must agree with `=` here
// too, or a signed-zero key becomes a silent lookup miss).
BEZEL_TEST(nan_reflexive_dict_set_4586, set_double_signed_zero_collapses_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run(
        "var zp: double := 0.0; var zn: double := -0.0; "
        "using var s := new Set<double>(); "
        "s.Add(zp); s.Add(zn); "
        "return s.Count * 10 + int(s.Contains(zn)) * 3 + int(s.Contains(zp));"),
        14);  // Count=1, Contains(zn)=true, Contains(zp)=true -> 10+3+1=14
}

// Control: ordinary (non-NaN, non-zero) doubles are unaffected — only exact
// duplicates collapse, distinct values remain distinct.
BEZEL_TEST(nan_reflexive_dict_set_4586, set_double_control_ordinary_values_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "using var s := new Set<double>(); "
        "s.Add(1.0); s.Add(2.0); s.Add(1.0); s.Add(3.5); "
        "return s.Count;"), 3);
}

// --- Dictionary<double, V> ------------------------------------------------

// Issue #4586's own repro for the dictionary half.
BEZEL_TEST(nan_reflexive_dict_set_4586, dictionary_double_containskey_nan_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run(
        "var n1: double := 0.0 / 0.0; var n2: double := 0.0 / 0.0; "
        "using var d := new Dictionary<double, int>(); "
        "d[n1] := 5; "
        "return int(d.ContainsKey(n2));"), 1);
}

// Same identity-blind oracle as the Set test above, plus a value round-trip
// through the indexer getter (not just ContainsKey) with the differently-
// payloaded NaN as the lookup key.
BEZEL_TEST(nan_reflexive_dict_set_4586,
           dictionary_double_lookup_finds_differently_payloaded_nan_spec_4_3_1_8_3) {
    BEZEL_EXPECT_EQ(run(
        "var n1: double := BitConverter.Int64BitsToDouble(0x7FF8000000000001L); "
        "var n2: double := BitConverter.Int64BitsToDouble(0x7FFF800000000002L); "
        "using var d := new Dictionary<double, int>(); "
        "d[n1] := 42; "
        "if not d.ContainsKey(n2) then return 0; end; "
        "return d[n2];"), 42);
}

// -0.0/+0.0 must collapse to ONE dictionary key too.
BEZEL_TEST(nan_reflexive_dict_set_4586,
           dictionary_double_signed_zero_collapses_spec_4_3_1) {
    BEZEL_EXPECT_EQ(run(
        "var zp: double := 0.0; var zn: double := -0.0; "
        "using var d := new Dictionary<double, int>(); "
        "d[zn] := 7; "
        "return int(d.ContainsKey(zp)) * 10 + d.Count;"), 11);  // found + Count=1
}

// --- float parity (the hash/eq trampolines share code for float/double) --

BEZEL_TEST(nan_reflexive_dict_set_4586, set_float_nan_dedup_and_cross_payload_contains) {
    BEZEL_EXPECT_EQ(run(
        "var f1: float := BitConverter.Int32BitsToFloat(0x7FC00001); "
        "var f2: float := BitConverter.Int32BitsToFloat(0x7FC00002); "
        "using var s := new Set<float>(); "
        "s.Add(f1); s.Add(f2); s.Add(1.0f); "
        "return s.Count * 10 + int(s.Contains(f2));"), 21);  // Count=2, Contains=true

}

// --- Control: the strict-IEEE escape hatch stays strict -------------------

// §4.3.1 / stdlib §9.2: `IeeeEquals` must NOT be affected by this fix — it is
// the one place NaN-vs-NaN must still answer false.
BEZEL_TEST(nan_reflexive_dict_set_4586, double_ieee_equals_nan_stays_strict_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "var nan := 0.0 / 0.0; "
        "return int(Double.IeeeEquals(nan, nan));"), 0);
}

BEZEL_TEST(nan_reflexive_dict_set_4586, double_bitwise_equals_distinguishes_payloads_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "var n1: double := BitConverter.Int64BitsToDouble(0x7FF8000000000001L); "
        "var n2: double := BitConverter.Int64BitsToDouble(0x7FFF800000000002L); "
        "return int(Double.BitwiseEquals(n1, n2));"), 0);
}
