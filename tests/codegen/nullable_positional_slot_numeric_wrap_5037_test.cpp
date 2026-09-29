// Regression tests for issue #5037 (§5.1 "Implicit conversions" / §9.1
// "Positional fields", v0.307) — the nullable positional-slot wrap silently
// produced the wrong VALUE for a narrower numeric argument, instead of
// applying §5.1's conversion rules.
//
// WHAT THE DEFECT WAS. A `new` call's positional (or named) argument into a
// NULLABLE VALUE field (`X: T?`) must arrive at the field's tagged
// `{i8 hasValue; T value}` slot with the argument converted to `T` per §5.1
// — a widening zero-extends an unsigned source, sign-extends a signed one,
// and an integer converts to a binary float via [S/U]IToFP. Two sites —
// `Emitter::buildRecordValue`'s positional/named-argument wrap
// (LlvmBackend_Exprs2.cpp) and its #4722a copy in
// `Emitter::storePrimaryCtorPositionalArgs` (the primary-ctor `this(...)`
// chain store) — instead hand-rolled the conversion as a bare
// `CreateSExtOrTrunc(v, valTy)`, gated only on "both operands are integers":
//
//   * `CreateSExtOrTrunc` always SIGN-extends when widening, so an UNSIGNED
//     narrower source (`ulong? x` from a `uint 4000000000` argument) read
//     back sign-extended (`18446744073414584320`) instead of the correct
//     zero-extended `4000000000` — §5.1's table makes uint→ulong a
//     zero-extending widening.
//   * the int-int gate had no int→float arm at all, so `double? x` from an
//     `int 5` argument left `v` as a bare i32; the later `coerceNumericToType`
//     call is a no-op between two already-mismatched types, so a raw
//     `store i32` landed on the `{i8 hasValue; double}` slot, corrupting
//     both halves — read back as `0`.
//
// THE FIX. Both sites now route the payload through
// `Emitter::wrapNullablePositionalPayload` (LlvmBackend_Layout.cpp), which
// composes the SAME `coerceInitializerToSlot` (§5.1 widen/narrow, int→float)
// + `coerceReturnToNullableValue` (wrap into the tagged struct) sequence the
// instance field-initializer path already used for the identical shape —
// one conversion body instead of a third hand-rolled copy.
//
// Rows below pin, all asserting the printed VALUE:
//   1. `ulong? <- uint 4000000000`, DIRECT `new`, class and record (site 1,
//      `buildRecordValue`) — the issue's headline repro.
//   2. `ulong? <- uint 4000000000`, through a `this(...)` CHAIN to the
//      primary constructor, class and record (site 2,
//      `storePrimaryCtorPositionalArgs`) — the #4722a copy.
//   3. `long? <- int -5` — a SIGNED narrower source must still read back
//      negative (a no-regression control: `CreateSExtOrTrunc` happened to
//      get this shape right, and the fix must not flip it).
//   4. `double? <- int 5` — the issue's sibling defect (no int-float arm).
//   5. `int? <- byte 200` — a distinct integer pair (unsigned, narrower than
//      the field, `byte` rather than `uint`).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// The issue's headline repro, DIRECT `new` — site 1 (`buildRecordValue`),
// class and record both. Pre-fix the class row printed
// `18446744073414584320` (sign-extended); the record row hit the identical
// wrap in the same function.
BEZEL_TEST(codegen_nullable_positional_slot_numeric_wrap_5037,
           ulong_from_uint_direct_new_zero_extends_spec_5_1) {
    auto r = runAot(
        "module M; "
        "class C(x: ulong?) "
        "  public method Show(): string => $\"{x}\"; "
        "end C; "
        "record R(X: ulong?) "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    var u: uint := 4000000000; "
        "    Console.WriteLine(new C(u).Show()); "
        "    var r := new R(u); "
        "    Console.WriteLine($\"{r.X}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4000000000\n4000000000\n");
}

// The #4722a copy — site 2 (`storePrimaryCtorPositionalArgs`), reached via a
// `this(...)` chain to the primary constructor, class and record both. An
// explicit constructor forces `new C(u)` / `new R(u)` off the direct fast
// path (site 1) and through the chain store (site 2) instead.
BEZEL_TEST(codegen_nullable_positional_slot_numeric_wrap_5037,
           ulong_from_uint_this_chain_zero_extends_spec_9_3) {
    auto r = runAot(
        "module M; "
        "class C(x: ulong?) "
        "  public constructor(u: uint) this(u); end constructor; "
        "  public method Show(): string => $\"{x}\"; "
        "end C; "
        "record R(X: ulong?) "
        "  public constructor(u: uint) this(u); end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int "
        "    var u: uint := 4000000000; "
        "    Console.WriteLine(new C(u).Show()); "
        "    var r := new R(u); "
        "    Console.WriteLine($\"{r.X}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4000000000\n4000000000\n");
}

// A SIGNED narrower source: `long? <- int -5` must stay `-5`. Both
// `CreateSExtOrTrunc` and the fix sign-extend a signed source identically —
// this is the no-regression control pinning the fix did not flip the signed
// direction while correcting the unsigned one.
BEZEL_TEST(codegen_nullable_positional_slot_numeric_wrap_5037,
           long_from_negative_int_stays_signed_spec_5_1) {
    auto r = runAot(
        "module M; "
        "class C(x: long?) "
        "  public method Show(): string => $\"{x}\"; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(-5).Show()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-5\n");
}

// The issue's sibling defect: `double? <- int 5` prints `0` both before and
// after #4722a, because the hand-rolled wrap's int-int gate has no int->float
// arm. Expected `5` (Bezel's double ToString renders an integral value
// without a trailing decimal point, per the landed #4922 sibling fix's own
// `G(f, 7)` row: a `float 2` argument converted to `double?` prints "2").
BEZEL_TEST(codegen_nullable_positional_slot_numeric_wrap_5037,
           double_from_int_widens_spec_5_1) {
    auto r = runAot(
        "module M; "
        "class C(x: double?) "
        "  public method Show(): string => $\"{x}\"; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new C(5).Show()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// A distinct unsigned-narrower pair: `int? <- byte 200`. `byte` is a
// different unsigned width than `uint` (1 byte vs 4), pinning the fix's
// zero-extension is not special-cased to the headline `uint`/`ulong` pair.
BEZEL_TEST(codegen_nullable_positional_slot_numeric_wrap_5037,
           int_from_byte_widens_spec_5_1) {
    auto r = runAot(
        "module M; "
        "class C(x: int?) "
        "  public method Show(): string => $\"{x}\"; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    var b: byte := 200; "
        "    Console.WriteLine(new C(b).Show()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}
