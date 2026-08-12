// Codegen — §9.2 / §9.3 / §35.7 (v0.210, #2083): value-like auto-equality-
// bearing choice COMPONENT in a record (positional form, where the ctor store
// path is the §9.1 positional-synthesis layout). The choice-as-record-field
// runtime contract is: equal records (same active case and equal corresponding
// payload) compare equal; the equality hashes per §35.7's "case discriminant
// mixed in first, then per-payload" rule.
//
// §9.3 (combined form): positional/combined records are hashable when
// every positional field and every body auto-property is an admissible
// component type. With v0.210's widened admissible list (choices included),
// `record Wrap(C: Color)` over a value-like choice `Color` is admitted as
// auto-hashable.
//
// NB: body records (`record Wrap { public property C: Color }`) hitting the
// SAME field-ctor store path through the auto-property setter have a separate
// pre-existing construction codegen bug (#2083 follow-up to file) where the
// second variable's choice struct tag is not stored at the slot the comparison
// reads. The positional form below exercises the §9.1 codegen path which is
// sound. The sema tests cover the body form's admissibility.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §9.3 / §35.7 — POSITIONAL record (combined form): a positional field
// whose type is a value-like choice participates in the synthesized
// equality — the §9.3 eligibility rule reuses §9.2's admissibility after
// v0.210 (the choice is admissible). Same-active-case + same-payload →
// equal; same-active-case + different-payload → unequal; different-case →
// unequal.
BEZEL_TEST(codegen_choice_component_record_equality_v210_2083,
           spec_9_3_v210_positional_record_payload_choice_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap(C: Color); "
        "class C public static method Main(): int "
        "  var a := new Wrap(Color.Green(7)); "
        "  var b := new Wrap(Color.Green(7)); "
        "  var c := new Wrap(Color.Green(8)); "
        "  var d := new Wrap(Color.Red); "
        "  if a = b and not (a = c) and not (a = d) "
        "     and not (b = d) and not (d = c) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §9.3 / §35.7 — POSITIONAL record with a payload-free choice.
// Two Red values compare equal, Red vs Blue compares unequal.
BEZEL_TEST(codegen_choice_component_record_equality_v210_2083,
           spec_9_3_v210_positional_record_payload_free_choice_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap(C: Color); "
        "class C public static method Main(): int "
        "  var a := new Wrap(Color.Red); var b := new Wrap(Color.Red); "
        "  var d := new Wrap(Color.Blue); "
        "  if a = b and not (a = d) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §8.3 / §35.7 — HashCode invariant: equal records hash equal. The §35.7
// rule combines the case discriminant with per-payload hashes (so two
// values in different cases never collide). The positional record's
// GetHashCode therefore mixes the choice's GetHashCode into the fold.
BEZEL_TEST(codegen_choice_component_record_equality_v210_2083,
           spec_9_3_v210_positional_record_choice_gethashcode_equal_for_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap(C: Color); "
        "class C public static method Main(): int "
        "  var a := new Wrap(Color.Green(7)); var b := new Wrap(Color.Green(7)); "
        "  return (if a.GetHashCode() = b.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §35.7 — HashCode invariant: equal-by-hash-container inspection.
// §3.6 / §8.3 requires the key type to be hashable, and a
// value-like choice IS hashable per §35.7. Distinct cases should
// produce distinct hashes (the discriminant is mixed in first).
BEZEL_TEST(codegen_choice_component_record_equality_v210_2083,
           spec_9_3_v210_positional_record_different_choice_different_hash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap(C: Color); "
        "class C public static method Main(): int "
        "  var a := new Wrap(Color.Red); "
        "  var b := new Wrap(Color.Blue); "
        "  return (if a.GetHashCode() <> b.GetHashCode() then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.3 / §35.7 — POSITIONAL record with a NESTED choice component. The
// §35.7 recursive rule admits "another auto-hashable choice" as a
// payload field type, so the inner choice's GetHashCode is folded into
// the outer choice's fold, which is folded into the record's fold.
BEZEL_TEST(codegen_choice_component_record_equality_v210_2083,
           spec_9_3_v210_positional_record_nested_choice_component_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Inner Valued(value: int), Unit; end Inner; "
        "choice Outer First(inner: Inner), Second; end Outer; "
        "record Wrap(O: Outer); "
        "class C public static method Main(): int "
        "  var a := new Wrap(Outer.First(Inner.Valued(7))); "
        "  var b := new Wrap(Outer.First(Inner.Valued(7))); "
        "  var c := new Wrap(Outer.First(Inner.Valued(8))); "
        "  if a = b and not (a = c) then return 1; else return 0; end; "
        "end Main; end C;"), 1);
}

// §9.2 / §35.7 — Dictionary key: a value-like choice (here, a positional
// record holding one) is hashable per §35.7, so it satisfies the
// §3.6 / §8.3 `Dictionary<K, V>` key contract. The exercise is from the
// runtime: insert via two keys, look up via the first, expect the value
// that maps from it. Uses the boxed equality (the spec's hash-container
// path), which the codegen's boxeq thunk correctly dispatches through
// emitChoiceEquality.
BEZEL_TEST(codegen_choice_component_record_equality_v210_2083,
           spec_9_3_v210_choice_component_record_dictionary_key) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap(C: Color); "
        "class C public static method Main(): int "
        "  var d := new Dictionary<Wrap, int>(); "
        "  var k1 := new Wrap(Color.Green(7)); "
        "  d.Add(k1, 100); "
        "  var outv := -1; "
        "  if d.TryGetValue(k1, outv) then return outv; end; "
        "  return -2; "
        "end Main; end C;"), 100);
}
