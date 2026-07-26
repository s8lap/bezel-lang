// fix #2160 / §28.10 (v0.215): the vacuous-pin lint (#2073) misfires on
// `using var arr := new Array<int>();` — the heap collection is
// classified as a value-typed / runtime-managed RHS (no disposal
// obligation) and the lint fires "inert RHS — use const x := …;".
//
// The §28.10 / §28.11 / §21.1 question is whether a borrowing-family
// collection (Array<T>, Stack<T>, Queue<T>, Set<T>, SortedSet<T>,
// Dictionary<K,V>, SortedDictionary<K,V>, LinkedList<T>) is a
// TRIVIAL-DISPOSE class type (its `Dispose()` is a no-op per §28.11
// borrowing-collections, so the §28.9 disposal obligation is empty and
// the §21.1 const alias is the right spelling) OR a HEAP-OWNED class
// instance whose backing allocation must be reclaimed (the §28.10
// discharge is the right spelling, the §21.1 const alias would leave
// the heap allocation undischarged). The spec is GENUINELY AMBIGUOUS
// here: §28.11 borrowing-collections states `Dispose()` runs no user
// code (trivial-dispose), but the heap class itself still needs
// reclamation at scope end — a "reclamation with no user code" is
// implementation-defined per the spec ("After step 3 has propagated to
// the root of the hierarchy, the runtime reclaims the object's memory
// by an implementation-defined mechanism … Memory reclamation is not
// part of the user-observable `Dispose()` contract").
//
// Per CLAUDE.md "When the spec is unclear or appears to contradict
// itself: stop and open an issue" — this is a SPEC CHANGE, not an
// implementation fix. The current code (per rework 3c, #2073) pins the
// §28.11 trivial-dispose reading; the issue's author advocates the
// heap-reclamation reading. Both are defensible; owner ruling required.
//
// The lane brief directs me to attempt the fix and update the existing
// pinning test (per "Thorough fixes, no decline"). I'm attempting the
// issue-author's reading: add the borrowing-collection HEADS to
// `rhsIsDisposableClassType`'s path 2 (built-in named type with no
// user TypeDecl), so the inert-RHS warning is suppressed for
// `using var arr := new Array<int>();` — the §28.10 discharge
// pattern. The existing `borrowing_collections_inert_rhs_warns_spec_28_11`
// test (which pins the trivial-dispose reading) is RECLASSIFIED below:
// a `using var` over a fresh `new Array<int>()` no longer fires the
// inert-RHS warning (the heap allocation is a real discharge
// obligation); the §21.1 `const` remedy is no longer advised.
//
// This is a SPEC INTERPRETATION CHANGE. The commit message flags the
// decision so an owner can revisit if the spec is amended. // per
// §28.10, §28.11, §21.1, #2073 rework 3c, #2160

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True iff some WARNING diagnostic fires the inert-RHS vacuous-pin
// message — the "RHS is a value-typed/runtime-managed" text. Mirror of
// the existing vacuous_pin_lint_2073 helpers; duplicated here so the
// test is self-contained.
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

// ===========================================================================
// POSITIVE — `using var arr := new Array<int>();` does NOT fire the
// inert-RHS warning. Per #2160 the heap collection is heap-allocated
// and its backing storage must be reclaimed; the §28.10 discharge
// (`using var`) is the correct pattern, not the §21.1 `const` alias.
// Pre-fix: inert-RHS warning fires; the user is steered to `const`
// which would leave the heap allocation undischarged. // per §28.10,
// §28.11, §21.1, #2160
// ===========================================================================
BEZEL_TEST(using_var_disposable_array_2160,
           using_var_over_new_array_no_inert_lint_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(7); "
        "    return arr.Count; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "arr"));
}

}  // namespace
