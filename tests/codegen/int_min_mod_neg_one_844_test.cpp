// fix #844 / §6.1: a signed `mod` of int.MinValue (or long.MinValue) by -1 must
// return 0, not raise OverflowException. Per §6.1 OverflowException is raised
// only when "the mathematical result is not representable in the result type";
// INT_MIN mod -1 is mathematically 0, which IS representable. The checked-arith
// codegen shared the INT_MIN/-1 overflow guard between `/` and `mod` — correct
// for division (result -INT_MIN is unrepresentable) but wrong for modulo. The fix
// also avoids the LLVM `srem` UB (and x86 IDIV SIGFPE) for INT_MIN,-1 by forcing
// the divisor to 1 and selecting 0 on that one path.
//
// Value tests — the program return value is observed as the process exit code.
// All expected values here are in 0..255, so they round-trip the 8-bit exit
// status (WEXITSTATUS); runAot compiles+runs the program natively (JIT->AOT
// migration, P1).

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

// int.MinValue mod -1 returns 0 (no OverflowException). The bug18 repro.
BEZEL_TEST(int_min_mod_neg_one_844, int_min_mod_neg_one_is_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int := int.MinValue; var b: int := -1; "
        "  try var c: int := a mod b; "
        "    if c = 0 then return 42; end; return 0; "
        "  catch OverflowException => return 43; end; "
        "end Main; end C;"), 42);
}

// long.MinValue mod -1 (i64) — same UB path, also returns 0.
BEZEL_TEST(int_min_mod_neg_one_844, long_min_mod_neg_one_is_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: long := long.MinValue; var b: long := -1L; "
        "  try var c: long := a mod b; "
        "    if c = 0L then return 42; end; return 0; "
        "  catch OverflowException => return 43; end; "
        "end Main; end C;"), 42);
}

// Division still correctly raises OverflowException for INT_MIN / -1 (§6.1:
// the mathematical result -INT_MIN is not representable).
BEZEL_TEST(int_min_mod_neg_one_844, int_min_div_neg_one_overflows) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int := int.MinValue; var b: int := -1; "
        "  try var c: int := a / b; return c; "
        "  catch OverflowException => return 7; end; "
        "end Main; end C;"), 7);
}

// mod by zero still raises DivideByZeroException (the zero-divisor guard is
// independent of the INT_MIN/-1 fix).
BEZEL_TEST(int_min_mod_neg_one_844, mod_by_zero_still_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int := 5; var b: int := 0; "
        "  try var c: int := a mod b; return c; "
        "  catch DivideByZeroException => return 9; end; "
        "end Main; end C;"), 9);
}

// Ordinary signed mod is unaffected (a non-boundary value, negative dividend).
BEZEL_TEST(int_min_mod_neg_one_844, ordinary_signed_mod_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int := -17; var b: int := 5; "
        "  return a mod b + 100; "          // -17 mod 5 = -2 → 98
        "end Main; end C;"), 98);
}

// Narrowed/nullable path (§7.4 / §7.11): int? mod -1 at int.MinValue also
// returns 0, exercising the sibling codegen site (LlvmBackend_Exprs.cpp
// ~L4064).
//
// BEHAVIOR FLIP (fix #4930, v0.315): this row was spelled as a LIFTED `mod`
// (`var a: int? := int.MinValue; var b: int? := -1; a mod b`), which is the
// retired v0.130/v0.267 behaviour and does not compile — §7.11 (v0.267) admits
// nothing at the operator, and #4930 established that init-narrowing does not
// admit it either. Respelled with the §7.4 null-test narrowing so the
// nullable-payload `mod` lowering this row exists for stays covered. `mod` at
// int.MinValue by -1 is 0 and does NOT raise (§6.1) — the DIVISION sibling
// above is the one that overflows.
BEZEL_TEST(int_min_mod_neg_one_844, nullable_int_min_mod_neg_one_is_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int? := int.MinValue; var c: int? := null; "
        "  try "
        "    if a <> null then c := a mod -1; end if; "
        "    if c = 0 then return 42; end; return 0; "
        "  catch OverflowException => return 43; end; "
        "end Main; end C;"), 42);
}
