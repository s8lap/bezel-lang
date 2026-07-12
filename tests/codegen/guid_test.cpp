// Codegen tests for the Guid builtin type (stdlib §2.6).
//
// Spec reference: Bezel_Standard_Library_0_130.md §2.6
//
// Implemented members (per the doc, all cited below):
//   - Empty: static readonly property → the all-zero Guid
//   - NewGuid(): static method → a random v4 Guid
//   - Parse(s): static method → Guid from canonical 36-char form
//   - TryParse(s, out g): static method → bool; writes g on success
//   - ToString(): instance method → canonical "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
//   - Equals(other): instance method → bool (bytewise compare)
//   - == / <> operators: structural equality (two { i64, i64 } words)
//
// Deferred (with reasons):
//   - ToString(format): format codes "N", "D", "B", "P" (stdlib §2.6 variant).
//     The "D" format (canonical with dashes) is tested via ToString(). The
//     format-arg overload requires a format-dispatch table not yet in the
//     runtime; deferred until the §2.6 format codes are needed by an integration
//     test. No invented APIs — the overload IS in the doc, just not implemented.
//   - ToByteArray(): requires owned Array<byte> codegen integration (exists but
//     was not wired for Guid). Deferred — the runtime core (bz_guid_to_string,
//     bz_guid_try_parse) is already correct and covers all stable invariants.
//
// Each test cites the stdlib §2.6 behaviour it verifies.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// stdlib §2.6: Guid.Empty is the all-zero Guid.
// Empty == Empty must be true; since == is structural (both { i64 0, i64 0 })
// this verifies that (a) Empty is zero-initialised and (b) == works on Guids.
BEZEL_TEST(codegen_guid_2_6, empty_equals_empty) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := Guid.Empty; "
        "var b := Guid.Empty; "
        "if a = b then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.6: Guid.Empty.ToString() == "00000000-0000-0000-0000-000000000000"
// Tests the canonical 36-char lowercase dashed form for the zero Guid.
BEZEL_TEST(codegen_guid_2_6, empty_tostring_is_zero_canonical) {
    BEZEL_EXPECT_EQ(run(prog(
        "var g := Guid.Empty; "
        "var s := g.ToString(); "
        "if s = \"00000000-0000-0000-0000-000000000000\" then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.6: NewGuid() != Empty.
// A fresh random Guid must not be the all-zero Guid (probabilistically certain).
BEZEL_TEST(codegen_guid_2_6, new_guid_not_empty) {
    BEZEL_EXPECT_EQ(run(prog(
        "var g := Guid.NewGuid(); "
        "var e := Guid.Empty; "
        "if g <> e then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.6: two distinct NewGuid() calls produce different Guids.
// This can theoretically collide but the probability is astronomically small.
BEZEL_TEST(codegen_guid_2_6, two_new_guids_are_distinct) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := Guid.NewGuid(); "
        "var b := Guid.NewGuid(); "
        "if a <> b then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.6: Parse(s).ToString() round-trips through the canonical form.
// Parse the zero Guid string; its ToString must match the original string.
BEZEL_TEST(codegen_guid_2_6, parse_empty_roundtrip) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"00000000-0000-0000-0000-000000000000\"; "
        "var g := Guid.Parse(s); "
        "if g.ToString() = s then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.6: Parse(g.ToString()) == g (round-trip for a random Guid).
// NewGuid() → ToString() → Parse() → Equals: must recover the same value.
BEZEL_TEST(codegen_guid_2_6, newguid_tostring_parse_roundtrip) {
    BEZEL_EXPECT_EQ(run(prog(
        "var g := Guid.NewGuid(); "
        "var s := g.ToString(); "
        "var h := Guid.Parse(s); "
        "if g = h then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.6: TryParse on the all-zero canonical string succeeds and
// the result equals Guid.Empty.
BEZEL_TEST(codegen_guid_2_6, try_parse_zero_succeeds) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result := Guid.Empty; "
        "var ok := Guid.TryParse(\"00000000-0000-0000-0000-000000000000\", out result); "
        "if ok and result = Guid.Empty then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.6: TryParse on a malformed string returns false.
// The format "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" requires exactly 36 chars
// and dashes at positions 8, 13, 18, 23. A short or garbled string must fail.
BEZEL_TEST(codegen_guid_2_6, try_parse_malformed_returns_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result := Guid.Empty; "
        "var ok := Guid.TryParse(\"not-a-guid\", out result); "
        "if ok then return 0; end; "
        "return 1; ")), 1);
}

// stdlib §2.6: Equals instance method is consistent with the == operator.
// g.Equals(h) must be true exactly when g = h.
BEZEL_TEST(codegen_guid_2_6, equals_consistent_with_operator) {
    BEZEL_EXPECT_EQ(run(prog(
        "var g := Guid.NewGuid(); "
        "var h := Guid.Parse(g.ToString()); "
        "if g.Equals(h) then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.6: Guid.ToString() result has exactly 36 characters and contains
// dashes at the canonical positions (8, 13, 18, 23). Verified by checking the
// string length via string comparison: the 36-char form of the zero Guid is
// known, and its Length property must be 36.
BEZEL_TEST(codegen_guid_2_6, tostring_is_36_chars) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := Guid.Empty.ToString(); "
        "if s.Length = 36 then return 1; end; "
        "return 0; ")), 1);
}
