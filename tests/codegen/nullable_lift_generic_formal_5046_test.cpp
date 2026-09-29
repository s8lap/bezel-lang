// Regression tests for issue #5046 — §5.1 "Nullable conversions" `S? → T?`
// LIFT at a GENERIC `T?` FORMAL: `Id<double>(a)` with `a: int?` and
// `Id<T>(x: T?): T? => x;`.
//
// #4830 taught `wrapArgForNullableValueFormal` (LlvmBackend_Calls2.cpp) to
// wrap a BARE payload or a null pointer into the generic formal's tagged
// `{i8, T}` struct, but left out the case where the argument ARRIVES already
// tagged at a DIFFERENT (narrower) numeric width — an `int?` argument is
// already `{i8, i32}`, and the formal (T bound to `double` at this call) is
// `{i8, double}`. Neither the exact-struct check nor the bare-payload check
// matched, so the struct fell through unwrapped: the specialized body's
// parameter — and its `T?` return, `Id`'s expression body is just `x` — used
// the CALLER's own `{i8, i32}` shape while the call site expected
// `{i8, double}`, and `verifyModule` rejected the mismatch (measured:
// "PHI node operands are not the same type as the result!" over
// `Id<double>(a) ?? -1.0`, `a: int?`).
//
// This is the generic-formal sibling of #4967 (buildOrderedArgs /
// fillOmittedParamDefaults, LlvmBackend_Assign.cpp) and #4940/#4979 (the
// imported-callee and closure-call arms, LlvmBackend_Calls2.cpp) — every one
// of those arms got the S? → T? lift; this arm (generic methods, both the
// block-body `emitGenericSpecialization` path and the expression-body
// `emitInlineGenericMethod` path, both of which route the argument through
// `wrapArgForNullableValueFormal`) had not.
//
// spec §5.1 "Implicit conversions" (int → double, int → long) and "Nullable
// conversions" ("S? implicitly converts to T? whenever S implicitly converts
// to T ... a present value converts by the underlying conversion, null
// converts to null"), §14.1 (generics), §22.6 stage 3, v0.307.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 / §14.1 (v0.307), fix #5046 — the issue's repro verbatim, present
// and null, consumed directly by `??` (the exact shape whose PHI merge
// `verifyModule` rejected pre-fix: the non-null arm produced the stray
// `{i8, i32}` extractvalue, the null-arm default `-1.0` is `double`).
BEZEL_TEST(nullable_lift_generic_formal_5046, int_nullable_lift_double_nullable_coalesce) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method Id<T>(x: T?): T? => x; "
        "  public static method Main(): int "
        "    var a: int? := 5; var z: int? := null; "
        "    Console.WriteLine(Id<double>(a) ?? -1.0); "
        "    Console.WriteLine(Id<double>(z) ?? -1.0); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "5\n-1\n");
}

// per §5.1 / §14.1 (v0.307), fix #5046 — the same pair, unwrapped through an
// explicit null test and `!` rather than `??`, and combined with a further
// arithmetic op to pin that the lifted PAYLOAD is a real, usable `double`
// (5.25), not merely a value that happens to print as "5" (e.g. a
// bit-reinterpreted i32 read back through the wrong accessor).
BEZEL_TEST(nullable_lift_generic_formal_5046, int_nullable_lift_double_nullable_unwrap) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method Id<T>(x: T?): T? => x; "
        "  public static method Main(): int "
        "    var a: int? := 5; "
        "    var r: double? := Id<double>(a); "
        "    if r = null then "
        "      Console.WriteLine(\"null\"); "
        "    else "
        "      Console.WriteLine(r! + 0.25); "
        "    end if; "
        "    var z: int? := null; "
        "    var r2: double? := Id<double>(z); "
        "    if r2 = null then "
        "      Console.WriteLine(\"null\"); "
        "    else "
        "      Console.WriteLine(r2!); "
        "    end if; "
        "    return 0; "
        "  end Main; "
        "end P;",
        "5.25\nnull\n");
}

// per §5.1 / §14.1 (v0.307), fix #5046 — a distinct integer width pair
// (`int? → long?`) so a fix that hard-codes int→double would still not pass
// this row.
BEZEL_TEST(nullable_lift_generic_formal_5046, int_nullable_lift_long_nullable_coalesce) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method Id<T>(x: T?): T? => x; "
        "  public static method Main(): int "
        "    var a: int? := 9; var z: int? := null; "
        "    Console.WriteLine(Id<long>(a) ?? -7); "
        "    Console.WriteLine(Id<long>(z) ?? -7); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "9\n-7\n");
}

// per §5.1 / §14.1 (v0.307), fix #5046 — a SAME-width generic instantiation
// (`int? → int?`, T bound to the argument's own type) still passes through:
// regression pin that the new struct-lift arm does not fire (and does not
// mis-tag the hasValue byte) when no widening is actually needed.
BEZEL_TEST(nullable_lift_generic_formal_5046, int_nullable_same_width_generic_passthrough) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method Id<T>(x: T?): T? => x; "
        "  public static method Main(): int "
        "    var a: int? := 42; var z: int? := null; "
        "    Console.WriteLine(Id<int>(a) ?? -1); "
        "    Console.WriteLine(Id<int>(z) ?? -1); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "42\n-1\n");
}

// per §5.1 / §14.1 (v0.307), fix #5046 review follow-up — an UNSIGNED source
// (`uint?`) lifted through a generic `T?` formal bound to `long` must
// ZERO-extend, not sign-extend. `3000000000` does not fit a 32-bit signed
// int (its bit pattern is negative as `int`), so treating the source as
// signed — as an earlier version of this fix's `wrapArgForNullableValueFormal`
// unconditionally did — sign-extends that bit pattern into the 64-bit
// negative value -1294967296 instead of the correct 3000000000: silent wrong
// code, not merely a rejected/uncovered shape. Pins the `isUnsignedTargetExpr`
// threading through `emitMethodCall` -> `emitInlineGenericMethod` ->
// `wrapArgForNullableValueFormal`, mirroring the closure-call sibling fix
// #4979's `uint_nullable_at_closure_long_nullable_zero_extends`. Plus the
// null case, so the fix cannot special-case only the present value.
BEZEL_TEST(nullable_lift_generic_formal_5046, uint_nullable_lift_long_nullable_zero_extends) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method Id<T>(x: T?): T? => x; "
        "  public static method Main(): int "
        "    var u: uint? := 3000000000; var z: uint? := null; "
        "    Console.WriteLine(Id<long>(u) ?? -1); "
        "    Console.WriteLine(Id<long>(z) ?? -1); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "3000000000\n-1\n");
}
