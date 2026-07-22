// Codegen — §2.7 rule 1 (v0.217, #2097) / §9.2 / §35.7 (v0.210): a BODY record
// whose auto-property is a value-like choice, constructed through an explicit
// constructor whose body assigns the property (`C := c;`). This exercises the
// body-record ctor store path (the inlined constructor body's bare-identifier
// field write), which is the §9.2 documented idiom — the same
// `property X; constructor(x) X := x;` shape body_record_equality_test.cpp
// relies on.
//
// #2097 root cause is NAME RESOLUTION, not choice codegen. The failing repro
// named the record's auto-property `C` AND the enclosing program's class `C`.
// Inside the constructor body the bare `C` in `C := c;` is an implicit-`this.C`
// reference, but ExprTC::lookupName resolved a bare name against the whole
// lexical scope chain — which reaches the MODULE scope holding the top-level
// `class C` — BEFORE consulting the enclosing type's members, so `C` resolved
// to the TYPE. Codegen's implicitFieldAddr then found no storage member for a
// TypeSymbol and the field store was silently dropped: the record kept its
// zero-initialized field, so `new Wrap(Color.Blue)` produced a Red (tag 0)
// value and two distinct constructions compared equal (the issue's "returns 11"
// / expects 22).
//
// Fix (§2.7 rule 1): resolving a bare name walks "local scope, then the current
// module's declarations", and its value result set is "(a local, field, or
// property)" — so an enclosing-type member outranks a same-named top-level TYPE.
// This applies in VALUE position only (a bare name that is not the head of a
// dotted access); a dotted HEAD (`Color.Red`, `Data.M()`) keeps resolving to
// the top-level type, so static access through a same-named unrelated field
// still works (the two ACCEPT pins below fix that scope deliberately).
// local/param still shadow the member (local/param > member > top-level type).
//
// The choice-field tests fail WITHOUT the fix (the dropped store makes every
// construction the zero value) and pass WITH it. Run under generated-code ASan
// (runAotChecked) because the fix concerns the record field's construction /
// memory layout.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §2.7 rule 1 / §9.2 / §35.7 — the issue's EXACT repro: the record's
// auto-property `C` and the program's class `C` collide. Two body-record
// constructions with distinct choice tags (Red vs Blue) must compare UNEQUAL —
// the issue reported them comparing equal (returns 11) because the second
// construction's tag was never stored. With the fix, `a = d` is false → 22.
BEZEL_TEST(codegen_choice_body_record_ctor_store_2097,
           spec_2_7_body_ctor_store_property_class_name_collision) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap "
        "  public property C: Color; "
        "  public constructor(c: Color) C := c; end constructor; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  var a := new Wrap(Color.Red); "
        "  var d := new Wrap(Color.Blue); "
        "  if a = d then return 11; else return 22; end; "
        "end Main; end C;"), 22);
}

// §9.2 / §35.7 — payload-FREE case through the body ctor path, both directions:
// equal (Red == Red) and unequal (Red <> Blue). Property `C` still collides
// with class `C` to keep the #2097 store path under test.
BEZEL_TEST(codegen_choice_body_record_ctor_store_2097,
           spec_9_2_body_ctor_payload_free_equality_both_directions) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap "
        "  public property C: Color; "
        "  public constructor(c: Color) C := c; end constructor; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  var a := new Wrap(Color.Red); var b := new Wrap(Color.Red); "
        "  var d := new Wrap(Color.Blue); "
        "  if a = b and not (a = d) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §9.2 / §35.7 — payload-CARRYING case through the body ctor path. Same active
// case + equal payload → equal; same case + different payload → unequal;
// different case → unequal. Confirms the ctor stores the whole choice value
// (tag AND active payload), not just the tag.
BEZEL_TEST(codegen_choice_body_record_ctor_store_2097,
           spec_9_2_body_ctor_payload_carrying_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap "
        "  public property C: Color; "
        "  public constructor(c: Color) C := c; end constructor; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  var a := new Wrap(Color.Green(7)); "
        "  var b := new Wrap(Color.Green(7)); "
        "  var c := new Wrap(Color.Green(8)); "
        "  var d := new Wrap(Color.Red); "
        "  if a = b and not (a = c) and not (a = d) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §8.3 / §35.7 — hash invariant: equal body-record values (equal stored choice)
// hash equal, and distinct choice tags hash distinct. Reads the stored field
// through GetHashCode.
BEZEL_TEST(codegen_choice_body_record_ctor_store_2097,
           spec_8_3_body_ctor_choice_gethashcode) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap "
        "  public property C: Color; "
        "  public constructor(c: Color) C := c; end constructor; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  var a := new Wrap(Color.Green(7)); var b := new Wrap(Color.Green(7)); "
        "  var d := new Wrap(Color.Blue); "
        "  if a.GetHashCode() = b.GetHashCode() "
        "     and a.GetHashCode() <> d.GetHashCode() then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §9.2 / §35.7 / §3.6 — Dictionary key: a body record whose stored field is a
// value-like choice is equality-bearing and usable as a hash-container key. The
// boxed-equality path must observe the tag the ctor stored: inserting under a
// Green(7) key and looking it up returns the value.
BEZEL_TEST(codegen_choice_body_record_ctor_store_2097,
           spec_9_2_body_ctor_choice_dictionary_key) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap "
        "  public property C: Color; "
        "  public constructor(c: Color) C := c; end constructor; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<Wrap, int>(); "
        "  var k1 := new Wrap(Color.Green(7)); "
        "  d.Add(k1, 100); "
        "  var outv := -1; "
        "  if d.TryGetValue(k1, outv) then return outv; end; "
        "  return -2; "
        "end Main; end C;"), 100);
}

// §2.7 rule 1 — the root cause is name resolution, not choices: a body record
// with a plain `int` auto-property whose name (`C`) collides with the top-level
// `class C` must still store the constructor argument. Without the fix the bare
// `C := c;` resolved to the class type and the store was dropped, so `a.C` read
// back the zero-initialized field (0) instead of 7.
BEZEL_TEST(codegen_choice_body_record_ctor_store_2097,
           spec_2_7_body_ctor_int_property_class_name_collision) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Wrap "
        "  public property C: int; "
        "  public constructor(c: int) C := c; end constructor; "
        "end Wrap; "
        "class C public static method Main(): int "
        "  var a := new Wrap(7); "
        "  return a.C; "
        "end Main; end C;"), 7);
}

// §2.7 rule 1 — a CLASS (not record) instance field whose name collides with a
// top-level type, assigned in the constructor body and read back, confirms the
// resolution fix is not record-specific. `Data` names both the class field and
// the top-level `class Data`; the bare value read `Data` resolves to the field.
BEZEL_TEST(codegen_choice_body_record_ctor_store_2097,
           spec_2_7_class_ctor_field_type_name_collision) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Data end Data; "
        "class Holder "
        "  private Data: int; "
        "  public constructor(v: int) Data := v; end constructor; "
        "  public method Get(): int => Data; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(9); "
        "  return h.Get(); "
        "end Main; end C;"), 9);
}

// §2.7 rule 1 (dotted head, "type-wins") — the narrowing boundary, ACCEPT: a
// dotted HEAD that names both an enclosing-type member and a top-level type
// resolves to the TYPE, so a same-named member does not break static/case
// access through that type. `property Color: Color` collides with `choice
// Color`; inside the record `Color = Color.Red` reads the member on the LEFT
// (value position → property) and resolves `Color.Red` on the RIGHT through the
// choice TYPE (dotted head → type). This works via exprType-based member-access
// resolution; pinned so the value-position member-preference fix never flips
// the head.
BEZEL_TEST(codegen_choice_body_record_ctor_store_2097,
           spec_2_7_dotted_head_member_typed_as_colliding_type_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap "
        "  public property Color: Color; "
        "  public constructor(c: Color) Color := c; end constructor; "
        "  public method IsRed(): bool => Color = Color.Red; "
        "end Wrap; "
        "class Prog public static method Main(): int "
        "  var a := new Wrap(Color.Red); "
        "  var b := new Wrap(Color.Blue); "
        "  if a.IsRed() and not b.IsRed() then return 1; else return 0; end; "
        "end Main; end Prog;"), 1);
}

// §2.7 rule 1 (dotted head, unrelated-typed member) — the narrowing boundary,
// ACCEPT: a member of UNRELATED type whose name collides with a top-level type
// that has a static member. `private Data: int` collides with `class Data`
// (which has `static M()`); inside Holder, the dotted head `Data.M()` resolves
// to the TOP-LEVEL type's static (the field's `int` type cannot host `M`),
// while the bare value read `Data` reads the int field. Applying the member
// preference to a dotted HEAD would flip `Data.M()` to the int field and it
// silently miscompiled to 0 — hence the fix requests member-preference only in
// value position. Result: Data(5) + Data.M()(42) = 47.
BEZEL_TEST(codegen_choice_body_record_ctor_store_2097,
           spec_2_7_dotted_head_unrelated_typed_member_static_access_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Data "
        "  public static method M(): int => 42; "
        "end Data; "
        "class Holder "
        "  private Data: int; "
        "  public constructor(v: int) Data := v; end constructor; "
        "  public method Combined(): int => Data + Data.M(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(5); "
        "  return h.Combined(); "
        "end Main; end C;"), 47);
}
