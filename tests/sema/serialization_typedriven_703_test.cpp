// Spec-derived sema tests — §9.8 type-driven serialization (v0.141), issue #703.
//
// SLICE S1 (sema eligibility): serialization-eligibility admits Shared<T> and
// Weak<T> component types, recursing on the element. Under the type-driven
// model (spec v0.141) a class participates in serialization by declaring
// `: ISerializable`/`: IDeserializable` and the field TYPES drive the wire
// shape: an owned/value field is an owning (inline) edge; a `Shared<T>` field
// is an identity participant ($id/$ref); a `Weak<T>` (or borrow) field is a
// back-reference ($ref). The eligibility rule (§9.8) is the structural gate: a
// component of type `Shared<T>`/`Weak<T>` is serialization-eligible iff T is.
//
// Migration sequencing: slices S1–S8 are ADDITIVE — the keyword path
// (`serialized tree`/`graph`/`cyclic`) stays active (the parser still accepts
// it) until S9 removes it. These tests therefore exercise the eligibility
// FUNCTION through the still-active keyword path (a `serialized graph` class
// with a `Shared<T>`/`Weak<T>` field), which is the smallest vehicle that
// reaches `isSerializationEligibleType` for those component types.
//
// SCOPE ISOLATION: a `serialized` clause synthesizes BOTH directions, so the
// IDeserializable constructor-selection rule (§9.8 v0.136 — every component must
// be a name-matched constructor parameter) always applies. The positive cases
// here satisfy that rule (each component is a matching ctor param) so that the
// ONLY diagnostic the eligibility change can flip is the eligibility one. The
// Weak<T> field is passed as a constructor parameter purely to satisfy the rule
// in isolation; the type-driven back-edge wiring (Downgrade post-construction)
// is a later slice's concern, not eligibility's.
//
// Tested at the SEMA layer only (parseAndCheckOwnership) — no codegen/JIT —
// because the type-driven SYNTHESIS (Shared identity tagging, Weak back-refs)
// lands in later slices (S2+). Asserting at the sema layer isolates the
// eligibility change and avoids triggering not-yet-implemented synthesis.
//
// All APIs verified against Bezel_Standard_Library.md. No invented APIs.
// Spec refs: §9.8 (v0.141), §28.13, stdlib §2.14/§2.16.

#include <string>
#include <string_view>

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::parseAndCheckOwnership;
using bezel::test::runJit;

// True iff the program is accepted (no errors) through ownership.
bool semaAccepts(std::string_view src) {
    auto r = parseAndCheckOwnership(src);
    return !r.diagnostics.hasErrors();
}

// True iff the program is rejected with the §9.8 not-serialization-eligible
// diagnostic (rather than some unrelated error).
bool rejectsAsIneligible(std::string_view src) {
    auto r = parseAndCheckOwnership(src);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not serialization-eligible") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// True iff compiling `src` fails with the #703 codegen GUARD diagnostic — the
// clean "Shared/Weak wire codegen not yet implemented" error that prevents a
// silent miscompile of a Shared/Weak component once S1 admits it at sema.
bool guardedAtCodegen(std::string_view src) {
    auto r = runJit(std::string(src));
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not yet implemented in codegen") !=
                std::string::npos &&
            d.message.find("Shared<T>") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// An eligible leaf class: a `serialized tree` class with a single int field.
const char* kEligibleInner =
    "class Inner serialized tree : ISerializable, IDeserializable "
    "  public property V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Inner; ";

// An INELIGIBLE leaf class: no graph-shape clause, no ISerializable.
const char* kPlainInner =
    "class Plain "
    "  public property V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Plain; ";

const char* kImport = "module M; import System.Serialization; ";

} // namespace

// ===========================================================================
// S1: Shared<T> component eligibility (§9.8 v0.141)
// ===========================================================================

// §9.8 v0.141: a class with an owned `Shared<Inner>` field, where Inner is
// serialization-eligible, is itself serialization-eligible. (Before S1 the
// `Shared<Inner>` field was rejected as ineligible because Shared<T> was not
// in the eligible component set.)
BEZEL_TEST(serialization_typedriven_703,
           s1_shared_field_of_eligible_class_accepted_spec_9_8_v0141) {
    BEZEL_EXPECT_TRUE(semaAccepts(
        std::string(kImport) + kEligibleInner +
        "class Holder serialized graph : ISerializable, IDeserializable "
        "  public owned Handle: Shared<Inner>; "
        "  public constructor(owned handle: Shared<Inner>) "
        "    Handle := handle; "
        "  end constructor; "
        "end Holder; "));
}

// §9.8 v0.141: `Shared<Plain>` where Plain is NOT serialization-eligible →
// the recursion on the element rejects it. The diagnostic must name field-level
// ineligibility, not silently accept.
BEZEL_TEST(serialization_typedriven_703,
           s1_shared_field_of_ineligible_class_rejected_spec_9_8_v0141) {
    BEZEL_EXPECT_TRUE(rejectsAsIneligible(
        std::string(kImport) + kPlainInner +
        "class Holder serialized graph : ISerializable, IDeserializable "
        "  public owned Handle: Shared<Plain>; "
        "  public constructor(owned handle: Shared<Plain>) "
        "    Handle := handle; "
        "  end constructor; "
        "end Holder; "));
}

// ===========================================================================
// S1: Weak<T> component eligibility (§9.8 v0.141)
// ===========================================================================

// §9.8 v0.141: a nullable `Weak<Inner>?` back-reference field, where Inner is
// serialization-eligible, is itself eligible (serialized as a $ref). Weak<T>
// is non-owning, so the field carries no disposal obligation.
BEZEL_TEST(serialization_typedriven_703,
           s1_weak_field_of_eligible_class_accepted_spec_9_8_v0141) {
    BEZEL_EXPECT_TRUE(semaAccepts(
        std::string(kImport) + kEligibleInner +
        "class Node serialized graph : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public Back: Weak<Inner>?; "
        "  public constructor(v: int, back: Weak<Inner>?) "
        "    V := v; "
        "    Back := back; "
        "  end constructor; "
        "end Node; "));
}

// §9.8 v0.141: `Weak<Plain>?` where Plain is NOT serialization-eligible →
// rejected by the recursion on the element.
BEZEL_TEST(serialization_typedriven_703,
           s1_weak_field_of_ineligible_class_rejected_spec_9_8_v0141) {
    BEZEL_EXPECT_TRUE(rejectsAsIneligible(
        std::string(kImport) + kPlainInner +
        "class Node serialized graph : ISerializable, IDeserializable "
        "  public property V: int; "
        "  public Back: Weak<Plain>?; "
        "  public constructor(v: int, back: Weak<Plain>?) "
        "    V := v; "
        "    Back := back; "
        "  end constructor; "
        "end Node; "));
}

// ===========================================================================
// S1: codegen guard — no latent miscompile (§9.8 v0.141, #703)
// ===========================================================================

// S1 admits Shared<T> components at SEMA, but their type-driven wire codegen
// ($id/$ref identity) lands in a later slice. Until then, actually CALLING
// Serialize on a class with a Shared<T> component must produce a clean
// "not yet implemented" codegen error — NOT a silent miscompile of the builtin
// handle as an owning class component. (Before S1 this program was rejected at
// sema; S1 moves the rejection to a clean codegen guard, a step toward the
// type-driven model.)
BEZEL_TEST(serialization_typedriven_703,
           s1_shared_component_serialize_codegen_guarded_703) {
    BEZEL_EXPECT_TRUE(guardedAtCodegen(
        std::string(kImport) + kEligibleInner +
        "class Holder serialized graph : ISerializable, IDeserializable "
        "  public owned Handle: Shared<Inner>; "
        "  public constructor(owned handle: Shared<Inner>) "
        "    Handle := handle; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var inner := new Inner(1); "
        "  using var sh := new Shared<Inner>(inner); "
        "  using var h := new Holder(sh.Clone()); "
        "  using var s := new BinarySerializer(); "
        "  h.Serialize(s); "
        "  return 0; "
        "end Main; end C;"));
}
