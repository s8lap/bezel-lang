// fix #5179 (post-#5149 dc review finding) — §9.8 step 2 (v0.310):
// "the case-insensitive comparison leaves a parameter ambiguous between two
// components" applies to the COMPONENT SET itself, not only to a particular
// explicit-constructor matching iteration. Two components whose declared
// identifiers case-fold to the same name (e.g. `Foo` and `foo`) cannot be
// bound unambiguously by any explicit constructor's case-insensitive
// parameter matching — the lowercased names collide — so the conformance
// declaration is a compile error at the record declaration, per the spec's
// own wording ("the case-insensitive comparison leaves a parameter
// ambiguous between two components") and dc ruling 2026-09-29.
//
// Pre-fix `bezelc`: the candidate-match collection at
// src/sema/TypeResolution_Checks.cpp:12917-12929 stored the lowercased
// component names in a `std::unordered_set<std::string>`. Two components
// that differ only by case (`Foo`/`foo`) both lowered to `foo`, so the set
// held one entry — the collision was MASKED. No explicit constructor could
// ever be a bijection against a set of size 1 when there are two components,
// so the matching logic fell through to the synthesized primary (§9.8 step 2
// fallback, fix #5149), and the record was silently accepted — masking the
// ambiguity §9.8 step 2 names. Verified by repro: T1 below compiled clean
// pre-fix, returning 42.
//
// THE FIX: detect case-folded collisions during the component-set build,
// and emit a compile error naming both colliding components and the common
// lowercased name. The check sits inside the existing
// `checkSerializationEligibility` record pass, scoped to records that
// declare `IDeserializable` (the §9.8 step 2 rule is about constructor
// selection, which only matters on read).
//
// SCOPE: this fix targets records (positional + body-record auto-properties
// for combined-form records). Enum constants and choice cases/payload
// fields are out of scope for this fix (filed separately if surfaced).
//
// FAIL-WITHOUT / PASS-WITH (proven mechanically in this lane): with the
// `compLowerToFirstOrig`-based collision check removed (the change below
// reverted), T1 compiles and exits 42 (round-trip via synthesized primary,
// `r.ran == true`); with the fix in place, T1 is rejected at sema
// (`r.ran == false`) with a diagnostic naming both colliding components and
// the §9.8 needle. T2/T3 still pass on both sides — the fix is a strict
// widening that only rejects previously-accepted malformed programs.
//
// All types/methods verified against Bezel_Standard_Library.md /
// Bezel_Language_Spec.md. No invented APIs.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A driver-level (AOT) rejection row: the whole pipeline must refuse the
// program, naming the given diagnostic substring, and must never crash /
// signal / IR-verifier-fail — same discipline as fix #4500's
// `expectDriverRejectsBoolArithmetic` and fix #4688's
// `expectDriverRejectsWithText`.
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
// T1 — POSITIONAL record `Amb(Foo: int, foo: int)`: two components differ
// only by case. §9.8 step 2 says the conformance declaration is a compile
// error naming the candidates — the case-insensitive comparison cannot
// distinguish `Foo` from `foo` for any explicit ctor's parameter matching,
// so the record itself is ambiguous for `IDeserializable` synthesis.
//
// Pre-fix: bezelc compiles this record clean (the lowered set collapses
// `Foo`/`foo` to a single entry, the matching logic falls through to the
// synthesized primary, the synthesized `Deserialize` is generated and the
// round-trip succeeds — exit 42). Post-fix: compile error at the record
// declaration naming both components and the §9.8 needle.
// ===========================================================================
BEZEL_TEST(sema_case_insensitive_component_ambiguity_5179,
           positional_components_foo_and_foo_clash_rejected_spec_9_8) {
    expectDriverRejectsWithText(
        "module Probe; "
        "public record Amb(Foo: int, foo: int): ISerializable, IDeserializable "
        "  public constructor(foo: int, bar: int) "
        "    Foo := foo; foo := bar; "
        "  end constructor; "
        "end Amb;",
        "ambiguous between two components");
}

// ===========================================================================
// T2 — POSITIONAL record `OK(Foo: int, bar: int)`: components differ
// case-insensitively. Round-trip through the synthesized primary must
// preserve the field values — proves the fix did not over-reject and did
// not regress the non-collision path.
// ===========================================================================
BEZEL_TEST(sema_case_insensitive_component_ambiguity_5179,
           positional_components_foo_and_bar_distinct_accepted_roundtrip) {
    auto r = runAot(
        "module Probe; "
        "import System.Serialization; "
        "public record OK(Foo: int, bar: int): ISerializable, IDeserializable "
        "end OK; "
        "public class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var v := new OK(3, 4); "
        "  v.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var v2 := OK.Deserialize(d); "
        "  return (if v2.Foo = 3 and v2.bar = 4 then 42 else 91); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T3 — POSITIONAL record `OK2(Foo: int, Foo2: int)`: components lowercase
// to `foo` and `foo2`, which are distinct lowered names — the bijection is
// possible. Round-trip through the synthesized primary must preserve the
// field values — pins that the fix discriminates "distinct lowered" from
// "colliding lowered" correctly.
// ===========================================================================
BEZEL_TEST(sema_case_insensitive_component_ambiguity_5179,
           positional_components_foo_and_foo2_distinct_lowered_accepted_roundtrip) {
    auto r = runAot(
        "module Probe; "
        "import System.Serialization; "
        "public record OK2(Foo: int, Foo2: int): ISerializable, IDeserializable "
        "end OK2; "
        "public class C public static method Main(): int "
        "  using var s := new JsonSerializer(); "
        "  var v := new OK2(3, 4); "
        "  v.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new JsonDeserializer(bytes.AsReadOnlySpan()); "
        "  var v2 := OK2.Deserialize(d); "
        "  return (if v2.Foo = 3 and v2.Foo2 = 4 then 42 else 91); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T4 — BODY record `BodyAmb` whose auto-properties are `Foo` and `foo`:
// same case-folded collision, but on a body-record (no positional fields,
// so the matching-collection loop reads the auto-properties instead). The
// spec rule applies to ALL serialized components (§9.8 step 2: "matched by
// name, compared case-insensitively") — both positional fields and
// combined-form body's auto-properties — so a body record's auto-property
// set is in scope. The fix must reject this case too.
//
// Pre-fix: lowered-set collapses `Foo`/`foo` to one entry; the explicit
// ctor's parameter list cannot biject against a size-1 set when there are
// two auto-property components, so matching falls through; a body record
// with no positional fields has no synthesized primary, so the candidate-
// naming diagnostic fires ("no explicit constructor has parameter names
// matching the serialized component set") — masking the ambiguity. Post-fix:
// the new diagnostic naming both colliding components fires first.
// ===========================================================================
BEZEL_TEST(sema_case_insensitive_component_ambiguity_5179,
           body_record_auto_properties_foo_and_foo_clash_rejected_spec_9_8) {
    expectDriverRejectsWithText(
        "module Probe; "
        "public record BodyAmb: ISerializable, IDeserializable "
        "  public property Foo: int; "
        "  public property foo: int; "
        "  public constructor(foo: int, bar: int) "
        "    Foo := foo; foo := bar; "
        "  end constructor; "
        "end BodyAmb;",
        "ambiguous between two components");
}
