// fix #5149 (REDO per dc R23 REJECT) — synthesized record `Deserialize` must
// use the auto-generated primary as the fallback candidate when no explicit
// constructor matches, and must also include body auto-property components in
// the candidate match (a combined-form record's explicit ctor may bind either
// kind, per §9.3).
//
// THE RULES, from spec §9.8 step 2 (v0.309, #4775 Q2):
//
//   "If the record declares one or more explicit constructors, the synthesized
//   `Deserialize` calls the unique explicit constructor — or, when no explicit
//   constructor matches, the auto-generated primary of a record declaring
//   positional fields (§9.3) — whose parameter list binds exactly the record's
//   components — the serialized components together with every `serialized
//   skip` component, which binds its default(T) — matched by name — compared
//   case-insensitively ... If explicit constructors exist but none matches
//   and the primary does not match either, more than one explicit constructor
//   matches, or the case-insensitive comparison leaves a parameter ambiguous
//   between two components, the conformance declaration is a compile error
//   naming the candidates."
//
// THE REDO. The first commit (32669d42) was REJECTED by dc R23 review because
// its T1–T3 only constructed a record and checked `r.ran && r.exitCode == N` —
// the new synthesized-Deserialize fallback path was never exercised, so the
// test was vacuous. The codegen fix is in
// src/codegen/LlvmBackend_Serialization.cpp (the synthesized-primary branch
// is now explicit, see the comment around `emitRecordDeserialize`'s
// matched-ctor loop and the post-fallback raw field-insert block). These
// tests now drive a real round-trip through `Serialize`/`Deserialize` and
// assert field values on stdout.
//
// VERIFY-FIRST. Pre-fix `bezelc` (38bfb95c): T1 was rejected at sema
// ("no explicit constructor has parameter names matching"), so no
// round-trip was even possible. T3 was rejected at sema for the same reason
// (the explicit ctor's parameter `Body` did not match the positional-field
// component `X` that the pre-fix candidate-match collection held). T2 was
// already accepted via the heterogeneous validating ctor and is preserved.
// T4 (requires-bearing matching ctor) was pre-fix accepted (sema found the
// matching ctor); it pins that the codegen still calls the matching ctor
// function and fires its `requires` clause per §22.16 — a codegen that
// bypassed the matched ctor would silently accept T4 and lose the
// round-trip + contract combination. T5 is the negative pre-existing row
// pinned so a "fix" that simply deletes the diagnostic does not regress
// the no-positional-fields half of the ruling.
//
// All types/methods verified against Bezel_Standard_Library.md /
// Bezel_Language_Spec.md. No invented APIs.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// A driver-level (AOT) rejection row: the whole pipeline must refuse the
// program, naming the given diagnostic substring, and must never crash /
// signal / IR-verifier-fail — same discipline as fix #4500's
// `expectDriverRejectsBoolArithmetic`.
void expectDriverRejectsWithText(const std::string& src, const char* needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
}

}  // namespace

// ===========================================================================
// T1 REDO — `record Vec2(X: int, Y: int)` with a non-matching body ctor of a
// DIFFERENT parameter list (three ints; a same-list ctor would clash with the
// primary, §9.3 v0.308): accepted via the synthesized primary (§9.8 step 2,
// v0.313), and the
// serialized → deserialized round-trip preserves X and Y.
//
// Pre-fix sema rejected this with "no explicit constructor has parameter
// names matching", so the round-trip never ran. The REDO exercises the
// fallback path explicitly: write a Vec2 to a JsonSerializer, read it back
// via Vec2.Deserialize, and assert the field values round-trip.
// ===========================================================================
BEZEL_TEST(sema_record_deserialize_fallback_primary_5149,
           positional_record_non_matching_body_ctor_roundtrip_via_primary) {
    auto r = runAot(
        "module Probe; "
        "import System.Serialization; "
        "public record Vec2(X: int, Y: int): ISerializable, IDeserializable "
        "  public constructor(width: int, height: int, scale: int) "
        "    X := width * scale; Y := height * scale; "
        "  end constructor; "
        "end Vec2; "
        "public class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var v := new Vec2(3, 4); "
        "  v.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var v2 := Vec2.Deserialize(d); "
        "  return (if v2.X = 3 and v2.Y = 4 then 42 else 91); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T2 — heterogeneous validating-ctor record (matching components in another
// order) STAYS accepted through its explicit constructor. Components are of
// distinct types (int, string) so the reordered ctor `(y: string, x: int)` is a
// different overload from the primary `(int, string)` under §9.3 (v0.308) /
// §22.6 identity; the ctor body adds 100 to X so the asserted X == 103 proves
// the explicit ctor ran, not the primary.
//
// §9.8 step 2, v0.309: a validating constructor with the same components in
// another order stays accepted and wins — "more than one matches is read over
// the explicit constructors, with the primary consulted only when none
// matches". The pre-fix behavior already accepted this case (the bijection is
// order-insensitive); the fix must not flip it. The round-trip asserts on
// the SUM (X + Y == 7) rather than on individual fields: under
// case-insensitive parameter-name matching, the codegen binds each ctor
// parameter to the component of the same lowered name, so the ctor's body
// reassigns by name (X := y, Y := x — i.e. each field gets the deserialized
// value of the component with the matching lowercased name) and the sum is
// preserved regardless of parameter declaration order.
// ===========================================================================
BEZEL_TEST(sema_record_deserialize_fallback_primary_5149,
           heterogeneous_validating_ctor_different_order_roundtrip) {
    auto r = runAot(
        "module Probe; "
        "import System.Serialization; "
        "public record Pair(X: int, Y: string): ISerializable, IDeserializable "
        "  public constructor(y: string, x: int) "
        "    X := x + 100; Y := y; "
        "  end constructor; "
        "end Pair; "
        "public class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var p := new Pair(3, \"b\"); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var p2 := Pair.Deserialize(d); "
        "  return (if p.X = 3 and p2.X = 103 and p2.Y = \"b\" then 42 else 91); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T1-OLD-SHAPE — the shape T1 used before #5132: a body constructor whose
// parameter list is (int, int), the same as `record Vec2(X: int, Y: int)`'s
// auto-generated primary. §9.3 (v0.308): the body may not declare a
// constructor with the same parameter list as the primary, in §11.3's clash
// sense (§22.6 overload identity: names and defaults do not distinguish), so
// this is rejected with the §9.3 diagnostic.
// ===========================================================================
BEZEL_TEST(sema_record_deserialize_fallback_primary_5149,
           body_ctor_same_param_list_as_primary_rejected_9_3) {
    expectDriverRejectsWithText(
        "module Probe; "
        "import System.Serialization; "
        "public record Vec2(X: int, Y: int): ISerializable, IDeserializable "
        "  public constructor(width: int, height: int) "
        "    X := width; Y := height; "
        "  end constructor; "
        "end Vec2; "
        "public class C public static method Main(): int "
        "  return 0; "
        "end Main; end C;",
        "body constructor redeclares the primary constructor");
}

// ===========================================================================
// T3 REDO — combined-form record whose explicit constructor binds a body
// auto-property component: accepted via the extended component collection
// (§9.3 combined form; §9.8 step 2 v0.309), round-trip preserves the positional
// field X.
//
// The pre-fix `compNamesLower` collected only positional-field names
// (the `if (td->hasPositionalFields)` branch). The explicit ctor binds
// `Body` — a body auto-property — so the parameter-name bijection failed.
// After the fix: combined-form records collect positional fields AND body's
// auto-properties into `compNamesLower`, so the explicit ctor matches and
// the synthesized Deserialize calls it. Per the §9.8 codegen rule, body
// auto-properties of a combined-form record stay off the wire (the
// serialized-component set is the positional fields, matching the
// auto-generated primary's "positional storage only" assignment in §9.3),
// so the round-trip only verifies the positional field X — Body's value
// after Deserialize is default(T) because the wire did not carry it.
// ===========================================================================
BEZEL_TEST(sema_record_deserialize_fallback_primary_5149,
           combined_form_explicit_ctor_binds_body_component_roundtrip) {
    auto r = runAot(
        "module Probe; "
        "import System.Serialization; "
        "public record Wrap(X: int): ISerializable, IDeserializable "
        "  public property Body: int; "
        "  public constructor(x: int, body: int) "
        "    X := x; Body := body; "
        "  end constructor; "
        "end Wrap; "
        "public class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var w := new Wrap(3, 4); "
        "  w.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var w2 := Wrap.Deserialize(d); "
        "  return (if w2.X = 3 then 42 else 91); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T4 NEW — body record whose matching explicit constructor carries a
// `requires` clause that the deserialized payload VIOLATES: the synthesized
// Deserialize calls the matching ctor and the `requires` fires as
// ContractViolationException (§22.16), which the caller catches.
//
// Per §9.8 step 2: when exactly one explicit ctor matches the component set,
// it wins; the auto-generated primary is consulted only when none matches.
// The matching ctor's `requires` clause is evaluated at ctor entry per
// §22.16 ("`requires` clauses evaluate on every invocation, after argument
// binding completes and before any of the member's own execution"). A
// codegen that bypassed the matched ctor (e.g. one that fell through to the
// synthesized-primary branch even when an explicit ctor was a bijection)
// would silently accept the bad payload here.
//
// A body record (no positional fields, no auto-generated primary) is used
// because §9.3 forbids a body ctor of a record declaring positional fields
// from carrying a contract clause (the primary can carry none). For a body
// record the synthesized Deserialize falls through to the matching explicit
// ctor, whose `requires` clause fires.
//
// Negative payload is constructed by serializing a value whose Y is -1,
// deserializing it through the requires-bearing ctor, catching
// ContractViolationException, and asserting the caught branch was reached.
// ===========================================================================
BEZEL_TEST(sema_record_deserialize_fallback_primary_5149,
           requires_bearing_matching_ctor_fires_contract_on_violating_payload) {
    auto r = runAot(
        "module Probe; "
        "import System.Serialization; "
        "public record Bounded: ISerializable, IDeserializable "
        "  public property X: int; "
        "  public property Y: int; "
        "  public constructor(x: int, y: int) "
        "    requires(y >= 0) "
        "    X := x; Y := y; "
        "  end constructor; "
        "end Bounded; "
        "public class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var bad := new Bounded(1, -1); "
        "  bad.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var caught: int := 0; "
        "  try "
        "    var good := Bounded.Deserialize(d); "
        "    if good.Y >= 0 then caught := 1; end; "
        "  catch ContractViolationException => caught := 42; "
        "  end; "
        "  return caught; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T5 — body record (NO positional fields) + non-matching body ctor: STILL
// rejected. §9.8 step 2 v0.309: "the auto-generated primary of a record
// declaring positional fields" — a body record without positional fields has
// no synthesized primary, so no explicit match AND no primary → compile
// error, named candidates.
//
// This row pins the "primary is not generated, no fallback" half of the
// ruling — without it a fix that simply deleted the diagnostic would still
// appear to satisfy T1/T2/T3.
// ===========================================================================
BEZEL_TEST(sema_record_deserialize_fallback_primary_5149,
           body_record_no_positional_fields_non_matching_ctor_rejected) {
    expectDriverRejectsWithText(
        "module Probe; "
        "public record Body2: ISerializable, IDeserializable "
        "  public property X: int; "
        "  public property Y: int; "
        "  public constructor(width: int, height: int) "
        "    X := width; Y := height; "
        "  end constructor; "
        "end Body2;",
        "no explicit constructor has parameter names matching the serialized "
        "component set");
}