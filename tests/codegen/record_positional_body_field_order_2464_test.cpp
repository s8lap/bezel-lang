// fix #2464 (§9.1, §32.13 clause 2, v0.235): a combined-form record's
// AUTO-GENERATED PRIMARY constructor (positional fields, no explicit `public
// constructor`) silently under-constructed — or outright failed to
// construct — the moment the record ALSO declared a body-level storage
// member (a field or auto-property with its own initializer).
//
// ROOT CAUSE (established, not inferred): `collectRecordStorage`/`t->members`
// order a record's BODY storage members ahead of its POSITIONAL fields,
// regardless of source declaration order (SymbolCollection.cpp's
// collectTypeDecl always walks `td.members` — the body — BEFORE appending
// `td.positionalFields`). `Emitter::buildRecordValue`'s positional fast path
// (LlvmBackend_Exprs2.cpp) gated on `args.size() == fields.size()` — i.e.
// "the supplied constructor argument count equals the TOTAL storage member
// count" — but per §9.1 the auto-generated primary constructor's parameters
// are "the positional fields, in declaration order" ONLY; body storage is
// never part of that parameter list. So `args.size()` can equal
// `fields.size()` (the total, positional+body) only when the record has NO
// body storage at all — the instant a combined-form record adds body
// storage, the fast path's gate fails and construction fell through to a
// path that looks for an EXPLICIT constructor via `ctorForArgs` — which does
// not exist for the compiler-synthesized primary constructor — so
// `buildRecordValue` returned nullptr and the positional fields were left at
// their zero-initialized default, silently, with no diagnostic.
//
// This is a LIVE defect independent of any cross-module import or of issue
// #2456 (a private/internal field's cross-module layout visibility) — every
// test below is a SINGLE-FILE, non-cross-module repro. It was found while
// investigating #2464 (filed as a cross-module-only "latent" gap during
// #2456's work); this file demonstrates the SAME `buildRecordValue` root
// cause is already reachable purely locally, with no import needed at all.
//
// THE FIX: `buildRecordValue` now (a) unconditionally zeros the struct and
// runs every body-declared field/auto-property initializer BEFORE attempting
// the positional fast path (previously that step ran only on fallthrough to
// the explicit-constructor path, so a combined-form record taking the fast
// path never got its body storage initialized at all), and (b) computes the
// positional-field subset as the TRAILING `args.size()` entries of `fields`
// — never `fields[i]` by raw index — since the primary constructor's arity
// always equals the record's true positional-field count for a
// sema-approved call, and positional fields are always a contiguous trailing
// block in `fields`/`t->members` (every positional field is auto-generated
// PUBLIC, hence always present; body storage is always collected first).
//
// Every expected value below is composed by ADDITION of distinct, non-zero
// components (never multiplication) so the sum cannot arise by coincidence
// from a slot swap, a dropped field, or a masked/truncated exit code
// (`runAot`'s exitCode is WEXITSTATUS-masked to 8 bits — every sum here
// stays well under 256).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// ===========================================================================
// T1 — CONTROL: a POSITIONAL-ONLY record (no body storage at all). Must keep
// working exactly as before the fix — the fast path's gate degenerates back
// to the pre-fix `args.size() == fields.size()` case since fields.size() ==
// posFields.size() when there is no body storage.
// ===========================================================================
BEZEL_TEST(codegen_record_positional_body_field_order_2464,
           positional_only_record_control_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point2464(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point2464(3, 4); return p.x + p.y; "
        "end Main; end C;"), 7);
}

// ===========================================================================
// T2 — the #2464 repro shape, single-file: ONE positional field + ONE body
// FIELD with its own initializer, NO explicit constructor. Pre-fix: `x`
// silently stayed 0 (buildRecordValue's fast path never fired since
// args.size()=1 != fields.size()=2; the fallback found no explicit ctor and
// returned nullptr, leaving the local's storage at its zero default).
// Post-fix: x=9, H=70 -> 79.
// ===========================================================================
BEZEL_TEST(codegen_record_positional_body_field_order_2464,
           combined_form_one_positional_one_body_field_no_ctor_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Rec2464a(x: int) "
        "  private _h: int := 70; "
        "  public property H: int => _h; "
        "end Rec2464a; "
        "class C public static method Main(): int "
        "  var r := new Rec2464a(9); return r.x + r.H; "
        "end Main; end C;"), 79);
}

// ===========================================================================
// T3 — same shape as T2, but the body storage is an AUTO-PROPERTY with a
// default initializer (`property Z: int := 50;`) rather than a private
// field + getter. Covers the PropertyDecl::autoInitializer branch of the
// hoisted zero+init loop, distinct from FieldDecl::initializer (T2).
// ===========================================================================
BEZEL_TEST(codegen_record_positional_body_field_order_2464,
           combined_form_one_positional_one_body_autoprop_no_ctor_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Vec2464(x: int) "
        "  public property Z: int := 50; "
        "end Vec2464; "
        "class C public static method Main(): int "
        "  var v := new Vec2464(11); return v.x + v.Z; "
        "end Main; end C;"), 61);
}

// ===========================================================================
// T4 — TWO positional fields + ONE body field. Exercises the trailing-slice
// computation with a positional block wider than one element, so an
// off-by-one in the slice boundary (fields.end() - args.size()) would show
// up as a wrong `a` or `b`, not just a wrong body value.
// ===========================================================================
BEZEL_TEST(codegen_record_positional_body_field_order_2464,
           combined_form_two_positional_one_body_field_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R2464b(a: int, b: int) "
        "  private _c: int := 100; "
        "  public property C: int => _c; "
        "end R2464b; "
        "class C public static method Main(): int "
        "  var r := new R2464b(1, 2); return r.a + r.b + r.C; "
        "end Main; end C;"), 103);
}

// ===========================================================================
// T5 — ONE positional field + THREE body fields (each with its own private
// storage + accessor property), no explicit constructor. Stresses the
// hoisted zero+init loop across MULTIPLE non-positional storage members —
// each must be independently initialized and independently addressable via
// fieldIndex_, regardless of how many body members precede the positional
// trailing block.
// ===========================================================================
BEZEL_TEST(codegen_record_positional_body_field_order_2464,
           combined_form_one_positional_three_body_fields_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R2464c(a: int) "
        "  private _p: int := 10; "
        "  private _q: int := 20; "
        "  private _r: int := 30; "
        "  public property P: int => _p; "
        "  public property Q: int => _q; "
        "  public property R: int => _r; "
        "end R2464c; "
        "class C public static method Main(): int "
        "  var r := new R2464c(5); return r.a + r.P + r.Q + r.R; "
        "end Main; end C;"), 65);
}

// ===========================================================================
// T6 — CONTROL: a BODY-ONLY record (no positional fields at all), no
// explicit constructor, constructed with ZERO arguments. Must be unaffected
// by the fix — `!args.empty()` is false, so the positional fast path (and
// its trailing-slice logic) is never entered at all; construction goes
// straight to the hoisted zero+init step, exactly as it did before.
// ===========================================================================
BEZEL_TEST(codegen_record_positional_body_field_order_2464,
           body_only_record_zero_arg_construction_control_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Bod2464 "
        "  public property W: int := 42; "
        "end Bod2464; "
        "class C public static method Main(): int "
        "  var b := new Bod2464(); return b.W; "
        "end Main; end C;"), 42);
}

// ===========================================================================
// T7 — NESTED records: a combined-form OUTER record whose single positional
// field is itself a record value (Inner2464), plus a body field on the
// outer. Confirms the trailing-slice fix composes with a positional field
// whose own construction/storage is itself record-typed (recordMemberType /
// recordMemberTypeResolved still resolve correctly against `posFields[i]`,
// not the stale `fields[i]`).
// ===========================================================================
BEZEL_TEST(codegen_record_positional_body_field_order_2464,
           nested_record_positional_field_with_body_field_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner2464(v: int); "
        "record Outer2464(i: Inner2464) "
        "  private _tag: int := 8; "
        "  public property Tag: int => _tag; "
        "end Outer2464; "
        "class C public static method Main(): int "
        "  var o := new Outer2464(new Inner2464(4)); "
        "  return o.i.v + o.Tag; "
        "end Main; end C;"), 12);
}

// ===========================================================================
// T8 — NAMED-ARGUMENT construction (§22.5) on a combined-form record: the
// named-arg matching loop was also rewritten to search `posFields` (not
// `fields`) for the argument's target name. Proves the name lookup still
// finds the positional field correctly when body storage precedes it in
// `fields`.
// ===========================================================================
BEZEL_TEST(codegen_record_positional_body_field_order_2464,
           combined_form_named_argument_construction_spec_9_1_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Named2464(x: int) "
        "  private _h: int := 15; "
        "  public property H: int => _h; "
        "end Named2464; "
        "class C public static method Main(): int "
        "  var r := new Named2464(x := 6); return r.x + r.H; "
        "end Main; end C;"), 21);
}

// ===========================================================================
// T9 — an INLINE-ARRAY positional field alongside a body field, exercising
// the special CollectionLiteralExpr in-place-build path (§9.1/§15.4 #1129)
// inside the fast path with `posFields[i]` (not `fields[i]`) driving the
// `fieldIndex_` GEP. Sums the array elements plus the body field.
// ===========================================================================
BEZEL_TEST(codegen_record_positional_body_field_order_2464,
           combined_form_inline_array_positional_with_body_field_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Arr2464(xs: [3]int) "
        "  private _extra: int := 6; "
        "  public property Extra: int => _extra; "
        "end Arr2464; "
        "class C public static method Main(): int "
        "  var a := new Arr2464([1, 2, 3]); "
        "  return a.xs[0] + a.xs[1] + a.xs[2] + a.Extra; "
        "end Main; end C;"), 12);
}
