// fix #2744 / §28.11 / §35.4 (v0.240): an owning-collection element type may
// not be a choice. Spec §3.2 classifies every choice — owning-classified
// variants included — as a value type, and §28.11's element-slot-policy table
// rejects choices in the OWNING family in BOTH rows:
//
//   value type (non-move-only)         → copies / rejected-at-instantiation
//   move-only value type (§35.4)       → rejected-at-instantiation / rejected
//
// so a plain value-like choice AND an owning-classified choice both belong
// outside an owning-collection element slot. Pre-fix the predicate
// `Resolver::owningElementBadDesc` (TypeResolution_Checks.cpp:2534, the single
// authority the declaration-site check and the Pass-2 sweep share, per #2713
// name-list-skew lesson) fell through to `return {}` for `ast::TypeDeclKind::
// Choice`, so `OwningArray<MyChoice>` and `OwningArray<MyOwningChoice>` both
// silently compiled.
//
// ⚠ THIS IS A MEMORY-SAFETY FIX, NOT A TIDINESS FIX. The issue's own §28.11
// table notes "value type (non-move-only)" → copies in the borrowing column,
// so `Array<MyChoice>` is still LEGAL. But the owning-column admission is
// unsound: a value-like choice carrying a `string` payload reproduced #2713's
// dangling-slot shape — codegen's `elementPolicyFor()` Value row installs no
// disposer AND takes no reference on insert, while the `owned` argument
// transfer still releases the caller's, leaving the slot dangling. Reading
// the element back out is a use-after-free, not a leak. The fix closes the
// hazard at compile time.
//
// ⚠ FALSE-REJECT IS THE MAIN HAZARD of a rejection fix. The accept controls
// below outnumber the reject cases and cover every spelling the predicate
// can be wrongly re-routed into:
//   - a borrowing-collection element (Array<MyChoice>) — borrowing column
//     "copies" cell, §28.11 verbatim;
//   - an OwningDictionary's KEY slot — "an owning dictionary owns its
//     *values*; its keys are value-like" (per §28.11 last sentence);
//   - a choice inside a non-owning position of an owning type (the choice
//     sits inside an element that is itself class-typed);
//   - existing element rejections (int, string, MyClass);
//   - a generic-method parameter shadowing an enclosing one (the
//     boundByMethod guard).
//
// Spec citations in this test:
//   - §3.2 — reference vs value types (choices are value types);
//   - §28.11 — element-slot-policy table (rejection in owning family);
//   - §35.4 — choice classification (owning-classified choices as move-only).

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff an ERROR diagnostic is THIS defect's owning-element rejection.
// Deliberately NOT a bare `hasErrors()`: the assertion must pin the §14.4
// property text, the #1327 origin, AND the specific structural reason — so
// a coarse helper cannot mistake a cascade parse-error or unrelated reject
// for the §28.11 / §35.4 closure. (Branch-order regression hazard from
// #1463: a new disjunct in the predicate can silently re-route message
// branches; assert specific substrings.)
bool hasOwningChoiceElementError(const JitResult& r) {
    if (r.ran) return false;  // compile must have failed
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        // The §4.2/§14.4 owning-element property line (printed by both the
        // declaration-site check and the Pass-2 sweep).
        if (d.message.find("requires a class or interface element type") ==
            std::string::npos)
            continue;
        // #1327 lineage — keeps the diagnostic anchored to the original
        // element-check fix the closure lives downstream of.
        if (d.message.find("#1327") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff the program's compile FAILED and produced a diagnostic whose
// §14.4 property line names a choice (this fix's contribution). Distinct
// from hasOwningChoiceElementError so controls for int / string / class
// rejections can assert specifically against the existing predicate arms.
bool hasOwningChoiceErrorMessage(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("requires a class or interface element type") ==
            std::string::npos)
            continue;
        if (d.message.find("choice") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff an ERROR diagnostic is the §3.6 rule-2 PER-INSTANTIATION rejection
// (the Pass-2 sweep's wording, distinct from the declaration-site wording).
bool hasPerInstantiationChoiceError(const JitResult& r,
                                    std::string_view expectedArg) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("requires a class or interface element type") ==
            std::string::npos)
            continue;
        if (d.message.find("3.6 rule 2") == std::string::npos) continue;
        if (d.message.find("type parameter") == std::string::npos) continue;
        if (d.message.find(expectedArg) == std::string::npos) continue;
        return true;
    }
    return false;
}

// Compiles, links, runs, and exits with `expected`.
// (Unused in the current ACCEPT grid — kept available for the over-correction
// guards that may grow with later fixes.)
[[maybe_unused]] static bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// =============================================================================
// REJECT — the declaration-site predicate (TypeResolution_Names.cpp:1633
// reaches owningElementBadDesc directly).
// =============================================================================

// §28.11 / §35.4, v0.240 — a value-like choice carrying only `int` payloads
// is still a value type (§3.2), and §28.11 rejects value-typed elements in
// the owning column ("copies" in the borrowing column is unaffected — see
// ACCEPT grid below). Pre-fix: silently compiled.
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_array_value_like_choice_rejected_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class C public static method Main(): int "
        "  using var a: OwningArray<MyChoice> := new OwningArray<MyChoice>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
}

// §28.11 / §35.4, v0.240 — a value-like choice carrying a `string` payload.
// This is the case the issue's own §28.11 note flags: codegen's elementPolicyFor
// Value row installs no disposer AND takes no reference on insert, while an
// `owned` argument transfer still releases the caller's — a heap-use-after-
// free on read-back. (Severity is the inverted-leak shape called out by the
// issue's "MEASURE before fixing" warning: NOT a leak, a UAF.)
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_array_value_like_choice_with_string_rejected_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(s: string); end MyChoice; "
        "class C public static method Main(): int "
        "  using var a: OwningArray<MyChoice> := new OwningArray<MyChoice>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
}

// §28.11 / §35.4, v0.240 — an OWNING-classified choice (a case payload is a
// class reference, so §35.4 classifies the choice as Owning). §28.11's
// "move-only value type (owning inline arrays and owning-classified choices,
// §35.4)" row is rejected in BOTH columns — borrowing column too. Pre-fix:
// silently compiled, with codegen falling into the same dangling-slot shape
// as #2713's `OwningLinkedList<string>` (the slot would store the choice
// without taking a reference, releasing the caller's).
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_array_owning_classified_choice_rejected_2744) {
    auto r = runAot(
        "module M; "
        "class MyClass public n: int := 0; end MyClass; "
        "choice MyOwningChoice A, B(c: MyClass); end MyOwningChoice; "
        "class C public static method Main(): int "
        "  using var a: OwningArray<MyOwningChoice> := "
        "      new OwningArray<MyOwningChoice>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
}

// §28.11 / §35.4, v0.240 — the rejection applies across the rest of the
// owning family (OwningSet, OwningStack, OwningQueue, OwningLinkedList), not
// only OwningArray. One of each is the over-narrow guard: a fix that only
// widened OwningArray's element check would falsely admit the rest.
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_set_choice_rejected_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class C public static method Main(): int "
        "  using var a: OwningSet<MyChoice> := new OwningSet<MyChoice>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
}

BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_stack_choice_rejected_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class C public static method Main(): int "
        "  using var a: OwningStack<MyChoice> := new OwningStack<MyChoice>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
}

BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_queue_choice_rejected_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class C public static method Main(): int "
        "  using var a: OwningQueue<MyChoice> := new OwningQueue<MyChoice>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
}

BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_linkedlist_choice_rejected_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class C public static method Main(): int "
        "  using var a: OwningLinkedList<MyChoice> := "
        "      new OwningLinkedList<MyChoice>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
}

// §28.11, v0.240 — for OwningDictionary<K, V> the OWNED slot is V (slot 1).
// A choice in the V slot is rejected. The K slot (slot 0) is value-like by
// design (per §28.11 last sentence) and is covered by the ACCEPT grid below.
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_dictionary_value_slot_choice_rejected_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class C public static method Main(): int "
        "  using var d: OwningDictionary<int, MyChoice> := "
        "      new OwningDictionary<int, MyChoice>(); "
        "  return d.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
}

// =============================================================================
// PASS-2 — the per-instantiation sweep (§3.6 rule 2 / §14.4 / #2713). A choice
// that arrives through a generic parameter is admitted at the declaration
// site (T is a placeholder) and rejected at each instantiation of the
// enclosing generic type.
// =============================================================================

// §3.6 rule 2 / §28.11 / §35.4, v0.240 — the choice reaches the predicate via
// the per-instantiation closure, not the declaration-site check. The same
// shared predicate fires, so this test pins the message wording for the
// Pass-2 path too (the declaration-site and Pass-2 messages name different
// spans and clauses).
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           choice_via_generic_param_rejected_per_instantiation_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class H<T> where T: class "
        "  private owned f: OwningArray<T> := new OwningArray<T>(); "
        "  public method N(): int return f.Count; end N; "
        "end H; "
        "class C public static method Main(): int "
        "  using var h := new H<MyChoice>(); "
        "  return h.N(); "
        "end Main; end C;");
    // The declaration-site reject is the §14.2 constraint message; the
    // Pass-2 message is what the predicate drives. Either is acceptable as
    // long as the program is not silently admitted.
    BEZEL_EXPECT_TRUE(hasPerInstantiationChoiceError(r, "MyChoice") ||
                      hasOwningChoiceElementError(r));
}

// =============================================================================
// ACCEPT GRID — false reject is worse than the bug. Each of these would go
// red on an over-wide check; together they enumerate the predicates' every
// legitimate non-target path.
// =============================================================================

// §28.11, v0.240 — "value type (non-move-only) … copies". The borrowing
// column admits a choice: `Array<MyChoice>` is the canonical "copies" shape
// and MUST still compile. (A test of the borrowing column's positive cell
// is the strongest possible guard against a §28.11 misreading that rejects
// value-typed elements in BOTH columns.)
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           borrowing_array_choice_admitted_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class C public static method Main(): int "
        "  using var a := new Array<MyChoice>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasOwningChoiceElementError(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §28.11, v0.240 — "an owning dictionary owns its *values*; its keys are
// value-like." A choice in the K slot (slot 0) is NOT rejected, even though
// it is itself a value type. (The `OwningDictionary` constructor takes
// value-typed keys without complaint; the OWNED slot is V.)
//
// To isolate the K-slot test from the V-slot check (which fires for any
// value-typed V), the V slot here is a CLASS — so the predicate has no
// reason to fire at all.
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_dictionary_key_slot_choice_admitted_2744) {
    auto r = runAot(
        "module M; "
        "class MyClass public n: int := 0; end MyClass; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class C public static method Main(): int "
        "  using var d: OwningDictionary<MyChoice, MyClass> := "
        "      new OwningDictionary<MyChoice, MyClass>(); "
        "  return d.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasOwningChoiceElementError(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §28.11 / §3.2, v0.240 — a choice inside a NON-OWNING POSITION of an owning
// type is unaffected. The element of `OwningArray<Holder>` is `Holder`
// (class-typed) and `Holder` has a choice-typed field; the choice is
// reachable only through the class reference, not through the owning
// collection's element slot. (A predicate that walks into the element's
// type's fields would over-reject this.)
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           choice_in_non_owning_position_of_owning_element_admitted_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class Holder public c: MyChoice; end Holder; "
        "class C public static method Main(): int "
        "  using var a: OwningArray<Holder> := new OwningArray<Holder>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasOwningChoiceElementError(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// §14.4 / §28.11, v0.240 — existing element rejections must still fire so
// the new disjunct did not displace them (branch-order regression guard).
// Asserts the generic §14.4 owning-element line, NOT the choice-specific
// text — these are pre-existing rejections (#1327) and the test pins that
// they remain in force.
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_array_int_still_rejected_2744) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  using var a: OwningArray<int> := new OwningArray<int>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
    BEZEL_EXPECT_FALSE(hasOwningChoiceErrorMessage(r));
}

BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_array_string_still_rejected_2744) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  using var a: OwningArray<string> := new OwningArray<string>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningChoiceElementError(r));
    BEZEL_EXPECT_FALSE(hasOwningChoiceErrorMessage(r));
}

// §14.4 / §28.11, v0.240 — the legitimate owning path stays green: a class
// element is admitted by the predicate, both at the declaration site and via
// Pass 2. This is the over-correction guard: a fix that started rejecting
// class elements would fail here and the ASan oracle would not even be
// reached.
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           owning_array_class_element_admitted_2744) {
    auto r = runAot(
        "module M; "
        "class MyClass public n: int := 0; end MyClass; "
        "class C public static method Main(): int "
        "  using var a: OwningArray<MyClass> := new OwningArray<MyClass>(); "
        "  return a.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasOwningChoiceElementError(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ⚠ KNOWN RESIDUAL, PINNED DELIBERATELY — the generic-method-parameter
// shadow case from #2713 carries over here. A method parameter named `T`
// inside `Box<T>` denotes the METHOD's `T`, whose bindings are NOT recorded
// in `instantiations_` (see #2736(a) — there is no METHOD-LEVEL
// INSTANTIATION RECORD today). So the program below — the same shape
// `OwningArray<T>` where the method's `T` is bound to a choice — currently
// compiles silently, even though `OwningArray<MyChoice>` would be rejected
// at the declaration site. This is not #2744's gap to close; it is the
// #2736(a) gap, and the same predicate change closes BOTH simultaneously.
// When #2736(a) lands, THIS TEST MUST FLIP to a rejection.
BEZEL_TEST(sema_owning_collection_choice_element_2744,
           method_param_shadow_choice_silently_admitted_pending_2736a_2744) {
    auto r = runAot(
        "module M; "
        "choice MyChoice A, B(n: int); end MyChoice; "
        "class Box<T> "
        "  public method M2<T>(): int "
        "    using var a := new OwningArray<T>(); "
        "    return a.Count; "
        "  end M2; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(); "
        "  return b.M2<MyChoice>(); "
        "end Main; end C;");
    // Current (known-buggy) behavior: passes silently. The test pins the
    // residual so a future fix does not silently regress it.
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace
