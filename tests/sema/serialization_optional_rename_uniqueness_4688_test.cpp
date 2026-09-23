// fix #4688 — two missing §9.8 (v0.182/v0.307) compile-time checks on the
// `serialized` clause, found via the rule corpus (tests/conformance/rules/)
// rows `s9_8_0004` and `s9_8_0007`, both `fail` in `baseline.tsv` against
// `bezelc` @ 556e2ee5 (spec v0.307).
//
// THE RULES, from the documents:
//
//   1. §9.8 field-name customization, `serialized optional` bullet: "A
//      `serialized optional` component whose type is not defaultable
//      (§17.11) is a compile error at the conformance declaration (a
//      missing field would have no value to supply), exactly as for
//      `serialized skip` below."
//
//      The `serialized skip` half of this sentence is implemented (fix
//      #3044, scoped to the §17.11 ENUM row — an enum with no declared
//      zero-valued constant is not defaultable, §17.11: "Enums: the value
//      with underlying integer 0 -- necessarily a declared constant"). The
//      `serialized optional` half — which the spec states is the identical
//      rule — was never added: `grep`ing TypeResolution_Checks.cpp for
//      `isOptional` before this fix finds it read only by the PARSER's
//      mutual-exclusion check, never by `checkSerializationEligibility`.
//
//   2. §9.8 field-name customization, `serialized "<name>"` bullet: "The
//      string must be a non-empty literal and must be unique among the
//      serialized names of the type's components ... a member's serialized
//      name is its rename where one is declared and its declared identifier
//      otherwise, so a rename equal to another member's identifier collides
//      too. A collision is a compile error at the conformance declaration."
//
//      No uniqueness check existed at all before this fix (verified: no
//      "unique"/"collides"/"collision" text anywhere in the serialization
//      checks).
//
// SCOPE: both checks are added for RECORD components (positional fields and
// body-record auto-properties) — what s9_8_0004/s9_8_0007 test and what the
// two call sites in `checkSerializationEligibility` cover. Enum constants
// and choice cases/payload fields are NOT covered by either check yet (filed
// as remaining scope in #4688) — this file does not claim coverage there.
//
// VERIFY-FIRST (scripts/agent_check.sh --probe, pre-fix `bezelc`): both
// corpus repros below compiled with rc=0 and printed no diagnostic at all.
//
// All types/methods verified against Bezel_Standard_Library.md /
// Bezel_Language_Spec.md. No invented APIs.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// A driver-level (AOT) rejection row: the whole pipeline must refuse the
// program, naming the given diagnostic substring, and must never crash /
// signal / IR-verifier-fail — distinguishing "rejected correctly" from
// "died" (the same discipline as fix #4500's `expectDriverRejectsBoolArithmetic`).
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
// s9_8_0004 — `serialized optional` on a non-defaultable enum component.
// ===========================================================================

// The corpus repro, verbatim (body-record auto-property form): `Level` has
// no zero-valued constant, so `default(Level)` — what a missing `Mode`
// field on read would need to supply — is itself a compile error (§17.11).
BEZEL_TEST(sema_serialization_optional_defaultable_4688,
           body_record_optional_enum_no_zero_rejected_spec_9_8_17_11) {
    expectDriverRejectsWithText(
        "module Conf; "
        "public enum Level "
        "  Low := 1, "
        "  High := 2; "
        "end Level; "
        "public record Setting: ISerializable, IDeserializable "
        "  public readonly property Mode: Level serialized optional; "
        "  public constructor(mode: Level) Mode := mode; end constructor; "
        "end Setting;",
        "defaultable");
}

// The positional-field form of the same rule — the OTHER call site the fix
// touches, exercised independently so a fix scoped to only one branch would
// still be caught.
BEZEL_TEST(sema_serialization_optional_defaultable_4688,
           positional_record_optional_enum_no_zero_rejected_spec_9_8_17_11) {
    expectDriverRejectsWithText(
        "module Conf; "
        "public enum Level "
        "  Low := 1, "
        "  High := 2; "
        "end Level; "
        "public record Setting(Mode: Level serialized optional): "
        "ISerializable, IDeserializable;",
        "defaultable");
}

// CONTROL: an enum WITH a zero-valued constant is defaultable, so
// `serialized optional` on it is fine — the fix must not over-reject.
// §17.11: "Enums: the value with underlying integer 0" is well-defined here.
BEZEL_TEST(sema_serialization_optional_defaultable_4688,
           optional_enum_with_zero_constant_accepted_spec_9_8_17_11) {
    auto r = runAot(
        "module Conf; "
        "public enum Level "
        "  Low := 0, "
        "  High := 1; "
        "end Level; "
        "public record Setting: ISerializable, IDeserializable "
        "  public readonly property Mode: Level serialized optional; "
        "  public constructor(mode: Level) Mode := mode; end constructor; "
        "end Setting; "
        "public class C public static method Main(): int "
        "  var s := new Setting(Level.High); "
        "  return int(s.Mode); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
}

// CONTROL: `serialized skip` on the same non-defaultable enum keeps its
// existing, unrelated diagnostic — the two clauses share the underlying
// §17.11 rule but the fix adds a SEPARATE check for `optional`, not a
// generic rewrite; this pins that `skip`'s own message (fix #3044,
// "which `serialized skip` reconstructs it with") is still what fires, not
// the new `optional` wording.
BEZEL_TEST(sema_serialization_optional_defaultable_4688,
           skip_enum_no_zero_still_rejected_with_skip_wording_spec_9_8_17_11) {
    expectDriverRejectsWithText(
        "module Conf; "
        "public enum Level "
        "  Low := 1, "
        "  High := 2; "
        "end Level; "
        "public record Setting: ISerializable, IDeserializable "
        "  public readonly property Mode: int; "
        "  public readonly property Skipped: Level serialized skip; "
        "  public constructor(mode: int, skipped: Level) "
        "    Mode := mode; Skipped := skipped; "
        "  end constructor; "
        "end Setting;",
        "which `serialized skip` reconstructs it with");
}

// ===========================================================================
// s9_8_0007 — a `serialized "<name>"` rename colliding with another
// component's serialized name (its own rename, or its plain identifier).
// ===========================================================================

// The corpus repro, verbatim: `A`'s rename ("B") collides with `B`'s own
// declared identifier (no rename on `B`, so its serialized name IS "B").
BEZEL_TEST(sema_serialization_rename_uniqueness_4688,
           positional_rename_collides_with_plain_identifier_rejected_spec_9_8) {
    expectDriverRejectsWithText(
        "module Conf; "
        "public record Pair(A: int serialized \"B\", B: int): "
        "ISerializable, IDeserializable;",
        "unique");
}

// The body-record auto-property form of the same collision — the OTHER call
// site the fix touches.
BEZEL_TEST(sema_serialization_rename_uniqueness_4688,
           body_record_rename_collides_with_plain_identifier_rejected_spec_9_8) {
    expectDriverRejectsWithText(
        "module Conf; "
        "public record Pair: ISerializable, IDeserializable "
        "  public readonly property A: int serialized \"B\"; "
        "  public readonly property B: int; "
        "  public constructor(a: int, b: int) A := a; B := b; end constructor; "
        "end Pair;",
        "unique");
}

// Two EXPLICIT renames colliding with each other (neither is a plain
// identifier) — the general form of the rule, not just the
// rename-vs-identifier corpus repro.
BEZEL_TEST(sema_serialization_rename_uniqueness_4688,
           two_explicit_renames_collide_rejected_spec_9_8) {
    expectDriverRejectsWithText(
        "module Conf; "
        "public record Pair(A: int serialized \"X\", B: int serialized \"X\"): "
        "ISerializable, IDeserializable;",
        "unique");
}

// CONTROL: distinct renames (and a rename that does not collide with the
// other field's plain identifier) are accepted and round-trip — the fix
// must not over-reject ordinary, non-colliding renames.
BEZEL_TEST(sema_serialization_rename_uniqueness_4688,
           distinct_renames_accepted_spec_9_8) {
    auto r = runAot(
        "module Conf; "
        "import System.Serialization; "
        "public record Pair(A: int serialized \"X\", B: int serialized \"Y\"): "
        "ISerializable, IDeserializable; "
        "public class C public static method Main(): int "
        "  var p := new Pair(3, 4); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var p2 := Pair.Deserialize(d); "
        "  return p2.A + p2.B; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 7);
}
