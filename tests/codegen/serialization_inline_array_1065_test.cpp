// Codegen tests — §9.8 v0.228 serialization synthesis for non-owning inline
// array `[N]T` record components (issue #1065).
//
// §9.8 v0.228 lists a standard sequence collection as serialization-eligible
// "whose element type is serialization-eligible (encoded with
// BeginArray/EndArray)", explicitly including "inline arrays [N]T". The
// record clause (same section) admits it as a record component only when
// NON-OWNING: "a non-owning inline array [N]T/[N]T? of record-admissible
// element type" — a `[N owned]T` inline array is never admitted (move-only,
// no spec text admits it either list).
//
// Scope of THIS slice: primitives-other-than-string and enums as element
// types (plus nested non-owning inline arrays of the same). A `string`
// element — or any Named (record/choice/class) element, which may itself
// carry a `string` — is deliberately EXCLUDED for now: the synthesized
// Deserialize constructs the array via a raw GEP-store loop
// (LlvmBackend_Serialization.cpp) that bypasses the record-scope-end
// string-release bookkeeping (LlvmBackend_impl.hpp recordHasStringFields /
// emitRecordStringFieldReleases, fix #1747), which inspects only a record's
// DIRECT storage members and does not see a string buried inside an inline
// array. Admitting it would synthesize a Deserialize that leaks every
// element string (verified under ASan/LSan: a `[N]string` field leaks 2
// allocations per round-trip). Filed as a follow-up (see issue tracker) —
// lifting the restriction is future work once that release walk is
// extended to recurse into inline arrays. This is the `Array<T>`/
// `OwningArray<T>`/`Dictionary<K,V>` issue's sibling gap for the OTHER
// standard-sequence-collection form the record-admissible list already
// covers today: the heap collections remain out of reach for a record
// (§9.8's record clause forbids ANY class-typed serialized component,
// including heap collections — they are class-only, so a record's route to
// them is the class envelope, whose serialization synthesis is itself a
// separate, much larger pre-existing gap; see the follow-up issue).
//
// BinarySerializer/BinaryDeserializer live in System.Serialization (§2.15)
// and are NOT auto-imported; tests include "import System.Serialization;".
// JSON is out of scope for tier 1 synthesis (see serialization_synthesis_550_test.cpp).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool compileFails(const std::string& src) {
    auto r = runAot(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

// Preamble for all BinarySerializer tests (§2.15: requires import).
const char* kImport = "import System.Serialization; ";

} // namespace

// ===========================================================================
// Eligibility (§9.8 v0.228): non-owning inline array of a serializable
// primitive element type is ACCEPTED (was rejected before this fix — the
// stale eligibility check had no InlineArray branch at all).
// ===========================================================================

BEZEL_TEST(codegen_serialization_1065, inline_array_int_eligible_spec_9_8_v0228) {
    BEZEL_EXPECT_TRUE(!compileFails(
        std::string("module M; ") + kImport +
        "record Bag(Items: [3]int): ISerializable, IDeserializable; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// §9.8 v0.228 record clause: an OWNING inline array (`[N owned]T`) is never
// record-admissible — no spec text admits it in either the record or the
// general component list (CLAUDE.md rule 4 — recorded choice, issue #1065).
// A class-typed element is required to make an inline array ownable, and a
// class-typed serialized component is independently forbidden for records,
// so this is doubly rejected; kept as its own regression pin on the
// InlineArray-specific `arrayOwned` guard.
BEZEL_TEST(codegen_serialization_1065, owning_inline_array_still_rejected_spec_9_8_v0228) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "class Leaf public property V: int => 0; end Leaf; "
        "record Bag(Items: [2 owned]Leaf): ISerializable, IDeserializable; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// A `string` element is excluded for now (the leak this slice declines to
// ship — see file header). Pins the CURRENT conservative-reject behavior so
// a future fix that lifts it does so deliberately (by replacing this test),
// not by accident.
BEZEL_TEST(codegen_serialization_1065, inline_array_string_element_still_rejected_1065_followup) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "record Strs(Names: [2]string): ISerializable, IDeserializable; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// A nested-RECORD element is likewise excluded for now (same conservative
// reason — it may itself carry a string). Pins the current behavior.
BEZEL_TEST(codegen_serialization_1065, inline_array_record_element_still_rejected_1065_followup) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string("module M; ") + kImport +
        "record Point(X: int, Y: int); "
        "record Path(Points: [2]Point): ISerializable, IDeserializable; "
        "class C public static method Main(): int return 0; end Main; end C;"));
}

// ===========================================================================
// Round-trip (§9.8 v0.228 / §2.15): serialize then deserialize, values equal.
// ===========================================================================

// Basic round trip: a `[3]int` field survives Serialize -> ToBytes ->
// BinaryDeserializer -> Deserialize with every element intact.
BEZEL_TEST(codegen_serialization_1065, inline_array_int_roundtrip_spec_9_8_v0228) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Bag(Items: [3]int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var original := new Bag([10, 20, 30]); "
        "  using var s := new BinarySerializer(); "
        "  original.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := Bag.Deserialize(d); "
        "  return (if back.Items[0] = 10 and back.Items[1] = 20 "
        "             and back.Items[2] = 30 then 1 else 0); "
        "end Main; end C;"), 1);
}

// Every element distinct + negative values, to rule out a broadcast/aliasing
// bug (all slots reading the same value).
BEZEL_TEST(codegen_serialization_1065, inline_array_int_distinct_and_negative_values_spec_9_8_v0228) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Bag(Items: [4]int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var original := new Bag([-5, 0, 7, 1000]); "
        "  using var s := new BinarySerializer(); "
        "  original.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := Bag.Deserialize(d); "
        "  return (if back.Items[0] = -5 and back.Items[1] = 0 "
        "             and back.Items[2] = 7 and back.Items[3] = 1000 "
        "          then 1 else 0); "
        "end Main; end C;"), 1);
}

// A record whose ONLY component is the inline array (declaration order /
// field-count bookkeeping with N=1 remaining field).
BEZEL_TEST(codegen_serialization_1065, inline_array_solo_component_spec_9_8_v0228) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Trio(V: [3]int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var original := new Trio([1, 2, 3]); "
        "  using var s := new BinarySerializer(); "
        "  original.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := Trio.Deserialize(d); "
        "  return back.V[0] + back.V[1] + back.V[2]; "
        "end Main; end C;"), 6);
}

// The inline array alongside an ordinary scalar component — declaration-
// order field dispatch must not be perturbed by the array's variable-shape
// wire encoding (BeginArray/EndArray around N elements, versus a scalar's
// single write).
BEZEL_TEST(codegen_serialization_1065, inline_array_alongside_scalar_field_spec_9_8_v0228) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kImport +
        "record Tagged(Id: int, Items: [2]int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var original := new Tagged(99, [1, 2]); "
        "  using var s := new BinarySerializer(); "
        "  original.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := Tagged.Deserialize(d); "
        "  return (if back.Id = 99 and back.Items[0] = 1 and back.Items[1] = 2 "
        "          then 1 else 0); "
        "end Main; end C;"), 1);
}

// NOTE: a nested inline array `[N][M]T` was tried here but is not valid
// Bezel at all — "an inline array may not be the element of another inline
// array (`[N][M]T`) — multi-dimensional inline arrays are not admitted
// (§15.1)", independent of serialization. The InlineArray-element-of-
// InlineArray recursion in emitSerializeField/emitDeserializeField is
// therefore unreachable in practice; left in place defensively (harmless)
// since isSerializationEligibleType's own recursion would need it if §15.1
// ever changes, but no test exercises it for that reason.
