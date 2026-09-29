// Codegen tests for issue #4802 / §5.2 (v0.307): decimal → uint / ushort
// conversion.
//
// Spec reference:
//   §5.2 (L749): "Explicit conversion uses function-call syntax: the target
//     type's name applied to the source value."  Examples include
//     "var i: int := int(d);             // truncates toward zero".
//   §5.2 ("Floating-point, decimal, and char conversion semantics"): the
//     integer-narrowing rule applies to every integer target type.  For a
//     `uint` target, the value must fit in uint and the conversion must
//     succeed for every value in [0, uint.MaxValue].  Same for `ushort`
//     against [0, ushort.MaxValue].
//
// Bug: codegen's decimal→int "Target fits in int32" arm at
//   src/codegen/LlvmBackend_Exprs2.cpp:9793-9813 range-checked every
//   32-bit-or-smaller target against the SIGNED int32 range
//   [INT32_MIN, INT32_MAX].  For a `uint` target this wrongly rejected
//   every value in (int32.MaxValue, uint.MaxValue] — half of uint's domain.
//   The sibling ulong arm (≥32-bit) was already fixed by #4785 via a
//   dedicated `bz_decimal_to_uint64` helper; this is the same defect
//   shape at the 32-bit-or-smaller codegen dispatch.
//
// Fix: the 32-bit-or-smaller upper-bound check now uses UINT32_MAX when
//   `target == P::UInt`.  The lower bound and the signed-target path are
//   unchanged.  `ushort` already happened to work via the existing
//   trunc + ZExt roundtrip check (its max is < INT32_MAX so the signed
//   upper bound never tripped on a valid ushort value), but the rows
//   below pin the target's contract explicitly so a follow-up refactor
//   cannot re-introduce a wrong boundary.
//
// Out of scope (separate latent defects, NOT addressed here):
//   - `uint(decimal)` of a negative decimal still silently converts to
//     a large unsigned value (e.g. uint(-1m) → 4294967295).  The signed
//     `bz_decimal_to_int64` helper handles negatives, and the new uint
//     upper-bound check passes for any v64 in (INT32_MIN, UINT32_MAX]
//     including negatives.  The proper fix is the same shape as #4785
//     (use an unsigned helper, or add an explicit unsigned-target
//     v64 < 0 guard) and warrants its own issue.
//   - `char(decimal)` of a value > 0x10FFFF or in the surrogate range
//     is silently accepted on this codegen path; the char code-point
//     range check only fires for INTEGER → char, not decimal → char.
//
// All tests run via the AOT oracle (runAot) — the strict path that runs
// ownership + definite-assignment + §33.5 entry-point checks the JIT harness
// deliberately skips.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ─── uint boundary rows ───────────────────────────────────────────────────

// §5.2 / #4802: uint(uint.MaxValue) — exact boundary, must succeed.
// Pre-fix this wrongly raised OverflowException because the codegen's
// signed-range check rejected 4294967295 (> INT32_MAX).  Post-fix the
// target-aware upper bound (UINT32_MAX) admits UINT_MAX exactly.
// This is the BUG PIN row — deleting the fix re-introduces the regression.
BEZEL_TEST(decimal_to_uint_ushort_4802, uint_from_decimal_at_uint_max_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 4294967295m; "
        "    try "
        "      var v: uint := uint(d); "
        "      if v = 4294967295U then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4802: uint(uint.MaxValue + 1) — one above the boundary, must
// raise OverflowException.  Pre-fix the rejection was for the wrong reason
// (the signed-range check happened to also reject this value); post-fix
// the rejection is structural — UINT32_MAX as the target's unsigned max.
BEZEL_TEST(decimal_to_uint_ushort_4802, uint_from_decimal_above_uint_max_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 4294967296m; "
        "    try var v: uint := uint(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2 / #4802: uint(INT_MAX + 1) — the exact value the pre-fix check
// rejected.  This is the canonical BUG PIN row for the issue's symptom
// shape: it is a representable uint value (2147483648U) that the
// signed-range check threw on.  Deleting the fix re-introduces the
// regression.
BEZEL_TEST(decimal_to_uint_ushort_4802, uint_from_decimal_at_int_max_plus_one_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 2147483648m; "
        "    try "
        "      var v: uint := uint(d); "
        "      if v = 2147483648U then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4802: uint(INT_MAX) — at the OLD boundary.  Both pre-fix and
// post-fix accept this value; pin the post-fix behaviour explicitly so a
// follow-up refactor that re-instates a wrong boundary is caught.
BEZEL_TEST(decimal_to_uint_ushort_4802, uint_from_decimal_at_int_max_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 2147483647m; "
        "    try "
        "      var v: uint := uint(d); "
        "      if v = 2147483647U then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2: small uint values (regression — must continue to work).
BEZEL_TEST(decimal_to_uint_ushort_4802, uint_from_decimal_small_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 12345m; "
        "    try "
        "      var v: uint := uint(d); "
        "      if v = 12345U then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4802: uint(decimal.MaxValue) — decimal holds values up to
// 79228162514264337593543950335, well above uint.MaxValue.  The cast
// MUST raise OverflowException (NOT silently wrap).  Pre-fix the rejection
// was for the wrong reason (the int32-range check happened to reject
// anything > INT32_MAX first); post-fix the rejection is structural.
BEZEL_TEST(decimal_to_uint_ushort_4802, uint_from_decimal_maxvalue_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := decimal.MaxValue; "
        "    try var v: uint := uint(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2 / #4802: uint round-trip (uint → decimal → uint) at the boundary.
// Exercises both directions end-to-end at UINT_MAX.  Any asymmetry in
// the unsigned path (one helper signed, the other unsigned) would surface
// here as a wrong roundtrip value.
BEZEL_TEST(decimal_to_uint_ushort_4802, uint_decimal_roundtrip_at_uint_max) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var u: uint := 4294967295U; "
        "    var d: decimal := decimal(u); "
        "    try "
        "      var back: uint := uint(d); "
        "      if back = u then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// ─── ushort boundary rows ─────────────────────────────────────────────────

// §5.2 / #4802: ushort(ushort.MaxValue) — exact boundary, must succeed.
// Pre-fix this happened to work via the trunc+roundtrip check (ushort's
// max < INT32_MAX, so the signed-range check never tripped), but the
// spec contract was implicit.  Pin it explicitly so a follow-up refactor
// cannot re-introduce a wrong boundary.
BEZEL_TEST(decimal_to_uint_ushort_4802, ushort_from_decimal_at_ushort_max_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 65535m; "
        "    try "
        "      var v: ushort := ushort(d); "
        "      if v = 65535 then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4802: ushort(ushort.MaxValue + 1) — one above the boundary,
// must raise OverflowException.  Pre-fix this was rejected at the signed
// range check (65536 < INT32_MAX → passes range check, then trunc + ZExt
// roundtrip mismatch correctly threw).  Post-fix the rejection happens
// at the same site with the same outcome.
BEZEL_TEST(decimal_to_uint_ushort_4802, ushort_from_decimal_above_ushort_max_raises) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 65536m; "
        "    try var v: ushort := ushort(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2 / #4802: ushort(INT16_MAX + 1) — the value the issue specifically
// cites.  Pre-fix this was accepted via the trunc + ZExt roundtrip (the
// bits happened to roundtrip correctly because USHRT_MAX > INT16_MAX).
// Pin the post-fix behaviour explicitly.
BEZEL_TEST(decimal_to_uint_ushort_4802, ushort_from_decimal_at_int16_max_plus_one_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 32768m; "
        "    try "
        "      var v: ushort := ushort(d); "
        "      if v = 32768 then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2 / #4802: ushort(INT16_MAX) — at the OLD boundary.  Both pre-fix
// and post-fix accept this value; pin the post-fix behaviour explicitly.
BEZEL_TEST(decimal_to_uint_ushort_4802, ushort_from_decimal_at_int16_max_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 32767m; "
        "    try "
        "      var v: ushort := ushort(d); "
        "      if v = 32767 then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2: small ushort values (regression — must continue to work).
BEZEL_TEST(decimal_to_uint_ushort_4802, ushort_from_decimal_small_succeeds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 12345m; "
        "    try "
        "      var v: ushort := ushort(d); "
        "      if v = 12345 then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}

// §5.2: signed-target regression — int(decimal.MaxValue) is well above
// INT32_MAX and must continue to raise.  The signed-target path uses the
// same INT32_MAX upper bound as before; the fix is targeted at the uint
// case only.
BEZEL_TEST(decimal_to_uint_ushort_4802, int_from_decimal_above_int_max_raises_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 2147483648m; "
        "    try var v: int := int(d); return 201; "
        "    catch OverflowException => return 42; end; "
        "  end Main; "
        "end C;"), 42);
}

// §5.2: signed-target regression — int(2147483647m) (= INT_MAX) must
// continue to succeed.  Pins the signed-target upper bound unchanged.
BEZEL_TEST(decimal_to_uint_ushort_4802, int_from_decimal_at_int_max_succeeds_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 2147483647m; "
        "    try "
        "      var v: int := int(d); "
        "      if v = 2147483647 then return 1; end; "
        "      return 201; "
        "    catch OverflowException => return 202; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}
