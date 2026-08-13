// Spec-derived regression tests for fix #3217 (R0 WRONG_VALUE) — a negative
// `long` const initializer was emitted at i32 then zero-extended to i64,
// reading 4294967256 instead of -40.
//
// Authority:
//   §4.2 (Integer types) — an unsuffixed integer literal without a suffix has
//   type `int` if it fits; the const initializer `-40` is typed `int`, but the
//   constant narrowing of §5.1 admits it at any wider target whose range
//   contains the value (signed `long` contains `-40`).
//   §5.1 (Implicit conversions / Constant narrowing) — a compile-time
//   constant integer expression implicitly converts to any integer type whose
//   range contains its value; the source may be `int`, the target any
//   integer type, signedness preserved by the sign of the source.
//   §22.12 (Static members) — a class-level `const` initializer must be a
//   compile-time-constant expression and is materialized at each read site
//   (§22.8 / issue #328 FINDING [3] — there is no storage global for a
//   compile-time-baked const; the value is FOLDED at the read).
//
// Bug shape (single-module, no import): `public const NEGL: long := -40;`
// lowerUnary's §6.1 fold emits the literal as `i32 -40` (== 0xFFFFFFD8), and
// the `i32 → i64` widening into the long-typed read was unsigned (LLVM's
// global-init coercion / a CreateZExt somewhere), reading 4294967256.
//
// Fix: constMemberValue (LlvmBackend_Setup.cpp:431) sign-aware-widens the
// initializer value to the const member's declared LLVM type before
// returning. For a `long`-typed const holding the int literal -40, the
// result is the i64 constant -40 (not the i64 constant 4294967256).
//
// Harness: runAot (forked Driver::compileFile + clang link + native exec).
// Each test returns its value through Console.WriteLine, captured via
// runAotOut — exitCode is 8-bit (WEXITSTATUS), but the values here exceed
// it (4294967256 wraps to 232 / 40 fits but a sibling would too). Stdout
// is the value channel and is unambiguous.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Print the value of K.<field>, the field is a class-level const of the
// specified type with the specified initializer. Returns the program's
// stdout trimmed of trailing newline (runAotOut).
std::string readConstField(const char* ty, const char* value) {
    return runAotOut(std::string(
        "module M; static class K public const V: ") + ty + " := " + value +
        "; end K; static class Main public static method Main(): int "
        "  Console.WriteLine(K.V.ToString()); return 0; "
        "end Main; end M;");
}

}  // namespace

// §4.2 / §5.1 / §22.12: the bug row. -40 in a `long` const must read -40,
// NOT 4294967256 (the i32 zero-extension of 0xFFFFFFD8). This is the
// exact defect shape from the issue's "minimal repro".
BEZEL_TEST(neg_const_long_3217, neg_long_const_reads_minus_40) {
    BEZEL_EXPECT_EQ(readConstField("long", "-40"), "-40");
}

// §5.1 positive twin: a positive int-literal const widening to `long`
// must read 40. The sign-aware fix path also covers the unsigned-extension
// shape (zext) — without it a blanket `rej_neg` would pass this row but
// miss the actual bug, so this row pins that the fix doesn't blanket-reject.
BEZEL_TEST(neg_const_long_3217, pos_long_const_reads_40) {
    BEZEL_EXPECT_EQ(readConstField("long", "40"), "40");
}

// §4.2 / §5.1: a `long` const whose initializer is the upper boundary
// (just below 2^31) — pins that an int-natural literal widens without
// truncation at the upper edge of the i32 range. Without widening, this
// would be 0 or 4294967295 (an i32 -1 zero-extended).
BEZEL_TEST(neg_const_long_3217, upper_int_boundary_long_const) {
    BEZEL_EXPECT_EQ(readConstField("long", "2147483647"), "2147483647");
}

// §4.2 / §5.1: a `long` const whose initializer exceeds int's range,
// which §4.2 typed-context widening admits at `long` (3_000_000_000 is
// the boundary example). The value fits in long's range, so the const
// reads 3000000000. Without the widening, an i32 fold would either
// reject (compile error) or read a truncated 0/-something.
BEZEL_TEST(neg_const_long_3217, above_int_range_long_const) {
    BEZEL_EXPECT_EQ(readConstField("long", "3000000000"), "3000000000");
}

// §4.2: a NEGATIVE `int` const must continue to read its declared value
// (no widening needed — int-to-int). This is the sibling that proves the
// fix doesn't blanket-rewrite all negative consts to long.
BEZEL_TEST(neg_const_long_3217, neg_int_const_unaffected) {
    BEZEL_EXPECT_EQ(readConstField("int", "-40"), "-40");
}

// §4.2 boundary carve-out: long.MinValue written as `-9223372036854775808L`
// (the L-suffixed boundary literal). The literal is typed `long` directly
// (the §4.2 carve-out admits the 2^63 magnitude as the immediate operand of
// unary `-` only). This pins that the fix's sext path doesn't perturb a
// literal that is already at the correct width.
BEZEL_TEST(neg_const_long_3217, long_min_value_literal_const) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; static class K "
            "  public const V: long := -9223372036854775808L; "
            "end K; static class Main public static method Main(): int "
            "  Console.WriteLine(K.V.ToString()); return 0; "
            "end Main; end M;"),
        "-9223372036854775808");
}

// §4.2 / §22.12 (issue #328 FINDING [3] BUG A side): the IMPORTED
// const path (`foldImportedConstText`) has always taken the width from
// the const's primitive type, so it does NOT regress. This row pins
// that a single-module negative const with the same shape is now
// equivalent to the imported shape — a single regression test would
// otherwise miss any drift between the two paths.
BEZEL_TEST(neg_const_long_3217, neg_int_const_reads_minus_3_in_pair) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; static class K "
        "  public const NEG: int := -3; "
        "  public const NEGL: long := -40; "
        "end K; static class Main public static method Main(): int "
        "  Console.WriteLine(K.NEG.ToString()); "
        "  Console.WriteLine(K.NEGL.ToString()); "
        "  return 0; "
        "end Main; end M;"),
        "-3\n-40");
}
