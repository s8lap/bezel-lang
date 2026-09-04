// Spec-derived regression test for issue #3700.
//
// Authority: §21.1 (Constants — runtime `const` of a sub-int nullable width
// stores the payload, not the default zero) / §6.3 (Nullable types — the
// `{i8 hasValue; T value}` layout) / §28.3 (Storage — the slot type matches
// the declared type, with discriminator set on a present value), v0.287.
//
// THE DEFECT. A runtime `const` declared with a SUB-INT nullable width
// (`byte?`, `sbyte?`, `short?`, `ushort?`) and an unsuffixed integer
// literal initializer (`const cb: byte? := 3`) used to store the payload as
// `i32 3` into the `{i8 hasValue; i8 value}` slot — 32 bits over a 16-bit
// slot, so the second byte of `3` (0x00) landed in the PAYLOAD slot and the
// discriminator read 3 (truthy because non-zero). Result: `cb!` extracted 0
// (the second byte of the i32), `cb == 3` was false, `cb ?? 9` was 0. Every
// observable property of the const was wrong; the printed value was "0".
//
// The `var` twin (`var cb: byte? := 3`) was correct because the LocalDecl
// handler calls `checkAssignable` (sema/ExprTypecheck.cpp), which re-types
// the unsuffixed `int` literal to the target's payload width via
// `narrowIntConstantToTarget` (§5.1 "Constant narrowing"). After that re-
// type, lowerExpr emits `i8 3` and the codegen wrap path produces
// `{i8 1, i8 3}`. The LocalConstDecl handler did NOT call checkAssignable
// (the var handler's twin call was missing here), so the literal stayed
// typed as `int` and lowerExpr emitted `i32 3`. The codegen wrap path
// (`lowerLocalDecl`, the "CG-nullable" block at Setup.cpp:14643) then ran
// `coerceNumericToType(i32, i8, false)` — widening-only — which returned
// the i32 unchanged, the `inner->getType() == st->getElementType(1)` check
// failed, and the wrap was skipped. The IR was `store i32 3, ptr %slot`
// against an `{i8, i8}` alloca.
//
// MEASURED on the pre-fix HEAD: this file's `byte_payload_round_trip` row
// printed `0`; every other row compiled and printed the expected value
// (the var twin works; non-nullable `const cb: byte := 50` is unaffected
// because the int slot holds the value 50).
//
// THE FIX: add `checkAssignable` to the LocalConstDecl handler at the same
// site the LocalDecl handler already uses, so a `const cb: byte? := 3`
// re-types the literal to byte? via narrowIntConstantToTarget exactly as
// the var path does. The codegen wrap path is left untouched — it already
// handles a correctly-narrowed literal. For compile-time-classified consts
// (`const cb: byte := 50`) the retype is a no-op for a value already in
// range and still in range after retype; for a value out of range it would
// have been rejected at fold time anyway (`fitsInIntType` short-circuits
// the retype). Verified across the six rows below: every one passes with
// the fix and the headline row fails without it.
//
// SECONDARY CARVE-OUT: a `null`-literal initializer is the §21.1 v0.201
// "permanently null" carve-out for runtime const locals — `const X: T :=
// null;` is admissible even for a non-nullable class type T, because the
// binding is permanently null and so never observable as a null-deref
// (a const cannot be re-assigned). The fix therefore SKIPS the
// `checkAssignable` call when the initializer is a `NullLiteralExpr` —
// the §7 strict nullability check inside `checkAssignable` would
// otherwise wrongly reject `const X: T := null;` (this is the regression
// the §21.1 v0.201 `named_type_const_no_longer_rejected` test of
// `runtime_const_locals_2049_test.cpp` exercises). The bare-null / no-
// annotation case (`const X := null;`) is still rejected by the parallel
// §7.1 / §21.1 check at the bottom of the LocalConstDecl handler.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// `runProg` exits the program with the int expression `e`. Used for the
// rows whose observable is a structural check (not a print) so the test
// pins the discriminator AND the payload together — `e` reads BOTH halves
// of the nullable slot and combines them into a single int the test
// framework can compare.
int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── Row 1: issue repro ───────────────────────────────────────────────────────

BEZEL_TEST(runtime_const_nullable_subint_3700, byte_payload_round_trip) {
    // Pre-fix: prints "0". Post-fix: prints "3".
    BEZEL_EXPECT_EQ(runAotOut(
        "module M3700R1; "
        "class C3700R1 "
        "  public static method Main(): int "
        "    const cb: byte? := 3; "
        "    Console.WriteLine(cb!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3700R1;"), "3");
}

// ── Row 2: control — var twin still works (no regression) ───────────────────

BEZEL_TEST(runtime_const_nullable_subint_3700, var_byte_nullable_twin_unregressed) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M3700R2; "
        "class C3700R2 "
        "  public static method Main(): int "
        "    var cb: byte? := 3; "
        "    Console.WriteLine(cb!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3700R2;"), "3");
}

// ── Row 3: control — short? (different sub-int width, same fix path) ───────

BEZEL_TEST(runtime_const_nullable_subint_3700, short_nullable_payload_round_trip) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M3700R3; "
        "class C3700R3 "
        "  public static method Main(): int "
        "    const cs: short? := 256; "
        "    Console.WriteLine(cs!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3700R3;"), "256");
}

// ── Row 4: control — int? (non-narrowing payload width, was always right) ──

BEZEL_TEST(runtime_const_nullable_subint_3700, int_nullable_payload_zero_round_trip) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M3700R4; "
        "class C3700R4 "
        "  public static method Main(): int "
        "    const ci: int? := 0; "
        "    Console.WriteLine(ci!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3700R4;"), "0");
}

// ── Row 5: control — signed sub-int negative value (signed-extension path) ─

BEZEL_TEST(runtime_const_nullable_subint_3700, sbyte_nullable_negative_payload_round_trip) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M3700R5; "
        "class C3700R5 "
        "  public static method Main(): int "
        "    const cb: sbyte? := -3; "
        "    Console.WriteLine(cb!.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3700R5;"), "-3");
}

// ── Row 6: control — null literal (discriminator = 0, payload = 0) ──────────
//
// Pin the `null` form too, so a future regression that "fixes" the payload
// by always writing a `1` discriminator (forgetting that null must be 0)
// fails this row.
BEZEL_TEST(runtime_const_nullable_subint_3700, byte_nullable_null_has_value_zero) {
    // Exit code 1 when `cb` is null (the null-coalesce arm ran); 0 when
    // it's the literal 9 (the bang-extracted arm ran). Pre-fix would
    // behave identically on this row because the null literal is the
    // lowered-null struct {0, 0}, not a plain integer — the path that
    // wrote `i32 3` into the slot is not the null path. The row stays
    // because it pins the negative space.
    BEZEL_EXPECT_EQ(runProg(
        "module M3700R6; "
        "class C3700R6 "
        "  public static method Main(): int "
        "    const cb: byte? := null; "
        "    if cb = null then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C3700R6;"), 1);
}