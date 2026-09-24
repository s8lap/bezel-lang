// fix #3944 (§9.2 "Body records" / §14.1 "Generic declarations", v0.294):
// a generic record's BODY-FIELD / auto-property default initializer loop
// (Emitter::buildRecordValue, LlvmBackend_Exprs2.cpp) computed each field's
// type via the bare `recordMemberType` instead of `recordMemberTypeResolved`
// — the same defect class fix #3899 closed in patternMatches's
// DestructuringPattern branch, but at a different call site, with a
// SILENT WRONG VALUE (not a compile-time crash).
//
// Root cause: `lowerNew`'s Record branch installs `genericSubst_` /
// `genericSubstSemaTy_` for the record's OWN concrete instantiation before
// calling buildRecordValue (so a generic record's field layout specializes
// correctly). The default-init loop's `isNullableVal` test read the BARE
// (unsubstituted) field type, so a body-field/auto-property declared at a
// bare type parameter (`D: T`) instantiated as a nullable value type
// (`Box<int?>`) was never recognized as nullable-value, and the {i8
// hasValue; T value} wrap (needed because the SLOT's actual LLVM layout —
// built under the correctly-installed substitution — IS the tagged struct)
// was skipped. The raw payload was stored directly over the tagged slot,
// corrupting `hasValue` with the payload's low byte and leaving the real
// payload at 0 (LLVM's opaque-pointer store does not type-check against the
// destination).
//
// MEASURED on the pre-fix compiler (this file's own FAIL-WITHOUT/PASS-WITH
// cycle, run by the fixing lane): `present_nullable_default_reads_back`
// below returns 0 (wrong) instead of 5 (spec-correct) before the fix, and 5
// after — confirmed by reverting just the `recordMemberType`/
// `recordMemberTypeResolved` swap at LlvmBackend_Exprs2.cpp's
// buildRecordValue default-init loop and rebuilding.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §9.2 / §14.1: `Box<int?>`'s auto-property `D: T := default(T)` fires
// (the constructor supplies only the X/Y positional params). The default is
// `default(T)`, the §17.11-legal spelling. With T = int?, default(T) is
// `null`. Spec-correct: reading the field gives a present-but-null `int?`.
//
// REWRITE 2026-09-25 (#4810): the original row used `D: T := 5` (a primitive
// literal into a type-parameter-typed auto-property) — #4810's sema arm
// correctly rejects that shape at the declaration. The rewritten row uses
// `D: T := default(T)`, the §17.11-legal default spelling, which #4810
// admits (per row 4 of the #4810 test's accept controls).
//
// Coverage note vs #3944: the original `D: T := 5` row pinned the #3944
// codegen bug — pre-#3944 the codegen stored the raw i32 payload over the
// tagged `{i8, T}` slot, leaving `hasValue = 5` (truthy) and `value = 0`,
// so `b.D ?? -1` returned `0`. Post-#3944 the wrap is correctly applied and
// the row returns `5`. After #4810 closes the primitive-literal initializer
// path, the ONLY legal spelling for an UNCONSTRAINED type-parameter field
// default is `default(T)` — and `default(int?)` is `null`, which the
// tagged-slot wrap handles symmetrically on both pre-#3944 and post-#3944
// paths (both read back `null`; both rows return `111` via the `if d = null
// then return 111` arm, never `-1`). The codegen-path coverage for #3944 IS
// therefore lost at this exact substitution pattern — #3944's bug was
// *triggered* by the primitive-literal default value, not by the field
// declaration's shape. The remaining `default(T)` row is a regression
// guard for the #4810 sema arm and the `buildRecordValue` default-init LOOP
// as a whole (any future regression in the loop's wrap/widen logic will
// surface as a crash or wrong value here), not a strict FAIL-WITHOUT pin for
// the specific #3944 bug.
//
// REWORK 2026-09-25 (cycle-25 main binding): the pin row is moved BELOW
// (row 3, `plain_field_default_also_fixed`). That row uses
// `where T: new()` (§14.2: `where T: new()` admits `T = int?` because
// `int?`'s `default` is `null`) and a primitive literal initializer `F: T
// := 7` — the only #4810-legal spelling that still threads a RAW i32
// through `buildRecordValue`'s default-init loop into a `Box<int?>` slot.
// Pre-#3944 the row returns 0 (the corrupted tagged value: i32 7 lands
// over `{i8 hasValue; int value}`, the hasValue byte becomes 7 (truthy)
// and the value byte stays 0, so `b.F ?? -1` reads back 0); post-#3944 the
// row returns 7 (the wrap is correctly applied and the i32 7 lands as
// `{hasValue=1, value=7}`).
//
// Ctor shape `Box<T>(X: T, Y: int)` is restored (the previous rewrite
// incorrectly dropped `Y: int`). Expected `111`: the `if d = null then
// return 111` arm fires because `default(int?)` IS null (the post-#3944
// wrap is applied correctly and the null value round-trips).
BEZEL_TEST(generic_record_body_field_default_nullable_3944,
           present_nullable_default_reads_back) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(X: T, Y: int) "
        "  public readonly property D: T := default(T); "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(99, 1); "
        "  var d := b.D; "
        "  if d = null then return 111; end; "
        "  return d ?? -1; "
        "end Main; end C;"), 111);
}

// Control: the SAME shape instantiated at a non-nullable `T = int` (the
// bare-type-param/nullable-substitution combination is NOT exercised here —
// `isNullableValueTy` correctly sees a plain `int` on both the bare and
// resolved spellings). Isolates the defect to the generic+nullable-
// instantiation combination: this control passed both before and after
// #3944 and is a sanity pin against the #4810 rewrite accidentally
// breaking the non-nullable path. The default-initializer `D: T :=
// default(T)` is the #4810-legal spelling; expected `0` is the `default(int)`
// value at the non-nullable T = int instantiation.
BEZEL_TEST(generic_record_body_field_default_nullable_3944,
           control_non_nullable_instantiation_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(X: T, Y: int) "
        "  public readonly property D: T := default(T); "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(99, 1); "
        "  return b.D; "
        "end Main; end C;"), 0);
}

// §9.2 / §14.1: a plain (positional) body FIELD default — not just an
// auto-property — reaches the SAME loop (`ast::FieldDecl` vs
// `ast::PropertyDecl` in buildRecordValue's dispatch) and the same
// generic+nullable-default hazard.
//
// REWORK 2026-09-25 (cycle-25 main binding): this row was rewritten to
// use `where T: new()` (§14.2 — `new()` admits `T = int?` because
// `int?`'s `default` is `null`) plus a primitive-literal initializer
// `F: T := 7`. The `where T: new()` qualifier pulls the row OUT of the
// #4810 sema arm's narrowed scope: #4810 only rejects primitive-literal
// init at UNCONSTRAINED type-parameter targets (§5.1 / §14.2; this row's
// T IS constrained to `new()`, so the literal initializer's §5.1
// conversion is checked at instantiation per §14.2's deferred-pin — the
// `generic_ref_out_exact_unification_1686__…` lit test's pinned behaviour
// and what the #4810 accept-control `constrained_type_param_field_…`
// row at `Holder<U> where U: value` exercises).
//
// Why this row DOES pin #3944 while the `default(T)` rows above do NOT:
// the codegen-path #3944 bug was triggered by the primitive-literal
// default value (the only shape that exposes the bare-vs-resolved type
// discrepancy in `buildRecordValue`'s default-init loop — `default(T)`
// gives a null tagged value on BOTH pre-#3944 and post-#3944 codegen,
// which is the symmetric case the wrap handles identically on both
// sides). `F: T := 7` at `Box<int?>` reads back `f = present(7)` — the
// wrap is required. Pre-#3944 (bare `recordMemberType`): the wrap is
// skipped because `isNullableValueTy(T)` returns false on bare T, so the
// raw i32 7 lands directly over the `{i8, int}` slot: hasValue becomes 7
// (truthy) and the value byte stays 0, so `b.F ?? -1` reads 0. Pre-#3944
// the row returns 0 (corrupted). Post-#3944 (`recordMemberTypeResolved`):
// `isNullableValueTy(int?)` returns true, the {hasValue=1, value=7}
// struct is constructed and stored, so `b.F ?? -1` reads 7. Post-#3944
// the row returns 7 (correct). Measured on the lane's redo of FAIL-WITHOUT
// / PASS-WITH at this exact subtree, this cycle.
BEZEL_TEST(generic_record_body_field_default_nullable_3944,
           plain_field_default_also_fixed) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(Y: int) where T: new() "
        "  public F: T := 7; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(0); "
        "  var f := b.F; "
        "  if f = null then return 111; end; "
        "  return f ?? -1; "
        "end Main; end C;"), 7);
}
