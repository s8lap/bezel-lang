// Spec-derived tests for issue #2210 — §9.7 / §35.4 (v0.226): a record field
// cannot hold an owning-classified choice (§35.4.1). Records declare no
// disposal path (§9.7: no destructor, no `owned` field, no `IDisposable`),
// and §35.4 makes owning choices move-only value types whose active case
// carries payloads requiring disposal at scope end (§28.3). Burying an
// owning choice in a record field would leak the owned payload — the
// `Res` in the issue's repro never reaches a destructor.
//
// The companion lit tests (tests/lit/ownership/record_owning_choice_field_2210__*
// _rejected.bzl + _accepted.bzl) pin the diagnostic on the FILE-CHECK path.
// This file pins the gtest/AOT side: the rejection fires at the field
// DECLARATION (the construction boundary), every §35.4 NON-OWNING case
// stays accepted (the over-rejection control), and the generic-choice
// instantiation re-check is exercised (an owning `<T>` is admitted at
// the declaration site IF `<T>` substitutes to a non-owning argument).
//
// // per §9.7 (v0.226), §35.4, issue #2210

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True when an ERROR diagnostic whose message contains `sub` was emitted.
// Matching on the §9.7-specific wording keeps each test pinned to the rule
// under test (an unrelated diagnostic never satisfies it).
bool hasError(const OwnershipResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — an owning-classified choice as a record field is rejected at
// the declaration boundary. The §35.4.1 verdict is structural: the choice
// has a case with an `owned` class payload. Per §9.7 / §15.3 / §35.4, the
// record declares no disposal path, so the owned payload would leak.
// =============================================================================

// The issue's exact repro, condensed: a positional record with a single
// owning-classified choice field. The choice has `Some(owned r: Res)`.
// Must be rejected at the field declaration — the construction `new HolderO(...)`
// never reaches the compiler's range here.
BEZEL_TEST(record_owning_choice_field_2210, positional_field_owning_choice_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public method V(): int return 5; end V; end Res; "
        "choice Own Some(owned r: Res), None; end Own; "
        "record HolderO(K: Own); "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "record `HolderO` may not have an owning-classified choice field `K`"));
    BEZEL_EXPECT_TRUE(hasError(r, "§35.4"));
    BEZEL_EXPECT_TRUE(hasError(r, "§9.7"));
}

// A body record with an auto-property whose type is an owning-classified
// choice — the DECL form `property K: Own;` is the storage-bearing member
// and the §35.4 verdict is the same. Mirrors the positional form so the
// declaration, not the syntactic shape, drives the rejection.
BEZEL_TEST(record_owning_choice_field_2210, body_autoprop_owning_choice_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public method V(): int return 5; end V; end Res; "
        "choice Own Some(owned r: Res), None; end Own; "
        "record HolderO property K: Own; end HolderO; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "record `HolderO` may not have an owning-classified choice "
           "auto-property `K`"));
    BEZEL_EXPECT_TRUE(hasError(r, "§35.4"));
}

// An owning-classified choice whose `owned` payload is on a NESTED choice
// mark (§35.4.1 transitivity: a move-only value payload of an owning
// choice's case makes the choice owning). `ChoiceB` has no `owned` directly
// but its case payload `Pair` is itself an owning choice, so the §35.4
// classifier returns Owning and the record field is rejected.
BEZEL_TEST(record_owning_choice_field_2210,
           nested_owning_choice_payload_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public method V(): int return 5; end V; end Res; "
        "choice Inner Some(owned r: Res), None; end Inner; "
        "choice Outer Wrap(p: Inner), Leaf; end Outer; "
        "record HolderO(K: Outer); "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "record `HolderO` may not have an owning-classified choice field `K`"));
}

// =============================================================================
// POSITIVE — value-like and borrow-bearing choices as record fields are
// accepted at the declaration site. The §35.4 verdict gates the rejection;
// BorrowBearing and ValueLike must pass through, subject only to the
// ordinary §9.7 rules (covered by #2159 for the borrow-bearing case, by
// #2044 for the value-like case in the conformance/equality-surface path).
// =============================================================================

// §35.4.3: a value-like choice (only int/string/runtime-managed payloads)
// is NOT owning — the record is value-shaped and the field is freely stored.
// This is the canonical "no over-rejection" control.
BEZEL_TEST(record_owning_choice_field_2210, value_like_choice_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "choice VOpt Some(n: int), Named(s: string), None; end VOpt; "
        "record HolderV(K: VOpt); "
        "class C public static method Main(): int "
        "  var h := new HolderV(VOpt.Some(42)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §35.4.2: a borrow-bearing choice (an unmarked class payload) is NOT
// owning — the record IS borrow-bearing by §9.7 transitivity, and the
// existing #2159 path governs its USE sites (return escape, instance field
// store, async parameter). The DECLARATION site must accept it.
BEZEL_TEST(record_owning_choice_field_2210, borrow_bearing_choice_field_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res public method V(): int return 5; end V; end Res; "
        "choice RefC Some(r: Res), None; end RefC; "
        "record HolderC(K: RefC); "
        "class C public static method Main(): int "
        "  using var local := new Res(); "
        "  var h := new HolderC(RefC.Some(local)); "
        "  return 0; "
        "end Main; end C;");
    // Note: the auto-drop warning on `local` is expected and intentionally
    // not asserted — the test pins the field-declaration admission.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// POSITIVE — the generic-choice instantiation re-check (#2183 family).
// A generic choice `Opt<T>` field at the declaration site is admitted
// because `T` is unresolved (the conservative reading: a bare type
// parameter is not classifiable as move-only, #2183). The instantiation-
// site re-check is the existing #2183 path's responsibility and runs at
// each use of the generic record with a concrete argument.
// =============================================================================

// Generic choice `Opt<T>` at the record declaration with a NON-OWNING
// argument (`int`) — value-like at this instantiation, record value-like.
// Stays accepted.
BEZEL_TEST(record_owning_choice_field_2210,
           generic_choice_instantiated_to_value_like_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "choice Opt<T> Some(v: T), None; end Opt; "
        "record HolderO(K: Opt<int>); "
        "class C public static method Main(): int "
        "  var h := new HolderO(Opt<int>.Some(42)); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Generic choice `Opt<T>` at the record declaration with a NON-OWNING
// argument (`string`) — runtime-managed reference, classified as value-like
// per §35.4.3. Stays accepted.
BEZEL_TEST(record_owning_choice_field_2210,
           generic_choice_instantiated_to_string_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "choice Opt<T> Some(v: T), None; end Opt; "
        "record HolderO(K: Opt<string>); "
        "class C public static method Main(): int "
        "  var h := new HolderO(Opt<string>.Some(\"hello\")); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
