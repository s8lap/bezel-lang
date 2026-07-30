// Codegen — issue #2425 / spec §9.8 ("Serialization synthesis" / "Choice
// serialization") + §35.1 ("Choice declaration", self-containment) +
// §28.3/§28.9 (exactly-once reclamation), docs revision v0.235.
//
// #2405 made `recordHasStringFields` / `emitRecordStringFieldReleases_impl`
// (src/codegen/LlvmBackend_impl.hpp) recurse into a non-nullable NESTED
// RECORD storage member to arbitrary depth, so the #2398 deserialize
// disposal-scope registration (an already-populated component/payload slot
// guarded against a LATER sibling's read throwing) reaches a string owned
// two-or-more record-levels deep. #2405's own writeup documented the
// residual: a component/payload whose type is a value-like CHOICE — a
// record-admissible component type per §9.8 ("a value-like serialization-
// eligible choice ... matching §9.2's admission of value-like choices as
// equality components") — had NO release walker at all, at any depth,
// whether reached directly (a record field / choice payload field IS a
// choice) or transitively (nested inside a record that is itself nested
// inside the top-level type).
//
// VERIFY-FIRST: reproduced exactly as filed against this lane's rebase —
// a record component whose type is a value-like choice with a DIRECT
// string payload (ONE level — the walker didn't exist at ANY depth) leaked
// 60 byte(s) in 2 allocation(s) under LSan when a later sibling field's
// read threw, matching #2405's own byte-count shape.
//
// Fix: two NEW functions mirroring recordHasStringFields /
// emitRecordStringFieldReleases_impl exactly — `choiceHasStringFields` and
// `emitChoiceStringFieldReleases_impl` (LlvmBackend_Disposal.cpp, declared
// in LlvmBackend_impl.hpp) — walk a value-like choice's cases, releasing a
// direct `string` payload field and recursing into a non-nullable nested
// record (reusing emitRecordStringFieldReleases_impl) or nested choice
// (recursing into itself) payload field, to ARBITRARY depth. Termination:
// §35.1's choice self-containment check
// (TypeResolution_Checks.cpp's checkChoiceSelfContainment, which extends
// the existing §9 recordValueContainsSelf walker to follow choice payloads)
// rejects a record/choice value-type graph cycle at the DECLARATION site —
// "a choice cannot recursively contain itself by value through its case
// payloads" — so the combined record+choice value-type graph reachable
// from any declaration is a finite DAG regardless of which kind starts the
// walk, mirroring #2405's own record-only termination argument.
// `recordHasStringFields` / `emitRecordStringFieldReleases_impl` gained a
// matching Choice-kind branch so a RECORD component that is itself a
// choice is ALSO covered (not just a choice component/payload directly).
// The #2398 registration sites (LlvmBackend_Serialization.cpp, record
// component ~line 975, choice payload ~line 2013) gained a new
// `else if` branch keyed on `choiceHasStringFields`, registering a NEW
// `Disposal.choiceStringFields` flag released via
// `emitChoiceStringFieldReleases` — a SEPARATE, address-based (GEP, not
// load+extractvalue), tag-switched walker distinct from #2115's
// `emitValueLikeChoiceActiveCaseStrRelease` (a value-based, one-level
// walker used ONLY by the scope-end LOCAL release, unrelated call site —
// see #2428's own test file for that mechanism).
//
// GENERIC value-like choices are excluded (`choiceHasStringFields` returns
// false unconditionally for a choice with `genericParams`) — the same
// narrow, documented scope #2405's own record recursion has (neither
// threads a RecordSubstGuard through a nested generic level); not
// exercised by any test here.
//
// Adversarial grid covered (full detail in the lane report):
//   * choice component nesting depth 1 (a record field IS a value-like
//     choice with a DIRECT string payload — the walker didn't exist at
//     ANY depth, so depth 1 alone is a genuine regression test).
//   * choice-in-choice (a choice payload field is ITSELF a value-like
//     choice) — the #2398 choice-payload registration site.
//   * three levels, MIXED record/choice/record nesting (Root->Outer(record)
//     ->Mid(choice)->Leaf(record).tag) — proves genuine arbitrary-depth,
//     mixed-kind recursion, not merely "one more level" or "choice-only".
//   * a clean (non-throwing) mixed 3-level round-trip — the fix's new
//     registration, popped WITHOUT disposal on the success path, must not
//     itself leak or double-free.
//   * a NULLABLE choice-typed record component — probed separately;
//     surfaced an UNRELATED pre-existing compiler crash serializing ANY
//     nullable value-like choice record component (an ICmp type-mismatch
//     assertion, independent of this fix — confirmed via patch isolation
//     against unmodified main) — filed as #2447, not exercised here.
//   * a pre-existing UAF discovered while scoping #2428 (a record-side
//     #1747 call-result release firing on a BORROWED field read, not just
//     a fresh construction) — filed as #2448, unrelated to this file.

#include "support/aot_support.hpp"   // runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
const char* kImport = "import System.Serialization; ";
}  // namespace

// ===========================================================================
// The issue's own minimal repro: a RECORD component whose type is a
// value-like CHOICE with a DIRECT string payload (Wrap.Holder(tag: string)).
// Root(wrap: Wrap, second: int) — Root.second's truncated read throws;
// Wrap's already-read string payload must be disposed, not orphaned. No
// release walker for a choice existed at ANY depth pre-fix, so depth 1
// alone demonstrates the defect.
// ===========================================================================
BEZEL_TEST(serialization_choice_string_release_2425,
           record_component_is_choice_with_direct_string_disposed_on_sibling_throw_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Wrap: ISerializable, IDeserializable "
        "  Holder(tag: string), Empty; end Wrap; "
        "record Root(wrap: Wrap, second: int): ISerializable, IDeserializable "
        "end Root; "
        "class C public static method Main(): int "
        "  var p: Root := new Root(Wrap.Holder(\"deep-inner-tag-value-string\"), 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Root := Root.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 60-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Choice-in-choice: a CHOICE payload field is itself a value-like choice
// with a direct string payload. Exercises the #2398 CHOICE-payload
// registration site (distinct code path from the record-component site
// above), and proves the walker recurses into a nested CHOICE, not just a
// nested record.
// ===========================================================================
BEZEL_TEST(serialization_choice_string_release_2425,
           choice_payload_is_nested_choice_with_string_disposed_on_sibling_throw_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "choice Inner: ISerializable, IDeserializable "
        "  Leaf(tag: string), Empty; end Inner; "
        "choice Outer: ISerializable, IDeserializable "
        "  Wrap(inner: Inner, second: int), Nope; end Outer; "
        "class C public static method Main(): int "
        "  var m := Outer.Wrap(Inner.Leaf(\"deep-inner-tag-value-string\"), 12345); "
        "  using var s := new BinarySerializer(); "
        "  m.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back := Outer.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 60-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Three levels deep, MIXED record/choice/record nesting:
// Root(outer: Outer, second: int) [record]
//   -> Outer(mid: Mid) [record]
//     -> Mid: Holder(leaf: Leaf) [CHOICE]
//       -> Leaf(tag: string) [record]
// Proves the fix is genuinely recursive across BOTH kinds (not merely
// "choice, one level" or "record, one level" in isolation) — the choice
// branch in recordHasStringFields/emitRecordStringFieldReleases_impl and
// the record branch in choiceHasStringFields/
// emitChoiceStringFieldReleases_impl must both fire for this shape to
// dispose cleanly.
// ===========================================================================
BEZEL_TEST(serialization_choice_string_release_2425,
           three_levels_mixed_record_choice_record_disposed_on_sibling_throw_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(tag: string): ISerializable, IDeserializable end Leaf; "
        "choice Mid: ISerializable, IDeserializable "
        "  Holder(leaf: Leaf), Empty; end Mid; "
        "record Outer(mid: Mid): ISerializable, IDeserializable end Outer; "
        "record Root(outer: Outer, second: int): ISerializable, IDeserializable "
        "end Root; "
        "class C public static method Main(): int "
        "  var p: Root := new Root(new Outer(Mid.Holder(new Leaf(\"deep-inner-tag-value-string\"))), 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  var cutLen: int := bytes.Length - 3; "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan(0, cutLen)); "
        "  var caught: bool := false; "
        "  try "
        "    var back: Root := Root.Deserialize(d); "
        "    return 0; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);   // false pre-fix: 60-byte / 2-alloc leak
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// Clean, full-read control over the SAME 3-level mixed shape (no
// truncation): round-trips correctly (value equality on the deepest string
// AND the sibling int) and the fix's now-recursive per-slot disposal
// registration — popped WITHOUT running its disposals once construction
// begins — does not itself leak or double-free on the ordinary success
// path.
// ===========================================================================
BEZEL_TEST(serialization_choice_string_release_2425,
           clean_three_levels_mixed_roundtrip_no_leak_spec_9_8) {
    auto r = runAotChecked(
        std::string("module M; ") + kImport +
        "record Leaf(tag: string): ISerializable, IDeserializable end Leaf; "
        "choice Mid: ISerializable, IDeserializable "
        "  Holder(leaf: Leaf), Empty; end Mid; "
        "record Outer(mid: Mid): ISerializable, IDeserializable end Outer; "
        "record Root(outer: Outer, second: int): ISerializable, IDeserializable "
        "end Root; "
        "class C public static method Main(): int "
        "  var p: Root := new Root(new Outer(Mid.Holder(new Leaf(\"deep-inner-tag-value-string\"))), 12345); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back: Root := Root.Deserialize(d); "
        "  var leafTag: string := (case back.outer.mid of "
        "    Holder(var leaf) => leaf.tag, Empty => \"\"); "
        "  if leafTag <> \"deep-inner-tag-value-string\" then return 1; end; "
        "  if back.second <> 12345 then return 2; end; "
        "  return 42; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
