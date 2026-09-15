// Codegen — issues #4281 / #4282 (§7.2 / §8.3 / §8.5 / §9.1 / §14.1, v0.298):
// a NULLABLE record field never combines with "nested inside a non-generic
// outer record" in the existing `generic_record_nullable_462` (6 rows) or
// `generic_record_eq_462` (12 rows) suites (tests/codegen/
// generic_record_with_eq_nullable_462_test.cpp) — those exercise `Box<T>?`
// directly as a LOCAL variable's type, never as another record's FIELD — nor
// in `nested_field_generic_record_4251` (this file's sibling,
// nested_field_generic_record_content_equality_4251_test.cpp), whose every
// nested field is non-nullable. This is exactly the axis both #4281's and
// #4282's repros live on, and this file closes it.
//
// ROOT CAUSE (both issues, ONE defect): `Emitter::emitValueHashCode`
// (LlvmBackend_String.cpp) is the ONLY one of the three nested-field
// recursion sites (`emitValueEquality` / `emitValueHashCode` /
// `emitValueToString`) whose "Nested record" arm was checked BEFORE its
// nullable-ref / nullable-value dispatch, instead of after. Since a nullable
// record field (`Box<string>?`) lowers to the tagged wrapper struct
// `{i8 hasValue, Box$spec0}` (isNullableValueTy/isNullableRecordTy,
// LlvmBackend_Layout.cpp), and that wrapper is ITSELF an `llvm::StructType`,
// the mis-ordered "Nested record" arm's condition (`fieldTy->kind == Named &&
// fieldTy->declaration->kind == Record && val->getType()->isStructTy()`) —
// which never checked `!fieldTy->nullable` — fired directly on the WRAPPER,
// treating it as the bare inner record. The memoized helper it builds
// (`recordHashCodeHelperFn`, fix #4234) then extracted field-index 0 of the
// WRAPPER (the `i8 hasValue` presence tag) believing it was the record's own
// first declared storage member:
//   - issue #4282 (pre-existing, INDEPENDENT of #4251, ZERO generics —
//     `record Mid(M: string); record SoloMid(BS: Mid?)`): `Mid.M`'s declared
//     type resolves to the concrete `string` regardless of any substitution
//     (Mid is not generic), so the mis-extracted `i8` tag was routed straight
//     into the string-content-hash arm and GEP'd as a `bz_string*` — an
//     IR-verifier crash with no generics involved at all, proving the defect
//     is this ORDERING bug, not a gap in #462/#4251's generic-substitution
//     machinery.
//   - issue #4281 (the fix-#4251-era regression, `Box<T>` generic): before
//     #4251, no `RecordSubstGuard` was installed on this path at all, so the
//     mis-extracted tag's declared type (`Box`'s bare `T`) stayed unresolved
//     and fell through to the function's safe `isIntegerTy()` catch-all —
//     WRONG value (a content-independent hash derived from the presence tag,
//     not the field's actual content), but well-typed IR. Once #4251 started
//     installing `RecordSubstGuard` at this SAME (still mis-ordered) arm, `T`
//     resolved to the concrete `string`, and the identical mis-extracted `i8`
//     was routed into the string-content-hash arm instead — upgrading the
//     latent wrong-VALUE bug into an IR-verifier CRASH: #4251 did not create
//     this defect, it changed which of its two failure modes fired.
//
// THE FIX (root, not narrow): reorder `emitValueHashCode` so its
// nullable-ref and nullable-value blocks run BEFORE the "Nested record" arm —
// exactly the order `emitValueEquality` and `emitValueToString` already use
// (confirmed correct: NEITHER of those two is reachable by either issue's
// repro — see the `..._equal`/`..._tostring_shows_value` rows below, which
// pass even on the pre-reorder tree). `isNullableValueTy` already includes
// `isNullableRecordTy` (LlvmBackend_impl.hpp), so this reorder alone fully
// unwraps a nullable record field's tag/payload BEFORE any Record-kind
// dispatch is attempted, for every depth of nesting — `RecordSubstGuard`'s
// own recursive chaining (unchanged from #4251) then does the rest. The
// (now provably unreachable) `!fieldTy->nullable` guard is kept on the
// "Nested record" arm anyway, as a documented invariant rather than one a
// reader must re-derive by tracing the whole function.
//
// Measured on `origin/main`-equivalent pre-this-fix tree (rev/4251 @
// 6876d93, isolated worktree /var/tmp/bezel-scratch/r18/iso-4251-nofix,
// `--version` confirms exact commit, no `-modified`) vs. this fix (isolated
// worktree /var/tmp/bezel-scratch/r18/lane-1... via
// /var/tmp/bezel-scratch/r18/4251/probes/check_rows.sh): 17 of the 22
// exit-code rows below FAIL WITHOUT this fix (15 as an outright IR-verify
// COMPILE CRASH, 2 — the `Box<int>?` and two-level-GENERIC `GetHashCode`
// rows — as a SILENT WRONG VALUE that compiles and runs) and PASS WITH it;
// see each row's own comment for which. All 3 `ToString` rows crash without
// the fix and render correctly with it.

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
// Depth 1 — nullable generic-record field nested in a non-generic outer
// (the #4281 repro shape). `=`/`<>` (§8.4/§9.1), then GetHashCode (§8.3).
// ═══════════════════════════════════════════════════════════════════════════

// The #4281 repro's own headline shape, `=` with distinct-but-equal-content
// pointers (mirrors the sibling file's non-nullable row). FAILS WITHOUT the
// fix: this program crashes at compile time (the synthesized GetHashCode is
// built regardless of whether it is ever called — §9.1 generates all of
// `=`/`Equals`/`GetHashCode`/`ToString` together — so even an equality-only
// program hits the malformed IR). PASSES WITH it.
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_nullable_box_string_equal_distinct_pointers) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var a := \"h\" + \"i\"; "
        "  var x := new SoloStrN(new Box<string>(a)); "
        "  var y := new SoloStrN(new Box<string>(\"hi\")); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

// Negative-content sibling of the above. FAILS WITHOUT (same compile crash).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_nullable_box_string_not_equal_different) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var x := new SoloStrN(new Box<string>(\"hi\")); "
        "  var y := new SoloStrN(new Box<string>(\"bye\")); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 0);
}

// `<>` sibling. FAILS WITHOUT (same compile crash).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_nullable_box_string_notequal_op_content_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var a := \"h\" + \"i\"; "
        "  var x := new SoloStrN(new Box<string>(a)); "
        "  var y := new SoloStrN(new Box<string>(\"hi\")); "
        "  return (if x <> y then 1 else 0); "
        "end Main; end C;"), 0);
}

// GetHashCode, "equal -> equal hash" half (§8.3's hash-equality invariant).
// This half ALONE would be vacuously satisfied by a content-independent
// constant hash — see the `_differs_for_different_values` row below, which
// is the one that actually discriminates. FAILS WITHOUT (compile crash).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_3_nested_nullable_gethashcode_equal_for_equal_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var a := \"h\" + \"i\"; "
        "  var x := new SoloStrN(new Box<string>(a)); "
        "  var y := new SoloStrN(new Box<string>(\"hi\")); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 1);
}

// THE discriminating GetHashCode row (§8.3): different content must NOT hash
// equal. FAILS WITHOUT the fix (this specific program is a hard compile
// crash pre-fix, not merely a wrong-value pass, because the outer field's
// element type is `string`, which routes the mis-extracted tag into the
// GEP-crashing string-hash arm — see this file's header).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_3_nested_nullable_gethashcode_differs_for_different_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var x := new SoloStrN(new Box<string>(\"hi\")); "
        "  var y := new SoloStrN(new Box<string>(\"bye\")); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Depth 1 — the field itself is NULL (§7.2/§7.3), not merely nullable-typed.
// ═══════════════════════════════════════════════════════════════════════════

// Both sides' nullable field is null -> equal (§8.4's null-safe `=`: both-
// null compares equal at every level of nesting, §9.1's per-field recursion
// inherited it). FAILS WITHOUT (same compile crash — the crash is in
// synthesis, not specific to the null/non-null runtime branch).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_7_2_nested_nullable_field_null_both_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var x := new SoloStrN(null); "
        "  var y := new SoloStrN(null); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

// One side null, the other present -> not equal. FAILS WITHOUT (compile
// crash).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_7_2_nested_nullable_field_null_vs_present_not_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var x := new SoloStrN(null); "
        "  var y := new SoloStrN(new Box<string>(\"hi\")); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 0);
}

// Both null -> same sentinel hash (§8.3's hash-equality invariant applied to
// the null case: both-null are Equals-true per §8.4, so they must hash
// identically too — the nullable-value arm's fixed sentinel-0 branch).
// FAILS WITHOUT (compile crash).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_3_nested_nullable_field_null_gethashcode_equal_for_both_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var x := new SoloStrN(null); "
        "  var y := new SoloStrN(null); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// ToString (§8.5/§9.1), present value and null field. Exact rendered output,
// measured (not hand-derived) via runAotOut against this fix's build.
// ═══════════════════════════════════════════════════════════════════════════

// FAILS WITHOUT (compile crash). PASSES WITH it, exact string measured:
// "SoloStrN(BS: Box(V: hi))".
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_5_nested_nullable_box_string_tostring_shows_value) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var t := new SoloStrN(new Box<string>(\"hi\")); "
        "  Console.WriteLine(t.ToString()); return 0; "
        "end Main; end C;"), "SoloStrN(BS: Box(V: hi))");
}

// §18.5's "a null value of a nullable-typed hole renders as the text null"
// rule, applied to a record-component field via §8.5's component-rendering
// clause. FAILS WITHOUT (compile crash). PASSES WITH it, exact string
// measured: "SoloStrN(BS: null)".
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_5_nested_nullable_field_null_tostring_shows_null) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Box<T>(V: T); record SoloStrN(BS: Box<string>?); "
        "class C public static method Main(): int "
        "  var t := new SoloStrN(null); "
        "  Console.WriteLine(t.ToString()); return 0; "
        "end Main; end C;"), "SoloStrN(BS: null)");
}

// ═══════════════════════════════════════════════════════════════════════════
// Sibling axis: a nullable VALUE-typed (not reference-typed) generic
// argument. Box<int>? nested in a non-generic outer. Unlike the `string`
// rows above, this one does NOT crash pre-fix — the mis-extracted `i8` tag
// lands on `emitValueHashCode`'s safe `isIntegerTy()` catch-all instead of
// the GEP-crashing string arm — so it is a SILENT WRONG VALUE pre-fix, not a
// compile failure. Included specifically because that distinction (crash vs.
// silent-wrong) is easy to miss if only the crashing shape is tested.
// ═══════════════════════════════════════════════════════════════════════════

// `=` is UNAFFECTED by this defect at any content — deliberate control per
// LANE_RULES §20: `emitValueEquality`'s own nullable-value dispatch was
// never mis-ordered, so this row PASSES IDENTICALLY on both the pre-fix and
// post-fix tree (measured).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_7_2_nested_nullable_box_int_equal_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloIntN(BI: Box<int>?); "
        "class C public static method Main(): int "
        "  var x := new SoloIntN(new Box<int>(3)); "
        "  var y := new SoloIntN(new Box<int>(3)); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

// THE discriminating row for the value-typed sibling axis: GetHashCode of
// two DIFFERENT-content `Box<int>?` fields must differ. FAILS WITHOUT the
// fix — measured: pre-fix this program COMPILES AND RUNS (no crash) but
// returns 1 (hashes compare EQUAL) because the mis-extracted presence tag
// (always 1 for two present values) is hashed instead of the actual int
// content 3 vs. 99; PASSES WITH the fix (returns 0, hashes differ).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_3_nested_nullable_box_int_gethashcode_differs_for_different_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloIntN(BI: Box<int>?); "
        "class C public static method Main(): int "
        "  var x := new SoloIntN(new Box<int>(3)); "
        "  var y := new SoloIntN(new Box<int>(99)); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Depth axis: TWO levels of nesting, exercised two ways (mirroring the
// sibling #4251 file's own depth-2 axis), each now with a NULLABLE field
// somewhere in the chain.
// ═══════════════════════════════════════════════════════════════════════════

// (a) Two levels of GENERIC nesting, nullable at the OUTER Box<Box<string>>
// level: `Box<Box<string>>?` reached as a non-generic record's field.
// Exercises `RecordSubstGuard` chaining THROUGH the nullable-unwrap
// recursion (the guard must still bind T -> Box<string> for the INNER Box
// once the OUTER wrapper has been correctly unwrapped by this fix's
// reordered nullable-value arm).
//
// `=` is UNAFFECTED at this specific shape — measured PASSING on the
// pre-fix tree too: the mis-extraction here lands on a SCALAR i8 with a
// declared type of Box<string> (a Record kind), whose own Record-kind arm
// requires `isStructTy()` — false for a scalar i8 — so it falls through
// harmlessly to the safe integer catch-all (same class of accidental
// non-crash as the Box<int>? row above, one level of type-nesting later).
// Included as a stated, deliberate control per LANE_RULES §20, not a gap.
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_two_level_nullable_generic_equal_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); "
        "record SoloBoxBoxN(BB: Box<Box<string>>?); "
        "class C public static method Main(): int "
        "  var x := new SoloBoxBoxN(new Box<Box<string>>(new Box<string>(\"hi\"))); "
        "  var y := new SoloBoxBoxN(new Box<Box<string>>(new Box<string>(\"hi\"))); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_two_level_nullable_generic_not_equal_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); "
        "record SoloBoxBoxN(BB: Box<Box<string>>?); "
        "class C public static method Main(): int "
        "  var x := new SoloBoxBoxN(new Box<Box<string>>(new Box<string>(\"hi\"))); "
        "  var y := new SoloBoxBoxN(new Box<Box<string>>(new Box<string>(\"bye\"))); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 0);
}

// THE discriminating row at this depth: GetHashCode DOES silently differ
// pre-fix here (unlike `=` above) — measured: pre-fix this program compiles
// and runs but returns 1 (hashes compare equal for DIFFERENT content,
// because the presence tag — not the Box<string> content — was hashed at
// the outer level); PASSES WITH the fix (returns 0).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_3_nested_two_level_nullable_generic_gethashcode_differs) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); "
        "record SoloBoxBoxN(BB: Box<Box<string>>?); "
        "class C public static method Main(): int "
        "  var x := new SoloBoxBoxN(new Box<Box<string>>(new Box<string>(\"hi\"))); "
        "  var y := new SoloBoxBoxN(new Box<Box<string>>(new Box<string>(\"bye\"))); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}

// (b) Two levels of RECORD nesting through a NON-generic intermediate
// (`Outer(O: Mid)`, `Mid(M: Box<string>?)`) — the NULLABLE generic field is
// reached at the SECOND level of field recursion, through a record (`Mid`)
// that carries no generic params of its own. Confirms the fix's reorder
// chains correctly through an intervening non-generic recursion, not just
// at the immediate top-level field. FAILS WITHOUT the fix: this shape IS a
// hard compile crash pre-fix (the mis-extracted tag here has a declared
// `string` type directly — Mid's own field is Box<string>? whose inner V is
// string — so it routes straight into the GEP-crashing string arm, same as
// the depth-1 rows above, just reached one non-generic hop later).
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_two_level_record_chain_nullable_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record Mid(M: Box<string>?); "
        "record Outer(O: Mid); "
        "class C public static method Main(): int "
        "  var x := new Outer(new Mid(new Box<string>(\"hi\"))); "
        "  var y := new Outer(new Mid(new Box<string>(\"hi\"))); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_3_nested_two_level_record_chain_nullable_gethashcode_differs) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record Mid(M: Box<string>?); "
        "record Outer(O: Mid); "
        "class C public static method Main(): int "
        "  var x := new Outer(new Mid(new Box<string>(\"hi\"))); "
        "  var y := new Outer(new Mid(new Box<string>(\"bye\"))); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Depth axis: THREE levels of RECORD nesting through two non-generic
// intermediates, nullable still at the innermost `Box<string>?`. Confirms
// the fix's reorder (and RecordSubstGuard's pre-existing recursive chaining)
// holds at depth beyond the minimum needed to demonstrate it once.
// ═══════════════════════════════════════════════════════════════════════════

BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_three_level_record_chain_nullable_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record Mid(M: Box<string>?); "
        "record Outer(O: Mid); record Outermost(OO: Outer); "
        "class C public static method Main(): int "
        "  var x := new Outermost(new Outer(new Mid(new Box<string>(\"hi\")))); "
        "  var y := new Outermost(new Outer(new Mid(new Box<string>(\"hi\")))); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_3_nested_three_level_record_chain_nullable_gethashcode_differs) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record Mid(M: Box<string>?); "
        "record Outer(O: Mid); record Outermost(OO: Outer); "
        "class C public static method Main(): int "
        "  var x := new Outermost(new Outer(new Mid(new Box<string>(\"hi\")))); "
        "  var y := new Outermost(new Outer(new Mid(new Box<string>(\"bye\")))); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Issue #4282's own shape: a NON-generic nullable nested record field — ZERO
// generics involved. Proves the root fix (the reorder) closes #4282, not
// just #4281's generic variant: this shape crashed identically on
// `origin/main` BEFORE #4251 ever touched this file, and #4251's
// `RecordSubstGuard` insertion is a no-op here (`Mid` is not generic), so
// only the reorder — not anything generics-specific — fixes it.
// ═══════════════════════════════════════════════════════════════════════════

BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_nullable_nongeneric_equal_issue_4282) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Mid(M: string); record SoloMid(BS: Mid?); "
        "class C public static method Main(): int "
        "  var a := \"h\" + \"i\"; "
        "  var x := new SoloMid(new Mid(a)); "
        "  var y := new SoloMid(new Mid(\"hi\")); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_nullable_nongeneric_not_equal_issue_4282) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Mid(M: string); record SoloMid(BS: Mid?); "
        "class C public static method Main(): int "
        "  var x := new SoloMid(new Mid(\"hi\")); "
        "  var y := new SoloMid(new Mid(\"bye\")); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 0);
}

// THE discriminating row for #4282: this is a hard compile crash pre-fix
// with ZERO generics anywhere in the program — the clearest possible proof
// that the underlying defect is `emitValueHashCode`'s dispatch ORDER, not a
// gap in generic-substitution handling.
BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_3_nested_nullable_nongeneric_gethashcode_differs_issue_4282) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Mid(M: string); record SoloMid(BS: Mid?); "
        "class C public static method Main(): int "
        "  var x := new SoloMid(new Mid(\"hi\")); "
        "  var y := new SoloMid(new Mid(\"bye\")); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}

BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_5_nested_nullable_nongeneric_tostring_shows_value_issue_4282) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Mid(M: string); record SoloMid(BS: Mid?); "
        "class C public static method Main(): int "
        "  var t := new SoloMid(new Mid(\"hi\")); "
        "  Console.WriteLine(t.ToString()); return 0; "
        "end Main; end C;"), "SoloMid(BS: Mid(M: hi))");
}

// ═══════════════════════════════════════════════════════════════════════════
// Regression controls: the ORIGINAL #4251 shape — a NON-nullable nested
// generic-record field — must remain unaffected by this fix's reorder of
// `emitValueHashCode`. These pass IDENTICALLY on the pre-fix and post-fix
// tree (measured) because no nullable field is involved anywhere, so the
// reordered blocks are never entered at all for these two rows.
// ═══════════════════════════════════════════════════════════════════════════

BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_9_1_nested_nonnullable_control_unaffected_issue_4251) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStr(BS: Box<string>); "
        "class C public static method Main(): int "
        "  var a := \"h\" + \"i\"; "
        "  var x := new SoloStr(new Box<string>(a)); "
        "  var y := new SoloStr(new Box<string>(\"hi\")); "
        "  return (if x = y then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nested_nullable_generic_record_4281,
           spec_8_3_nested_nonnullable_control_gethashcode_differs_issue_4251) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T); record SoloStr(BS: Box<string>); "
        "class C public static method Main(): int "
        "  var x := new SoloStr(new Box<string>(\"hi\")); "
        "  var y := new SoloStr(new Box<string>(\"bye\")); "
        "  return (if x.GetHashCode() = y.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 0);
}
