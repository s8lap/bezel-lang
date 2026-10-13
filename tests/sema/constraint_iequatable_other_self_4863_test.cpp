// fix #4863 / spec §14.2 *Constraints* / §14.2 *Interface conformance*
// (v0.307): an interface-constraint check is an interface INSTANTIATION,
// not a declaration-identity match. `where T: IEquatable<Other>` requires T
// to conform to `IEquatable<Other>` specifically — `C: IEquatable<C>` does
// not conform to `IEquatable<Other>`, and the body's `a.Equals(o)` would
// then be called with an `Other` argument against a `Self`-typed `Equals`
// (type confusion: the synthetic record equality returns the wrong value,
// the class equality answers with the wrong view of the receiver).
//
// ROOT CAUSE (traced, src/sema/TypeResolution_Checks.cpp):
// `Resolver::argSatisfiesTypeBound` walked the candidate's `baseTypes`
// (TypeSymbol*) and returned `true` on declaration-identity match,
// explicitly punting generic-argument matching "to the variance/
// substitution slice". No such follow-up exists in this code path — the
// §14.2 per-instantiation constraint check at every `Named<...>` site
// uses `argSatisfiesTypeBound` as its sole satisfiability authority, so
// the punt became a silent over-acceptance.
//
// THE FIX walks `baseTypeRefs` in parallel with `baseTypes` so each
// candidate base's INSTANTIATION is visible at the comparison; when the
// bound carries type arguments, structural equality (`typeRefEqual`) of
// the full TypeRef — not just declaration match — is required. A non-
// generic bound (e.g. `IDisposable`) keeps the legacy accept-on-match
// behaviour unchanged. A candidate base whose `baseTypeRefs` slot is
// missing AND the bound has type args is treated as "cannot confirm" and
// rejected rather than silently admitted — the conservative direction
// for the unsoundness class.
//
// SCOPE, MEASURED: §17 non-author review of the L8r series (#4831 / narrow
// #4755 / #4788). The companion codegen fix in `tryLowerValueSemanticsCall`
// (refusing an `internal:` error when the call's argument type is not the
// record) is a separate concern — this file tests the SEMa rejection, which
// removes the silent-wrong-code failure mode at the constraint layer.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// §14.2 "type argument `C` does not satisfy the constraint `where T:
// <IB>` (§14.2)" — the diagnostic `checkOneConstraint` already emits at
// the instantiation site for a failed type-bound (see
// TypeResolution_Names.cpp ~L3793 and TypeResolution_Checks.cpp ~L8200).
// The bound name is the declaration's bare name — the §14.2
// instantiation-site diagnostic intentionally drops the bound's generic
// args (a deliberate spec choice for readability of the common case,
// not the args a more elaborate spelling would add). Every reject row
// below asserts the SAME shape.
bool hasUnsatisfiedConstraint(const bezel::test::JitResult& r,
                              std::string_view boundName) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not satisfy") != std::string::npos &&
            d.message.find("14.2") != std::string::npos &&
            d.message.find(boundName) != std::string::npos)
            return true;
    return false;
}

// Shared prelude: an `Other` class (the wrong type for IEquatable<Self>),
// a non-conforming `C: IEquatable<C>` class, and a genuinely-conforming
// `C2: IEquatable<Other>` class. `H.F<T>(a: T, o: Other) where T:
// IEquatable<Other>` is the constrained generic — the same body as the
// issue's Repro 1. The issue's Repro 2 (record `Point: IEquatable<Point>`)
// is NOT included here because the record-conformance synthesis fix is a
// separate work-in-progress (the L8r series — #4831 / #4755 / #4788 is
// not in the branch this fix is built on); the SEMa constraint check is
// independent of it and the class case already pins the same acceptance-
// vs-rejection axis (a class listing IEquatable<Self> vs. IEquatable<Other>
// exhibits identical BFS-vs-typeRefEqual divergence).
const std::string kPrelude =
    "module M; "
    "public class Other "
    "  public S: string; "
    "  public constructor(s: string) S := s; end constructor; "
    "end Other; "
    "public class C: IEquatable<C> "
    "  public constructor() end constructor; "
    "  public method Equals(other: C): bool => true; "
    "  public override method GetHashCode(): int => 0; "
    "end C; "
    "public class C2: IEquatable<Other> "
    "  public constructor() end constructor; "
    "  public method Equals(other: Other): bool => true; "
    "end C2; "
    "public static class H "
    "  public static method F<T>(a: T, o: Other): bool "
    "    where T: IEquatable<Other> => a.Equals(o); "
    "end H; "
    // §14.2 control: a generic with `where T: IEquatable<T>` (the
    // "identity" bound). `C: IEquatable<C>` satisfies this — the
    // bound's type argument IS the candidate.
    "public static class IdH "
    "  public static method F<T>(a: T, o: T): bool "
    "    where T: IEquatable<T> => a.Equals(o); "
    "end IdH; "
    // §14.2 control: a generic with NO `where` clause at all — purely to
    // confirm the fix does not regress an unconstrained generic.
    "public static class NH "
    "  public static method F<T>(a: T): T return a; end F; "
    "end NH; ";

bezel::test::JitResult runMain(const std::string& mainBody) {
    return runAot(kPrelude +
                  "public class Program "
                  "  public static method Main(): int " + mainBody +
                  "  end Main; "
                  "end Program;");
}

}  // namespace

// ============================================================
// REJECT ROWS — #4863's own repair. Each fails without the fix
// (compiles clean, then the body either runs with a wrong value
// or, for the record, with a degraded zero) and rejects with it.
// ============================================================

// Case 1: the issue's verbatim class repro. `C: IEquatable<C>` is NOT
// `IEquatable<Other>`, so `H.F<C>(c, other)` is unsatisfied.
BEZEL_TEST(constraint_iequatable_other_self_4863,
           class_conforming_only_to_self_other_constraint_rejected_spec_14_2) {
    auto r = runMain(
        "using var c := new C(); "
        "Console.WriteLine(H.F(c, new Other(\"hi\"))); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUnsatisfiedConstraint(r, "IEquatable"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT-SIDE CONTROLS — every shape the fix must NOT regress.
// ============================================================

// Case 3: a class genuinely conforming to `IEquatable<Other>` — must
// still satisfy `where T: IEquatable<Other>`. The fix's typeRefEqual
// returns true here because the base's instantiation IS `IEquatable<Other>`.
BEZEL_TEST(constraint_iequatable_other_self_4863,
           class_conforming_to_other_other_constraint_accepted_control_spec_14_2) {
    auto r = runMain(
        "using var c := new C2(); "
        "Console.WriteLine(H.F(c, new Other(\"hi\"))); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Case 4: the "identity" bound `where T: IEquatable<T>` — the bound's
// type argument is the candidate itself, so `C: IEquatable<C>` satisfies.
// The fix walks bases (C → IEquatable<C>), declaration matches IEquatable,
// typeRefEqual compares [C] to [C]. The previous (declaration-only) code
// also accepted this — pinned here so a future rewrite cannot silently
// regress it.
BEZEL_TEST(constraint_iequatable_other_self_4863,
           identity_iequatable_self_constraint_accepted_control_spec_14_2) {
    auto r = runMain(
        "using var c := new C(); "
        "Console.WriteLine(IdH.F(c, new C())); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Case 5: a generic with NO `where` clause at all — the fix's
// `argSatisfiesTypeBound` is only called for "type" bounds, so this
// control confirms the fix does not touch the unconstrained-generic path.
BEZEL_TEST(constraint_iequatable_other_self_4863,
           unconstrained_generic_accepted_control_spec_14_2) {
    auto r = runMain(
        "using var c := new C(); "
        "Console.WriteLine(NH.F(c).Equals(new C())); "
        "return 0;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}