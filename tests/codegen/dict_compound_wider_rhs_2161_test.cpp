// Codegen — #2161 (§6.6 / §6.2 / §6.1 / §5.2): a Dictionary-indexer compound
// assignment (`d[k] op= e`) whose RHS is a WIDER integer must run the operation
// at the §6.2-promoted width and apply the §6.6 CHECKED narrow-integer
// write-back — exactly as #2140 (arithmetic) and #2147 (bitwise) established
// for the local/field/property/array-indexer paths — not silently truncate the
// RHS to the value type, and not emit an invalid narrowing cast (crash).
//
// Per §6.6 (spec v0.226) the compound `x op= e` is `x := x op e` with the result
// typed R = type of `x op e` under §6.2 promotion; when T (the LHS — here the
// dict value type) is an integer and R is a wider integer, the result is
// converted back to T by the §5.2 CHECKED narrowing: a value outside T's range
// raises OverflowException ("§6.1's checked posture; never a silent
// truncation"). All nine operators (`+=`, `-=`, `*=`, `/=`, `&=`, `|=`, `^=`,
// `<<=`, `>>=`) share the same write-back rule; only shifts do not raise
// (their result already has the LHS type, §6.5).
//
// Pre-existing on main @dd33abd (not introduced by #2140/#2147 — the local/
// field/property/array-indexer paths were all correct after those fixes; only
// the Dictionary indexer path was broken). The dict branch of
// `lowerCollectionIndexerCompound` in `src/codegen/LlvmBackend_Assign.cpp`
// pre-widened the RHS to the dict value type with an unconditional
// `CreateSExt(rhs, valLlvm)`:
//
//   * VARIABLE wider RHS  →  an invalid narrowing cast (the LLVM verifier
//     rejects `sext i64 %13 to i32` as "Type too small for SExt", aborting
//     compile).
//   * CONSTANT wider RHS  →  release-LLVM's constant folder silently truncated
//     the i64 to i32, then stored the wrong value with no raise (`+= 2^32L`
//     stored 0, `^= (2^32+1)L` stored 1, `|= 2^32L` stored 0).
//
// The array-indexer branch of the same function has no pre-widen — it passes
// the RHS unchanged to emitCompoundOp, whose #2140/#2147 wide branch runs the
// op at the promoted width and applies the §6.6/§5.2 checked narrow back. The
// fix replaces the dict branch's unconditional SExt with
// `coerceNumericToType(rhs, valLlvm, rUns)` — the widening rule already shared
// by every other numeric conversion in the backend: widen only when the RHS is
// strictly narrower than valLlvm, and only by the RHS's own signedness. A
// wider RHS is left at its natural width so emitCompoundOp takes the same
// path the array branch uses.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body-only program returning an int exit code.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Returns 1 if `body` raises OverflowException, 0 if it runs to completion,
// -999 on a front-end failure.
int throwsOverflow(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Convenience: store d["a"] := <int>, perform <body> that reads d["a"] and
// returns it, throw OverflowException otherwise. The d["a"] return lets
// non-trivially-fittable results still be checked via `run` (the exit code can
// only carry 0..255).
std::string dictBody(const std::string& init /*int*/,
                     const std::string& mutate,
                     const std::string& returnRhs) {
    return "  var d := new Dictionary<string, int>(); "
           "  d[\"a\"] := " + init + "; "
           "  " + mutate + "; "
           "  return " + returnRhs + ";";
}

}  // namespace

// ============================================================================
// Issue repros (variable RHS — crashes, constant RHS — silent wrong value).
// ============================================================================

// The literal issue repro: variable long RHS to int dict.
BEZEL_TEST(codegen_2161_wider_rhs, var_long_or_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 0; "
        "var w: long := 4294967296L; "
        "d[\"a\"] |= w; "
        "return 0;"), 1);
}

// The sharp silent-truncation case: `d["a"] |= 4294967296L` (constant RHS).
// Truncated 2^32 → 0 and stored 0 with no raise. After fix: 2^32 is out of int
// range → OverflowException.
BEZEL_TEST(codegen_2161_wider_rhs, const_or_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "d[\"a\"] |= 4294967296L", "0")), 1);
}

// `d["a"] ^= 4294967297L` — the sharper silent case: 2^32+1 truncated to a
// VALID in-range 1 (a WRONG value silently stored). After fix: 2^32+1 is out
// of int range → OverflowException.
BEZEL_TEST(codegen_2161_wider_rhs, const_xor_2_32_plus_1_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "d[\"a\"] ^= 4294967297L", "0")), 1);
}

// Arithmetic `d["a"] += 4294967296L` was silently truncated to 0. After fix:
// raise. (The #2140 fix never reached this path.)
BEZEL_TEST(codegen_2161_wider_rhs, const_add_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "d[\"a\"] += 4294967296L", "0")), 1);
}

// ============================================================================
// All nine operators — variable RHS — wider (long) — out-of-range raises.
// ============================================================================

// `+=` — already covered above; keep as named anchor + the negative-sign edge.
BEZEL_TEST(codegen_2161_var_long_oor, plus_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "var w: long := 4294967296L; d[\"a\"] += w", "0")), 1);
}
// Negative wide signed RHS computes signed (`0 + -2^32` = -2^32, out of int
// range → raise). FAILS WITHOUT the fix (invalid SExt crash).
BEZEL_TEST(codegen_2161_var_long_oor, plus_neg_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "var w: long := -4294967296L; d[\"a\"] += w", "0")), 1);
}

// `-=` — 0 - 2^32 = -2^32, out of int range → raise.
BEZEL_TEST(codegen_2161_var_long_oor, minus_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "var w: long := 4294967296L; d[\"a\"] -= w", "0")), 1);
}

// `*=` — 2 * 2^31 = 2^32, out of int range → raise.
BEZEL_TEST(codegen_2161_var_long_oor, times_2_31_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("2", "var w: long := 2147483648L; d[\"a\"] *= w", "0")), 1);
}

// `/=` — int(1) / long(2^32) = 0, in range, stores 0 (no raise). The variable
// was the crash repro (invalid SExt); same code path as the OOR cells.
BEZEL_TEST(codegen_2161_var_long_oor, div_wide_no_raise) {
    BEZEL_EXPECT_EQ(run(dictBody("1", "var w: long := 4294967296L; d[\"a\"] /= w",
                                  "d[\"a\"]")), 0);
}

// `&=` — 0 & 2^32 = 0, IN range → stores 0 (no raise). The bug here is the
// IR-verify crash; the §6.6 write-back is satisfied at the promoted width.
BEZEL_TEST(codegen_2161_var_long_oor, and_2_32_in_range) {
    BEZEL_EXPECT_EQ(run(dictBody("0",
                                  "var w: long := 4294967296L; d[\"a\"] &= w",
                                  "d[\"a\"]")), 0);
}
// `|=` — exact issue repro (listed again here under the all-nine taxonomy).
BEZEL_TEST(codegen_2161_var_long_oor, or_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "var w: long := 4294967296L; d[\"a\"] |= w", "0")), 1);
}
// `^=` — 0 ^ 2^32 = 2^32, out of int range → raise.
BEZEL_TEST(codegen_2161_var_long_oor, xor_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "var w: long := 4294967296L; d[\"a\"] ^= w", "0")), 1);
}

// `<<=` — shifts per §6.5 stay on the LHS-width path (the count is masked
// mod the operand's bit width and a shift never raises). The variable RHS
// was the crash repro; after the fix the count widens by the RHS's signedness
// then is masked to i32. 1 << 33 = 2 (mask 33 & 31 = 1).
BEZEL_TEST(codegen_2161_var_long_oor, shl_wide_count_masked) {
    BEZEL_EXPECT_EQ(run(dictBody("1",
                                  "var w: long := 33L; d[\"a\"] <<= w",
                                  "d[\"a\"]")), 2);
}
// `>>=` — 8 >> 33 = 8 >> 1 = 4.
BEZEL_TEST(codegen_2161_var_long_oor, shr_wide_count_masked) {
    BEZEL_EXPECT_EQ(run(dictBody("8",
                                  "var w: long := 33L; d[\"a\"] >>= w",
                                  "d[\"a\"]")), 4);
}

// ============================================================================
// All nine operators — CONSTANT RHS — wider (long literal) — out-of-range raises.
// ============================================================================

// Covers the silent-truncation class for each operator. The pre-fix code
// truncated the i64 ConstFolder-handled value to i32 and stored the wrong
// bits; after the fix the wide branch's checked narrow fires.
BEZEL_TEST(codegen_2161_const_long_oor, plus_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "d[\"a\"] += 4294967296L", "0")), 1);
}
BEZEL_TEST(codegen_2161_const_long_oor, minus_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "d[\"a\"] -= 4294967296L", "0")), 1);
}
BEZEL_TEST(codegen_2161_const_long_oor, times_2_31_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("2", "d[\"a\"] *= 2147483648L", "0")), 1);
}
BEZEL_TEST(codegen_2161_const_long_oor, div_no_raise) {
    BEZEL_EXPECT_EQ(run(dictBody("1", "d[\"a\"] /= 4294967296L", "d[\"a\"]")), 0);
}
BEZEL_TEST(codegen_2161_const_long_oor, and_2_32_in_range) {
    BEZEL_EXPECT_EQ(run(dictBody("0", "d[\"a\"] &= 4294967296L", "d[\"a\"]")), 0);
}
BEZEL_TEST(codegen_2161_const_long_oor, or_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "d[\"a\"] |= 4294967296L", "0")), 1);
}
BEZEL_TEST(codegen_2161_const_long_oor, xor_2_32_plus_1_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0", "d[\"a\"] ^= 4294967297L", "0")), 1);
}
BEZEL_TEST(codegen_2161_const_long_oor, shl_33_no_raise) {
    BEZEL_EXPECT_EQ(run(dictBody("1", "d[\"a\"] <<= 33L", "d[\"a\"]")), 2);
}
BEZEL_TEST(codegen_2161_const_long_oor, shr_33_no_raise) {
    BEZEL_EXPECT_EQ(run(dictBody("8", "d[\"a\"] >>= 33L", "d[\"a\"]")), 4);
}

// ============================================================================
// #2155 (§6.2/§6.6, spec v0.226): the two `var ulong` RHS cells that used to
// live here — `d["a"] |= <ulong var>` and `d["a"] &= <ulong var>` on an int
// dict — were REMOVED. They previously asserted a runtime OverflowException,
// but that was the OLD WRONG behavior: the pair (`int`, `ulong`) has no
// implicit conversion in either direction (§5.1 — `int` widens only to
// long/float/double/decimal; `ulong` widens only to float/double/decimal; the
// signed↔unsigned pair is "never implicit" in both directions). The binary
// `int | ulong` / `int & ulong` is itself a §6.2 compile error, so the
// compound form (desugared `x := x op e`, §6.6) is the same compile error.
// The prior §6.6 family check admitted the compound (diverging from the
// binary) and raised at runtime; #2155's §6.2 pair-admissibility fix makes the
// compound match the binary, so these cells now compile-reject (the RHS is a
// variable, so the §5.1 constant-narrowing / literal-derived exemption does
// NOT apply). The compile-reject is covered on the dict-indexer path by the
// lit sema track: compound_mixed_signedness_2155__dict_int_or_equals_ulong_rejected.bzl
// and compound_mixed_signedness_2155__dict_int_and_equals_ulong_rejected.bzl.
// A literal-derived RHS above int.MaxValue is still admitted by §5.1 constant
// narrowing — see uint_in_range_stores below (the literal fits int range).
// ============================================================================

// ============================================================================
// Signedness of the widening itself — a RHS UNSIGNED and NARROWER than the
// value type widens by ZExt (per §5.1: byte/ushort/uint/ulong → zero-extended),
// not SExt (the old unconditional SExt would sign-extend a uint down to int
// and corrupt negatives / positive values above INT_MAX). FAILS WITHOUT the
// fix if the cell is reachable (the pre-fix code only triggered for the wider
// RHS too, but the narrower unsigned RHS Cell is the same-code-path bug).
// ============================================================================

// uint → int via `d["a"] |= <uint>` widens by ZExt (uint is unsigned per
// §5.1), so the bit-pattern is preserved. The §6.2-promoted wide OR runs
// unsigned (`arithUns = rUns`); the narrow-back check uses the LHS's
// signedness (signed int → SExt). Pick a value in int range that exercises
// the ZExt path on a non-trivial high bit: 2000000000U is in int range
// (INT_MAX = 2147483647) and has bit 31 cleared after widening. The exit
// code is masked to 0..255, so compare via a 0/1 flag instead of the raw
// value.
BEZEL_TEST(codegen_2161_signedness, uint_in_range_stores) {
    BEZEL_EXPECT_EQ(run(
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 0; "
        "d[\"a\"] |= 2000000000U; "
        "if d[\"a\"] = 2000000000 then return 1; end; "
        "return 0;"), 1);
}
// Negative signed wider RHS computes signed: int(5) |= -1L = -1 (in range).
// FAILS WITHOUT the fix (invalid SExt crash). Exit code for -1 is 255, so
// compare via a flag.
BEZEL_TEST(codegen_2161_signedness, var_negative_long_or_in_range) {
    BEZEL_EXPECT_EQ(run(
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 5; "
        "var w: long := -1L; "
        "d[\"a\"] |= w; "
        "if d[\"a\"] = -1 then return 1; end; "
        "return 0;"), 1);
}

// ============================================================================
// Non-regression — narrower RHS, same-width RHS, and the local/field/property
// arithmetic path (#2140) and bitwise path (#2147) must NOT be perturbed by
// this fix. The dict branch's pre-widen block had a single bug Surface; the
// fix removes it and the existing neighbor cells stay green.
// ============================================================================

// Narrower RHS — int += short widens by SExt (short is signed per §5.1).
// 1000 fits in the 8-bit exit code.
BEZEL_TEST(codegen_2161_neighbors, int_plus_short_in_range_stores) {
    BEZEL_EXPECT_EQ(run(
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 0; "
        "var r: short := 1000; "
        "d[\"a\"] += r; "
        "return d[\"a\"];"), 232);  // 1000 & 0xFF
}
// Narrower RHS — int += short out-of-range raises. The narrow operand is
// short (signed, -32768..32767), but the dict value is int — short + int
// promotes to int, so 32767 + 1 = 32768 fits in int; use a value that the
// §6.2 promoted wide op finds out of int range. With a long RHS the path
// raises (the #2161 fix); with a short RHS the narrow op runs at int width
// and the result is in range. Skip this cell — the same-shape test is
// already covered by `int_plus_int_overflow_raises` below for the same-width
// pair (a short RHS never overflows int since int is wider).

// Same-width RHS — int += int unchanged.
BEZEL_TEST(codegen_2161_neighbors, int_plus_int_in_range_stores) {
    BEZEL_EXPECT_EQ(run(dictBody("100",
                                  "d[\"a\"] += 200",
                                  "d[\"a\"]")), 44);  // 300 & 0xFF
}
BEZEL_TEST(codegen_2161_neighbors, int_plus_int_overflow_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("2000000000", "d[\"a\"] += 2000000000", "0")), 1);
}
// Negative same-width signed RHS computes signed.
BEZEL_TEST(codegen_2161_neighbors, int_plus_negative_int_stores) {
    BEZEL_EXPECT_EQ(run(dictBody("5", "d[\"a\"] += -3", "d[\"a\"]")), 2);
}
// Local/field anchor — the #2140 arithmetic cell: int += 2^32L raises. (Not
// a dict cell; included so the #2140 anchor is reaffirmed in this test file.)
BEZEL_TEST(codegen_2161_neighbors, local_2140_anchor_unchanged) {
    BEZEL_EXPECT_EQ(throwsOverflow("var i: int := 0; i += 4294967296L;"), 1);
}
// Local/field bitwise anchor — the #2147 cell: int(-1) &= 2^32L raises.
BEZEL_TEST(codegen_2161_neighbors, local_2147_anchor_unchanged) {
    BEZEL_EXPECT_EQ(throwsOverflow("var i: int := -1; i &= 4294967296L;"), 1);
}
// Array-indexer anchor — the #2140 arithmetic cell on a list-interface value:
// used to confirm the array branch is unaffected by the dict-branch fix.
BEZEL_TEST(codegen_2161_neighbors, array_local_2140_anchor_unchanged) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var a := new Array<int>(); a.Add(0); a[0] += 4294967296L;"), 1);
}

// ============================================================================
// Multiple keys in the same dict — the fix must not reset or overwrite the
// wrong slot. Tests that the dict's normal indexer dispatch is preserved.
// The mutation raises OverflowException (the cell is the all-nine OOR set);
// wrap it in a try-catch so the test can verify d["b"] is unchanged AND
// the catch actually fired (sentinel `caught`).
// ============================================================================

BEZEL_TEST(codegen_2161_isolation, only_targeted_key_widened) {
    BEZEL_EXPECT_EQ(run(
        "var d := new Dictionary<string, int>(); "
        "d[\"a\"] := 10; "
        "d[\"b\"] := 20; "
        "var w: long := 4294967296L; "
        "var caught := 0; "
        "try d[\"a\"] |= w; catch OverflowException => caught := 1; end; "
        "if caught = 1 then if d[\"b\"] = 20 then return 1; end; end; "
        "return 0;"), 1);
}

// ============================================================================
// Substring of the LHS-side error catching sanity: a WIDER RHS that is OUT OF
// RANGE for the dict value type must raise, not silently truncate. This is
// the §6.6/§5.2 contract — the same one #2140/#2147 enforced for the other
// paths. Spot-check the wider signed long path for both variable and
// constant RHS with the arithmetic and bitwise ops.
// ============================================================================

BEZEL_TEST(codegen_2161_wider_long_arith, var_int_add_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0",
                  "var w: long := 4294967296L; d[\"a\"] += w",
                  "0")), 1);
}
BEZEL_TEST(codegen_2161_wider_long_arith, var_int_sub_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0",
                  "var w: long := 4294967296L; d[\"a\"] -= w",
                  "0")), 1);
}
BEZEL_TEST(codegen_2161_wider_long_arith, var_int_or_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0",
                  "var w: long := 4294967296L; d[\"a\"] |= w",
                  "0")), 1);
}
BEZEL_TEST(codegen_2161_wider_long_arith, var_int_xor_2_32_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        dictBody("0",
                  "var w: long := 4294967296L; d[\"a\"] ^= w",
                  "0")), 1);
}
