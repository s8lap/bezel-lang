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

// §9.2 / §14.1: `Box<int?>`'s auto-property `D: T := 5` default fires (the
// constructor supplies X/Y only), and D's ACTUAL type (T = int?) is a
// present, non-null nullable value. Spec-correct: reading `d ?? -1` yields
// the wrapped default `5`. Pre-fix this read back `0` (the issue's own
// measured repro, verbatim).
BEZEL_TEST(generic_record_body_field_default_nullable_3944,
           present_nullable_default_reads_back) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(X: T, Y: int) "
        "  public readonly property D: T := 5; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(3, 1); "
        "  var d := b.D; "
        "  if d = null then return 111; end; "
        "  return d ?? -1; "
        "end Main; end C;"), 5);
}

// Control: the SAME shape instantiated at a non-nullable `T = int` (the
// bare-type-param/nullable-substitution combination is NOT exercised here —
// `isNullableVal` correctly sees a plain `int` on both the bare and resolved
// spellings). Isolates the defect to the generic+nullable-instantiation
// combination: this control passed both before and after the fix.
BEZEL_TEST(generic_record_body_field_default_nullable_3944,
           control_non_nullable_instantiation_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(X: T, Y: int) "
        "  public readonly property D: T := 5; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(3, 1); "
        "  return b.D; "
        "end Main; end C;"), 5);
}

// §9.2 / §14.1: a plain (positional) body FIELD default — not just an
// auto-property — reaches the SAME loop (`ast::FieldDecl` vs
// `ast::PropertyDecl` in buildRecordValue's dispatch) and the same
// generic+nullable-default hazard.
BEZEL_TEST(generic_record_body_field_default_nullable_3944,
           plain_field_default_also_fixed) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(X: T, Y: int) "
        "  public F: T := 7; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(3, 1); "
        "  var f := b.F; "
        "  if f = null then return 111; end; "
        "  return f ?? -1; "
        "end Main; end C;"), 7);
}
