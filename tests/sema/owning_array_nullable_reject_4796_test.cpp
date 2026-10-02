// fix #4796 / §14.2 (v0.307): `where T: class` admits ONLY non-nullable
// reference types. The owning-collection families (OwningArray<T>,
// OwningSet<T>, OwningStack<T>, OwningQueue<T>, OwningLinkedList<T>,
// OwningDictionary<K, V>) declare `where T: class` per stdlib §4.2;
// rejecting nullable elements was the load-bearing §14.2 row the class-only
// restriction above did not enforce. Pre-fix: `OwningArray<Foo?>` was
// admitted — the comment near `owningCollectionOwnedSlot` (TypeResolution_Names.cpp:70,
// stdlib §4.2 / §14.4, #1327) explicitly said "Nullable class / interface
// wrappers are admitted (the `?` does not change the underlying type's
// class-ness)". The §14.2 row says otherwise — non-nullability is the
// load-bearing word — and #4796 closes the gap.
//
// This test is the AOT twin of the lit fixture
// `tests/lit/sema/owning_collection_element_761__owned_array_nullable_class_admitted.bzl`
// (test file and suite name carry #4796 per [[feedback-testfile-and-suite-inherit-issue-number]]).
// The lit file is the file-resident regression check; this gtest adds the
// family's other cases (Set/Stack/Queue/LinkedList/Dictionary, interface
// elements, the non-`?` over-correction guards, and the §14.4 verbatim
// OwningTask<Lock?> ADMIT control) so the fix's blast radius across the
// owning families is pinned.
//
// ⚠ Memory-safety framing: a nullable slot in an owning collection is not
// only a spec violation — the runtime has no `null`-skipping machinery for
// this family (`§28.11`'s `null_element_is_skipped_safely` shape is owned by
// `OwningBuffer<T?>` / inline-array form, not by `OwningArray<T?>` / etc.),
// so a `Clear()` cascade that calls `Dispose` on a slot the runtime treats
// as live dereferences null. The reject closes this UAF shape at compile
// time. The §14.4 verbatim `OwningTask<Lock?>` example is admissible because
// the `?` is the Pattern B task RESULT nullability, NOT T substitution — the
// `T = Lock` (non-nullable) substitution still satisfies `where T: class`.
//
// Spec citations in this test:
//   - §14.2 — `where T: class` row, "Non-nullable reference types only";
//   - §14.4 — class-only restriction on owning generics; the verbatim
//     `OwningTask<Lock?>` Pattern B task-result example;
//   - §28.11 — owning-family element-slot-policy (null is not admitted
//     here, only in the buffer/array form).

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff an ERROR diagnostic is THIS defect's owning-nullable rejection.
// Distinct from the pre-existing `requires a class or interface element
// type` predicate text — the new message names "non-nullable element type"
// and points to §14.2 / #4796. Asserting the precise substring is what
// separates this fix's diagnostic from the value-type reject arm above
// (`#1327`) and from any cascade diagnostic; see [[feedback-probe-discriminator]]
// — a coarse helper cannot distinguish them.
bool hasOwningNullableElementError(const JitResult& r) {
    if (r.ran) return false;  // compile must have failed
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        // §14.2 / #4796 specific property line — disambiguates from the
        // pre-existing class-only-restriction reject (which says
        // "requires a class or interface element type").
        if (d.message.find("non-nullable element type") == std::string::npos)
            continue;
        if (d.message.find("§14.2") == std::string::npos) continue;
        if (d.message.find("#4796") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff an ERROR diagnostic is the pre-existing value-type reject
// (the §14.4 / #1327 line). Used to confirm controls below still fire the
// OLD reject when a value-type element appears — a fix that re-routed
// the #1327 message path through the new helper would silently
// disarm the old guard.
bool hasOwningElementClassOnlyError(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("requires a class or interface element type") ==
            std::string::npos)
            continue;
        if (d.message.find("#1327") == std::string::npos) continue;
        return true;
    }
    return false;
}

// =============================================================================
// REJECT — the new §14.2 / #4796 owning-nullable-element check fires.
// =============================================================================

// §14.2, v0.307 — the issue's own reproducer: an `OwningArray<Foo?>` where
// `Foo` is a non-nullable class. The OLD predicate (`owningElementBadDesc`)
// returned empty for this element (it's a class), so sema admitted it;
// #4796 closes that gap.
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_array_nullable_class_rejected_4796) {
    auto r = runAot(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Foo?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §14.2, v0.307 — nullable INTERFACE element (the `?` is the
// interface-wrapper's nullability, not the underlying type's class-ness).
// Same shape as the class case — both are absorbed by #4796.
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_array_nullable_interface_rejected_4796) {
    auto r = runAot(
        "module M; "
        "interface IFoo method F(): int; end IFoo; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<IFoo?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §14.2, v0.307 — the rejection applies across the rest of the owning
// family at the SAME slot (slot 0). One case per non-array owning type
// pins the over-narrow guard: a fix that only widened OwningArray's
// element check would falsely admit these. (OwningDictionary's nullable
// case is its V slot — see below.)
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_set_nullable_class_rejected_4796) {
    auto r = runAot(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var a := new OwningSet<Foo?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_stack_nullable_class_rejected_4796) {
    auto r = runAot(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var a := new OwningStack<Foo?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_queue_nullable_class_rejected_4796) {
    auto r = runAot(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var a := new OwningQueue<Foo?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_linked_list_nullable_class_rejected_4796) {
    auto r = runAot(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var a := new OwningLinkedList<Foo?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §14.2, v0.307 — `OwningDictionary<K, V>`'s V slot (slot 1) is the
// OWNED slot; a nullable V triggers #4796 the same way. The K slot
// (slot 0) is value-typed by design (§28.11 last sentence) and was
// never covered by `owningCollectionOwnedSlot` — that pre-existing
// shape is unaffected by #4796.
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_dictionary_nullable_value_rejected_4796) {
    auto r = runAot(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<int, Foo?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =============================================================================
// ADMIT controls — over-correction guards. A fix that turned the #4796
// reject into a blanket "any non-class? reject" would fail one of these.
// =============================================================================

// §14.2, v0.307 — the LEGITIMATE owning path stays green: a class element
// (no nullable wrapper) is admitted. Baseline regression guard.
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_array_class_element_admitted_4796) {
    auto r = runAot(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Foo>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_FALSE(hasOwningElementClassOnlyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §14.2 / §14.4, v0.307 — interface element (no `?`) is the canonical
// §14.4 admitted case. Pinning this keeps the §14.4 own-admitting example
// ("OwningArray<IConnection> is admitted because IConnection may be
// implemented by class types with real disposal obligations") alive
// alongside the new reject.
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_array_interface_element_admitted_4796) {
    auto r = runAot(
        "module M; "
        "interface IFoo method F(): int; end IFoo; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<IFoo>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_FALSE(hasOwningElementClassOnlyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §14.2, v0.307 — existing value-type rejections (the §14.4 / #1327
// "requires a class or interface element type" line) must still fire so
// the new nullable-reject did not displace them. A predicate rewrite that
// dropped the value-type arm would silently disarm #1327.
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_array_int_still_rejected_4796) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<int>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningElementClassOnlyError(r));
    BEZEL_EXPECT_FALSE(hasOwningNullableElementError(r));
}

// §14.4 verbatim `OwningTask<Lock?>` example — Pattern B task RESULT
// (a borrowed example from the spec, not T substitution). The `?` is
// the task's RESULT nullability, so `T = Lock` (non-nullable) still
// satisfies `where T: class`. A fix that extended the #4796 reject
// across the OwningTask / IOwning* interfaces would silently disarm
// §14.4's worked example — the test pins the ADMIT behaviour so a
// future merge that crosses the family boundary surfaces here.
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_task_nullable_result_admitted_4796) {
    auto r = runAot(
        "module M; "
        "public class Lock public constructor() end constructor; end Lock; "
        "public class Holder "
        "  private _t: OwningTask<Lock?>?; "
        "  public constructor() end constructor; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §14.2, v0.307 — the §4.14 / #4679 family OWNING-BUFFER/SPAN admits
// nullable elements (its null-skipping shape is part of its
// per-element contract). A fix that broadened the #4796 branch to
// cover `owningBufferSpanMoveOnlySites_` (e.g. by widening the
// `owningCollectionOwnedSlot` recognised-name list) would silently
// reject `OwningBuffer<Foo?>` — this control pins the admitted shape
// so that hazard is gated. (See [[feedback-corpus-reject-diagnostic-blinded]]
// on the cost of admitting over-corrections silently.)
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_buffer_nullable_class_admitted_4796) {
    auto r = runAot(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<Foo?>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasOwningNullableElementError(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §14.2, v0.307 — alias-of-builtin probe (residual #4): the fix's src comment
// claims alias-transparent head lookup (`type O<T> := OwningArray<T>; ...
// new O<Foo?>()`) routes through the same `owningCollectionOwnedSlot` name
// recognition, but this path was NOT exercised by the new tests. The
// type-aliases dereference to the alias HEAD (`OwningArray`) before the
// collection-name check fires; if the resolver's recorder-key path runs on
// the aliased name rather than the head, the fix would silently admit this.
//
// FINDING (pre-existing, not #4796 regression): the alias dereference path
// falls into the "`new OwningArray(…)` produced no value" arm BEFORE the
// §14.2 reject reaches — the same arm fires for `type O<T> := Array<T>; new
// O<Foo>()` (no #4796 involvement) and is keyed on the alias-of-builtin
// pattern itself, not on the §14.2 rule. The fix's §14.2 reject works on the
// DIRECT path (`new OwningArray<Foo?>()`); the alias-of-builtin route is
// blocked upstream by a separate pre-existing issue (alias dereference does
// not see `OwningArray` / `Array` as having a user declaration, falling into
// the static-class "may not be instantiated" arm — diagnostic cites #2974).
//
// This test pins the OBSERVED behaviour (any compile failure) without
// asserting the specific §14.2 diagnostic — the diagnostic is unreachable
// on the alias path because of the upstream alias-of-builtin issue, and the
// rejection is correct (it is a hard compile error, not a silent admit).
// The pre-existing alias-of-builtin gap is a separate diagnostic track and
// is NOT in scope for #4796; tracked as #4796-residual — see also
// [§17 handback note for the alias-of-builtin probe].
//
// The fix's correctness for the user-facing case (direct `OwningArray<Foo?>`)
// is pinned by `owning_array_nullable_class_rejected_4796` above; this probe
// is informational only.
BEZEL_TEST(sema_owning_array_nullable_reject_4796,
           owning_array_alias_of_builtin_does_not_run) {
    auto r = runAot(
        "module M; "
        "class Foo public n: int := 0; end Foo; "
        "type O<T> := OwningArray<T>; "
        "class C public static method Main(): int "
        "  using var a := new O<Foo?>(); "
        "  return 0; "
        "end Main; end C;");
    // The alias-of-builtin path errors with a static-class "may not be
    // instantiated" diagnostic upstream of the §14.2 reject. The program
    // must NOT run (silent admit would be the failure mode). Diagnostic
    // text is intentionally NOT asserted — the upstream diagnostic is
    // keyed on the alias-of-builtin shape, not on the §14.2 rule.
    BEZEL_EXPECT_FALSE(r.ran);
}

}  // namespace
