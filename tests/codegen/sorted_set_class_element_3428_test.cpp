// Spec-derived regression tests for fix #3428 and its narrowing by fix #2773
// (B1) — `new SortedSet<T>()` with a REFERENCE element type.
//
// AUTHORITY. stdlib §4.18 (v0.286) declares, verbatim:
//
//     public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
//       public constructor();
//       ...
//     end SortedSet;
//
// under the heading sentence "A comparison-ordered set: membership by
// `CompareTo`-equality, iteration in ascending order.", and the prose adds
// "for `SortedSet<T>` that identity is `CompareTo`-equality, and adding an
// equal element leaves the set unmodified".
//
// THE ORIGINAL #3428 DEFECT (measured on 0c5ecd1, v0.283). A class
// `Ord: IComparable<Ord>` satisfies the sole constraint, yet
// `new SortedSet<Ord>()` reported lowerLocalDecl's #2974 degrade —
// "`new SortedSet(…)` produced no value … matches no documented constructor"
// — which was FALSE (§4.18 documents a public parameterless constructor).
// #3428 turned that false #2974 degrade into a TRUTHFUL rejection instead of
// an implementation, because at the time nothing in this compiler could
// dispatch a class/interface element's `CompareTo` through the vtable for a
// SORTED collection.
//
// WHAT CHANGED (fix #2773 (B1), v0.286). `fix #3488` (SortedDictionary's
// reference-key ordering) lifted the #1195/#2768 `Array.Sort` comparator
// thunks into the shared `buildObjectCompareToThunk`, giving EVERY class or
// interface element with a dispatchable `CompareTo` a real
// `CompareTo`-through-the-vtable comparator — #3428's stated objection no
// longer holds for such an element. Fix #2773 (B1):
//   * `genericBackingType` admits a SortedSet<T> reference element (named
//     explicitly by `collTy->headName() == "SortedSet"` — `Set`/`OwningSet`
//     are UNCHANGED and keep their array-backed path, #1070);
//   * `lowerNew`'s SortedSet arm builds the generic-key table with
//     `buildSortedObjectKeyIdentity`'s (constant-hash, `CompareTo == 0`) pair
//     — the same identity `fix #3488` gives `SortedDictionary`'s reference
//     KEY — so membership is `CompareTo`-equality, not §8.3 reference
//     identity, honoring "adding an equal element leaves the set unmodified";
//   * every other SortedSet consumer (`for`-in / `GetEnumerator` via
//     `wrapSortedSetAsIEnumerable`, `.First`/`.Last`) already dispatches
//     through `buildSortedKeyComparator`, which itself already routes a
//     reference key through `buildObjectCompareToThunk` (`fix #3488`) — so
//     once construction succeeds with the right identity pair, ordering is
//     free; no change was needed at those sites.
//
// #3428's rejection SURVIVES, narrowed, for the residual case: `Object` (no
// declaration to search) or a class/interface that declares no dispatchable
// `CompareTo`. That is a strict NARROWING of the rejection surface — no
// program that compiled before this fix stops compiling, and no program
// that was silently wrong becomes silently wrong in a NEW way (§6c: a loud
// rejection is kept exactly where the alternative would be quiet-wrong).
//
// A rejection is asserted on the diagnostic TEXT, never via a coarse
// `hasErrors()`: `runAot`'s WIFSIGNALED path synthesizes an `Error` diagnostic
// for a runtime SIGSEGV too, so a bare "did not run" cannot tell a genuine
// rejection from a crash — and `hasErrors()` cannot tell THIS rule from any
// other error in the program.
//
// The ordering/dedup CORRECTNESS surface for the newly-admitted class and
// interface elements (for-in, GetEnumerator, First/Last, Add/TryAdd/Remove
// dedup, the §15.4 fresh-store warning) is pinned in
// tests/codegen/sorted_set_class_element_2773_test.cpp, not here — this file
// stays scoped to `lowerNew`'s admission/rejection boundary, matching its
// original #3428 scope.
//
// The FALSE-REJECT guard below is unchanged in intent: every element category
// the generic-key backing already supported must still compile AND still
// produce the right answer, and `Set<T>`/`OwningSet<T>` must still accept a
// class element via their own (untouched) array-backed path.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// The VALUE channel for an observable outside 0..255 — a process exit status is
// masked to 8 bits, so `1233` would arrive as `209`.
std::string out(const std::string& src) { return runAotOut(src); }

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// True when NO diagnostic in the compile carries `needle`.
bool lacks(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return false;
    return true;
}

// A class that satisfies §4.18's `where T: IComparable<T>` constraint, plus a
// record / enum for the value-element controls.
const char* PRELUDE =
    "module M; import System.Collections.Generic; "
    "record Rec(A: int); "
    // `Col` LISTS `IComparable<Col>` and declares `CompareTo`, which is what
    // makes an enum satisfy `where T: IComparable<T>` under spec §14.2 rule 4
    // ("T is an enum that lists I ... in its inheritance clause and declares
    // the matching public members"). A bare `enum Col Red, Green;` does NOT
    // conform — fix #4318 (v0.315) rejects `SortedSet<Col>` in sema before
    // any lowering — so `enum_element_still_constructs` keeps its subject
    // (an enum element lowers to i32 and dedups) by spelling the enum the way
    // spec §10.5's own `enum Priority: IComparable<Priority>` example does.
    "enum Col: IComparable<Col>  Red, Green; "
    "  public method CompareTo(other: Col): int => "
    "    (if int(this) < int(other) then -1 "
    "     elif int(this) > int(other) then 1 "
    "     else 0); "
    "end Col; "
    "class Ord: IComparable<Ord> "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method CompareTo(other: Ord): int return V - other.V; end CompareTo; "
    "end Ord; "
    // A SELF-comparable interface (`IOrd: IComparable<IOrd>`) — §4.18's
    // constraint on an INTERFACE element T is `T: IComparable<T>`, which
    // `IComparable<Ord>` (used bare AS an element type) does NOT satisfy: it
    // declares `CompareTo(other: Ord): int`, not `CompareTo(other:
    // IComparable<Ord>): int`, so it is not self-comparable and is exactly
    // the "no dispatchable CompareTo" residual case, not the accepted one.
    "interface IOrd: IComparable<IOrd> "
    "  method Val(): int; "
    "end IOrd; "
    "class OrdI: IOrd "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method Val(): int => V; "
    "  public method CompareTo(other: IOrd): int return V - other.Val(); end CompareTo; "
    "end OrdI; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

// The head of the RESIDUAL diagnostic (fix #2773 (B1), v0.286) — stable
// across the two element categories that still reject (`Object`, and a
// class/interface with no dispatchable `CompareTo`).
const char* NEEDLE =
    "`SortedSet<T>` requires an element type with a dispatchable `CompareTo`";

// Fix #4318 (v0.315) decides `where T: IComparable<T>` in SEMANTIC ANALYSIS
// (`argSatisfiesTypeBound`, spec §14.2 rule 2) before any lowering, so for
// every NAMED element type the sema message is what the user sees and the
// codegen `NEEDLE` above is unreachable for it. `NEEDLE` is NOT dead: sema has
// no `Object`→`IComparable` knowledge, so `object_element_still_diagnosed` and
// `stale_2974_constructor_claim_gone` still reach it and still pin it. The
// needle below stops BEFORE the constraint text on purpose: #4318 emits the
// bare `where T: IComparable` where stdlib §4.18 and §14.2 rule 2 spell it
// `where T: IComparable<T>` — issue #5423 — so pinning only the spec's part
// keeps these rows correct across that fix.
std::string constraintNeedle(const std::string& typeName) {
    return "type argument `" + typeName +
           "` does not satisfy the constraint `where ";
}

// The head of the NULLABLE-element diagnostic (review fix, post-0c8612d), a
// SEPARATE message from NEEDLE above: a nullable reference element was excluded
// by `genericBackingType`'s pre-existing `!ty->nullable` guard BEFORE any
// `CompareTo` was ever consulted, so the message must not claim "no
// dispatchable CompareTo" — a claim false whenever the underlying type (e.g.
// `Ord: IComparable<Ord>`) actually declares one.
//
// The emitter (LlvmBackend_Exprs2.cpp's `rejElemTy->nullable` arm) is now
// UNREACHABLE for a named element type: fix #4318's sema check (v0.315)
// rejects the type argument before any lowering, so no test pins this text and
// the constant is removed rather than left dangling (an unused `const char*`
// at namespace scope is not a build error but reads as a live pin). The
// anti-false-claim half of that review fix IS still pinned — by
// `nullable_class_element_diagnoses_nullability` and
// `nullable_interface_element_diagnoses_nullability` below, which assert it
// over every diagnostic the compiler now emits for those shapes. See those
// rows' comments for the measured replacement message.

}  // namespace

// ── ACCEPTED as of fix #2773 (B1) (stdlib §4.18, v0.286) ─────────────────────
//
// #3428's original filing: a CLASS element declaring `CompareTo` was
// rejected. Fix #2773 (B1) implements it instead — full ordering/dedup
// coverage lives in sorted_set_class_element_2773_test.cpp; the rows below
// pin only that the OLD rejection is gone for the newly-accepted shapes, and
// that it correctly SURVIVES for the residual ones (§7: a fix that reverses
// a prior rejection must update the tests that pinned it).

// per stdlib §4.18 (v0.286), fix #2773 (B1): the #3428 filed repro, verbatim
// in shape, now CONSTRUCTS — `Ord` declares a single-argument `CompareTo`.
BEZEL_TEST(sorted_set_class_element_3428, class_element_with_compareto_constructs) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var b := new SortedSet<Ord>(); "
        "  return b.Count; ")),
        0);
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): a SELF-comparable INTERFACE
// element (`IOrd: IComparable<IOrd>`) constructs —
// `buildObjectCompareToThunk`'s interface arm reuses #2768's
// `ensureIfaceCmpThunk` itable dispatch, selected by the exact
// `selfComparableIfaceCompareTo` constraint.
BEZEL_TEST(sorted_set_class_element_3428, interface_element_with_compareto_constructs) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var b := new SortedSet<IOrd>(); "
        "  return b.Count; ")),
        0);
}

// per stdlib §4.18 (v0.286), spec §14.2 (v0.286), review fix (post-0c8612d):
// a NULLABLE class element (`SortedSet<Ord?>`) must not be told `Ord`
// "declares no ... CompareTo" — that claim is FALSE: `Ord` declares exactly
// one (`CompareTo(other: Ord): int`, the very same declaration
// `class_element_with_compareto_constructs` above proves is dispatchable),
// and the element is excluded because a NULLABLE type cannot satisfy §14.2's
// `where T: IComparable<T>` at all.
//
// ⚠ WHAT CHANGED, AND WHAT THIS ROW NOW PINS. These two rows originally
// required the message to NAME NULLABILITY (`NULLABLE_NEEDLE`, emitted by
// LlvmBackend_Exprs2.cpp's `rejElemTy->nullable` arm). Fix #4318 (v0.315)
// moved the whole decision into sema, which rejects the TYPE ARGUMENT before
// any lowering runs — so that arm is no longer reached for a named element and
// `NULLABLE_NEEDLE` is unreachable for this shape. MEASURED on f8c6ab5f, the
// only diagnostic is:
//
//   type argument `Ord` does not satisfy the constraint `where T: IComparable
//
// which is TRUE (a nullable `Ord?` does not satisfy the constraint) but names
// neither the nullability nor the `?`. So the positive assertion is replaced
// by the constraint needle, and the anti-false-claim assertion — the part
// these rows were actually added for, per the review fix above — is KEPT
// VERBATIM and still checked over every diagnostic. The lost nullability
// naming is reported with the measurements; it is not silently dropped.
BEZEL_TEST(sorted_set_class_element_3428, nullable_class_element_diagnoses_nullability) {
    auto r = runAot(prog(
        "  using var b := new SortedSet<Ord?>(); "
        "  return b.Count; "));
    BEZEL_EXPECT_FALSE(r.ran);
    bool foundConstraintNeedle = false;
    bool foundFalseNoCompareToClaim = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(constraintNeedle("Ord")) != std::string::npos)
            foundConstraintNeedle = true;
        if (d.message.find("`Ord` declares no") != std::string::npos)
            foundFalseNoCompareToClaim = true;
    }
    BEZEL_EXPECT_TRUE(foundConstraintNeedle);
    BEZEL_EXPECT_FALSE(foundFalseNoCompareToClaim);
}

// per stdlib §4.18 (v0.286), spec §14.2 (v0.286), review fix (post-0c8612d):
// same shape, a NULLABLE SELF-comparable interface element
// (`SortedSet<IOrd?>`) — `IOrd` is exactly the interface
// `interface_element_with_compareto_constructs` above proves constructs when
// non-nullable, so the message must not claim it "does not declare a
// SELF-comparable CompareTo" (a DIFFERENT false claim than the class case,
// since `selfComparableIfaceCompareTo` is never even reached for a nullable
// element — the nullable guard fires first, in `genericBackingType`).
BEZEL_TEST(sorted_set_class_element_3428, nullable_interface_element_diagnoses_nullability) {
    auto r = runAot(prog(
        "  using var b := new SortedSet<IOrd?>(); "
        "  return b.Count; "));
    BEZEL_EXPECT_FALSE(r.ran);
    // See the class-case row above for why NULLABLE_NEEDLE is replaced here.
    bool foundConstraintNeedle = false;
    bool foundFalseNotSelfComparableClaim = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(constraintNeedle("IOrd")) != std::string::npos)
            foundConstraintNeedle = true;
        if (d.message.find("does not declare a SELF-comparable") !=
            std::string::npos)
            foundFalseNotSelfComparableClaim = true;
    }
    BEZEL_EXPECT_TRUE(foundConstraintNeedle);
    BEZEL_EXPECT_FALSE(foundFalseNotSelfComparableClaim);
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): `IComparable<Ord>` used BARE as
// the element type is an interface, but NOT self-comparable — it declares
// `CompareTo(other: Ord): int`, not `CompareTo(other: IComparable<Ord>): int`
// — so `selfComparableIfaceCompareTo` finds no match and this shape stays in
// the residual rejection, distinguishing "is an interface" from "is a
// SELF-comparable interface" (the actual §4.18 constraint).
//
// THE TWO HALVES OF THE MESSAGE, AND WHY THE OFFENDING ARGUMENT IS SPELLED
// `IComparable` AND NOT `IComparable<Ord>` (fix-forward of #5423). The
// diagnostic is built in two independent pieces, and they are spelled by two
// different helpers that were never the same helper:
//
//   * the OFFENDING ARGUMENT comes from `arg->headName()`
//     (TypeResolution_Names.cpp:3435, the kNullDeclSelfRefConstraints arm),
//     and `TypeRef::headName()` returns `astNode->name->parts[0]` — the HEAD
//     NAME ONLY, generic arguments dropped. The same is true of the general
//     `where`-constraint emitter `argSpelling` (TypeResolution_Checks.cpp:
//     10921), which joins `name->parts` and appends `?` for nullability and
//     likewise never renders `genericArgs`. MEASURED on a second shape
//     (`SortedSet<IOther<IOther>>`) the argument is likewise named `IOther`.
//   * the CONSTRAINT comes from `d.boundName`, which #5423 correctly changed
//     from the bare `IComparable` to `IComparable<T>` — matching stdlib
//     §4.18's declared `where T: IComparable<T>`.
//
// So the complete message on this revision is, verbatim:
//
//   type argument `IComparable` does not satisfy the constraint
//   `where T: IComparable<T>` (§14.2)
//
// and this row now pins that WHOLE message. That is STRICTLY STRONGER than
// `constraintNeedle("IComparable<Ord>")`: the earlier form asserted a
// substring the compiler never emits on any input (it asked for the head name
// to be followed by `<Ord>`), so it could only ever fail; it pinned neither
// half. `constraintNeedle` above deliberately stops before the `where ` text,
// which is why it cannot express the constraint half — hence the full needle.
//
// This is NOT a rejection→acceptance flip: `rejectsWith` still requires
// `!r.ran`, so the program must still be REJECTED, and it is (MEASURED, the
// only diagnostic is the one quoted above). The soundness question this row
// exists to ask — whether satisfying the type-BOUND is being wrongly treated
// as equivalent to discharging the interface's METHOD obligation — resolves
// NEGATIVE: `argSatisfiesTypeBound` returns false here (trace in the handback),
// so `IComparable<Ord>` is correctly NOT admitted as satisfying
// `IComparable<IComparable<Ord>>`, exactly as this row's comment above states.
BEZEL_TEST(sorted_set_class_element_3428, non_self_comparable_interface_still_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  using var b := new SortedSet<IComparable<Ord>>(); "
        "  return b.Count; "),
        "type argument `IComparable` does not satisfy the constraint "
        "`where T: IComparable<T>` (§14.2)"));
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): `Object` is a Primitive-kind
// TypeRef (no declaration to search for `CompareTo`) and STILL rejects — the
// residual case the NEEDLE above targets.
BEZEL_TEST(sorted_set_class_element_3428, object_element_still_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  using var b := new SortedSet<Object>(); "
        "  return b.Count; "),
        NEEDLE));
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): a class that implements NO
// `CompareTo` at all also still rejects — `buildObjectCompareToThunk` finds
// no method to dispatch, so `elemTy` is nulled back out and `lowerNew` falls
// to the same rejection block `Object` takes.
BEZEL_TEST(sorted_set_class_element_3428, class_without_compareto_still_diagnosed) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; class Plain public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Plain; "
        "class C public static method Main(): int "
        "  using var b := new SortedSet<Plain>(); "
        "  return b.Count; "
        "end Main; end C;",
        constraintNeedle("Plain")));
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): the point of #3428's ORIGINAL
// fix is still true for the residual case — #2974's stale "matches no
// documented constructor" text must not resurface on the new rejection.
BEZEL_TEST(sorted_set_class_element_3428, stale_2974_constructor_claim_gone) {
    BEZEL_EXPECT_TRUE(lacks(prog(
        "  using var b := new SortedSet<Object>(); "
        "  return b.Count; "),
        "matches no documented constructor"));
}

// per stdlib §4.18 (v0.286), fix #2773 (B1): the §15.4 fresh-store shape
// #3428's "knock-on" section flagged is NO LONGER dormant for a
// CompareTo-bearing class element — construction now succeeds, so the fresh
// store must draw the mandatory §15.4 warning (Ownership_Checks.cpp's two
// whitelists were widened in the same commit; the warning ITSELF is pinned in
// sorted_set_class_element_2773_test.cpp, which uses the ownership-check
// harness this codegen-oriented file does not link). This row only confirms
// the program still COMPILES (no error) now that it is a real, non-dormant
// program — dormancy is gone, per #3428's own note that this row "goes red
// the moment construction is implemented".
BEZEL_TEST(sorted_set_class_element_3428, fresh_store_shape_now_reachable) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var b := new SortedSet<Ord>(); "
        "  b.Add(new Ord(1)); "
        "  return b.Count; ")),
        1);
}

// ── FALSE-REJECT GUARD: every supported element category still works ─────────
//
// Each row below compiles and produces the same value on the PRE-fix compiler,
// which is what makes it a control. They cover the whole eligibility list
// `genericBackingType`'s set arm accepts: record, string, non-nullable
// primitive (int / char / double / decimal), enum.

// per stdlib §4.18 (v0.283), #3428: `int` elements — construct, dedup by
// value, and iterate in ascending order.
BEZEL_TEST(sorted_set_class_element_3428, int_element_still_sorted_and_dedups) {
    // The observable is 1233 — outside 0..255, so it travels on stdout rather
    // than the exit code, which WEXITSTATUS would mask to 209.
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s := new SortedSet<int>(); "
        "  s.Add(3); s.Add(1); s.Add(3); s.Add(2); "
        "  var acc: int := 0; "
        "  for var x in s do acc := acc * 10 + x; end; "
        "  Console.WriteLine(acc * 10 + s.Count); "
        "  return 0; ")),
        /* 123 ascending, then Count 3 (the duplicate 3 dedups) */
        std::string("1233"));
}

// per stdlib §4.18 (v0.283), #3428: `string` elements — the D8 row of
// `genericBackingType`'s set arm (strings go generic for sets, unlike dicts).
BEZEL_TEST(sorted_set_class_element_3428, string_element_still_constructs) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<string>(); "
        "  s.Add(\"b\"); s.Add(\"a\"); s.Add(\"b\"); "
        "  return s.Count; ")),
        2);
}

// per stdlib §4.18 (v0.283), #3428, UPDATED by fix #3947 (v0.294): `Rec` is
// an auto-hashable RECORD element (§9.2) with NO `CompareTo`. THIS ROW USED
// TO pin that it constructed and deduped by STRUCTURAL equality — "the
// oldest generic-key row (#343)". Fix #3947 closes that gap: §4.18's SOLE
// constraint is `T: IComparable<T>`, and hashability is a different property
// (§3.6/§9.2), so an element with no dispatchable `CompareTo` is not a legal
// `SortedSet<T>` instantiation regardless of whether it is hashable — `.Add`
// now reports the same located diagnostic `class_without_compareto_still_diagnosed`
// above gets for a class with no `CompareTo`.
BEZEL_TEST(sorted_set_class_element_3428, record_element_without_compareto_now_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  var s := new SortedSet<Rec>(); "
        "  s.Add(new Rec(1)); "
        "  return 0; "),
        constraintNeedle("Rec")));
}

// per stdlib §4.18 (v0.283), #3428: an ENUM element (lowers to i32).
BEZEL_TEST(sorted_set_class_element_3428, enum_element_still_constructs) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Col>(); "
        "  s.Add(Col.Red); s.Add(Col.Green); s.Add(Col.Red); "
        "  return s.Count; ")),
        2);
}

// per stdlib §4.18 (v0.283), #3428: `decimal` — the #685 row (a 16-byte struct
// with well-defined value equality), and `char`/`double`, so the guard covers
// the whole primitive width the arm admits rather than `int` alone.
BEZEL_TEST(sorted_set_class_element_3428, other_primitive_elements_still_construct) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var a := new SortedSet<decimal>(); a.Add(1.5m); a.Add(1.5m); "
        "  using var b := new SortedSet<char>();    b.Add('x');  b.Add('y'); "
        "  using var c := new SortedSet<double>();  c.Add(2.5);  c.Add(2.5); "
        "  return a.Count * 100 + b.Count * 10 + c.Count; ")),
        121);
}

// per stdlib §4.10 (v0.283), #3428: the arm this fix edits is shared with
// `Set<T>` / `OwningSet<T>`, whose CLASS element is legal and works today via
// the Array-backed linear scan (#1070). The rejection must not leak onto them.
BEZEL_TEST(sorted_set_class_element_3428, class_element_set_still_accepted) {
    // Deliberately NOT a re-add of the same reference. `Set<Ord>.Add(o1)` twice
    // was MEASURED (on 0c5ecd1, i.e. before this fix) to leave `Count == 3`
    // while `TryAdd(o1)` on the same set reports the element present — a
    // pre-existing §4.10 defect in the array-backed class-element path that this
    // commit neither causes nor repairs, filed separately. Pinning it here would
    // couple this guard to a bug. The row asserts only what is correct today and
    // must stay correct: two DISTINCT instances both land, and the set finds one.
    BEZEL_EXPECT_EQ(run(prog(
        "  using var o1 := new Ord(3); "
        "  using var o2 := new Ord(1); "
        "  var s := new Set<Ord>(); "
        "  s.Add(o1); s.Add(o2); "
        "  var n: int := s.Count; "
        "  if s.Contains(o1) then n := n + 10; end; "
        "  return n; ")),
        /* Count 2, and Contains(o1) is true */ 12);
}

// per stdlib §4.10 (v0.283), #3428: the `OwningSet<T>` half of the same arm.
BEZEL_TEST(sorted_set_class_element_3428, class_element_owning_set_still_accepted) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new OwningSet<Ord>(); "
        "  s.Add(new Ord(3)); s.Add(new Ord(1)); "
        "  return s.Count; ")),
        2);
}

// per stdlib §4.17 (v0.283), #3428: the `SortedDictionary<K, V>` arm is a
// DIFFERENT branch of `lowerNew` and is deliberately left alone by this commit
// — its class-key defects are filed separately. Pinning that it still compiles
// keeps this fix honest about its blast radius.
BEZEL_TEST(sorted_set_class_element_3428, sorted_dictionary_arm_untouched) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var d := new SortedDictionary<int, int>(); "
        "  d.Add(3, 1); d.Add(1, 2); "
        "  return d.Count; ")),
        2);
}
