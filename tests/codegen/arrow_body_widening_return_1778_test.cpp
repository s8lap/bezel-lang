// Regression for #1778 / §5.1: an implicit numeric WIDENING in the return of an
// EXPRESSION-BODIED (arrow) method was not lowered. `method M(): long => 42`
// emitted the bare `ret i32 42` into an `i64`-returning function — malformed
// LLVM IR (verifier failure: "return type does not match operand type") for
// VALID code. The BLOCK form (`... return 42; end M;`) and the lambda paths
// already coerce the return value via coerceNumericToType; the method arrow-body
// codegen path (LlvmBackend_Setup.cpp) emitted the body value directly as the
// `ret` operand WITHOUT that coercion. The fix applies the same
// coerceNumericToType the ReturnStmt path uses (LlvmBackend_Stmts.cpp) before
// emitReturn.
//
// §5.1 widenings the arrow path was dropping (all covered below): int -> long,
// signed int -> long (sign-extend), uint -> long (zero-extend), int -> double,
// float -> double, byte -> int.
//
// Fails-WITHOUT the fix: malformed IR is rejected by the verifier, the program
// never compiles/runs (ran == false), so run() yields the -999 sentinel and
// runOut() yields "<did-not-run>". Passes-WITH: the widened value is correct.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Small-value channel: `M`'s widened result fits an 8-bit exit code. `Main`
// narrows it back with the explicit `int(...)` of §5.2 (the widened value fits,
// so the checked narrowing never overflows) so a `long`-returning `M` can be
// observed through `Main`'s `int` exit code. run() returns -999 if the program
// failed to run — the without-fix outcome, since `M`'s malformed IR makes the
// whole program un-runnable.
int run(const std::string& members) {
    auto r = runAot("module M; class C " + members +
                    " public static method Main(): int => int(C.M()); end C;");
    return r.ran ? r.exitCode : -999;
}

// Wide-value channel: values outside 0..255 (and the double results) cannot
// round-trip an 8-bit exit code, so the program PRINTS the widened result and
// the test checks captured stdout (Console.WriteLine, stdlib §2.1; appends \n).
std::string runOut(const std::string& members) {
    auto r = runAot("module M; class C " + members +
                    " public static method Main(): int "
                    "  Console.WriteLine(C.M()); return 0; end Main; end C;");
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// §5.1: int literal widened to a `long` return — the exact malformed-IR case
// from the bug report (`ret i32 42` into an i64 function).
BEZEL_TEST(arrow_body_widening_return_1778, int_to_long_small) {
    BEZEL_EXPECT_EQ(run("public static method M(): long => 42;"), 42);
}

// The widened value must be preserved for a magnitude that only fits in `long`
// (an int-typed source value > 255, sign-extended, not truncated).
BEZEL_TEST(arrow_body_widening_return_1778, int_to_long_wide) {
    BEZEL_EXPECT_EQ(runOut("public static method M(): long => 2000000000;"),
                    "2000000000\n");
}

// §5.1: a NEGATIVE int widened to `long` must sign-extend (SExt), not zero-fill.
BEZEL_TEST(arrow_body_widening_return_1778, int_to_long_negative) {
    BEZEL_EXPECT_EQ(runOut("public static method M(): long => -7;"), "-7\n");
}

// §5.1: an UNSIGNED int (`u` suffix) widened to `long` must zero-extend (ZExt) —
// 3000000000u exceeds int.MaxValue, so a sign-extend would corrupt it.
BEZEL_TEST(arrow_body_widening_return_1778, uint_to_long) {
    BEZEL_EXPECT_EQ(runOut("public static method M(): long => 3000000000u;"),
                    "3000000000\n");
}

// §5.1: an integer widened to `double` (SIToFP).
BEZEL_TEST(arrow_body_widening_return_1778, int_to_double) {
    BEZEL_EXPECT_EQ(runOut("public static method M(): double => 42;"), "42\n");
}

// §5.1: the one binary-float widening, `float` -> `double` (FPExt).
BEZEL_TEST(arrow_body_widening_return_1778, float_to_double) {
    BEZEL_EXPECT_EQ(runOut("public static method M(): double => 1.5f;"),
                    "1.5\n");
}

// §5.1: a `byte` value widened to `int` (ZExt) — narrower-integer -> int.
BEZEL_TEST(arrow_body_widening_return_1778, byte_to_int) {
    BEZEL_EXPECT_EQ(run("public static method M(): int => byte(200);"), 200);
}

// Control: the BLOCK form (which already coerced correctly) still works — the
// fix must not regress it.
BEZEL_TEST(arrow_body_widening_return_1778, block_form_still_widens) {
    BEZEL_EXPECT_EQ(
        run("public static method M(): long return 42; end M;"), 42);
}

// Control: an already-matching (non-widening) arrow return is unchanged —
// coerceNumericToType is a no-op when the widths already agree.
BEZEL_TEST(arrow_body_widening_return_1778, exact_width_unchanged) {
    BEZEL_EXPECT_EQ(run("public static method M(): int => 200;"), 200);
}
