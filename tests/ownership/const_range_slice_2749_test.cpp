// Ownership / diagnostics — §21.1's `const` rejection reaches the RANGE-SLICE
// INDEXER form `arr[r]`, not only the `arr.Slice(r)` method spelling.
// Issue #2749 item 2, the residual surface of #2709.
//
// Spec, v0.240:
//   §21.1 "Variable declarations", the **Ownership** paragraph, verbatim:
//     "The initializer of a runtime `const` must not be an **owned production**
//      (§28.3): the binding never takes a disposal obligation and never holds an
//      affine (consumable) value. Admitted: value-typed expressions […],
//      `string` and other runtime-managed values, `null` […], and **borrows**
//      […]. Rejected, with a diagnostic naming this rule and the `using var`
//      remedy: a fresh `new`, an `owned`-returning call, […]."
//   §28.3 "Producers" table, the range-slice row, verbatim:
//     "| Range slice `arr[r]` on `Array<T>` (§15.6) | Owned — a fresh
//      `Array<T>`, the compiler-recognized `Slice(r: Range<int>): owned
//      Array<T>` form (stdlib §4.4); **not** the indexer row above. The
//      container is owned by the caller and discharged under the standard rules
//      […] |"
//   §28.3's *indexer* row, by contrast: "| Indexer access `coll[i]` | Borrowed
//      (qualified by family […]) |" — and §21.1 admits borrows verbatim.
//
// So the spec makes `arr[0..2]` and `arr.Slice(0..2)` ONE operation, and §21.1
// therefore reaches both spellings identically.
//
// THE BUG, measured on 0b9693e (the #2709 fix itself). #2709 closed the remedy
// loop for the CALL spelling only: its predicate is keyed on an `ast::CallExpr`,
// `arr[0..2]` is an `ast::IndexExpr`, and `producerState` has no `IndexExpr` arm
// either. Both halves of the loop were still live, and through the *more*
// idiomatic spelling:
//
//   using var s := a[0..2];   → WARNED "is vacuous … use `const x := …;`"
//   const     s := a[0..2];   → ACCEPTED  ← §21.1 says reject
//
// which also made `a.Slice(0..2)` (rejected) and `a[0..2]` (accepted) disagree
// about the same operation — an inconsistency #2709 introduced rather than a gap
// it merely left.
//
// THE FIX. A fourth disjunct on the SHARED predicate
// `MethodOwnership::constInitializerRejected` — `common::
// isBuiltinArrayRangeSliceIndex` — so the const arm and the #2073 lint guard
// still cannot skew, plus a message branch of its own (the branches remain total
// under a trailing `else`, so a future disjunct degrades to a less specific
// rejection, never to a silent accept). Diagnostic-only, exactly like #2709: no
// disposal, retain, or dataflow state changes.
//
// ⚠ WHAT THIS DOES NOT FIX, and it matters for reading these tests. The `arr[r]`
// slice leaks under EVERY binding spelling, not just `const`. Measured on
// 0b9693e with 64-iteration amplified probes under LSan, values pinned through
// distinct exit codes: `const`, `using var` and plain `var` each leaked 64 of 64
// `bz_array_slice` allocations, while `using var s := a.Slice(0..2)` ran clean
// (the positive control proving LSan was armed and discriminating). So codegen
// reclaims the CALL spelling and reclaims nothing for the INDEX spelling. That
// is a DISPOSAL defect, filed separately as #2753 and deliberately untouched
// here — widening a free is the double-free direction #2709 declined. What this
// fix does is remove the `const` binding, which by construction has no free path
// at all, and stop the lint from advertising it.
//
// NARROWNESS. The predicate mirrors the ONE sema arm that produces this
// classification on all three of its conditions and additionally cross-checks
// sema's verdict by TypeRef pointer identity; every condition fails OPEN. The
// accept-controls below are the enumeration of what reaches it: a single-element
// index, a `string` slice, a dictionary read, a span range index, an inline
// array, a user type with a range indexer, and `OwningArray<T>` (whose §15.6
// rejection must stay the only diagnostic).

#include <string>

#include "support/aot_support.hpp"       // runAot / runAotChecked
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// §21.1's rejection in its NEW range-slice wording. Matching the SPECIFIC text
// is what pins this fix: the pre-existing `new`-RHS branch, #2709's materializer
// branch and the trailing general branch all also say "must not take an owned
// production", so a bare `hasErrors()` (or even a match on that phrase alone)
// would pass on the wrong branch.
bool hasConstRangeSliceError(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("must not take an owned production") ==
            std::string::npos)
            continue;
        if (d.message.find("a range slice `arr[r]` produces a fresh `owned` "
                           "Array<T>") == std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// ANY §21.1 const rejection, whichever branch produced it. Used by the
// accept-controls: an over-wide predicate must not merely land on a different
// message, it must not reject at all.
bool hasAnyConstOwnedProductionError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("must not take an owned production") !=
                std::string::npos ||
            d.message.find("must not take a deferred-activation producer") !=
                std::string::npos)
            return true;
    }
    return false;
}

// The #2073 vacuous-`using var` lint, Case A, matched on its TEXT.
bool hasInertRhsWarning(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("RHS is a value-typed/runtime-managed") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// §15.6's own rejection of slicing on `OwningArray<T>`.
bool hasOwningArraySliceError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("range slicing `[a..b]` is not admitted on") !=
            std::string::npos)
            return true;
    }
    return false;
}

// A single-class unit whose `M()` body is `body`, with an `Array<int>` `a` in
// scope carrying three elements — the same fixture shape #2709's test uses.
std::string prog(const std::string& body) {
    return "module M; class C "
           "  public static method M(): int "
           "    using var a := new Array<int>(); "
           "    a.Add(3); a.Add(1); a.Add(2); " + body +
           "    return 0; end M; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

}  // namespace

// =====================================================================
// (1) THE #2749 item-2 REPRO — the remedy loop, both halves, on `a[0..2]`.
// FAILS WITHOUT THE FIX in BOTH halves: pre-fix the lint fired on the `using
// var` spelling and the `const` spelling it recommended was silently accepted.
// =====================================================================
BEZEL_TEST(const_range_slice_2749,
           range_slice_remedy_loop_closed_spec_28_3_21_1) {
    // Half A: the lint must not advertise a spelling §21.1 forbids.
    auto pinned = parseAndCheckOwnership(prog("using var s := a[0..2];"));
    BEZEL_EXPECT_FALSE(pinned.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(pinned, "s"));

    // Half B: and the spelling it used to recommend is rejected by name.
    auto remedy = parseAndCheckOwnership(prog("const s := a[0..2];"));
    BEZEL_EXPECT_TRUE(hasConstRangeSliceError(remedy, "s"));
}

// =====================================================================
// (2) The two spellings of ONE operation now agree. §28.3's row calls `arr[r]`
// "the compiler-recognized `Slice(r: Range<int>): owned Array<T>` form", so a
// rejection of one and an acceptance of the other is a spec inconsistency.
// #2709 rejected only the second.
// =====================================================================
BEZEL_TEST(const_range_slice_2749,
           index_and_method_spellings_agree_spec_28_3_stdlib_4_4) {
    auto idx = parseAndCheckOwnership(prog("const s := a[0..2];"));
    auto call = parseAndCheckOwnership(prog("const s := a.Slice(0..2);"));
    BEZEL_EXPECT_TRUE(hasAnyConstOwnedProductionError(idx));
    BEZEL_EXPECT_TRUE(hasAnyConstOwnedProductionError(call));
}

// =====================================================================
// (3) The Range-VALUE form `var r := 0..2; a[r]` (#1079) is the same producer —
// sema types it through the same arm — so it is rejected identically. A
// predicate that only matched a syntactic `RangeExpr` would leave this open.
// =====================================================================
BEZEL_TEST(const_range_slice_2749,
           range_variable_form_rejected_spec_28_3_1079) {
    auto r = parseAndCheckOwnership(
        prog("var rg := 0..2; const s := a[rg];"));
    BEZEL_EXPECT_TRUE(hasConstRangeSliceError(r, "s"));
}

// =====================================================================
// (4) A slice OF a slice is still an owned production at both levels.
// =====================================================================
BEZEL_TEST(const_range_slice_2749, nested_slice_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(prog("const s := a[0..2][0..1];"));
    BEZEL_EXPECT_TRUE(hasConstRangeSliceError(r, "s"));
}

// =====================================================================
// ACCEPT-CONTROLS. Each is a shape that reaches the new predicate and must come
// out ADMITTED — a false reject breaks working code, which is worse than the
// leak this fixes. Every one of these goes RED on an over-wide predicate (e.g.
// one keyed on the RESULT type's `Array` head instead of on sema's range-slice
// arm).
// =====================================================================

// §28.3's *indexer* row: "Indexer access `coll[i]` | Borrowed", and §21.1 admits
// borrows verbatim. A single-element read must keep compiling as a `const`.
BEZEL_TEST(const_range_slice_2749,
           accept_single_element_index_spec_28_3_indexer_row) {
    auto r = parseAndCheckOwnership(
        "module M; class Cell public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; end Cell; "
        "class C public static method M(): int "
        "    using var cell := new Cell(7); "
        "    using var cells := new Array<Cell>(); "
        "    cells.Add(cell); "
        "    const c := cells[0]; "          // borrow of a slot — admitted
        "    return c.V; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// The int-element case of the same row, on the very receiver type the predicate
// keys on: `Array<int>` with a scalar index is NOT a slice.
BEZEL_TEST(const_range_slice_2749,
           accept_scalar_index_on_array_receiver_spec_28_3) {
    auto r = parseAndCheckOwnership(prog("const v := a[0];"));
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// §28.9: a `string` range slice produces a `string` — runtime-managed, with no
// disposal obligation — and §21.1 admits it verbatim ("`string` and other
// runtime-managed values"). Same `[a..b]` syntax, different receiver.
BEZEL_TEST(const_range_slice_2749,
           accept_string_range_slice_spec_28_9_21_1) {
    auto r = parseAndCheckOwnership(
        prog("var t := \"abcdef\"; const u := t[0..2];"));
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// A dictionary key read is §28.3's indexer row too; its result is
// `genericArgs[1]`, never the receiver's own TypeRef.
BEZEL_TEST(const_range_slice_2749, accept_dictionary_key_read_spec_28_3) {
    auto r = parseAndCheckOwnership(
        prog("using var d := new Dictionary<string, int>(); "
             "d.Add(\"k\", 5); const v := d[\"k\"];"));
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// A span index. The span families' stdlib `Slice` overloads return span heads,
// NOT owned arrays, and sema types a range index on them as the ELEMENT — so
// the receiver-head gate excludes them. This is also the control for the
// TypeRef-identity condition: an element read whose result head happened to be a
// declaration-less `Array` must not be mistaken for a slice.
BEZEL_TEST(const_range_slice_2749, accept_span_index_spec_4_15) {
    auto r = parseAndCheckOwnership(
        prog("var sp: ArraySpan<int> := a.AsSpan(); const v := sp[0];"));
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// An inline array `[N]T` — a value type; §15.6 forbids slicing it, and a scalar
// index is a value read. The receiver's TypeRef kind is InlineArray, not Named,
// so it never reaches the head test.
BEZEL_TEST(const_range_slice_2749, accept_inline_array_index_spec_15_1) {
    auto r = parseAndCheckOwnership(
        prog("var inl: [3]int := [4, 5, 6]; const v := inl[1];"));
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// A USER type with a range-taking indexer. Its receiver TypeRef carries a
// `declaration`, which the predicate requires to be null (BUILTIN-only), so the
// user indexer keeps §28.3's ordinary indexer classification.
BEZEL_TEST(const_range_slice_2749,
           accept_user_type_range_indexer_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class Grid "
        "  private _v: int := 3; "
        "  public property this[rg: Range<int>]: int => _v; "
        "end Grid; "
        "class C public static method M(): int "
        "    using var g := new Grid(); "
        "    const v := g[0..2]; "
        "    return v; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasAnyConstOwnedProductionError(r));
}

// §15.6 rejects slicing `OwningArray<T>` outright ("it would manufacture a
// second owner for each owned element"). That rejection must remain the ONLY
// diagnostic — a §21.1 message on top would be a second, misleading error
// recommending `using var` for a form that is not admitted in any spelling.
BEZEL_TEST(const_range_slice_2749,
           owning_array_slice_keeps_only_the_15_6_rejection) {
    auto r = parseAndCheckOwnership(
        "module M; class Cell public V: int := 0; end Cell; "
        "class C public static method M(): int "
        "    using var oa := new OwningArray<Cell>(); "
        "    const s := oa[0..2]; "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningArraySliceError(r));
    BEZEL_EXPECT_FALSE(hasConstRangeSliceError(r, "s"));
}

// A plain `var` binding is untouched: the new disjunct is reached only from the
// `const` arm and the `using var` lint guard, so plain-`var` diagnostics are
// exactly what they were. (It still leaks — #2753 — which this fix does not
// claim to address.)
BEZEL_TEST(const_range_slice_2749, accept_plain_var_slice_unchanged) {
    auto r = parseAndCheckOwnership(prog("var s := a[0..2]; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
