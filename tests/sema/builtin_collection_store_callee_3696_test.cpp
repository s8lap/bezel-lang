// Spec-derived regression test for issue #3696 — `SortedSet<T>` is absent
// from `Ownership_Checks.cpp`'s `builtinCollectionStoreCallee` (§28.14
// call-boundedness) collection-name list.
//
// THIRD whitelist gap in the file. The two #3428 named (both fixed by
// #2773 (B1)) are the §15.4 fresh-store warning gates
// (`checkCollectionBorrowStoreEscape` / `checkIndexerSetBorrowStoreEscape`)
// and admit `SortedSet<T>` because stdlib §4.18 gives it the same
// borrowing-family value semantics as §4.17 / `SortedDictionary` (already
// in those lists). This third site is the §28.14 CALL-BOUNDEDNESS walk's
// sink — `callBoundedWalkExpr` uses it to decide whether a call's receiver
// is itself a "collection store" for the purpose of classifying whether an
// argument is reachable-beyond-the-call. The fix is to add `SortedSet` to
// its disjunction (NOT `OwningSortedSet` — stdlib §4.18 declares no owning
// twin, mirroring `SortedDictionary` / `Set` here, both of which appear by
// their borrowing names; owning collection `Add` takes OWNED §28.8
// transfer and is gated by `builtinOwnedSinkArg`).
//
// AUTHORITY. Spec §28.14 (v0.177 / v0.196 with deferred-activation), v0.286:
// "the callback may … not … be stored into any collection that is itself
// reachable beyond the call (a parameter, a borrow, or a field — a purely
// local collection is a local binding) … These obligations are
// compiler-verified for user-authored callees." stdlib §4.18 (v0.286):
// `SortedSet<T>` carries the same borrowing-family value semantics as
// §4.17 / `SortedDictionary`.
//
// WHAT THIS FILE PINS.
//   * Row 1: the FIX ITSELF — `SortedSet<T>.Add(callBoundedSource)` through
//     a parameter (reachable beyond the call) is rejected with
//     "collection reachable beyond the call". This is the diagnostic
//     `callBoundedWalkExpr` is supposed to fire on the §28.14 collection-
//     store sink; on HEAD-before-fix `builtinCollectionStoreCallee` returned
//     `false` for a `SortedSet` receiver and the diagnostic did NOT fire —
//     the §28.14 admission became a silent miss.
//   * Row 2: same shape, FIELD receiver — also reachable beyond the call
//     (a borrowed field of `this`), so the same sink must fire.
//   * Row 3: the positive control — `Array<T>` (already in the list) must
//     still be caught with the same diagnostic. Proves the SortedSet fix
//     did not widen the rule's precision past the existing family.
//   * Row 4: the negative control — `SortedSet<T>.Add(value_typed_x)` on a
//     parameter receiver must NOT fire (the argument is not a call-bounded
//     source — it is a plain int, no §28.14 obligation). Proves the fix
//     gates correctly on the CALL-BOUNDED SIDE of the classification, not
//     by blanket-rejecting every `SortedSet.Add` argument.
//   * Row 5: same for the field-receiver negative control — no diagnostic
//     fires for a value-typed `Add`.
//   * Row 6: scan the family — every borrowing-family collection name
//     (Array, Dictionary, SortedDictionary, Set, SortedSet, Stack, Queue,
//     LinkedList) caught; the §28.14 obligation is symmetric across the
//     family.
//
// FAIL-WITHOUT: Row 1, Row 2 (silent admit). PASS-WITH: every row.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// Row 1 — the FIX ITSELF. §28.14 call-boundedness: storing a call-bounded
// parameter into a parameter-reachable SortedSet<T> is the §28.14
// collection-store sink. Pinned diagnostic text.
BEZEL_TEST(builtin_collection_store_callee_3696,
           sorted_set_param_receiver_rejected_call_bounded_source) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Enlist(f: () -> int, xs: SortedSet<() -> int>) "
        "    xs.Add(f); "
        "  end Enlist; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "collection reachable beyond the call"));
}

// Row 2 — same shape, FIELD receiver. A field of `this` is reachable
// beyond the call (`exprReachableBeyondCall` returns true); the same sink
// must fire on a `SortedSet` field.
BEZEL_TEST(builtin_collection_store_callee_3696,
           sorted_set_field_receiver_rejected_call_bounded_source) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _handlers: SortedSet<() -> int> := new SortedSet<() -> int>();"
        "  public method Register(f: () -> int) "
        "    _handlers.Add(f); "
        "  end Register; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "collection reachable beyond the call"));
}

// Row 3 — positive control: Array (already in the list) must still be
// caught. Proves the fix preserved the existing family's precision
// (didn't widen to add SortedSet AND accidentally drop Array).
BEZEL_TEST(builtin_collection_store_callee_3696,
           array_param_receiver_still_rejected_call_bounded_source) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Enlist(f: () -> int, xs: Array<() -> int>) "
        "    xs.Add(f); "
        "  end Enlist; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "collection reachable beyond the call"));
}

// Row 4 — negative control: SortedSet<T>.Add(value_typed_x) on a parameter
// receiver must NOT fire (the argument is plain int, no §28.14
// obligation). Proves the fix gates correctly on the call-bounded side of
// the classification, not by blanket-rejecting every SortedSet.Add
// argument.
BEZEL_TEST(builtin_collection_store_callee_3696,
           sorted_set_param_receiver_value_typed_add_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Enlist(x: int, xs: SortedSet<int>) "
        "    xs.Add(x); "
        "  end Enlist; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 5 — same negative control for the field receiver. No call-bounded
// source — no §28.14 obligation — no diagnostic. `owned` field avoids the
// §28.5 fresh-store warning that would otherwise shadow the §28.14 walk's
// classification (and is unrelated to this fix).
BEZEL_TEST(builtin_collection_store_callee_3696,
           sorted_set_field_receiver_value_typed_add_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private owned _xs: SortedSet<int> := new SortedSet<int>();"
        "  public method Register(x: int) "
        "    _xs.Add(x); "
        "  end Register; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 6 — family scan. Every borrowing-family collection name in the
// §28.14 walk's whitelist must reach the same sink. SortedSet joined the
// family at this fix.
BEZEL_TEST(builtin_collection_store_callee_3696,
           borrowing_family_all_reject_call_bounded_source) {
    for (const std::string& cn :
         {"Array", "Dictionary", "SortedDictionary", "Set", "SortedSet",
          "Stack", "Queue", "LinkedList"}) {
        auto r = parseAndCheckOwnership(
            "module M; class C "
            "  public static method Enlist(f: () -> int, xs: " + cn +
            "<() -> int>) "
            "    xs.Add(f); "
            "  end Enlist; "
            "end C;");
        BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
        BEZEL_EXPECT_TRUE(diagContains(r, "collection reachable beyond the call"));
    }
}