// fix-forward #5424 / spec §14.2 rule 1 (*Interface conformance*, v0.315)
// and §14.2 *Constraints* ("A type-bound constraint — naming a class or
// interface — implies non-null T").
//
// fix #5424 (`5c959e0c`) added §14.2 rule 1's INHERITED arm to
// `argSatisfiesTypeBound` (`src/sema/TypeResolution_Checks.cpp`): when the
// bare-parameter-substituted bound argument (e.g. `Derived`) does not
// `typeRefEqual` the inherited interface's type argument (e.g. `GBase`),
// accept if the candidate's declaration is a subtype of that declaration.
// That arm is correct and stays — these rows pin it (`inherited_compareto_`
// `3566` pins its runtime consequences; the accept rows below pin its
// boundary).
//
// WHAT 5c959e0c GOT WRONG — TWO GUARDS, ONE ROOT CAUSE. The arm compares
// DECLARATIONS (`TypeSymbol*`), which are blind to two things the
// instantiation-level comparison (`typeRefEqual`) is not:
//
//   (a) NULLABILITY. `typeRefEqual` rejects `Ord?` against `Ord` on
//       `a->nullable != b->nullable` (Checks.cpp, `Resolver::typeRefEqual`).
//       A declaration walk cannot see that flag, so `Ord?` "matched" `Ord`
//       and a nullable element silently satisfied `where T: IComparable<T>`.
//       MEASURED on 5c959e0c, `SortedSet<Ord?>` produced NO §14.2 diagnostic
//       at all — it fell through to codegen's stdlib §4.18 nullable message
//       (`LlvmBackend`'s `rejElemTy->nullable` arm), which names neither the
//       constraint nor the nullability. The §14.2 rejection that
//       `checkOneConstraint`'s own `arg->nullable` arm exists to report was
//       simply gone.
//
//   (b) THE DEGENERATE SELF-MATCH, which is the mechanism behind (a). The
//       walk was seeded with `arg->declaration` ITSELF and the loop's first
//       iteration tests `cur == tArgDecl` — so when the candidate and the
//       interface's type argument named the SAME declaration, the walk
//       succeeded at distance 0, unconditionally, before looking at a single
//       base. Every same-declaration mismatch was therefore accepted, not
//       just the nullable one. §14.2 rule 1 is about INHERITANCE ("inherits
//       from a class which conforms to I"); `argDecl == tArgDecl` is not
//       inheritance — it is the direct-listing case, already decided by the
//       `typeRefEqual` immediately above, whose failure is real.
//
// THE FIX seeds the walk with the candidate's BASES (so a hit is always a
// strict subtype, never the candidate itself) and refuses the arm outright
// when either side is nullable. Neither guard widens what §14.2 admits: the
// negative probe below (a class with no listing and no conforming base) is
// still rejected, and the no-listing/no-base shape is the shape the guard
// that (a) replaces could otherwise have laundered.
//
// SCOPE. `SortedSet`/`SortedDictionary` are the reachable spellings of the
// §4.18 / §4.17 self-referential `where T: IComparable<T>` bound through
// `kNullDeclSelfRefConstraints` (`TypeResolution_Names.cpp`), which calls
// `argSatisfiesTypeBound` directly. The INHERITED + NULLABLE combination is
// pinned here as `inherited_nullable_...`: it was broken by 5c959e0c in the
// same way, and is the shape that shows guard (a) is not merely a
// same-declaration patch — `Derived?` names a DIFFERENT declaration from
// `GBase`, so only the nullability guard rejects it.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The §14.2 instantiation-site diagnostic, asserted on its TEXT: a bare
// `hasErrors()` cannot tell this rule from any other error in the program,
// and `runAot`'s WIFSIGNALED path synthesizes an Error for a runtime
// SIGSEGV, so "did not run" alone cannot distinguish a genuine rejection
// from a crash.
bool rejectsWithUnsatisfiedConstraint(const std::string& src,
                                      std::string_view argName,
                                      std::string_view boundName) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(std::string("type argument `") +
                           std::string(argName) +
                           "` does not satisfy the constraint") !=
                std::string::npos &&
            d.message.find(boundName) != std::string::npos &&
            d.message.find("14.2") != std::string::npos)
            return true;
    return false;
}

// stdlib §4.18 (v0.286) declares `SortedSet<T> where T: IComparable<T>`;
// §4.17 (v0.286) declares `SortedDictionary<K, V> where K: IComparable<K>`.
const char* kPrelude =
    "module M; import System.Collections.Generic; "
    // Directly conforming base: `IComparable<GBase>` listed on GBase itself.
    "class GBase: IComparable<GBase> "
    "  public Key: int := 0; "
    "  public constructor(k: int) Key := k; end constructor; "
    "  public method CompareTo(other: GBase): int "
    "    return Key - other.Key; "
    "  end CompareTo; "
    "end GBase; "
    // Inherits the conformance from GBase; lists no IComparable of its own.
    "class Derived: GBase "
    "  public constructor(k: int) base(k); end constructor; "
    "end Derived; "
    // Directly self-comparable, for the NULLABLE direct-listing shape.
    "class Ord: IComparable<Ord> "
    "  public Key: int := 0; "
    "  public constructor(k: int) Key := k; end constructor; "
    "  public method CompareTo(other: Ord): int "
    "    return Key - other.Key; "
    "  end CompareTo; "
    "end Ord; "
    // NO listing and NO conforming base: the negative probe. §14.2 rule 1
    // has no arm that admits this, and the fix must not create one.
    "class Plain "
    "  public Key: int := 0; "
    "  public constructor(k: int) Key := k; end constructor; "
    "end Plain; ";

std::string prog(const std::string& body) {
    return std::string(kPrelude) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// GUARD (a) — NULLABILITY IS NOT INHERITED (§14.2, v0.315).
// ===========================================================================

// The row this fix-forward exists for: the DIRECT-listing nullable shape.
// `Ord?` and `Ord` name the SAME declaration, which is exactly why 5c959e0c
// accepted it (its walk self-matched at distance 0) and why the declaration
// walk could not see the difference. A nullable type argument does not
// satisfy a type-bound constraint — "nullable types `MyClass?` and `IFoo?`
// are distinct from their non-nullable forms (§7.1) and do not themselves
// implement the interfaces of their non-nullable counterparts: a `null`
// value satisfies no interface contract" (§14.2, v0.315).
//
// MEASURED fail-without: on 5c959e0c this program compiles with NO §14.2
// diagnostic (only stdlib §4.18's codegen nullable message), so this row's
// `rejectsWithUnsatisfiedConstraint` is false. Passes with the fix.
BEZEL_TEST(inherited_conformance_nullability_5424,
           direct_listing_nullable_type_argument_rejected_spec_14_2) {
    BEZEL_EXPECT_TRUE(rejectsWithUnsatisfiedConstraint(
        prog("  using var b := new SortedSet<Ord?>(); return b.Count; "),
        "Ord", "IComparable"));
}

// The INHERITED + NULLABLE shape, which distinguishes guard (a) from the
// degenerate-self-match alone: `Derived?` and `GBase` name DIFFERENT
// declarations, so a walk that merely skipped the distance-0 self-match
// would still accept this. Only the nullability guard rejects it.
//
// MEASURED fail-without: on 5c959e0c the walk finds GBase one step up
// Derived's base chain and accepts, so sema emits nothing and this row is
// false.
BEZEL_TEST(inherited_conformance_nullability_5424,
           inherited_nullable_type_argument_rejected_spec_14_2_rule_1) {
    BEZEL_EXPECT_TRUE(rejectsWithUnsatisfiedConstraint(
        prog("  using var b := new SortedSet<Derived?>(); return b.Count; "),
        "Derived", "IComparable"));
}

// The same guard through the OTHER reachable spelling of the same
// self-referential bound — §4.17's `SortedDictionary<K, V> where K:
// IComparable<K>` — so the fix is not pinned only on the SortedSet path.
BEZEL_TEST(inherited_conformance_nullability_5424,
           inherited_nullable_dictionary_key_rejected_spec_4_17) {
    BEZEL_EXPECT_TRUE(rejectsWithUnsatisfiedConstraint(
        prog("  using var d := new SortedDictionary<Derived?, int>(); "
             "  return d.Count;"),
        "Derived", "IComparable"));
}

// ===========================================================================
// THE NEGATIVE PROBE — §14.2 IS NOT WEAKENED.
// ===========================================================================

// A class with NO `IComparable` listing and NO conforming base. Neither
// rule 1's direct arm, its inherited arm, nor the implicit arm can admit it,
// and a fix that widened the check to rescue #5424's rows would be tempted to
// reach here. `SortedSet<Plain>` must stay rejected.
BEZEL_TEST(inherited_conformance_nullability_5424,
           no_listing_no_conforming_base_still_rejected_spec_14_2) {
    BEZEL_EXPECT_TRUE(rejectsWithUnsatisfiedConstraint(
        prog("  using var b := new SortedSet<Plain>(); return b.Count; "),
        "Plain", "IComparable"));
}

// ===========================================================================
// ACCEPT CONTROLS — the #5424 fix is NOT narrowed away by these guards.
// ===========================================================================

// Direct listing, non-nullable: the ordinary §14.2 satisfaction. If guard
// (a) or guard (b) over-reached, this is the first thing that would break.
BEZEL_TEST(inherited_conformance_nullability_5424,
           direct_listing_nonnullable_still_accepted_spec_14_2) {
    auto r = runAot(prog(
        "  using var b := new SortedSet<Ord>(); return b.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// INHERITED listing, non-nullable: the exact shape #5424 fixed and the one
// the inherited arm exists for. This is the row that fails if the walk is
// seeded wrongly (e.g. if it started from the candidate and the guard were
// instead written to reject every hit) — it must still CONSTRUCT.
BEZEL_TEST(inherited_conformance_nullability_5424,
           inherited_nonnullable_still_accepted_spec_14_2_rule_1) {
    auto r = runAot(prog(
        "  using var b := new SortedSet<Derived>(); return b.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The inherited shape through SortedDictionary (§4.17), non-nullable.
BEZEL_TEST(inherited_conformance_nullability_5424,
           inherited_nonnullable_dictionary_key_accepted_spec_4_17) {
    auto r = runAot(prog(
        "  using var d := new SortedDictionary<Derived, int>(); "
        "  return d.Count;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}