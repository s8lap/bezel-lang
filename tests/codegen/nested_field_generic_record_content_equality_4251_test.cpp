// Codegen — issue #4251 (§9.1 / §14.1 / §4.6 / §8.3, v0.298): a NON-generic
// record whose FIELD is a generic-record instantiation over a reference type
// (`string`) lost §4.6 content-equality through the record `=` / `Equals` /
// `GetHashCode` / `ToString` path — it fell back to reference-type default
// behaviour even though the field's true type (a generic record over
// `string`) has value semantics.
//
// ROOT CAUSE: issue #462's fix installs `Emitter::RecordSubstGuard` (the
// generic type-parameter substitution, `genericSubst_`/`genericSubstSemaTy_`)
// only at the TOP-LEVEL `=`/`Equals` dispatch (`LlvmBackend_Exprs.cpp`'s
// `eqSubst`). A non-generic record's field of a generic-record type never
// goes through that dispatch — it is reached via the NESTED-FIELD recursion
// inside `emitValueEquality` / `emitValueHashCode` / `emitValueToString`
// (`LlvmBackend_Layout.cpp` / `LlvmBackend_String.cpp`), which — before this
// fix — never installed ANY guard. `recordMemberTypeResolved` therefore
// resolved the generic field's own `T`-typed storage member bare/unresolved
// (`Named`, no `declaration`), which:
//   - `emitValueEquality`'s string-content arm requires `fieldTy->kind ==
//     Primitive`, which an unresolved `T` never satisfies, so it fell to the
//     raw-LLVM-shape fallback: `ICmpEQ` on two `bz_string*` pointers —
//     REFERENCE equality, not §4.6 content equality.
//   - `emitValueHashCode` has no arm at all for an unresolved `Named`/no-decl
//     type; it fell all the way to the function's trailing catch-all,
//     `return builder_.getInt32(0)` — a CONTENT-INDEPENDENT CONSTANT. Two
//     records differing ONLY in that nested string field hash identically,
//     which makes a bare `GetHashCode()`-equal-for-equal-values assertion
//     VACUOUS against this bug — see the `_differs_for_different_values`
//     rows below, which are the ones that actually catch it.
//
// This reproduces identically before AND after #4234 (confirmed by the
// issue's own revert-and-rebuild of #4234's two touched files); it is a gap
// in #462, not a #4234 regression.
//
// THE FIX: install a `RecordSubstGuard` keyed on the field's own (already
// concrete) `TypeRef` immediately before each nested-field recursion site
// builds/calls its record-comparison/hash/format helper (the three call
// sites `#4234` added memoized helpers for:
// `recordEqualityHelperFn`/`recordHashCodeHelperFn`/`recordToStringHelperFn`,
// `LlvmBackend_Setup.cpp`). A no-op for a non-generic nested record. Because
// `RecordSubstGuard`'s own constructor resolves each of ITS generic args
// through whatever OUTER substitution is currently active before
// overwriting it, this also chains correctly through multiple levels of
// nesting (a generic record nested inside another generic record's own
// field, or a chain of non-generic records around the innermost generic
// one) — both exercised below.
//
// Measured on `origin/main` base f8b12ba (pre-fix) via
// /var/tmp/bezel-scratch/r18/4251/probes/repro_4251.bzl (mirrors the issue's
// own repro): `SoloStr t1=t2: False` (should be True) and
// `SoloStr t1.hash=t2.hash: True` / `t1.hash=t3.hash: True` (t3 has DIFFERENT
// content — should be False; both compared True because the hash was the
// constant 0). Post-fix: `True` / `True` / `False` respectively.
//
// Scope note: the SAME nested-field-recursion gap reproduces for a
// non-generic record's field of a GENERIC CHOICE type (e.g.
// `record Solo(R: Result<string, int>)`), through a DIFFERENT code path
// (`emitChoiceEquality`/`emitChoiceHashCode`/`emitChoiceToString`, which
// #4234 never wrapped in a memoized helper). That is a distinct fix (a
// guard would need installing at those choice call sites, which this issue
// does not name) and is filed separately; it is NOT touched here.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// `=` / `<>` — content equality through the nested field (§9.1 / §4.6)
// ═══════════════════════════════════════════════════════════════════════════

// Headline repro (matches the issue verbatim): `SoloStr(Box(a)) =
// SoloStr(Box("hi"))` where `a` is a DISTINCT pointer built at runtime to the
// SAME content ("h"+"i"). FAILS WITHOUT the fix (compares the two `bz_string`
// pointers by identity → false); PASSES WITH it (content compare → true).
BEZEL_TEST(nested_field_generic_record_4251,
           spec_9_1_nested_box_string_equal_distinct_pointers) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStr(BS: Box<string>); "
        "class C public static method Main(): int "
        "  var a := \"h\" + \"i\"; "
        "  var x := new SoloStr(new Box<string>(a)); "
        "  var y := new SoloStr(new Box<string>(\"hi\")); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

// Sibling of the above: content-DIFFERENT nested string fields must compare
// unequal. This row is unaffected by the bug either way (a raw pointer
// ICmpEQ on two genuinely different allocations is ALSO false) — included as
// the paired negative control so the positive row above is not read in
// isolation.
BEZEL_TEST(nested_field_generic_record_4251,
           spec_9_1_nested_box_string_not_equal_different) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStr(BS: Box<string>); "
        "class C public static method Main(): int "
        "  var x := new SoloStr(new Box<string>(\"hi\")); "
        "  var y := new SoloStr(new Box<string>(\"bye\")); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 0);
}

// `<>` sibling of the headline row (both operators route through the same
// `emitStructEquality`/nested-record-arm path; #462/#4251 do not
// special-case `<>`, but a dedicated row pins it rather than assuming).
BEZEL_TEST(nested_field_generic_record_4251,
           spec_9_1_nested_box_string_notequal_op_content_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStr(BS: Box<string>); "
        "class C public static method Main(): int "
        "  var a := \"h\" + \"i\"; "
        "  var x := new SoloStr(new Box<string>(a)); "
        "  var y := new SoloStr(new Box<string>(\"hi\")); "
        "  return (if x <> y then 1 else 0); "        // content-equal -> <> is 0
        "end Main; end C;"), 0);
}

// Control: `Box<int>` (a VALUE type, not a reference type) was already
// correct pre-fix (a raw `int` ICmpEQ already IS content comparison) — this
// row passes IDENTICALLY before and after the fix. Included as a
// regression-safety control per LANE_RULES §20 (an unaffected comparison,
// stated as such) so the axis "generic-over-string vs generic-over-int" is
// pinned, not just the broken half.
BEZEL_TEST(nested_field_generic_record_4251,
           spec_9_1_nested_box_int_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloInt(BI: Box<int>); "
        "class C public static method Main(): int "
        "  var x := new SoloInt(new Box<int>(3)); "
        "  var y := new SoloInt(new Box<int>(3)); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// `GetHashCode` — BOTH directions (§8.3). The "equal -> equal hash" half
// alone is VACUOUS against this bug (a content-independent constant hash
// trivially satisfies it); the "different -> different hash" half is the
// one that actually discriminates a constant from a real content hash.
// ═══════════════════════════════════════════════════════════════════════════

BEZEL_TEST(nested_field_generic_record_4251,
           spec_8_3_nested_gethashcode_equal_for_equal_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStr(BS: Box<string>); "
        "class C public static method Main(): int "
        "  var a := \"h\" + \"i\"; "
        "  var x := new SoloStr(new Box<string>(a)); "
        "  var y := new SoloStr(new Box<string>(\"hi\")); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 1);
}

// THE row that pins the "content-independent constant" defect: FAILS
// WITHOUT the fix (both sides hash to the same constant -> returns 1, wrong)
// and PASSES WITH it (distinct content -> distinct hash -> returns 1 via the
// `<>` branch below reading correctly... — the assertion is direct: hashes
// of DIFFERENT-content values must differ, i.e. NOT be equal).
BEZEL_TEST(nested_field_generic_record_4251,
           spec_8_3_nested_gethashcode_differs_for_different_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStr(BS: Box<string>); "
        "class C public static method Main(): int "
        "  var x := new SoloStr(new Box<string>(\"hi\")); "
        "  var y := new SoloStr(new Box<string>(\"bye\")); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// `ToString` — the third nested-field recursion site (§8.5 / §9.1). Renders
// the record with its field listing; the nested `Box<string>` field must
// render its OWN listing with the CONTENT ("hi"), not garbage from an
// unresolved bare type parameter. Measured exact output (not derived by
// hand): "SoloStr(BS: Box(V: hi))", length 23.
// ═══════════════════════════════════════════════════════════════════════════

BEZEL_TEST(nested_field_generic_record_4251,
           spec_8_5_nested_box_string_tostring_shows_value) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Box<T>(V: T); record SoloStr(BS: Box<string>); "
        "class C public static method Main(): int "
        "  var t := new SoloStr(new Box<string>(\"hi\")); "
        "  Console.WriteLine(t.ToString()); return 0; "
        "end Main; end C;"), "SoloStr(BS: Box(V: hi))");
}

// ═══════════════════════════════════════════════════════════════════════════
// Depth axis: TWO levels of nesting, exercised two ways.
// ═══════════════════════════════════════════════════════════════════════════

// (a) Two levels of GENERIC nesting: `Box<Box<string>>` reached as a
// non-generic record's field. Exercises `RecordSubstGuard` chaining through
// TWO nested-field recursions (outer `Box<string>` binds T->string for the
// INNER Box; #4234's memoized helpers are keyed per-instantiation
// `llvm::StructType*`, so the outer and inner `Box` calls must each get
// their OWN correctly-scoped guard, not share one).
BEZEL_TEST(nested_field_generic_record_4251,
           spec_9_1_nested_two_level_generic_box_box_string_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); "
        "record SoloBoxBox(BB: Box<Box<string>>); "
        "class C public static method Main(): int "
        "  var x := new SoloBoxBox(new Box<Box<string>>(new Box<string>(\"hi\"))); "
        "  var y := new SoloBoxBox(new Box<Box<string>>(new Box<string>(\"hi\"))); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nested_field_generic_record_4251,
           spec_9_1_nested_two_level_generic_box_box_string_not_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); "
        "record SoloBoxBox(BB: Box<Box<string>>); "
        "class C public static method Main(): int "
        "  var x := new SoloBoxBox(new Box<Box<string>>(new Box<string>(\"hi\"))); "
        "  var y := new SoloBoxBox(new Box<Box<string>>(new Box<string>(\"bye\"))); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(nested_field_generic_record_4251,
           spec_8_3_nested_two_level_generic_gethashcode_differs) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); "
        "record SoloBoxBox(BB: Box<Box<string>>); "
        "class C public static method Main(): int "
        "  var x := new SoloBoxBox(new Box<Box<string>>(new Box<string>(\"hi\"))); "
        "  var y := new SoloBoxBox(new Box<Box<string>>(new Box<string>(\"bye\"))); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}

// (b) Two levels of RECORD nesting through a NON-generic intermediate
// (`Outer(O: Mid)`, `Mid(M: Box<string>)`) — the generic instantiation is
// only reached at the SECOND level of field recursion, through a record
// (`Mid`) that carries no generic params of its own. Confirms the fix does
// not depend on the immediately-enclosing record being generic.
BEZEL_TEST(nested_field_generic_record_4251,
           spec_9_1_nested_two_level_record_chain_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record Mid(M: Box<string>); "
        "record Outer(O: Mid); "
        "class C public static method Main(): int "
        "  var x := new Outer(new Mid(new Box<string>(\"hi\"))); "
        "  var y := new Outer(new Mid(new Box<string>(\"hi\"))); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nested_field_generic_record_4251,
           spec_8_3_nested_two_level_record_chain_gethashcode_differs) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record Mid(M: Box<string>); "
        "record Outer(O: Mid); "
        "class C public static method Main(): int "
        "  var x := new Outer(new Mid(new Box<string>(\"hi\"))); "
        "  var y := new Outer(new Mid(new Box<string>(\"bye\"))); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}
