// Spec-derived test for the rule-corpus row s28_11_0006
// (tests/conformance/rules/cases/s28_11_0006.bzl) — §28.11's element-category
// table (docs v0.307), read together with issue #4393's ruling (v0.300):
//
//   "In the element-category table, a runtime-managed reference element type
//   — `string`, a function type, `Type` (§28.9) — is rejected at
//   instantiation in the owning-family column: no holder acquires an
//   obligation for such a value, so an owned element slot has nothing to
//   own."
//
// #4393's ruling made `Type` join `string` and function values as
// runtime-managed (§28.9/§28.1/§28.11/§17.10/stdlib §2.13/rationale §4,
// v0.300) and explicitly flagged that "At the gate, §28.11's element-
// category table ... was caught missing `Type` from its runtime-managed row,
// which would have let `OwningArray<Type>` take owned element slots against
// this very ruling — now fixed." That claim was INCOMPLETE: the owned-
// MARKER-admission check (TypeResolution_Names.cpp's `owned`-admissibility
// switch) was taught `nm == "Type"`, but the SEPARATE owning-COLLECTION-
// ELEMENT predicate (`Resolver::owningElementBadDesc`,
// TypeResolution_Checks.cpp — the single authority §3.6 rule 2's
// declaration-site check and the per-instantiation Pass-2 sweep both share,
// #2713) was not: its "No TypeSymbol" arm recognises only
// `isValueBuiltinByName`, which deliberately excludes `Type` (`Type` is not
// a value-type builtin — it is a class-shaped runtime-managed reference).
// `OwningArray<Type>` therefore instantiated with ZERO diagnostics.
// MEASURED on the corpus case directly (FAIL-WITHOUT: the row read
// UNBANKED/pass-when-banked-reject pre-fix; green on the fix).
//
// per §28.9, §28.11, v0.300/v0.307, #4393

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The predicate's own §14.4 property line, shared by every owning-element
// rejection (mirrors owning_collection_choice_element_2744_test.cpp's own
// helper) — asserting the specific text distinguishes this fix's
// contribution from an unrelated cascade error.
bool hasOwningElementTypeRejection(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("requires a class or interface element type") ==
            std::string::npos)
            continue;
        if (d.message.find("runtime `Type` token") == std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// REJECT — the declaration-site predicate.
// =============================================================================

// The corpus case's exact shape: `new OwningArray<Type>()` at a `using var`
// declaration site.
BEZEL_TEST(owning_collection_type_element_0006,
           owning_array_type_element_rejected) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public method Size(): int "
        "    using var types := new OwningArray<Type>(); "
        "    return types.Count; "
        "  end Size; "
        "end Program; "
        "class Entry public static method Main(): int "
        "  var p := new Program(); return p.Size(); end Main; end Entry;");
    BEZEL_EXPECT_TRUE(hasOwningElementTypeRejection(r));
}

// `OwningDictionary<K, V>`'s VALUE slot (the owned slot, §28.11) is the same
// owning-family shape.
BEZEL_TEST(owning_collection_type_element_0006,
           owning_dictionary_value_type_element_rejected) {
    auto r = runAot(
        "module M; "
        "class Program public static method Main(): int "
        "  using var d := new OwningDictionary<int, Type>(); "
        "  return d.Count; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(hasOwningElementTypeRejection(r));
}

// =============================================================================
// ACCEPT — no over-rejection.
// =============================================================================

// The borrowing-family counterpart (`Array<Type>`) is unaffected — `Type` is
// admissible as a plain (non-owned) element; only the OWNING column rejects
// it (the rule is symmetric with the existing `string` / function-value
// rows, both of which the same predicate already carves out identically).
// An empty literal is used deliberately: obtaining an actual `Type` VALUE
// needs `typeof`/`GetType`, and stdlib §2.13 records both as not yet
// codegen-reachable (#487, #511) — orthogonal to this fix, which is about
// `Type` naming a generic ARGUMENT, never about constructing one. An empty
// `Array<Type>` still exercises the type-position resolution this fix
// touches without tripping the unrelated, unimplemented-reflection gap.
BEZEL_TEST(owning_collection_type_element_0006,
           borrowing_array_type_element_accepted) {
    auto r = runAot(
        "module M; "
        "class Program public static method Main(): int "
        "  const xs: Array<Type> := []; "
        "  return xs.Count; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// `OwningArray<Widget>` (a class element) stays admitted — the existing
// accept path for the owning family is unaffected by teaching the predicate
// about `Type`.
BEZEL_TEST(owning_collection_type_element_0006,
           owning_array_class_element_still_accepted) {
    auto r = runAot(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Program public static method Main(): int "
        "  using var arr := new OwningArray<Widget>(); "
        "  arr.Add(new Widget()); "
        "  return arr.Count; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
