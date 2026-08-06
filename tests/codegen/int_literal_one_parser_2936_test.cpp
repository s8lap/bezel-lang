// Codegen/sema — #2936 (grammar A.8 / §4.2 / §15.1 / §10.2 / §17.14, v0.240):
// "parse a Bezel integer literal lexeme to its value" must have ONE definition.
//
// It had four. Grammar A.8 admits hex, binary and `_` digit separators:
//
//     HexLiteral ::= "0x" HexDigit ("_"* HexDigit)* IntSuffix?
//     BinLiteral ::= "0b" ("0" | "1") ("_"* ("0" | "1"))* IntSuffix?
//
// but three of the four call sites parsed base 10:
//
//   src/sema/ConstFold.cpp:45            std::stoll(text)
//   src/codegen/LlvmBackend_Layout.cpp   std::stoull(text)      (inline-array size)
//   src/module/ModuleMetadataBuilder.cpp strtoll(text, &end, 0) (§10.2 enum values;
//                                        base 0 gets `0x` but not `0b` and not `_`)
//
// The failure is SILENT: `std::stoll("0x60")` does not throw — it reads the
// leading `0`, stops at the `x`, and returns 0 — so ConstFold's two `catch`
// arms could never fire and the caller received a confidently wrong constant.
//
// Measured before the fix, through §15.1 inline-array sizes:
//
//     [0x5]int     -> folded 0  -> REJECTED "size must be greater than 0"
//     [0b101]int   -> folded 0  -> REJECTED
//     [5_0]int     -> sized 5   -> SILENTLY WRONG (source says 50)
//     [0x3 + 2]int -> sized 2   -> SILENTLY WRONG (0x3 read as 0)
//
// ⚠ WHY THIS HID FOR SO LONG, and why the cases below are split BARE vs
// COMPOUND: LlvmBackend_Layout tries its own `IntLiteralExpr` arm BEFORE
// falling back to sema's folded `arraySizeValue`, so a bare literal never
// reached `foldIntConst` at all. Fixing only `foldIntConst` makes the COMPOUND
// rows pass while every BARE row stays broken. A suite containing only one of
// the two shapes would have reported a half-fix as complete.
//
// The fix is one definition — `bezel::common::parseIntLiteral`
// (src/common/IntLiteral.hpp), codegen's correct implementation moved there and
// called by all four sites.
//
// The oracle is the array's REAL size, not "it compiled": each case writes and
// reads the LAST slot the declared size should have, so an undersized array
// faults and an oversized one would read back the wrong value.
//
// Note `0o5` is deliberately absent: grammar A.8 defines only HexLiteral and
// BinLiteral, so Bezel has no octal form and the parser rejects it. (The shared
// parser carries a `0o` arm inherited from codegen's version; it is unreachable
// from Bezel source.)

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `var arr: [<sizeExpr>]int;` then write+read slot `n-1`, returning 7 when the
// array genuinely has `n` elements. A too-small array raises
// IndexOutOfRangeException (a nonzero, non-7 exit); a too-large one still
// returns 7 but the sibling `oversize` case below pins that direction.
int arrayOfSize(const std::string& sizeExpr, int n) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var arr: [" + sizeExpr + "]int; "
        "  arr[" + std::to_string(n - 1) + "] := 7; "
        "  return arr[" + std::to_string(n - 1) + "]; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// --- BARE literals: the LlvmBackend_Layout arm ------------------------------

// Control: plain decimal was always correct. Present so a regression that
// breaks the shared parser outright is distinguishable from one that only
// breaks a non-decimal base.
BEZEL_TEST(int_literal_one_parser_2936, bare_decimal_control_spec_a8) {
    BEZEL_EXPECT_EQ(arrayOfSize("5", 5), 7);
}

// A.8 HexLiteral as an inline-array size. Pre-fix: folded to 0, so the compile
// was REJECTED with "an inline-array size must be greater than 0".
BEZEL_TEST(int_literal_one_parser_2936, bare_hex_sizes_the_array_spec_15_1) {
    BEZEL_EXPECT_EQ(arrayOfSize("0x5", 5), 7);
}

// A.8 BinLiteral. Pre-fix: folded to 0, rejected.
BEZEL_TEST(int_literal_one_parser_2936, bare_binary_sizes_the_array_spec_15_1) {
    BEZEL_EXPECT_EQ(arrayOfSize("0b101", 5), 7);
}

// A.8 `_` digit separators in decimal. Pre-fix: `5_0` parsed as 5 — the array
// was SILENTLY sized 5 where the source says 50, and `arr[49]` faulted. This is
// the worst row: no diagnostic at any severity.
BEZEL_TEST(int_literal_one_parser_2936, bare_underscore_decimal_spec_a8) {
    BEZEL_EXPECT_EQ(arrayOfSize("5_0", 50), 7);
}

// `_` inside a HEX literal — both features at once.
BEZEL_TEST(int_literal_one_parser_2936, bare_underscore_hex_spec_a8) {
    BEZEL_EXPECT_EQ(arrayOfSize("0x0_5", 5), 7);
}

// `_` inside a BINARY literal.
BEZEL_TEST(int_literal_one_parser_2936, bare_underscore_binary_spec_a8) {
    BEZEL_EXPECT_EQ(arrayOfSize("0b1_01", 5), 7);
}

// --- COMPOUND expressions: the sema::foldIntConst arm -----------------------

// Control: decimal arithmetic already folded correctly.
BEZEL_TEST(int_literal_one_parser_2936, compound_decimal_control_spec_17_14) {
    BEZEL_EXPECT_EQ(arrayOfSize("3 + 2", 5), 7);
}

// A hex literal as an OPERAND. Pre-fix `0x3` folded to 0, so the size was 2 and
// the array was silently wrong — a different code path from the bare rows above
// (foldIntConst, not the Layout arm), which is why both shapes are pinned.
BEZEL_TEST(int_literal_one_parser_2936, compound_hex_operand_spec_17_14) {
    BEZEL_EXPECT_EQ(arrayOfSize("0x3 + 2", 5), 7);
}

// Mixed bases in one constant expression.
BEZEL_TEST(int_literal_one_parser_2936, compound_mixed_bases_spec_17_14) {
    BEZEL_EXPECT_EQ(arrayOfSize("0x2 + 0b11", 5), 7);
}

// Subtraction, so a fold that produced 0 for the hex operand would give a
// NEGATIVE size and be rejected rather than merely mis-sized — a second,
// louder way for the same defect to show.
BEZEL_TEST(int_literal_one_parser_2936, compound_hex_subtraction_spec_17_14) {
    BEZEL_EXPECT_EQ(arrayOfSize("0xA - 5", 5), 7);
}

// --- §10.2: explicit enum values -------------------------------------------

// ⚠ ACCEPT-CONTROL, NOT a fix pin — measured, not assumed. This passes on the
// UNFIXED compiler too (verified: 3 of 12 green when src/ is reverted, and this
// is one of the 3). A SINGLE-module enum takes codegen's own `parseIntLiteral`,
// which was already the correct one; `ModuleMetadataBuilder`'s
// `tryEvalIntLiteral` — the base-0 `strtoll` that misses `0b` and `_` — is only
// consulted when the enum CROSSES a module boundary.
//
// It is kept because the fix touches the §10.2 path and this proves the
// single-module spelling is undisturbed. The real §10.2 regression pin for the
// metadata arm lives in tests/module/xmod_int_literal_2936_test.cpp, where the
// enum is actually imported.
BEZEL_TEST(int_literal_one_parser_2936, enum_binary_single_module_control_spec_10_2) {
    auto r = runAot(
        "module M; enum E A := 0b101, B, C; end E; "
        "class Q public static method Main(): int "
        "  var a := E.A; var b := E.B; var c := E.C; "
        "  if a = E.A and b = E.B and c = E.C then return 7; end; "
        "  return 1; "
        "end Main; end Q;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// --- direction control ------------------------------------------------------

// The oracle above (write+read the last slot) catches an UNDER-sized array by
// faulting. This case pins the other direction: an array declared `[0x5]int`
// must not be sized LARGER than 5 either — index 5 is out of range and must
// raise, so a parser that over-read the lexeme is caught too.
BEZEL_TEST(int_literal_one_parser_2936, hex_size_is_exact_not_merely_enough) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var arr: [0x5]int; "
        "  try arr[5] := 1; return 1; "
        "  catch IndexOutOfRangeException => return 7; end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
