// Codegen / sema — #3045 (proposal 0079, graduated at spec v0.242; text read at
// docs v0.247): the shift count's DOMAIN.
//
// §6.5, verbatim: "The shift count's **domain** is `0` through `width−1`, where
// width is the bit width of the **left (shifted) operand's type** — `0..7` for
// `byte`/`sbyte`, `0..15` for `short`/`ushort`, `0..31` for the 32-bit types
// (`int`/`uint`), `0..63` for the 64-bit types (`long`/`ulong`). A count outside
// the domain — negative, or ≥ width — is a fault: a **compile error** when the
// count is a compile-time constant (§17.14), and `OverflowException` at runtime
// otherwise. Within the domain, bits shifted beyond the left operand's width are
// discarded silently — that is the operation, exactly as `&` discards bits — and
// the result never faults. Code that wants the modular count deliberately writes
// it: `v << (k & 31)` (`& 63` for 64-bit left operands)."
//
// THE PROMOTION TRAP, settled by §6.5's own preceding paragraph: "A shift's
// result type is the type of its **left** operand. The right operand supplies
// only the shift count: it does **not** participate in the mixed-type promotion
// of §6.2, so a narrower left operand is *not* widened to match a wider count
// (`byte << int` yields `byte`, not `int`)." The domain is therefore the left
// operand's SOURCE width — `byte << k` is `0..7` even though `k` is an `int` and
// even though 8..31 are perfectly good `int` counts. That reading is what the
// per-width cells below pin.
//
// §6.6: "`x <<= k` and `x >>= k` inherit §6.5's count-domain rule through the
// desugaring: an out-of-domain count is the same fault — compile error for a
// constant count, `OverflowException` at runtime otherwise."
//
// §17.14: "An evaluation **fault** — integer or `decimal` overflow, division or
// `mod` by zero, a shift count outside the §6.5 domain — is a **compile error at
// the constant expression**, never a deferred runtime fault: … and
// `const F := 1 << 64;` are rejected where they are written."
//
// §31.4: `OverflowException` — "a value outside the destination type's or
// operation's domain: … out-of-domain shift counts (§6.5) …".
//
// §28.8.1's throwable inventory conforms: "shifts whose count is not a
// compile-time constant (an out-of-domain count raises `OverflowException` per
// §6.5)" are throwable.
//
// BEFORE this fix the count was silently reduced modulo the width at every one
// of the four sites (`ConstFold::foldIntConst`, the binary-shift lowering, the
// nullable-lifted lowering, and `emitCompoundOp`), so every cell below that now
// expects a fault produced a value instead. The masking pins that recorded the
// old rule are flipped in place — see numeric_shift_nan_char_513_test.cpp,
// int_arithmetic_test.cpp, compound_assign_test.cpp,
// dict_compound_wider_rhs_2161_test.cpp, and
// bitwise_compound_wider_rhs_2147_test.cpp.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Body-only program returning an int exit code.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// The RUNTIME arm: 1 = raised OverflowException, 0 = ran to completion,
// -999 = the front end failed (which would mean the count was treated as
// constant — a different bug, and one this helper reports distinctly).
int throwsOverflow(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// The CONSTANT arm: rejected, and rejected by the §6.5/§17.14 domain rule
// specifically (a bare hasErrors() would pass for any unrelated reject).
bool rejects(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is outside the domain") != std::string::npos &&
            d.message.find("(\xC2\xA7""6.5/\xC2\xA7""17.14)") !=
                std::string::npos)
            return true;
    return false;
}

// Accepts cleanly (no errors at all) — the in-domain control.
bool accepts(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran && !r.diagnostics.hasErrors();
}


bool runs(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran;
}

}  // namespace

// ===========================================================================
// A. The width axis — the domain is the LEFT operand's own width (§6.5).
//    Each type gets its boundary pair: width−1 shifts, width faults.
//    The RUNTIME arm is used here so the fault is observable as the exception
//    §6.5 names; the constant arm is section C.
// ===========================================================================

BEZEL_TEST(shift_count_domain_3045, byte_domain_is_0_to_7_runtime) {
    BEZEL_EXPECT_EQ(run(
        "var v: byte := 1; var k: int := 7; var r: byte := v << k; "
        "return int(r);"), 128);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: byte := 1; var k: int := 8; var r: byte := v << k;"), 1);
}

BEZEL_TEST(shift_count_domain_3045, sbyte_domain_is_0_to_7_runtime) {
    // sbyte(1) << 6 == 64, in range; count 8 is outside `0..7`.
    BEZEL_EXPECT_EQ(run(
        "var v: sbyte := 1; var k: int := 6; var r: sbyte := v << k; "
        "return int(r);"), 64);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: sbyte := 1; var k: int := 8; var r: sbyte := v << k;"), 1);
}

BEZEL_TEST(shift_count_domain_3045, short_domain_is_0_to_15_runtime) {
    // short(1) << 14 == 16384 — observed on stdout (outside an 8-bit exit).
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var v: short := 1; var k: int := 14; var r: short := v << k; "
        "Console.WriteLine(int(r)); return 0; end Main; end C;"), "16384");
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: short := 1; var k: int := 16; var r: short := v << k;"), 1);
}

BEZEL_TEST(shift_count_domain_3045, ushort_domain_is_0_to_15_runtime) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var v: ushort := 1; var k: int := 15; var r: ushort := v << k; "
        "Console.WriteLine(int(r)); return 0; end Main; end C;"), "32768");
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: ushort := 1; var k: int := 16; var r: ushort := v << k;"), 1);
}

BEZEL_TEST(shift_count_domain_3045, int_domain_is_0_to_31_runtime) {
    // 1 << 31 is int.MinValue — the sign test keeps it inside an exit code.
    BEZEL_EXPECT_EQ(run(
        "var v: int := 1; var k: int := 31; "
        "return (if (v << k) < 0 then 1 else 0);"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: int := 1; var k: int := 32; var r := v << k;"), 1);
}

BEZEL_TEST(shift_count_domain_3045, uint_domain_is_0_to_31_runtime) {
    BEZEL_EXPECT_EQ(run(
        "var v: uint := 1u; var k: int := 31; "
        "return (if (v >> 31u) = 0u then 1 else 0);"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: uint := 1u; var k: int := 32; var r := v << k;"), 1);
}

BEZEL_TEST(shift_count_domain_3045, long_domain_is_0_to_63_runtime) {
    BEZEL_EXPECT_EQ(run(
        "var v: long := 1L; var k: int := 63; "
        "return (if (v << k) < 0L then 1 else 0);"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: long := 1L; var k: int := 64; var r := v << k;"), 1);
}

BEZEL_TEST(shift_count_domain_3045, ulong_domain_is_0_to_63_runtime) {
    BEZEL_EXPECT_EQ(run(
        "var v: ulong := 1uL; var k: int := 63; "
        "return (if (v << k) > 0uL then 1 else 0);"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: ulong := 1uL; var k: int := 64; var r := v << k;"), 1);
}

// THE PROMOTION TRAP, stated as its own cell: 8 is a perfectly good `int` count
// and the count's own type IS `int`, but the left operand is a `byte`, so §6.5's
// domain is `0..7` and the shift faults. If the implementation ever promotes the
// left operand to the count's width, this cell returns 0 (1 << 8 == 256, which
// the byte target then truncates) instead of 1.
BEZEL_TEST(shift_count_domain_3045, domain_is_source_width_not_promoted_width) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: byte := 1; var k: int := 8; var r: byte := v << k;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: short := 1; var k: int := 16; var r: short := v << k;"), 1);
    // …while the same counts are fine when the left operand really is an int.
    // (256 and 65536 are outside the 8-bit exit channel — observed on stdout.)
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var v: int := 1; var k: int := 8; Console.WriteLine(v << k); "
        "return 0; end Main; end C;"), "256");
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var v: int := 1; var k: int := 16; Console.WriteLine(v << k); "
        "return 0; end Main; end C;"), "65536");
}

// ===========================================================================
// B. The operator axis and the count's own signedness.
// ===========================================================================

BEZEL_TEST(shift_count_domain_3045, right_shift_faults_like_left_shift) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: int := 8; var k: int := 32; var r := v >> k;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: byte := 8; var k: int := 8; var r: byte := v >> k;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: long := 8L; var k: int := 64; var r := v >> k;"), 1);
}

// §6.5: "negative … is a fault". A negative RUNTIME count raises; it does not
// read as a huge unsigned count and it does not mask to `width−1` (which is
// what `& 31` used to do: -1 & 31 == 31, so `1 << -1` silently gave int.MinValue).
BEZEL_TEST(shift_count_domain_3045, negative_runtime_count_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: int := 1; var k: int := -1; var r := v << k;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: long := 1L; var k: int := -64; var r := v << k;"), 1);
}

// An UNSIGNED count is checked on its own (unsigned) reading: 4_000_000_000 is
// not negative, it is simply ≥ 32. Under the retired mask it was 4000000000 & 31
// == 0, a silent no-op shift.
BEZEL_TEST(shift_count_domain_3045, unsigned_runtime_count_above_width_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: int := 1; var k: uint := 4000000000u; var r := v << k;"), 1);
    // …and a small unsigned count still shifts.
    BEZEL_EXPECT_EQ(run(
        "var v: int := 1; var k: uint := 5u; return v << k;"), 32);
}

// A `long`-typed count whose value does not fit the left operand's width must be
// judged BEFORE any coercion to that width: 2^32 truncates to 0 in i32, which
// would read as a perfectly in-domain count.
BEZEL_TEST(shift_count_domain_3045, wide_count_checked_before_narrowing) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: int := 1; var k: long := 4294967296L; var r := v << k;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: int := 1; var k: long := 4294967296L; v <<= k;"), 1);
}

// ===========================================================================
// C. The CONSTANT arm — compile errors (§17.14), one per width and operator.
// ===========================================================================

BEZEL_TEST(shift_count_domain_3045, const_count_at_width_rejected_per_type) {
    BEZEL_EXPECT_TRUE(rejects("var v: byte := 1; var r: byte := v << 8;"));
    BEZEL_EXPECT_TRUE(rejects("var v: sbyte := 1; var r: sbyte := v << 8;"));
    BEZEL_EXPECT_TRUE(rejects("var v: short := 1; var r: short := v << 16;"));
    BEZEL_EXPECT_TRUE(rejects("var v: ushort := 1; var r: ushort := v << 16;"));
    BEZEL_EXPECT_TRUE(rejects("var v: int := 1; var r := v << 32;"));
    BEZEL_EXPECT_TRUE(rejects("var v: uint := 1u; var r := v << 32;"));
    BEZEL_EXPECT_TRUE(rejects("var v: long := 1L; var r := v << 64;"));
    BEZEL_EXPECT_TRUE(rejects("var v: ulong := 1uL; var r := v << 64;"));
}

BEZEL_TEST(shift_count_domain_3045, const_count_below_width_accepted_per_type) {
    BEZEL_EXPECT_TRUE(accepts(
        "var v: byte := 1; var r: byte := v << 7; return int(r);"));
    BEZEL_EXPECT_TRUE(accepts(
        "var v: short := 1; var r: short := v << 15; return 0;"));
    BEZEL_EXPECT_TRUE(accepts("var v: int := 1; var r := v << 31; return 0;"));
    BEZEL_EXPECT_TRUE(accepts("var v: long := 1L; var r := v << 63; return 0;"));
}

BEZEL_TEST(shift_count_domain_3045, const_right_shift_rejected) {
    BEZEL_EXPECT_TRUE(rejects("var v: int := 8; var r := v >> 32;"));
    BEZEL_EXPECT_TRUE(rejects("var v: byte := 8; var r: byte := v >> 8;"));
}

// §6.5: "The constant-count diagnostic for a negative count notes the direction
// confusion: a negative count does not reverse the shift's direction — use the
// opposite operator, or mask deliberately with `& 31`."
BEZEL_TEST(shift_count_domain_3045, const_negative_count_rejected_with_hint) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "var v: int := 1; var r := v << -1; return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool sawHint = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not reverse the shift's direction") !=
                std::string::npos &&
            d.message.find("use the opposite operator (`>>`)") !=
                std::string::npos)
            sawHint = true;
    BEZEL_EXPECT_TRUE(sawHint);
}

// The diagnostic names the domain AND §6.5's own remedy spelling, with the mask
// constant that matches the left operand's width (`& 7` for a byte, not `& 31`).
BEZEL_TEST(shift_count_domain_3045, diagnostic_names_domain_and_mask) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "var v: byte := 1; var q: byte := v << 9; return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool ok = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`0..7`") != std::string::npos &&
            d.message.find("`byte`") != std::string::npos &&
            d.message.find("(k & 7)") != std::string::npos)
            ok = true;
    BEZEL_EXPECT_TRUE(ok);
}

// §17.14's own rejected example, verbatim: `const F := 1 << 64;`. The literal
// `1` is an `int`, so the domain is `0..31` and 64 is far outside it.
BEZEL_TEST(shift_count_domain_3045, const_decl_shift_64_rejected_spec_17_14) {
    auto r = runAot(
        "module M; class C public const F: long := 1 << 64; "
        "public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §17.14's ACCEPTED example beside it: `const Flags: long := (1 << 10) | (1 << 3);`
// — both counts are inside `int`'s domain, so it still folds to 1032.
BEZEL_TEST(shift_count_domain_3045, const_decl_in_domain_folds_spec_17_14) {
    auto r = runAot(
        "module M; class C public const Flags: long := (1 << 10) | (1 << 3); "
        "public static method Main(): int var f: long := Flags; "
        "Console.WriteLine(f); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran && !r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText.substr(0, 4), std::string("1032"));
}

// A LOCAL `const` initializer is the same constant expression (§21.1's
// compile-time classification, §17.14's evaluation rule).
BEZEL_TEST(shift_count_domain_3045, local_const_out_of_domain_rejected) {
    BEZEL_EXPECT_TRUE(rejects("const L: int := 1024 >> 33; return L;"));
    BEZEL_EXPECT_TRUE(accepts("const L: int := 1024 >> 5; return L;"));
}

// An inline-array SIZE (§15.1) is a constant expression too — and it is the one
// constant context the expression typechecker does not visit, so it carries its
// own diagnostic. `[1 << 40]int` used to fold to `[256]int` under the mask.
BEZEL_TEST(shift_count_domain_3045, inline_array_size_out_of_domain_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "var a: [1 << 40]int; return a.Length; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool ok = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("inline-array size shifts by 40") !=
            std::string::npos)
            ok = true;
    BEZEL_EXPECT_TRUE(ok);
    // The in-domain size still folds to a real array type.
    BEZEL_EXPECT_EQ(run("var a: [1 << 4]int; return a.Length;"), 16);
}

// An ENUM member value is a constant expression (§10.2) — same fault, same
// posture. It used to take the masked value (`1 << 40` → 256) silently.
BEZEL_TEST(shift_count_domain_3045, enum_member_out_of_domain_rejected) {
    auto r = runAot(
        "module M; public enum E: int A := 1 << 40; end E; "
        "class C public static method Main(): int return int(E.A); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // An IN-DOMAIN count still folds: `1 << 8` is 256 (an int shift, count 8 in
    // `0..31`), which the member takes as its value. 256 is outside the 8-bit
    // exit channel, so compare in-program.
    auto ok = runAot(
        "module M; public enum E: int A := 1 << 8; end E; "
        "class C public static method Main(): int "
        "return (if int(E.A) = 256 then 1 else 0); end Main; end C;");
    BEZEL_EXPECT_TRUE(ok.ran && !ok.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(ok.exitCode, 1);
}

// ===========================================================================
// D. The compound forms (§6.6) — the same fault on both arms.
// ===========================================================================

BEZEL_TEST(shift_count_domain_3045, compound_const_count_rejected) {
    BEZEL_EXPECT_TRUE(rejects("var x: int := 1; x <<= 32;"));
    BEZEL_EXPECT_TRUE(rejects("var x: int := 1; x >>= 32;"));
    BEZEL_EXPECT_TRUE(rejects("var b: byte := 1; b <<= 8;"));
    BEZEL_EXPECT_TRUE(rejects("var l: long := 1L; l <<= 64;"));
}

BEZEL_TEST(shift_count_domain_3045, compound_runtime_count_raises) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var x: int := 1; var k: int := 32; x <<= k;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var b: byte := 1; var k: int := 8; b <<= k;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var l: long := 1L; var k: int := 64; l >>= k;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var x: int := 1; var k: int := -1; x <<= k;"), 1);
}

// §6.6's desugaring identity: `x <<= k` behaves exactly as `x := x << k` — same
// acceptance, same runtime fault, same in-domain value.
BEZEL_TEST(shift_count_domain_3045, compound_matches_its_desugaring) {
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var x: int := 1; var k: int := 40; x <<= k;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var x: int := 1; var k: int := 40; x := x << k;"), 1);
    BEZEL_EXPECT_TRUE(rejects("var x: int := 1; x <<= 40;"));
    BEZEL_EXPECT_TRUE(rejects("var x: int := 1; x := x << 40;"));
    BEZEL_EXPECT_EQ(run("var x: int := 1; var k: int := 4; x <<= k; return x;"),
                    16);
    BEZEL_EXPECT_EQ(run("var x: int := 1; var k: int := 4; x := x << k; "
                        "return x;"), 16);
}

// §6.6's own example: `var s: short := 1; s <<= 2;` — "R = short (§6.5) — 4, no
// write-back involved". In domain, unchanged.
BEZEL_TEST(shift_count_domain_3045, spec_6_6_example_short_shl_2) {
    BEZEL_EXPECT_EQ(run("var s: short := 1; s <<= 2; return int(s);"), 4);
}

// ===========================================================================
// E. §7.11 lifting — the domain is the UNDERLYING type's, and the check runs
//    only where the operator does (the both-present branch).
// ===========================================================================

BEZEL_TEST(shift_count_domain_3045, nullable_lifted_uses_underlying_domain) {
    // §7.11 / §7.4 (v0.286, #3298): `a` is a local initialised with a non-null
    // literal, so §7.4 narrows it and §7.11 reads it at the non-nullable `byte`
    // — "the ordinary operator applies to it unchanged". The COUNT-DOMAIN rule
    // this row exists for is therefore unchanged and still asserted with its
    // original values: §6.5's domain is the LEFT operand's own width, so `<< 8`
    // on a `byte` faults and `<< 7` gives 128. (A draft of #3298 flipped all
    // three arms to "expect rejection", which deleted #3045's coverage of the
    // domain and was wrong on the spec besides.)
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var a: byte? := 1; var k: int := 8; var c: byte? := a << k;"), 1);
    BEZEL_EXPECT_EQ(run(
        "var a: byte? := 1; var k: int := 7; var c: byte? := a << k; "
        "return int(c!);"), 128);
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var a: int? := 1; var k: int := 32; var c: int? := a << k;"), 1);
}

// §7.11: "the runtime checks each operator already performs … apply only when
// [both operands are present]". A null operand yields null and never faults,
// however out-of-domain the count is.
//
// §7.11 (v0.286, #3298) — REJECTION, and the row keeps its name's point in a
// stronger form: the question "does an absent operand fault?" no longer
// arises, because an UN-narrowed operand (`:= null`, so §7.4 narrows nothing)
// is diagnosed at COMPILE time. §7.11 says exactly that — "This section
// introduces no fault of its own … an absent operand is diagnosed at compile
// time". The narrowed sibling above keeps the runtime-domain coverage.
BEZEL_TEST(shift_count_domain_3045, nullable_null_operand_never_faults) {
    BEZEL_EXPECT_FALSE(runs(
        "var a: byte? := null; var k: int := 40; var c: byte? := a << k; "
        "return (if c = null then 7 else 0);"));
}

// ===========================================================================
// F. The deliberate-masking spelling §6.5 prescribes — this is the migration
//    path for every program the new rule rejects, so it must work exactly.
// ===========================================================================

// §6.5: "Code that wants the modular count deliberately writes it:
// `v << (k & 31)` (`& 63` for 64-bit left operands) — the mask is then in the
// source, visible and in-domain by construction."
BEZEL_TEST(shift_count_domain_3045, masked_count_is_the_documented_remedy) {
    // The value is exactly what the retired implicit mask produced: 40 & 31 = 8.
    // 256 is outside the 8-bit exit channel — observed on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var v: int := 1; var k: int := 40; Console.WriteLine(v << (k & 31)); "
        "return 0; end Main; end C;"), "256");
    // `& 63` for a 64-bit left operand: 65 & 63 = 1 → 2.
    BEZEL_EXPECT_EQ(run(
        "var v: long := 1L; var k: int := 65; return int(v << (k & 63));"), 2);
    // `& 7` for a byte: 9 & 7 = 1 → 2.
    BEZEL_EXPECT_EQ(run(
        "var v: byte := 1; var k: int := 9; var r: byte := v << (k & 7); "
        "return int(r);"), 2);
    // A negative count masks into the domain too: -1 & 31 = 31.
    BEZEL_EXPECT_EQ(run(
        "var v: int := 1; var k: int := -1; "
        "return (if (v << (k & 31)) < 0 then 1 else 0);"), 1);
    // …and the compound form takes the same spelling.
    BEZEL_EXPECT_EQ(run(
        "var v: int := 1; var k: int := 40; v <<= (k & 31); "
        "return (if v = 256 then 1 else 0);"), 1);
}

// A CONSTANT masked count is in-domain by construction, so it folds rather than
// faulting — the constant arm of the same remedy.
BEZEL_TEST(shift_count_domain_3045, masked_constant_count_folds) {
    // 256 is outside the 8-bit exit channel — observed on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "Console.WriteLine(1 << (40 & 31)); return 0; end Main; end C;"),
        "256");
    // …and as a constant-expression ARRAY SIZE, which must fold to [256]int.
    BEZEL_EXPECT_EQ(run("var a: [1 << (40 & 31)]int; return a.Length - 255;"),
                    1);
}

// ===========================================================================
// G. The unchanged half of §6.5 — bits shifted beyond the width are still
//    discarded silently, and an in-domain shift still never faults.
// ===========================================================================

BEZEL_TEST(shift_count_domain_3045, in_domain_shift_never_faults) {
    // byte(200) << 1 == 400, whose bits beyond byte's width are discarded → 144.
    BEZEL_EXPECT_EQ(run(
        "var v: byte := 200; var k: int := 1; var r: byte := v << k; "
        "return int(r);"), 144);
    // int overflow by shifting is likewise not a fault: 1 << 31 is int.MinValue.
    BEZEL_EXPECT_EQ(throwsOverflow(
        "var v: int := 1; var k: int := 31; var r := v << k;"), 0);
    // and a signed `>>` still fills with the sign bit.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var v: int := -8; var k: int := 1; Console.WriteLine(v >> k); "
        "return 0; end Main; end C;"), "-4");
}
