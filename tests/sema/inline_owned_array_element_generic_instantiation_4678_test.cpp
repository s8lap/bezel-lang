// Spec-derived regression test for issue #4678 — §15.1.1's class-or-interface
// restriction on an OWNING inline array (`[N owned]T`) was never checked at a
// GENERIC INSTANTIATION site.
//
// Spec §15.1.1, v0.307: "The form `[N owned]T` ... denotes an inline array
// whose slots own their (non-null) class-typed contents. T must be a class or
// interface type; value types and runtime-managed reference types are not
// admitted with the `owned` marker. When T is a generic type parameter, the
// class-or-interface restriction is checked at instantiation ... paralleling
// the generic-class-fields rule of §28.5." Per #4367's ruling (v0.300), a
// field-type mention (no `new` required) is itself an instantiation site.
//
// ROOT CAUSE (measured on this lane's base, r20-c25, with build/bezelc):
// `Resolver::resolveType`'s InlineArray arm (TypeResolution_Names.cpp) admits
// a bare generic parameter's `owningElementBadDesc` result unconditionally —
// correct at the DECLARATION site (no concrete type is known there) but never
// re-decided once a concrete instantiation supplies one, because nothing
// recorded the site. `[N owned]T` cannot reuse the existing NAMED
// owning-collection recorder (`owningElementSites_` /
// checkOwningElementInstantiations, #2713): it is TypeRef::Kind::InlineArray,
// not a Named type with a `headName()` to key `container` by. This fix adds a
// parallel recorder/Pass-2 pair (`owningInlineArrayElementSites_` /
// checkOwningInlineArrayElementInstantiations, both TypeResolution_Names.cpp)
// modelled on the same shape, including the #2742 method-generic-parameter
// shadowing guard and the chained-binding worklist (`class Outer<U>` holding
// `Inner<U>` whose body holds the owning inline array).
//
// Corpus: rule-corpus case s15_1_1_0003 (tests/conformance/rules/cases) is
// this exact repro; it flips fail -> pass with this fix (measured against the
// full 2185-row rule corpus: 0 REGRESSION, 1 UNBANKED = s15_1_1_0003).
//
// ⚠ FALSE-REJECT IS THE MAIN HAZARD (a fix that turns accepts into rejects),
// so the controls below cover: (1) the exact issue repro (bare-parameter,
// field-type-mention instantiation site) rejects with the specific §15.1.1
// diagnostic; (2) the identical shape with a legal class element type
// compiles AND RUNS (not just rc==0 — the CLAUDE.md §13 "silent-accept-then-
// crash" caution); (3) a generic METHOD's own type parameter that SHADOWS the
// enclosing type's (the #2742 shape) must NOT be validated against the
// enclosing type's (bad) binding — it must run.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff compilation failed with an ERROR diagnostic carrying `needle`.
// Deliberately not a bare "did it reject": a coarse check cannot distinguish
// a genuine compile-time rejection from a codegen crash's synthesized
// diagnostic, and this fix's whole point is to turn a *silent accept* into a
// specific, sourced §15.1.1 diagnostic.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// True iff the program compiles, links, runs and exits with `expected`.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// The issue's own repro, verbatim (== rule-corpus case s15_1_1_0003): a
// field-type mention `Pool<string>` at `Holder._p` is the instantiation site
// per #4367's ruling — no `new` is involved anywhere in this program.
const char* kFieldMentionRepro =
    "module M4678; "
    "public class Pool4678<T> where T: class "
    "  private _slots: [4 owned]T?; "
    "  public constructor() end constructor; "
    "end Pool4678; "
    "public class Holder4678 "
    "  private _p: Pool4678<string>?; "
    "  public constructor() end constructor; "
    "end Holder4678;";

// Same shape, legal class element type — the fix must not disturb this.
std::string classElementSrc(const char* elementCtorAndFields) {
    return std::string(
               "module M4678b; "
               "class Elem4678 public n: int := 0; end Elem4678; "
               "public class Pool4678b<T> where T: class "
               "  private _slots: [4 owned]T?; "
               "  public constructor() end constructor; "
               "end Pool4678b; "
               "public class Holder4678b "
               "  private _p: Pool4678b<Elem4678>?; "
               "  public constructor() end constructor; "
               "end Holder4678b; "
               "class P4678b "
               "  public static method Main(): int ") +
           elementCtorAndFields +
           "  end Main; "
           "end P4678b;";
}

}  // namespace

// ===========================================================================
// THE BUG — field-type mention of `Pool4678<string>` (rule-corpus
// s15_1_1_0003, spec §15.1.1 / #4367). Pre-fix: compiled with exit 0, no
// diagnostic at any severity.
// ===========================================================================
BEZEL_TEST(inline_owned_array_generic_instantiation_4678,
           field_type_mention_string_rejected_spec_15_1_1) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        kFieldMentionRepro,
        "`[N owned]T` requires T to be a class or interface type"));
    // Names the DECLARING type and its parameter, and the offending binding
    // (§3.6 rule 2's "naming the property, the offending type argument, and
    // the structural reason").
    BEZEL_EXPECT_TRUE(rejectsWith(kFieldMentionRepro, "\xC2\xA7""15.1.1"));
    BEZEL_EXPECT_TRUE(rejectsWith(kFieldMentionRepro, "Pool4678"));
    BEZEL_EXPECT_TRUE(rejectsWith(
        kFieldMentionRepro,
        "runtime-managed reference type"));  // string's owningElementBadDesc
}

// ===========================================================================
// ACCEPT CONTROL — the identical shape at a legal class element type must
// still compile, link, run and dispose cleanly.
// ===========================================================================
BEZEL_TEST(inline_owned_array_generic_instantiation_4678,
           field_type_mention_class_element_accepted_and_runs) {
    BEZEL_EXPECT_TRUE(runs(42, classElementSrc(
        "var h := new Holder4678b(); "
        "return 42; ")));
}

// ===========================================================================
// SHADOWING CONTROL (#2742 shape) — a generic METHOD's own type parameter
// that shares a name with the enclosing type's must NOT be validated against
// the enclosing type's binding. `Box4678<int>` (int is NOT a valid owning-
// inline-array element) holds a method `M4678<T>()` whose body's `[2 owned]T`
// refers to the METHOD's `T`, bound to a real class at the call site. Without
// the nameBoundByCurrentMethod guard this fix carries (mirroring #2713's own
// guard on the sibling owning-collection recorder), Pass 2 would misattribute
// the method-local `T` to Box4678's `int` binding and falsely reject a legal
// program.
// ===========================================================================
// FIX #5393 (§14.1, v0.314): the declaration is now rejected at the type-
// parameter list. The #2713 guard is no longer needed for this shape.
// The old test pinned the accept; it now flips to a declaration-site reject.
BEZEL_TEST(inline_owned_array_generic_instantiation_4678,
           method_generic_param_shadowing_not_misattributed_spec_3_6) {
    auto src = std::string(
        "module M4678c; "
        "class Elem4678c public n: int := 0; end Elem4678c; "
        "class Box4678c<T> "
        "  public method M4678c<T>(): int "
        "    var arr: [2 owned]T?; "
        "    return 7; "
        "  end M4678c; "
        "end Box4678c; "
        "class P4678c "
        "  public static method Main(): int "
        "    var b := new Box4678c<int>(); "
        "    return b.M4678c<Elem4678c>(); "
        "  end Main; "
        "end P4678c;");
    BEZEL_EXPECT_TRUE(rejectsWith(src, "rename one of them"));
}
