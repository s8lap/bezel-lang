// Ownership — issue #3706 (§28.3 clause 1 / §28.9 / §28.11 / §29.2 "Iterator
// yield ownership", v0.294): the field-store half of the issue. Storing a
// bare read of a plain-`var` / `using var` / §21.1 `const` `Array<T>`
// COLLECTION LITERAL local into a field compiled clean with no diagnostic —
// the array's backing then leaks (it is never freed: the local's own
// scope-end reclaim is correctly declined because the store is a genuine
// escape, and nothing else ever reclaims it).
//
// Spec, v0.294:
//   K1.5 (§28.3 kernel): "Fresh productions are owned: `new`, reference-typed
//     collection literals, `owned`-returning calls, `move` results, owned
//     operator results."
//   §28.3 clause 1 (quoted at #3687's own citation, MEASURED verbatim from
//     the "Borrow lifetime rules" bullet this issue's own diagnostic cites):
//     "A borrowed scope-bound local never satisfies the rule ... so a
//     borrowed local may not be assigned to any field of any object."
//   §28.9 / §28.11 (collection-element ownership): `Array<T>` is
//     trivial-dispose (its `Dispose()` is a no-op) but its BACKING is
//     "reclaimed with the instance" — the compiler's own scope-end
//     `bz_array_free` of the local that solely owns it.
//   §29.2's changelog entry (v0.287, #3706's own tracking comment): "the
//     field-store half (`b.Items := xs`) is §28.3 clause 1's existing
//     rejection" — i.e. an implementation gap, not an open ruling.
//
// ROOT CAUSE: `MethodOwnership::isBorrowableType` (Ownership_Checks.cpp)
// classifies EVERY `Array<T>`-typed identifier as non-borrowable — a
// declaration-less Named `TypeRef` (Array<T> is a builtin with no user
// `TypeDecl`) that is not the one recognized builtin-exception carve-out —
// so `returnedExprMayBorrow`'s gate is false for it and
// `borrowSourceForExpr` / `ownedLocalBorrowSource` can never see a bare
// `Array<T>` identifier as a live borrow source, REGARDLESS of the local's
// `states_` classification (measured: true for a plain `var`, a
// `using var`, AND a `const` alike — all three compiled clean before this
// fix, with no diagnostic).
//
// THE FIX: `MethodOwnership::borrowingArrayLiteralLocalFieldStoreSource`
// (Ownership_Checks.cpp) is a new, TYPE/SHAPE-based recognizer — independent
// of `states_` and of `isBorrowableType` — that `checkBorrowStoreEscape`
// consults directly for the `Dest::Field` destination: is the RHS a bare
// identifier naming a `var` / `using var` / `const` local bound directly to
// a fresh, non-dict `[...]` collection literal of the builtin `Array<T>`
// head? If so, the store is rejected under the SAME §28.3 clause 1 text as
// any other owned scope-bound local's field-store escape.
//
// Scoped to the `Array<T>` head only (this issue's exact title and measured
// repro); `Set<T>` / `Dictionary<K,V>` literals are the identical K1.5 shape
// and share the SAME gap (see the CONTROL row below, which pins that they
// are DELIBERATELY left unrejected here) — filed as a follow-up rather than
// folded in silently into this fix.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// `binding` supplies the full local-declaration statement; `xs` must be the
// bound name in every case (the field-store line is fixed).
std::string fieldStoreProg(const std::string& binding) {
    return "module M; "
           "class Box public Items: Array<int> := []; end Box; "
           "class C "
           "  public static method Main(): int "
           "    var b := new Box(); " +
           binding +
           "    b.Items := xs; "
           "    return b.Items.Count; "
           "  end Main; "
           "end C;";
}

}  // namespace

// =============================================================================
// NEGATIVE — plain `var xs: Array<int> := [1, 2, 3];` stored into a field.
// FAILS WITHOUT THE FIX: compiles clean (hasErrors() is false); the array's
// backing then leaks (measured under ASan/LSan, see the codegen-side
// companion test's field-store probe for the runtime confirmation).
// =============================================================================
BEZEL_TEST(borrowing_array_literal_field_store_3706,
           var_bare_array_literal_field_store_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        fieldStoreProg("var xs: Array<int> := [1, 2, 3]; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "may not be stored into a field"));
    BEZEL_EXPECT_TRUE(diagContains(r, "xs"));
}

// =============================================================================
// NEGATIVE — the §21.1 `const` twin. `borrowingArrayLiteralLocalFieldStoreSource`
// recognizes BOTH `LocalDecl` and `LocalConstDecl` from a single shared view.
// =============================================================================
BEZEL_TEST(borrowing_array_literal_field_store_3706,
           const_bare_array_literal_field_store_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        fieldStoreProg("const xs: Array<int> := [1, 2, 3]; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "may not be stored into a field"));
    BEZEL_EXPECT_TRUE(diagContains(r, "xs"));
}

// =============================================================================
// NEGATIVE — the `using var` twin. MEASURED discovery during this fix: a
// `using var` binding of the SAME literal was ALSO accepted before this fix
// (not merely the plain-`var` shape the issue's title names) — `states_`
// classifying it Owned via §28.10 does not help, because
// `borrowSourceForExpr`'s gate (`isBorrowableType`) is false for Array<T>
// regardless of `states_` state. This recognizer does not exclude `isUsing`
// for exactly that reason.
// =============================================================================
BEZEL_TEST(borrowing_array_literal_field_store_3706,
           using_var_bare_array_literal_field_store_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        fieldStoreProg("using var xs: Array<int> := [1, 2, 3]; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "may not be stored into a field"));
    BEZEL_EXPECT_TRUE(diagContains(r, "xs"));
}

// =============================================================================
// OVER-REJECT GUARD — the SAME literal local, used only in borrow positions
// (no field store), must stay clean. Proves the new recognizer is keyed on
// the field-STORE, not on the mere existence of a bare-array-literal local.
// =============================================================================
BEZEL_TEST(borrowing_array_literal_field_store_3706,
           array_literal_no_field_store_accepted_control) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    return xs.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// OVER-REJECT GUARD — a DERIVED expression (`xs[0..2]`, the §28.3 range-slice
// row / #2753: a FRESH `owned Array<int>` slice, not a bare identifier) is
// untouched by this recognizer (it only matches a bare `IdentifierExpr`).
// MEASURED: this program is rejected anyway, but by a DIFFERENT, pre-existing
// rule — "a freshly-produced owned value cannot be stored into the
// non-`owned` field `Items`" (§28.5/§28.6) — never by this fix's own
// diagnostic. Confirms the new recognizer does not fire on a
// non-bare-identifier RHS and produce a confusing double diagnostic.
// =============================================================================
BEZEL_TEST(borrowing_array_literal_field_store_3706,
           derived_expression_field_store_unaffected_by_this_recognizer) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box public Items: Array<int> := []; end Box; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(); "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    b.Items := xs[0..2]; "
        "    return b.Items.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagContains(r, "collection literal local"));
    BEZEL_EXPECT_TRUE(diagContains(r, "freshly-produced owned value"));
}

// =============================================================================
// CONTROL (known, deliberately-scoped gap) — a `Set<T>` collection literal
// local stored into a field the SAME way is NOT rejected by this fix: the
// recognizer is scoped to the `Array<T>` head only (this issue's exact title
// and measured repro). `Set<T>`/`Dictionary<K,V>` share the identical K1.5
// shape and gap; widening to them is filed as a follow-up rather than folded
// in silently here. This row pins the CURRENT (unchanged) behavior so a
// future widening is a deliberate, reviewed change to this test, not a
// silent one.
// =============================================================================
BEZEL_TEST(borrowing_array_literal_field_store_3706,
           set_literal_field_store_not_yet_rejected_known_gap_control) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box public Items: Set<int> := []; end Box; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(); "
        "    var xs: Set<int> := [1, 2, 3]; "
        "    b.Items := xs; "
        "    return b.Items.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
