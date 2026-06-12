// Sema tests for §9.8 serialization synthesis (issue #550, tier 1).
//
// Covers: synthesis-eligibility checks — records and enums that declare
// ISerializable / IDeserializable and are eligible get no sema error; those
// that are ineligible get a compile error AT the conformance declaration
// (§9.8 "A type that declares `: ISerializable` or `: IDeserializable` but
// is not serialization-eligible is a compile error at the conformance
// declaration, naming the first offending member and the reason.").
//
// Spec: §9.8 (synthesis-eligibility), stdlib §2.14.
//
// What is serialization-eligible (§9.8):
//   - Enums: always eligible.
//   - Positional records: every positional field's type is eligible.
//   - Body records: auto-equality-bearing (§9.2 conditions) AND every
//     auto-property's type is eligible.
//   - Eligible component types: primitives (bool, int family, float, double,
//     decimal, char), string, enum, another eligible record.
//   - A field marked `serialized skip` is excluded from the component set,
//     so it does not disqualify the type even if its type is ineligible.
//
// What is NOT (tier 1 — classes without serialized-class clause still rejected):
//   - A positional record with a borrow-bearing (class-typed) field whose
//     type does not declare a serialized-class clause → ineligible.
//   - A class type without a `serialized tree`/`graph`/`cyclic` clause
//     is NOT serialization-eligible (§9.8: "a serialization-eligible
//     synthesized class" requires the clause).
//
// ISerializable / IDeserializable are auto-imported with System (stdlib §2.14);
// BinarySerializer/BinaryDeserializer live in System.Serialization (§2.15,
// explicit import required — not tested here since it is codegen-tier).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

// ─── Eligible: enums are always serialization-eligible (§9.8) ────────────────

// §9.8: an enum declaring ISerializable is always eligible — no error.
BEZEL_TEST(sema_serialization_550, enum_iserializable_eligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "enum Priority: ISerializable "
        "  Low, Medium, High; "
        "end Priority;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.8: an enum declaring IDeserializable is always eligible — no error.
BEZEL_TEST(sema_serialization_550, enum_ideserializable_eligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "enum Color: IDeserializable "
        "  Red, Green, Blue; "
        "end Color;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.8: an enum declaring both ISerializable and IDeserializable — eligible.
BEZEL_TEST(sema_serialization_550, enum_both_eligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "enum Status: ISerializable, IDeserializable "
        "  Active, Inactive; "
        "end Status;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── Eligible: positional records with primitive fields ───────────────────────

// §9.8: a positional record whose fields are primitives (int, string) is
// serialization-eligible.
BEZEL_TEST(sema_serialization_550, positional_record_primitives_eligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "record Point(X: int, Y: int): ISerializable, IDeserializable;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.8: string field — eligible component type.
BEZEL_TEST(sema_serialization_550, positional_record_string_field_eligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "record Named(Name: string, Age: int): ISerializable, IDeserializable;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.8: all primitive types are eligible component types.
BEZEL_TEST(sema_serialization_550, positional_record_all_primitives_eligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "record Primitives(B: bool, C: char, I: int, L: long, F: float, "
        "  D: double): ISerializable;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.8: an enum field is an eligible component type.
BEZEL_TEST(sema_serialization_550, positional_record_enum_field_eligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "enum Status Active, Inactive; end Status; "
        "record Item(Name: string, St: Status): ISerializable, IDeserializable;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.8: nested record (another serialization-eligible record) is eligible.
BEZEL_TEST(sema_serialization_550, positional_record_nested_record_eligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "record Point(X: int, Y: int): ISerializable, IDeserializable; "
        "record Line(A: Point, B: Point): ISerializable, IDeserializable;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.8 / ISerializable only (no IDeserializable) — still eligible and no error.
BEZEL_TEST(sema_serialization_550, iserializable_only_no_error_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "record Point(X: int, Y: int): ISerializable;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── Eligible: body records with auto-properties ──────────────────────────────

// §9.8 / §9.2: a body record that is auto-equality-bearing AND whose
// auto-properties are all primitive types is serialization-eligible.
BEZEL_TEST(sema_serialization_550, body_record_auto_props_eligible_spec_9_8) {
    // §9.8: a body record whose auto-properties are all primitive types is
    // serialization-eligible.  Base-type conformances go on the record header.
    auto r = parseSema(
        "module M; "
        "record Pt: ISerializable, IDeserializable "
        "  public property X: int; "
        "  public property Y: int; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end Pt;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── Ineligible: class-typed field without serialized-class clause ────────────

// §9.8: a positional record with a CLASS-typed field (not a serialized-class)
// is NOT serialization-eligible. A class without a `serialized tree`/`graph`/
// `cyclic` clause is not a "serialization-eligible synthesized class" (§9.8).
BEZEL_TEST(sema_serialization_550, positional_record_class_field_ineligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "class C end C; "
        "record R(X: int, Obj: C): ISerializable;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §9.8: another ineligible case — record whose nested record is NOT itself
// ISerializable/IDeserializable but is used as a field type.
// A record field type is eligible only if that record is itself
// serialization-eligible (§9.8: "another serialization-eligible record").
BEZEL_TEST(sema_serialization_550, positional_record_non_serializable_record_field_ineligible_spec_9_8) {
    auto r = parseSema(
        "module M; "
        // Inner does NOT declare ISerializable/IDeserializable — not eligible
        "record Inner(V: int); "
        "record Outer(A: Inner, B: int): ISerializable;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ─── serialized skip removes ineligible field from consideration (§9.8) ───────

// §9.8: "`serialized skip` interacts with the eligibility rule by *removing*
// its component from consideration: a field that would otherwise make the
// type ineligible … but is marked `serialized skip` does not disqualify the
// type."
BEZEL_TEST(sema_serialization_550, serialized_skip_removes_ineligible_component_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "class C end C; "
        // C is a class (not eligible), but marked skip — must NOT disqualify
        "record R(X: int, Obj: C serialized skip): ISerializable;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── No ISerializable declaration → no synthesis, no error ────────────────────

// §9.8 suppression: a record that does NOT declare ISerializable or
// IDeserializable gets no synthesis and no eligibility error. The `serialized`
// clause on the field is a separate compile error (§9.8: "A `serialized` clause
// on a member of a type that declares no serialization conformance … is a
// compile error."). But the ELIGIBILITY error itself must not fire.
// (We test only that a plain record with no conformance declaration compiles.)
BEZEL_TEST(sema_serialization_550, plain_record_no_conformance_no_error_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "class C end C; "
        // C is ineligible but no ISerializable declared → no eligibility error
        "record R(X: int, Obj: C);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── Suppression: hand-written Serialize suppresses synthesis (§9.8) ──────────

// §9.8: a record with a hand-written `Serialize` method suppresses synthesis
// on that direction, so the record is always valid even if ineligible, provided
// it supplies its own implementation. A class-typed field no longer disqualifies.
BEZEL_TEST(sema_serialization_550, hand_written_serialize_suppresses_synthesis_spec_9_8) {
    auto r = parseSema(
        "module M; "
        "class C end C; "
        "record R(X: int, Obj: C): ISerializable "
        "  public method Serialize(s: ISerializer) "
        "  end Serialize; "
        "end R;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ─── §2.15 v0.139 / issue #675: `serialized "$..."` rename is a compile error ─

// §2.15 v0.139: a positional record field with a `serialized "$xxx"` rename is
// a compile error — `$`-initial wire names are reserved for JSON graph-identity.
// Syntax: `Name: Type serialized "wireName"` (SerializedClause follows the type).
BEZEL_TEST(sema_serialization_550,
           dollar_rename_positional_field_is_error_spec_2_15_v0139) {
    auto r = parseSema(
        "module M; "
        "record R(X: int serialized \"$foo\"): ISerializable, IDeserializable;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §2.15 v0.139: a non-`$` rename is still accepted (no regression).
BEZEL_TEST(sema_serialization_550,
           non_dollar_rename_positional_field_is_ok_spec_2_15_v0139) {
    auto r = parseSema(
        "module M; "
        "record R(X: int serialized \"myField\"): ISerializable, IDeserializable;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
