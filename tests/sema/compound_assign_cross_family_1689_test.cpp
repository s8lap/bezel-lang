// fix #1689 / §6.6 (v0.184, "Typing; the narrow-integer write-back"): a
// compound assignment `x op= e` desugars to `x := x op e`. Let T be the
// left-hand side's type and R the type of `x op e` under this chapter's
// rules (§6.2 mixed-type promotion, §6.5 shifts/bitwise). §6.6 requires R to
// be T, or to implicitly convert to T (§5.1); otherwise, when T is an
// integer type or `char` and R is an integer type, the result is a checked
// narrowing (§5.2) back to T at runtime. "In every other case the compound
// form is a compile error" — the spec's own examples are exactly the two
// repros in the issue: `b += 1.5` on `b: byte` and `f += 2.5` on
// `f: float` (R = double, no implicit conversion back). Before this fix
// the frontend never checked this: the mismatched types reached codegen
// unchecked, and the checked-arith intrinsic (or the bitwise/shift op) was
// fed one integer and one floating-point LLVM operand — LLVM IR
// verification crashed (a compiler ICE) instead of the program being
// cleanly rejected. These tests pin the exact issue repros (types-probes/
// q1_byte_plus_double.bz, q2_int_plus_double.bz), the closely-related
// bitwise/shift right-operand gap the same fix closes (only the LEFT
// operand's kind was checked against §6.5's bool/char/float restriction),
// and confirm every accepted form from the spec's own compound-assignment
// example block (§6.6) still compiles clean.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// True iff a diagnostic mentions the §6.6 compound-assignment result-type
// rule this fix adds.
bool hasCrossFamilyError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("compound assignment") != std::string::npos &&
            d.message.find("has no valid result") != std::string::npos)
            return true;
    return false;
}

// True iff a diagnostic mentions the §6.5 bitwise/shift operand-kind
// restriction (either the pre-existing target-side check or this fix's new
// right-hand-operand side of the same check).
bool hasBitwiseKindError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("bitwise and shift operators require integer "
                            "operands") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// --- The issue's exact repros -----------------------------------------

// §6.6: types-probes/q1_byte_plus_double.bz — `b += 1.5` on `b: byte`.
// R = double (the promoted type of `byte + double`); double does not
// implicitly convert to byte, and byte/double is not the integer-target/
// integer-result write-back pair, so this is a compile error. Before the
// fix this passed the frontend and crashed LLVM IR verification
// (`zext double ... to i32` — ZExt requires an integer source).
BEZEL_TEST(compound_assign_cross_family_1689, byte_plus_double_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var b: byte := 250; b += 1.5; return int(b); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCrossFamilyError(r));
}

// §6.6: types-probes/q2_int_plus_double.bz — `i += 2.5` on `i: int`. Same
// rule: R = double does not convert back to int. Before the fix this
// crashed LLVM IR verification (`llvm.sadd.with.overflow.i32` called with a
// double operand — "Call parameter type does not match function
// signature").
BEZEL_TEST(compound_assign_cross_family_1689, int_plus_double_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var i: int := 2; i += 2.5; return i; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCrossFamilyError(r));
}

// §6.6's own commented-out negative example: `f: float := 1.0f; f += 2.5;`
// — R = double (2.5 defaults to double, §4.3), which does not implicitly
// convert back to float.
BEZEL_TEST(compound_assign_cross_family_1689, float_plus_unsuffixed_double_lit_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var f: float := 1.0f; f += 2.5; return int(f); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCrossFamilyError(r));
}

// §6.2/§6.6: `decimal` and `double` have no implicit conversion in either
// direction (§6.2's own example), so `d += 5.0` on `d: decimal` is the same
// family-crossing case. Before the fix this was silently ACCEPTED (a
// wrong-accept, not a crash): the double literal's LLVM value was fed
// straight to the decimal-arithmetic helper expecting the 4xi32 decimal
// struct.
BEZEL_TEST(compound_assign_cross_family_1689, decimal_plus_double_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 1.0m; d += 5.0; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCrossFamilyError(r));
}

// --- The related bitwise/shift right-operand gap this fix also closes --

// §6.5: bitwise/shift operators require BOTH operands to be true integer
// types. The pre-existing compound-assignment guard checked only the
// TARGET's kind; `x &= 1.5` on `x: int` slipped through with a bad
// right-hand operand and crashed LLVM IR verification ("Both operands to a
// binary operator are not of the same type!", `and i32 %_, double _`).
BEZEL_TEST(compound_assign_cross_family_1689, bitwise_float_rhs_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var x: int := 5; x &= 1.5; return x; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBitwiseKindError(r));
}

// Same gap, `<<=`: `s <<= 1.5` on `s: short` crashed IR verification the
// same way ("shl i16 %_, i0 0" — the malformed count operand).
BEZEL_TEST(compound_assign_cross_family_1689, shift_float_rhs_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var s: short := 21; s <<= 1.5; return int(s); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBitwiseKindError(r));
}

// --- Every accepted form from §6.6's own example block still compiles --

// `b += 4;` — R = int (byte widens to int, §6.2); byte is an integer
// target receiving an integer result, so this is the checked-narrowing
// write-back, not a compile error (`byte(264)` raises OverflowException at
// RUNTIME on an out-of-range sum, never here).
BEZEL_TEST(compound_assign_cross_family_1689, byte_plus_int_literal_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var b: byte := 250; b += 4; return int(b); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCrossFamilyError(r));
    BEZEL_EXPECT_FALSE(hasBitwiseKindError(r));
}

// `checksum ^= b;` where both are `byte` — R = T directly, no write-back.
BEZEL_TEST(compound_assign_cross_family_1689, same_type_bitwise_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var checksum: byte := 0; var b: byte := 254; "
        "  checksum ^= b; return int(checksum); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCrossFamilyError(r));
    BEZEL_EXPECT_FALSE(hasBitwiseKindError(r));
}

// `acc += n;` where `acc: byte`, `n: int` — the spec's own "admitted: R =
// int; checked write-back at runtime" example.
BEZEL_TEST(compound_assign_cross_family_1689, byte_plus_int_var_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var n: int := 2; var acc: byte := 40; acc += n; "
        "  return int(acc); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCrossFamilyError(r));
    BEZEL_EXPECT_FALSE(hasBitwiseKindError(r));
}

// `s <<= 2;` — a shift's result is always the left operand's type (§6.5),
// so there is never a write-back mismatch to reject.
BEZEL_TEST(compound_assign_cross_family_1689, shift_int_literal_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var s: short := 1; s <<= 2; return int(s); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCrossFamilyError(r));
    BEZEL_EXPECT_FALSE(hasBitwiseKindError(r));
}

// `c += 1;` where `c: char` — the §5.2 code-point checked-narrowing case:
// `char` is named explicitly alongside the true integer types in §6.6's
// write-back rule ("T is an integer type or `char`").
BEZEL_TEST(compound_assign_cross_family_1689, char_plus_int_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var c: char := 'a'; c += 1; return int(c); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCrossFamilyError(r));
}

// `f += 2.5f;` — the float suffix keeps the RHS in the float family (R = T
// directly), unlike the unsuffixed-double negative case above.
BEZEL_TEST(compound_assign_cross_family_1689, float_plus_float_literal_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var f: float := 1.0f; f += 2.5f; return int(f); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCrossFamilyError(r));
}

// `decimal += <int literal>` and `decimal += <decimal literal>` both keep
// R = T (an int widens into decimal per §6.2) and must stay accepted.
BEZEL_TEST(compound_assign_cross_family_1689, decimal_plus_int_and_decimal_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 1.0m; d += 5; d += 2m; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCrossFamilyError(r));
}

// String compound concatenation (§4.6) is untouched: the target's kind is
// `string`, which this check explicitly excludes (concatenation has its own
// typing rule, not §6.2's numeric promotion table).
BEZEL_TEST(compound_assign_cross_family_1689, string_concat_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method Main(): int "
        "  var s: string := \"a\"; s += \"b\"; s += 5; return s.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasCrossFamilyError(r));
    BEZEL_EXPECT_FALSE(hasBitwiseKindError(r));
}
