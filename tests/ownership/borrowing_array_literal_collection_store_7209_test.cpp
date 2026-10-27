// Ownership — issue #7209 (§28.11 *Borrowing collections* → *Storing a
// borrow*, clause 2; §28.3 *Producers*; §28.9 *Trivial-dispose classes*;
// stdlib §4.4, v0.343): the COLLECTION-store half of #3706's field-store
// rule. A `using var` collection local inserted into a longer-lived
// container was FREED at its scope end while the container still held it —
// an ASan heap-use-after-free.
//
// THE DEFECT (MEASURED on the staging tip, `bezelc --sanitize=address`):
//
//   using var l: Array<Array<int>> := [];
//   do
//     using var a: Array<int> := [1, 2];
//     l.Add(a);                       // the borrow escapes its scope
//   end do;
//   Console.WriteLine(l[0][0]);        // <- reads freed memory
//
// compiled clean, and the run died with
//   "AddressSanitizer: heap-use-after-free … READ of size 4 … in memcpy",
//   freed by `bz_array_free` (src/runtime/bezel_runtime_collections.cpp:741)
//   and allocated by `bz_array_alloc` (…:257). The plain-`var` spelling of
// the SAME program compiled clean too, and LSan reported "144 byte(s)
// leaked in 2 allocation(s)" — two defects, one per spelling, of ONE
// program.
//
// THE SPEC ANSWER — the STORE IS ILLEGAL (not "the free is spurious"):
//
//   * §28.3's *Producers* table (v0.343) classifies a "collection literal
//     (`[1, 2, 3]`) targeting a reference type (`Array<T>`, `OwningArray<T>`,
//     `Set<T>`, `Dictionary<K, V>`, etc.)" as **Owned**. So `a` is an owned
//     scope-bound binding — it carries the disposal obligation itself.
//
//   * §28.9's *Trivial-dispose classes* (v0.343): every trivial-dispose
//     instance "has exactly one statically-known holder at every point … and
//     the compiler inserts a **free** … at that holder's release point: scope
//     end for locals and `using var`", and the free "releases the instance's
//     own allocation (including the intrinsic backing storage of the
//     compiler-known borrowing collections, part of the instance's single
//     allocation)". `Array<T>` is a CLASS (stdlib §4.4: `public class
//     Array<T>`) and is NOT on §28.9's runtime-managed tier — that tier is
//     `string` / `Type` / boxed value types / function-typed values, and
//     §28.9 says "This tier is exhaustive".
//     ⇒⇒ THE SCOPE-END FREE IS MANDATORY, and the compiler's own §28.10
//     lint ("the RHS is a value-typed/runtime-managed value with no disposal
//     obligation") is the half that is wrong. Answer (A) — "the free is
//     spurious" — is REFUTED by §28.9.
//
//   * §28.11's *Borrowing collections* → *Storing a borrow* (v0.343),
//     clause 2: "a borrow of a **scope-bound binding** — an `owned` parameter
//     included, its value being disposed at the method's end — stored into a
//     collection that provably outlives that scope — the collection is a
//     field, is reachable from one, is held by a parameter that is not
//     `owned` or reached through one (caller storage, which outlives every
//     scope of the body), escapes the method, or is a local of strictly wider
//     scope — is **rejected** at the store site."
//     `Array<T>.Add(item: T)` takes a PLAIN (non-`owned`) parameter (stdlib
//     §4.4), so the store is a BORROW store; and `l` is "a local of strictly
//     wider scope" than `a`.
//     ⇒⇒ THE PROGRAM MUST BE REJECTED. A "make it safe" change that dropped
//     the free would trade a use-after-free for a silent leak — the
//     silent-acceptance inversion this project treats as its top defect
//     class — and would contradict §28.9's mandatory free.
//
// THE FIX: §28.11's collection-store check (`checkCollectionBorrowStoreEscape`)
// now resolves the same collection-literal local that #3706's FIELD-store
// check resolves (`borrowingArrayLiteralLocalFieldStoreSource`) as the
// store's SOURCE. The general machinery cannot see it: `borrowSourceForExpr`
// gates on `returnedExprMayBorrow` → `isBorrowableType`, which answers FALSE
// for EVERY `Array<T>`-typed identifier (a declaration-less Named `TypeRef` —
// `Array<T>` is a builtin with no user `TypeDecl` — outside the one
// recognized builtin-exception carve-out), REGARDLESS of the local's `states_`
// classification. The recognizer is deliberately SHARED between the two
// sinks so they cannot drift.
//
// §28.11 clause 2's own admission test — "a collection that provably
// outlives that scope" — is the test applied: the SAME scope is admitted, a
// WIDER scope is rejected, a NARROWER scope is admitted.
//
// ROOT CAUSE, for the record: `MethodOwnership::isBorrowableType`
// (Ownership_Checks.cpp) mis-classifies the builtin CLASS heads as value
// types, so EVERY §28.3/§28.11 escape rule that gates on "is this read a
// borrow?" is blind to a borrow of a builtin collection instance. Fixing that
// predicate globally is a WIDER change than this issue's scope (it moves the
// §28.10 "adopting a named owned binding" rejection, the §28.10 lint, and
// several other rules at once); the per-sink recognizer is #3706's
// established answer to the same root cause, and this file pins the
// collection sink of it. The sibling sinks of the same root cause are
// measured and filed separately — see the RESIDUALS section of the lane
// handback.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// The stable fragments of the diagnostic this fix owns. A re-wording that
// keeps these tokens is a wording change; a re-wording that drops them is a
// behaviour change this file is meant to catch.
constexpr const char* kDiagMarker = "collection literal local";
constexpr const char* kDiagSpec = "§28.11";

bool hasErrorWith(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The issue's own repro shape: a `using var` collection local inserted into
// a longer-lived borrowing collection. `binding` supplies the inner
// declaration; the stored name is always `a`.
std::string escapeProg(const std::string& binding) {
    return "module M; "
           "public class C "
           "  public static method Main(): int "
           "    using var l: Array<Array<int>> := []; "
           "    do "
           "      " + binding +
           "      l.Add(a); "
           "    end do; "
           "    Console.WriteLine(l[0][0]); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// =============================================================================
// NEGATIVE — the issue's own repro: a `using var` `Array<T>` collection
// literal local stored into a borrowing collection of STRICTLY WIDER scope.
//
// FAILS WITHOUT THE FIX: the program compiles clean (no diagnostic at all)
// and the run dies with an ASan heap-use-after-free. The row asserts the
// DIAGNOSTIC — never "the program did not crash", which would be vacuous
// for a use-after-free (the crash may or may not happen).
// =============================================================================
BEZEL_TEST(borrowing_array_literal_collection_store_7209,
           using_var_bare_array_literal_collection_store_rejected_spec_28_11) {
    auto r = parseAndCheckOwnership(
        escapeProg("using var a: Array<int> := [1, 2]; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(r, kDiagMarker));
    BEZEL_EXPECT_TRUE(hasErrorWith(r, kDiagSpec));
    // The diagnostic must name the offending local and the collection.
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "`a`"));
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "`l`"));
}

// =============================================================================
// NEGATIVE — the plain-`var` spelling of the SAME program. MEASURED: it
// compiled clean and LSan reported 144 B in 2 allocations leaked (the
// local's scope-end reclaim is declined because the value escaped, and
// nothing else reclaims it). §28.3's *Producers* table makes the plain-`var`
// binding OWNED exactly as it makes the `using var` binding owned, so the
// same §28.11 clause 2 rejects it.
// =============================================================================
BEZEL_TEST(borrowing_array_literal_collection_store_7209,
           var_bare_array_literal_collection_store_rejected_spec_28_11) {
    auto r = parseAndCheckOwnership(
        escapeProg("var a: Array<int> := [1, 2]; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(r, kDiagMarker));
    BEZEL_EXPECT_TRUE(hasErrorWith(r, kDiagSpec));
}

// =============================================================================
// OVER-REJECT GUARD — the collection at the SAME scope as the source. §28.11
// clause 2 rejects a store "into a collection that provably outlives that
// scope"; a same-scope collection does not outlive it, and the `using var`
// cleanup order disposes the collection BEFORE the source (reverse of
// establishment), so the store is sound. MEASURED before the fix: this
// program compiles clean and runs clean under ASan/LSan.
// =============================================================================
BEZEL_TEST(borrowing_array_literal_collection_store_7209,
           same_scope_collection_store_accepted_control_spec_28_11) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    using var a: Array<int> := [1, 2]; "
        "    using var l: Array<Array<int>> := []; "
        "    l.Add(a); "
        "    Console.WriteLine(l[0][0]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// OVER-REJECT GUARD — the collection is SHORTER-lived than the source (the
// collection is declared inside the source's scope). The store is plainly
// fine: the container dies first. MEASURED before the fix: compiles clean,
// runs clean under ASan/LSan.
// =============================================================================
BEZEL_TEST(borrowing_array_literal_collection_store_7209,
           narrower_scope_collection_store_accepted_control_spec_28_11) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    using var a: Array<int> := [1, 2]; "
        "    do "
        "      using var l: Array<Array<int>> := []; "
        "      l.Add(a); "
        "      Console.WriteLine(l[0][0]); "
        "    end do; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// CONTROL — the USER-CLASS element twin (`Array<R>` over a user class `R`)
// was ALREADY rejected before this fix, by the general §28.11 machinery
// (the source is an ordinary owned scope-bound local, and `isBorrowableType`
// answers true for a user class). This row pins that the verdict for the
// user-class twin is unchanged — the fix must not double-report it, and the
// two channels must agree on the VERDICT even though their wordings differ
// (the user-class path names the general §28.11 rule; the built-in-class
// path names the collection-literal rule).
// =============================================================================
BEZEL_TEST(borrowing_array_literal_collection_store_7209,
           user_class_element_collection_store_still_rejected_control) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class R public V: int := 7; end R; "
        "public class C "
        "  public static method Main(): int "
        "    var l: Array<R> := []; "
        "    do "
        "      using var r: R := new R(); "
        "      l.Add(r); "
        "    end do; "
        "    Console.WriteLine(l[0].V); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "would outlive its source"));
    // The user-class path must NOT be reported by this fix's own recognizer.
    BEZEL_EXPECT_FALSE(hasErrorWith(r, kDiagMarker));
}

// =============================================================================
// CONTROL — the FIELD sink of the same recognizer (#3706) is untouched: a
// collection-literal local stored into a FIELD is still rejected by §28.3
// clause 1, with #3706's own wording. This row pins that the two sinks
// still answer the same question the same way after the fix.
// =============================================================================
BEZEL_TEST(borrowing_array_literal_collection_store_7209,
           field_store_of_array_literal_local_unchanged_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Box public Items: Array<int> := []; end Box; "
        "public class C "
        "  public static method Main(): int "
        "    var b := new Box(); "
        "    using var xs: Array<int> := [1, 2, 3]; "
        "    b.Items := xs; "
        "    return b.Items.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "may not be stored into a field"));
}

// =============================================================================
// END-TO-END — the strict AOT oracle: the issue's program is REJECTED by
// the compiler, so the use-after-free can no longer happen at run time.
// The row asserts the DIAGNOSTIC (and that the program never ran), not
// "it did not crash".
// =============================================================================
BEZEL_TEST(borrowing_array_literal_collection_store_7209,
           aot_oracle_rejects_the_escaping_insert_spec_28_11) {
    JitResult r = runAot(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    using var l: Array<Array<int>> := []; "
        "    do "
        "      using var a: Array<int> := [1, 2]; "
        "      l.Add(a); "
        "    end do; "
        "    Console.WriteLine(l[0][0]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kDiagMarker));
    BEZEL_EXPECT_TRUE(
        bezel::test::failedAtCompileTimeWithRealLocation(r));
}
